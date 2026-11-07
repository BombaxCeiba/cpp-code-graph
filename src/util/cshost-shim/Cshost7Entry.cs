// Cshost7Entry.cs — 7 线入口：宿主经 hostfxr_get_runtime_delegate 取得本
// 程序集中 [UnmanagedCallersOnly] 方法的原生指针后直接调用——指针进出、
// 无跨封送限制。对象协议三件（ObjCreate/ObjCall/ObjDestroy）走结构体桥：
// 参数 = 结构体指针 + 字节数，载荷与结果都不经共享内存。
//
// 结构体桥 ABI（DAS 形态：size + abi_version 双字段前缀，宿主与垫片
// 同构镜像；未来加字段时旧垫片按 size 判据直接暴露不匹配，绝不误读）：
//   struct CcgCsCallV1 {          // LayoutKind.Sequential，自然对齐
//     uint32  size;               // 调用方填结构体总字节数（判据字段）
//     uint32  abi_version;        // 协议版本（当前 1）
//     void*   cmd;                // 载荷指针（ObjCreate/ObjCall：UTF-8
//                                 //  JSON；ObjDestroy：token 十进制串）
//     uint64  cmd_len;            // 载荷字节数
//     void*   out_slot;           // 出参槽（字段就地写回）：垫片把
//                                 //  AllocCoTaskMem 的 UTF-8 NUL 串指针
//                                 //  直接写进本字段，宿主调用返回后从
//                                 //  本方结构体读回并 CoTaskMemFree
//   }
// 调用形态（cdecl）：int ObjCreate/ObjCall/ObjDestroy(结构体指针, 字节数)。
// 返回值语义与 51 线一致：ObjCreate 正数 = token、三件负数 = ObjectHost
// 专用错误码；out_slot 在成功时写方法返回 JSON、失败时写诊断文本。
//
// 本线仅承载 SelectStringCore（VS 自带 Microsoft.Build.dll 是 net472
// 程序集，CoreCLR 装不下 .NET Framework 家族）：ObjCreate 请求
// "MsbuildEval" 返回 ErrNotOnLine 专用错误码，宿主侧据此报"求值仅
// 51 线可用"。
//
// SMA 及其依赖不在默认解析路径上：静态构造挂 AssemblyResolve，按环境
// 变量 CCG_PSHOME 指向的 pwsh 安装目录以简单名加载（宿主在初始化前
// 设置该变量）。
//
// 注意：本文件与 SelectStringCore.cs/CshostTable.cs 同受 LangVersion=5
// 约束（csproj 钉死）；JSON 解析用 System.Text.Json（net10 自带库，
// 从 C# 5 语法调用其非泛型/预声明 out 的 API 面）。
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using System.Text.Json;

namespace CcgCsHost7
{
    /** 7 线对象协议入口（hostfxr load_assembly_and_get_function_pointer
     *  目标：类型 "CcgCsHost7.NativeEntry, ccg-cshost7"，方法名即下列
     *  三件之名，UNMANAGED_CALLERSONLY 哨兵取原生指针）。 */
    public static class NativeEntry
    {
        const uint AbiVersion = 1;
        const long MaxPayload = 8 * 1024 * 1024;

        static readonly string s_psHome =
            Environment.GetEnvironmentVariable("CCG_PSHOME") ?? string.Empty;

        static NativeEntry()
        {
            if (s_psHome.Length == 0) { return; }
            AppDomain.CurrentDomain.AssemblyResolve += delegate(object sender, ResolveEventArgs e)
            {
                string simpleName = new AssemblyName(e.Name).Name;
                string path = Path.Combine(s_psHome, simpleName + ".dll");
                return File.Exists(path)
                    ? AssemblyLoadContext.Default.LoadFromAssemblyPath(path)
                    : null;
            };
        }

        /** 结构体桥载荷（宿主与垫片同构镜像；字段语义见文件头注）。 */
        [StructLayout(LayoutKind.Sequential)]
        internal struct CcgCsCallV1
        {
            public uint Size;
            public uint AbiVersion;
            public IntPtr Cmd;
            public ulong CmdLen;
            public IntPtr OutSlot;
        }

        /** 读结构体并校验 ABI（size/版本双判据）；slotAddr 回带出参槽
         *  的地址（垫片经它写结果指针）。 */
        static bool TryReadCall(
            IntPtr args,
            int argsSize,
            out CcgCsCallV1 call,
            out IntPtr slotAddr)
        {
            call = new CcgCsCallV1();
            slotAddr = IntPtr.Zero;
            int selfSize = Marshal.SizeOf(typeof(CcgCsCallV1));
            if (args == IntPtr.Zero || argsSize < selfSize)
            {
                return false;
            }
            object boxed = Marshal.PtrToStructure(args, typeof(CcgCsCallV1));
            if (boxed == null)
            {
                return false;
            }
            call = (CcgCsCallV1)boxed;
            if (call.Size != (uint)selfSize
                || call.AbiVersion != AbiVersion)
            {
                return false;
            }
            slotAddr = (IntPtr)((long)args
                + Marshal.OffsetOf(typeof(CcgCsCallV1), "OutSlot").ToInt64());
            return true;
        }

        /** 读载荷指针为字符串；越界/空指针时 err 携带诊断。 */
        static string ReadPayload(
            ref CcgCsCallV1 call, out string err)
        {
            err = null;
            if (call.Cmd == IntPtr.Zero)
            {
                err = "payload pointer is null";
                return null;
            }
            long len = (long)call.CmdLen;
            if (len < 0 || len > MaxPayload)
            {
                err = "payload length out of range: " + len;
                return null;
            }
            byte[] buf = new byte[len];
            Marshal.Copy(call.Cmd, buf, 0, (int)len);
            return Encoding.UTF8.GetString(buf);
        }

        /** 出参槽写 UTF-8 NUL 串（AllocCoTaskMem 分配，宿主
         *  CoTaskMemFree 释放）；槽未提供（零值）则跳过。 */
        static void WriteOut(IntPtr slotAddr, string text)
        {
            if (slotAddr == IntPtr.Zero) { return; }
            byte[] bytes = Encoding.UTF8.GetBytes(
                text == null ? string.Empty : text);
            IntPtr mem = Marshal.AllocCoTaskMem(bytes.Length + 1);
            Marshal.Copy(bytes, 0, mem, bytes.Length);
            Marshal.WriteByte(mem, bytes.Length, 0);
            Marshal.WriteIntPtr(slotAddr, mem);
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int ObjCreate(IntPtr args, int argsSize)
        {
            CcgCsCallV1 call;
            IntPtr slotAddr;
            if (!TryReadCall(args, argsSize, out call, out slotAddr))
            {
                return CcgCsHost.ObjectHost.ErrAbi;
            }
            try
            {
                string err;
                string payload = ReadPayload(ref call, out err);
                if (payload == null)
                {
                    WriteOut(slotAddr, err);
                    return CcgCsHost.ObjectHost.ErrPayload;
                }
                string className;
                JsonDocument doc;
                try
                {
                    doc = JsonDocument.Parse(payload);
                }
                catch (Exception)
                {
                    WriteOut(slotAddr, "payload parse failed");
                    return CcgCsHost.ObjectHost.ErrPayload;
                }
                using (doc)
                {
                    JsonElement cls;
                    className = doc.RootElement.TryGetProperty(
                                     "class", out cls)
                                 && cls.ValueKind == JsonValueKind.String
                             ? cls.GetString()
                             : null;
                }
                if (className == "SelectStringCore")
                {
                    int token = CcgCsHost.ObjectHost.Register(
                        CcgCsHost.SelectStringCore.Create());
                    if (token > 0)
                    {
                        Dictionary<string, int> handle =
                            new Dictionary<string, int>();
                        handle["handle"] = token;
                        WriteOut(slotAddr, JsonSerializer.Serialize(
                            handle, typeof(Dictionary<string, int>),
                            (JsonSerializerOptions)null));
                    }
                    else
                    {
                        WriteOut(slotAddr, "handle table full");
                    }
                    return token;
                }
                if (className == "MsbuildEval")
                {
                    WriteOut(slotAddr,
                        "MsbuildEval requires the 51 line (net48); "
                        + "this CoreCLR line cannot host it");
                    return CcgCsHost.ObjectHost.ErrNotOnLine;
                }
                WriteOut(slotAddr, "unknown class: "
                    + (className == null ? "<missing>" : className));
                return CcgCsHost.ObjectHost.ErrUnknownClass;
            }
            catch (Exception)
            {
                return CcgCsHost.ObjectHost.ErrChannel;
            }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int ObjCall(IntPtr args, int argsSize)
        {
            CcgCsCallV1 call;
            IntPtr slotAddr;
            if (!TryReadCall(args, argsSize, out call, out slotAddr))
            {
                return CcgCsHost.ObjectHost.ErrAbi;
            }
            try
            {
                string err;
                string payload = ReadPayload(ref call, out err);
                if (payload == null)
                {
                    WriteOut(slotAddr, err);
                    return CcgCsHost.ObjectHost.ErrPayload;
                }
                long handle = -1;
                string method = null;
                Dictionary<string, string> args2 =
                    new Dictionary<string, string>();
                JsonDocument doc;
                try
                {
                    doc = JsonDocument.Parse(payload);
                }
                catch (Exception)
                {
                    WriteOut(slotAddr, "payload parse failed");
                    return CcgCsHost.ObjectHost.ErrPayload;
                }
                using (doc)
                {
                    JsonElement root = doc.RootElement;
                    JsonElement h;
                    if (root.TryGetProperty("handle", out h)
                        && h.ValueKind == JsonValueKind.Number)
                    {
                        long parsed;
                        if (h.TryGetInt64(out parsed))
                        {
                            handle = parsed;
                        }
                    }
                    JsonElement m;
                    if (root.TryGetProperty("method", out m)
                        && m.ValueKind == JsonValueKind.String)
                    {
                        method = m.GetString();
                    }
                    JsonElement a;
                    if (root.TryGetProperty("args", out a)
                        && a.ValueKind == JsonValueKind.Object)
                    {
                        foreach (JsonProperty kv in a.EnumerateObject())
                        {
                            if (kv.Value.ValueKind == JsonValueKind.String)
                            {
                                args2[kv.Name] = kv.Value.GetString();
                            }
                        }
                    }
                }
                if (handle < int.MinValue || handle > int.MaxValue)
                {
                    WriteOut(slotAddr, "handle out of range: " + handle);
                    return CcgCsHost.ObjectHost.ErrBadHandle;
                }
                string result;
                int rc = CcgCsHost.ObjectHost.Call(
                    (int)handle, method, args2, out result);
                WriteOut(slotAddr, result);
                return rc;
            }
            catch (Exception)
            {
                return CcgCsHost.ObjectHost.ErrChannel;
            }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int ObjDestroy(IntPtr args, int argsSize)
        {
            CcgCsCallV1 call;
            IntPtr slotAddr;
            if (!TryReadCall(args, argsSize, out call, out slotAddr))
            {
                return CcgCsHost.ObjectHost.ErrAbi;
            }
            try
            {
                string err;
                string payload = ReadPayload(ref call, out err);
                if (payload == null)
                {
                    return CcgCsHost.ObjectHost.ErrPayload;
                }
                int token;
                if (!int.TryParse(payload, out token))
                {
                    return CcgCsHost.ObjectHost.ErrBadHandle;
                }
                string detail;
                return CcgCsHost.ObjectHost.Destroy(token, out detail);
            }
            catch (Exception)
            {
                return CcgCsHost.ObjectHost.ErrChannel;
            }
        }
    }
}

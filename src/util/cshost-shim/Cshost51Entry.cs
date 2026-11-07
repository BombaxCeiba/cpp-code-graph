// Cshost51Entry.cs — 51 线入口：宿主经 ICLRRuntimeHost::ExecuteInDefaultAppDomain
// 调用，该方法只接受 "public static int Method(string)" 签名、单字符串
// 参数、整数返回，因此对象协议三件（ObjCreate/ObjCall/ObjDestroy）的
// 载荷经命名共享内存传递。
//
// 导出面（宿主侧装载：程序集 ccg-cshost51.dll、类型 "CcgCsHost.Entry"、
// 方法名即下列三件之名；句柄 token 的正数即 ObjCreate 的 int 返回，
// 负数为 ObjectHost 的专用错误码）：
//   ObjCreate(mapName)  mapName = 共享内存映射名；命令区写构造 JSON
//                       （{"class":"SelectStringCore"} 或
//                       {"class":"MsbuildEval","assembly_dir":"..."}），
//                       静态工厂造对象 → 句柄表登记 → 返回 token；
//                       结果区同写 {"handle":N}（与 int 返回双保险）。
//   ObjCall(mapName)    命令区写 {"handle":N,"method":"Extract",
//                       "args":{"script":"..."}}（方法名与命名实参键见
//                       各类方法的头注）→ 反射分派 → 方法返回串（JSON）
//                       写结果区；失败时结果区是诊断文本（只取证不解析）。
//   ObjDestroy(handle)  参数 = token 十进制串（非映射名）；拆除三件套。
//                       仅由宿主侧引用计数归零路径调用；无通道可回传
//                       诊断，宿主按错误码处理。
//
// 共享内存布局（与宿主侧逐常量一致；结果区上限按句柄类分级——分级
// 查表靠视图自身尺寸：视图 ≥ 大档全量即按大档解释，宿主按目标句柄类
// 建对应档视图，垫片从容量自证档位，坏句柄/失效句柄路径不产生歧义）：
//   [0..4)   int32 cmdLen     命令文本 UTF-16 字符数（宿主写入）
//   [4..8)   int32 resultLen  结果 UTF-8 字节数（本入口写入）
//   [8..RES_OFF)              命令文本区
//   [RES_OFF...)              结果区
//   小档（SelectStringCore 句柄与 ObjCreate）：CMD_CAP 4096 wchar、
//     结果区 64KB——与历史单函数协议的常量逐值相同，行为零变化。
//   大档（MsbuildEval 句柄）：CMD_CAP 64K wchar（Begin 请求载荷是
//     全工程清单，8KB 装不下 59 工程量级）、结果区 4MB（单工程事件
//     最坏数百 TU × 37 字段）。
//
// 语言版本钉 C# 5（.NET Framework 自带 csc 编译）；JSON 解析用
// System.Web.Extensions 的 JavaScriptSerializer（自带库纪律）。
using System;
using System.Collections.Generic;
using System.IO.MemoryMappedFiles;
using System.Text;
using System.Web.Script.Serialization;

namespace CcgCsHost
{
    /** 51 线对象协议入口（ExecuteInDefaultAppDomain 目标类型）。 */
    public class Entry
    {
        // —— 小档布局常量（与宿主侧逐常量一致）。 ——
        const int CMD_CAP = 4096;                    // wchar 计
        const int CMD_OFF = 8;
        const long RES_OFF_S = CMD_OFF + 2 * CMD_CAP;
        const long VIEW_S = RES_OFF_S + 65536;

        // —— 大档布局常量（MsbuildEval 句柄）。 ——
        const int CMD_CAP_M = 65536;                 // wchar 计
        const long RES_OFF_M = CMD_OFF + 2 * CMD_CAP_M;
        const long VIEW_M = RES_OFF_M + 4 * 1024 * 1024;

        /** 视图容量自证档位：≥ 大档全量即大档，否则小档。 */
        static long ResOffOf(long capacity)
        {
            return capacity >= VIEW_M ? RES_OFF_M : RES_OFF_S;
        }

        static int CmdCapOf(long capacity)
        {
            return capacity >= VIEW_M ? CMD_CAP_M : CMD_CAP;
        }

        /** 读命令区：返回命令文本；载荷非法（长度越界/超出视图）时
         *  返回 null 且 err 携带诊断。 */
        static string ReadCommand(
            MemoryMappedViewAccessor acc, out string err)
        {
            err = null;
            int cmdLen = acc.ReadInt32(0);
            int cap = CmdCapOf(acc.Capacity);
            if (cmdLen < 0 || cmdLen > cap
                || CMD_OFF + 2 * (long)cmdLen > acc.Capacity)
            {
                err = "cmdLen " + cmdLen + " out of range for view "
                      + acc.Capacity;
                return null;
            }
            char[] buf = new char[cmdLen];
            acc.ReadArray<char>(CMD_OFF, buf, 0, cmdLen);
            return new string(buf);
        }

        /** 写结果区（档位由视图容量自证）：成功返回 0；结果装不下
         *  返回 ObjectHost.ErrOverflow（头部不动，宿主读到 resultLen
         *  保持 0——视图页本就零初始化）。 */
        static int WriteResult(
            MemoryMappedViewAccessor acc, string text)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(
                text == null ? string.Empty : text);
            long resOff = ResOffOf(acc.Capacity);
            if (resOff + bytes.Length > acc.Capacity)
            {
                return ObjectHost.ErrOverflow;
            }
            acc.Write(4, bytes.Length);
            acc.WriteArray<byte>(resOff, bytes, 0, bytes.Length);
            return 0;
        }

        static Dictionary<string, object> ParseObject(
            JavaScriptSerializer json, string payload, out string err)
        {
            err = null;
            try
            {
                Dictionary<string, object> parsed =
                    json.Deserialize<Dictionary<string, object>>(payload);
                if (parsed == null)
                {
                    err = "payload is not a JSON object";
                }
                return parsed;
            }
            catch (Exception ex)
            {
                err = "payload parse failed: " + ex.Message;
                return null;
            }
        }

        // ------------------------------------------------------------------
        // 对象协议三件。
        // ------------------------------------------------------------------

        /** 构造入口：构造 JSON 分派静态工厂 → 句柄表登记 → 正数返回
         *  即 token；负数为专用错误码（类名未知/构造异常/表满）。 */
        public static int ObjCreate(string mapName)
        {
            try
            {
                JavaScriptSerializer json = new JavaScriptSerializer();
                using (MemoryMappedFile mmf =
                    MemoryMappedFile.OpenExisting(mapName))
                using (MemoryMappedViewAccessor acc =
                    mmf.CreateViewAccessor(
                        0, 0, MemoryMappedFileAccess.ReadWrite))
                {
                    string err;
                    string payload = ReadCommand(acc, out err);
                    if (payload == null)
                    {
                        WriteResult(acc, err);
                        return ObjectHost.ErrPayload;
                    }
                    Dictionary<string, object> req =
                        ParseObject(json, payload, out err);
                    if (req == null)
                    {
                        WriteResult(acc, err);
                        return ObjectHost.ErrPayload;
                    }
                    object cls;
                    req.TryGetValue("class", out cls);
                    string className = cls as string;
                    ICshostObject obj;
                    if (className == "SelectStringCore")
                    {
                        obj = SelectStringCore.Create();
                    }
                    else if (className == "MsbuildEval")
                    {
                        object dir;
                        req.TryGetValue("assembly_dir", out dir);
                        try
                        {
                            obj = MsbuildEval.Create(dir as string);
                        }
                        catch (Exception ex)
                        {
                            WriteResult(
                                acc, "MsbuildEval construct failed: "
                                     + ex.Message);
                            return ObjectHost.ErrConstruct;
                        }
                    }
                    else
                    {
                        WriteResult(acc, "unknown class: "
                            + (className == null ? "<missing>" : className));
                        return ObjectHost.ErrUnknownClass;
                    }
                    int token = ObjectHost.Register(obj);
                    if (token <= 0)
                    {
                        WriteResult(acc, "handle table full");
                        return token;
                    }
                    Dictionary<string, object> handle =
                        new Dictionary<string, object>();
                    handle["handle"] = token;
                    // 同 ObjCall：写入失败转发专用码而非吞掉（小档
                    // 64KB 对 {"handle":N} 绰绰有余，此处实际不触发，
                    // 形态对齐以防御结果区扩容或载荷加字段）。
                    int w = WriteResult(acc, json.Serialize(handle));
                    return w != 0 ? w : token;
                }
            }
            catch (Exception)
            {
                return ObjectHost.ErrChannel;
            }
        }

        /** 调用入口：{"handle":N,"method":...,"args":{...}} 反射分派；
         *  成功 0 且结果区为方法返回 JSON；失败负错误码且结果区为
         *  诊断文本（只取证不解析）。 */
        public static int ObjCall(string mapName)
        {
            try
            {
                JavaScriptSerializer json = new JavaScriptSerializer();
                using (MemoryMappedFile mmf =
                    MemoryMappedFile.OpenExisting(mapName))
                using (MemoryMappedViewAccessor acc =
                    mmf.CreateViewAccessor(
                        0, 0, MemoryMappedFileAccess.ReadWrite))
                {
                    string err;
                    string payload = ReadCommand(acc, out err);
                    if (payload == null)
                    {
                        WriteResult(acc, err);
                        return ObjectHost.ErrPayload;
                    }
                    Dictionary<string, object> req =
                        ParseObject(json, payload, out err);
                    if (req == null)
                    {
                        WriteResult(acc, err);
                        return ObjectHost.ErrPayload;
                    }
                    object h;
                    req.TryGetValue("handle", out h);
                    long handle;
                    try
                    {
                        handle = h == null ? -1 : Convert.ToInt64(h);
                    }
                    catch (Exception)
                    {
                        WriteResult(
                            acc, "handle value not a number: "
                                 + (h == null ? "<missing>" : h.ToString()));
                        return ObjectHost.ErrBadHandle;
                    }
                    if (handle < int.MinValue || handle > int.MaxValue)
                    {
                        WriteResult(acc, "handle out of range: "
                                         + handle);
                        return ObjectHost.ErrBadHandle;
                    }
                    object m;
                    req.TryGetValue("method", out m);
                    string method = m as string;
                    Dictionary<string, string> args =
                        new Dictionary<string, string>();
                    object a;
                    req.TryGetValue("args", out a);
                    Dictionary<string, object> argObj =
                        a as Dictionary<string, object>;
                    if (argObj != null)
                    {
                        foreach (KeyValuePair<string, object> kv in argObj)
                        {
                            if (kv.Value is string)
                            {
                                args[kv.Key] = (string)kv.Value;
                            }
                        }
                    }
                    string result;
                    int rc = ObjectHost.Call(
                        (int)handle, method, args, out result);
                    // 结果区写不进（单工程事件超 4MB 大视图）时必须把
                    // WriteResult 的溢出专用码转发给宿主：丢弃它会让
                    // 宿主拿到 rc=0 加空结果区，把溢出误读成坏事件行，
                    // 通道回专用码的承诺就断了线。
                    int w = WriteResult(acc, result);
                    return (rc == 0 && w != 0) ? w : rc;
                }
            }
            catch (Exception)
            {
                return ObjectHost.ErrChannel;
            }
        }

        /** 拆除入口：参数是句柄 token 十进制串；执行拆除三件套
         *  （条目移除 → OnFinalRelease → GCHandle.Free）。 */
        public static int ObjDestroy(string handle)
        {
            int token;
            if (handle == null
                || !int.TryParse(handle, out token))
            {
                return ObjectHost.ErrBadHandle;
            }
            string detail;
            return ObjectHost.Destroy(token, out detail);
        }
    }
}

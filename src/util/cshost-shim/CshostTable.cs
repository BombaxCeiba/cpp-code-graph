// CshostTable.cs — 垫片对象宿主的公共底座（51 与 7 两个入口程序集
// 都编译本文件；对象协议的垫片侧核心）。
//
// 职责三件：
//  1. 句柄表：代次 token + ConcurrentDictionary，登记静态工厂造出的
//     对象（GCHandle 持活防回收——垫片侧不给原生任何对象地址，
//     Normal 句柄即可满足"活到拆除"的语义，Pinned 反而把大对象钉死
//     在堆上碎片化）；
//  2. 拆除三件套（引用计数归零路径，仅由宿主侧 Release 归零触发的
//     ObjDestroy 调用）：条目移除（token 即刻失效）→ OnFinalRelease
//     钩子 → GCHandle.Free；
//  3. 按方法名反射分派实例方法（协议方法 = 公有实例方法、全 string
//     形参、string 返回值；OnFinalRelease 是拆除专用钩子，不在分派面）。
//
// 生命周期纪律（双侧联控，计数在 C++ 宿主侧、拆除动作在垫片侧）：
//  - token 折入代次号（低 10 位槽号 + 高位代次），槽每次被登记都推进
//    代次——条目移除或槽复用后旧 token 永久失效，ObjCall 命中即回
//    stale-handle 专用错误码，杜绝 UAF 式复用；
//  - 线程纪律：ConcurrentDictionary + Interlocked，两线宿主侧都可能
//    多线程触桥（hook 进程的 Select-String 面与 msbuild 进程的求值面），
//    并发安全垫片侧自带，不外包给调用方。
//
// 协议载荷的 JSON 解析不在本文件：51 线用 System.Web.Extensions 的
// JavaScriptSerializer、7 线用 System.Text.Json，由各自入口完成
//（序列化用平台自带库，严禁手写）。
//
// 语言版本钉 C# 5：51 入口由 .NET Framework 自带 csc 编译，7 入口
// csproj 钉 LangVersion=5；禁字符串插值、?.、表达式体、out 变量声明
// 等一切新语法。
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;

namespace CcgCsHost
{
    /** 垫片对象协议面：句柄表内对象必须实现终态释放钩子（两类对象
     *  都实现，空体即满足接口纪律）。 */
    public interface ICshostObject
    {
        /**
         * 引用计数归零拆除路径的终态钩子（GCHandle.Free 之前调用）：
         * 对象在此释放跨调用持有的重资源（如 ProjectCollection）；
         * 无状态实现空体即可。
         */
        void OnFinalRelease();
    }

    /** 句柄表与反射分派（两线入口共用；线程安全）。 */
    internal static class ObjectHost
    {
        // ---- 协议错误码（负数专用；0 成功、正数为句柄 token）。 ----
        /** 共享内存/结构体通道异常。 */
        public const int ErrChannel = -1;
        /** 命令载荷非法（坏 JSON/形参缺失/长度越界）。 */
        public const int ErrPayload = -2;
        /** ObjCreate 类名未知。 */
        public const int ErrUnknownClass = -3;
        /** 静态工厂构造异常（异常文本随通道回传）。 */
        public const int ErrConstruct = -4;
        /** 句柄表满。 */
        public const int ErrTableFull = -5;
        /** 结构体桥 ABI 不匹配（尺寸/版本判据）。 */
        public const int ErrAbi = -6;
        /** 句柄值形态非法（非数字/越界）。 */
        public const int ErrBadHandle = -10;
        /** 句柄已失效（拆除后或从未存在的 token）。 */
        public const int ErrStaleHandle = -11;
        /** 方法名未知或签名不符协议。 */
        public const int ErrUnknownMethod = -12;
        /** 实例方法执行异常（诊断文本随通道回传，只取证不解析）。 */
        public const int ErrMethodFailure = -13;
        /** 结果超出该句柄级的通道视图容量（溢出兜底通道另行立项）。 */
        public const int ErrOverflow = -14;
        /** 类不被本线承载（7 线请求 MsbuildEval：net472 程序集装不进
         *  CoreCLR，求值面钉 51 线）。 */
        public const int ErrNotOnLine = -20;

        // ---- 句柄 token 编码：低 IndexBits 位槽号，其余位代次。 ----
        const int IndexBits = 10;
        const int Capacity = 1 << IndexBits;
        const int IndexMask = Capacity - 1;

        sealed class Slot
        {
            public int Generation;
        }

        sealed class TableEntry
        {
            public GCHandle Gc;
            public ICshostObject Obj;
        }

        static readonly ConcurrentDictionary<int, TableEntry> s_table =
            new ConcurrentDictionary<int, TableEntry>();
        static readonly Slot[] s_slots = NewSlots();
        static int s_cursor;

        static Slot[] NewSlots()
        {
            Slot[] slots = new Slot[Capacity];
            for (int i = 0; i < slots.Length; i++)
            {
                slots[i] = new Slot();
            }
            return slots;
        }

        /**
         * 对象登记入表：槽代次折进 token 高位（本槽每次被登记都推进
         * 代次，旧 token 因此永久失效）。成功返回正数 token；表满返回
         * ErrTableFull。
         */
        public static int Register(ICshostObject obj)
        {
            GCHandle gc = GCHandle.Alloc(obj, GCHandleType.Normal);
            for (int probe = 0; probe < Capacity; probe++)
            {
                int slot = Interlocked.Increment(ref s_cursor) & IndexMask;
                int generation =
                    Interlocked.Increment(ref s_slots[slot].Generation);
                int token = (generation << IndexBits) | slot;
                if (token <= 0)
                {
                    continue; // 代次回绕出非正 token：换槽重试
                }
                TableEntry e = new TableEntry();
                e.Gc = gc;
                e.Obj = obj;
                if (s_table.TryAdd(token, e))
                {
                    return token;
                }
                // 与仍存活的同号 token 撞车（代次回绕到同一值）：换槽。
            }
            gc.Free();
            return ErrTableFull;
        }

        /**
         * 拆除三件套：条目移除（token 即刻失效，后续登记推进槽代次）
         * → OnFinalRelease 钩子（异常不阻断拆除，只随诊断回传）→
         * GCHandle.Free。返回 0；坏句柄/失效句柄返回专用错误码。
         */
        public static int Destroy(int token, out string detail)
        {
            detail = null;
            if (token <= 0)
            {
                detail = "bad handle " + token;
                return ErrBadHandle;
            }
            TableEntry e;
            if (!s_table.TryRemove(token, out e))
            {
                detail = "stale handle " + token;
                return ErrStaleHandle;
            }
            try
            {
                e.Obj.OnFinalRelease();
            }
            catch (Exception ex)
            {
                detail = "OnFinalRelease threw: " + ex.Message;
            }
            e.Gc.Free();
            return 0;
        }

        /**
         * 按句柄与方法名反射分派实例方法。成功时 result 为方法返回串
         * （按协议约定是 JSON 载荷）；失败时 result 为诊断文本（宿主侧
         * 只取证不解析），返回值携带专用错误码。
         * @param args 命名实参表（键 = 形参名，值 = 字符串）。
         */
        public static int Call(
            int token,
            string method,
            Dictionary<string, string> args,
            out string result)
        {
            result = null;
            if (token <= 0)
            {
                result = "bad handle " + token;
                return ErrBadHandle;
            }
            TableEntry e;
            if (!s_table.TryGetValue(token, out e))
            {
                result = "stale handle " + token;
                return ErrStaleHandle;
            }
            if (method == null || method.Length == 0)
            {
                result = "missing method name";
                return ErrUnknownMethod;
            }
            if (method == "OnFinalRelease")
            {
                // 拆除专用钩子不在分派面：只能经 ObjDestroy 的引用计数
                // 归零路径触达。
                result = "method not dispatchable: " + method;
                return ErrUnknownMethod;
            }
            MethodInfo found = FindProtocolMethod(e.Obj.GetType(), method);
            if (found == null)
            {
                result = "unknown method " + method + " on "
                         + e.Obj.GetType().Name;
                return ErrUnknownMethod;
            }
            ParameterInfo[] pars = found.GetParameters();
            object[] argv = new object[pars.Length];
            for (int i = 0; i < pars.Length; i++)
            {
                string value;
                if (args == null
                    || !args.TryGetValue(pars[i].Name, out value))
                {
                    result = "missing argument '" + pars[i].Name
                             + "' for method " + method;
                    return ErrPayload;
                }
                argv[i] = value;
            }
            try
            {
                result = (string)found.Invoke(e.Obj, argv);
                return 0;
            }
            catch (TargetInvocationException tie)
            {
                Exception inner =
                    tie.InnerException != null ? tie.InnerException : tie;
                result = "method " + method + " threw: " + inner.Message;
                return ErrMethodFailure;
            }
            catch (Exception ex)
            {
                result = "method " + method + " dispatch failed: "
                         + ex.Message;
                return ErrMethodFailure;
            }
        }

        /** 协议方法筛选：公有实例、本类声明、全 string 形参、string
         *  返回（OnFinalRelease 已被调用方排除；接口/object 继承面经
         *  DeclaredOnly 排除）。 */
        static MethodInfo FindProtocolMethod(Type type, string method)
        {
            MethodInfo[] methods = type.GetMethods(
                BindingFlags.Public | BindingFlags.Instance
                | BindingFlags.DeclaredOnly);
            foreach (MethodInfo m in methods)
            {
                if (m.Name != method || m.ReturnType != typeof(string))
                {
                    continue;
                }
                ParameterInfo[] pars = m.GetParameters();
                bool allString = true;
                foreach (ParameterInfo p in pars)
                {
                    if (p.ParameterType != typeof(string))
                    {
                        allString = false;
                        break;
                    }
                }
                if (allString)
                {
                    return m;
                }
            }
            return null;
        }
    }
}

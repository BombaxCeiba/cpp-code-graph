// SelectStringCore.cs — PowerShell 引擎内结构化查找请求提取核心（对象
// 宿主路线的 shim 共享体，51 与 7 两个入口程序集都编译本文件）。
//
// 对象化形态：规则层（引擎 AST 之上的宁漏勿猜判定）与用例矩阵
// （tests/shell-matrix.inc）逐字不动，仅由原先的 internal static class
// Core 改为公有实例类 + 静态工厂 Create，经句柄表以 ObjCall 分派
// Extract 实例方法；无状态对象，OnFinalRelease 空体。
//
// 语言版本钉 C# 5：51 入口由 .NET Framework 自带的 csc 编译，禁用
// 字符串插值、?.、表达式体等一切新语法；7 入口的 csproj 同样把
// LangVersion 钉在 5，保证两份程序集从同一份源码出发零分叉。
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Management.Automation.Language;
using System.Text;

namespace CcgCsHost
{
    /** Select-String 查找意图提取对象（无状态；规则层与用例矩阵逐字
     *  对齐：动态成分/通配路径/多 pattern/空 pattern/控制字符/超长/
     *  别名遮蔽/未知参数/反选语义全部放弃并携带矩阵口径的原因串）。 */
    public class SelectStringCore : ICshostObject
    {
        // Select-String 参数表：5.1(26100) 与 7(7.6.9) 实测交集
        //（Get-Command 双版本核对；7 独有 raw/noemphasis/culture 不收，
        //  5.1 上遇到即 unknown-param 放弃，恰是宁漏勿猜方向）。
        static readonly HashSet<string> Params = new HashSet<string>
        {
            "pattern", "literalpath", "simplematch", "casesensitive",
            "notmatch", "quiet", "list", "include", "exclude",
            "encoding", "allmatches", "path"
        };
        // 带值参数（其余为开关：开关不吞后面的值——实测踩坑点）。
        static readonly HashSet<string> ValueParams = new HashSet<string>
        {
            "pattern", "path", "literalpath", "include", "exclude",
            "encoding"
        };

        // 认识的命令名（小写）：cmdlet 全名加官方别名。
        const string CmdSelectString = "select-string";
        const string CmdSls = "sls";

        // 数组多值的内部分隔符（单字符串不可能含控制字符 \u0001，
        // 静态值遇到即判动态，见 StaticValue）。
        const char MultiSep = '\u0001';

        /** 静态工厂：句柄协议 ObjCreate 的构造分派目标（无参构造）。 */
        public static SelectStringCore Create()
        {
            return new SelectStringCore();
        }

        /** 无状态对象的终态钩子：接口纪律空体实现。 */
        public void OnFinalRelease()
        {
        }

        string ResolveParam(string raw)
        {
            string n = raw.ToLowerInvariant();
            if (Params.Contains(n)) { return n; }
            string hit = null;
            foreach (string p in Params)
            {
                if (p.StartsWith(n, StringComparison.Ordinal))
                {
                    if (hit != null) { return null; }  // 前缀歧义
                    hit = p;
                }
            }
            return hit;
        }

        static bool IsCommand(Ast a)
        {
            return a is CommandAst;
        }

        static void AppendJsonString(StringBuilder sb, string s)
        {
            sb.Append('"');
            if (s != null)
            {
                foreach (char ch in s)
                {
                    if (ch == '"' || ch == '\\') { sb.Append('\\'); sb.Append(ch); }
                    else if (ch < ' ') { sb.Append(' '); }
                    else { sb.Append(ch); }
                }
            }
            sb.Append('"');
        }

        // 元素的静态串值：null 表示动态（运行期才可知）。数组元素全
        // 静态时以 MultiSep 连接成多值。
        static string StaticValue(CommandElementAst e)
        {
            StringConstantExpressionAst s = e as StringConstantExpressionAst;
            if (s != null)
            {
                if (s.Value.IndexOf(MultiSep) >= 0) { return null; }
                return s.Value;
            }
            ArrayLiteralAst arr = e as ArrayLiteralAst;
            if (arr != null)
            {
                StringBuilder sb = new StringBuilder();
                foreach (ExpressionAst item in arr.Elements)
                {
                    StringConstantExpressionAst lit = item as StringConstantExpressionAst;
                    if (lit == null || lit.Value.IndexOf(MultiSep) >= 0)
                    {
                        return null;
                    }
                    if (sb.Length > 0) { sb.Append(MultiSep); }
                    sb.Append(lit.Value);
                }
                return sb.ToString();
            }
            return null;
        }

        // 冒号粘值（-Pattern:foo）只可能是裸字或带引号的字面串：按
        // 原文剥一层引号。含 $ 的形态一律按动态放弃。
        static string AttachedStaticValue(string raw)
        {
            if (raw.Length >= 2 && raw[0] == '\'' && raw[raw.Length - 1] == '\'')
            {
                return raw.Substring(1, raw.Length - 2).Replace("''", "'");
            }
            if (raw.Length >= 2 && raw[0] == '"' && raw[raw.Length - 1] == '"')
            {
                if (raw.IndexOf('$') >= 0) { return null; }
                return raw.Substring(1, raw.Length - 2);
            }
            if (raw.IndexOf('$') >= 0) { return null; }
            return raw;
        }

        // 单值展开：多值串按 MultiSep 拆分、普通值按逗号拆分
        //（PowerShell 的 a,b 本就是数组语义）。
        static string[] SplitValues(string value)
        {
            if (value.IndexOf(MultiSep) >= 0)
            {
                return value.Split(MultiSep);
            }
            return value.Split(',');
        }

        /// <summary>
        /// 单条 Select-String 命令的规则层与序列化。
        /// 成功时返回请求体 JSON 且 skip 为 null；命中硬线时返回
        /// null 且 skip 携带矩阵口径的原因串。
        /// </summary>
        string ParseOne(CommandAst cmd, out string skip)
        {
            skip = null;
            List<KeyValuePair<string, string>> pairs =
                new List<KeyValuePair<string, string>>();
            // pending：已见的带值参数名，等待值；null 表示无。
            string pending = null;

            ReadOnlyCollection<CommandElementAst> elements = cmd.CommandElements;
            for (int i = 1; i < elements.Count; i++)
            {
                CommandElementAst e = elements[i];
                CommandParameterAst par = e as CommandParameterAst;
                if (par != null)
                {
                    if (pending != null)
                    {
                        pairs.Add(new KeyValuePair<string, string>(pending, null));
                        pending = null;
                    }
                    string resolved = ResolveParam(par.ParameterName);
                    if (resolved == null)
                    {
                        skip = "unknown-param";
                        return null;
                    }
                    // 冒号粘值形态：值直接附着在参数原文里。
                    string text = par.Extent.Text;
                    int colon = text.IndexOf(':');
                    if (colon > 0 && colon + 1 < text.Length)
                    {
                        if (ValueParams.Contains(resolved))
                        {
                            string v = AttachedStaticValue(text.Substring(colon + 1));
                            if (v == null)
                            {
                                skip = "dynamic-argument";
                                return null;
                            }
                            pairs.Add(new KeyValuePair<string, string>(resolved, v));
                        }
                        else
                        {
                            // 开关参数的冒号显式布尔：$true 置位、
                            // $false 不置位，其余表达式静态不可知即放弃。
                            string flag = text.Substring(colon + 1);
                            if (flag != "$true" && flag != "$false")
                            {
                                skip = "dynamic-argument";
                                return null;
                            }
                            pairs.Add(new KeyValuePair<string, string>(resolved, flag));
                        }
                        continue;
                    }
                    pending = resolved;
                    continue;
                }
                // 非参数元素：位置上的值（可能为动态 null）。
                if (pending != null)
                {
                    if (ValueParams.Contains(pending))
                    {
                        string value = StaticValue(e);
                        if (value == null)
                        {
                            skip = "dynamic-argument";
                            return null;
                        }
                        pairs.Add(new KeyValuePair<string, string>(pending, value));
                        pending = null;
                    }
                    else
                    {
                        // 前一个是开关：不吞值，本元素落位置参数。
                        pairs.Add(new KeyValuePair<string, string>(pending, "$true"));
                        pending = null;
                        pairs.Add(new KeyValuePair<string, string>(null, StaticValue(e)));
                    }
                }
                else
                {
                    pairs.Add(new KeyValuePair<string, string>(null, StaticValue(e)));
                }
            }
            if (pending != null)
            {
                pairs.Add(new KeyValuePair<string, string>(pending, "$true"));
            }

            // 位置绑定：无命名 Pattern 时第一个位置值绑定 Pattern、
            // 其余是 Path；命名 Pattern（含冒号粘值）已出现时位置值
            // 全部落 Path（Select-String 真实参数绑定语义）。
            bool patternNamed = false;
            foreach (KeyValuePair<string, string> kv in pairs)
            {
                if (kv.Key == "pattern")
                {
                    patternNamed = true;
                    break;
                }
            }
            bool firstPosition = true;
            List<KeyValuePair<string, string>> bound =
                new List<KeyValuePair<string, string>>();
            foreach (KeyValuePair<string, string> kv in pairs)
            {
                if (kv.Key == null)
                {
                    bool asPattern = !patternNamed && firstPosition;
                    bound.Add(new KeyValuePair<string, string>(
                        asPattern ? "pattern" : "path", kv.Value));
                    firstPosition = false;
                }
                else
                {
                    bound.Add(kv);
                }
            }

            List<string> patterns = new List<string>();
            List<string> paths = new List<string>();
            bool fixedLit = false;
            foreach (KeyValuePair<string, string> kv in bound)
            {
                if (kv.Key == "notmatch")
                {
                    skip = "invert";
                    return null;
                }
                if (kv.Key == "pattern")
                {
                    if (kv.Value == null)
                    {
                        skip = "dynamic-argument";
                        return null;
                    }
                    foreach (string p in SplitValues(kv.Value))
                    {
                        if (p.Length == 0)
                        {
                            skip = "no-pattern";
                            return null;
                        }
                        patterns.Add(p);
                    }
                }
                else if (kv.Key == "path")
                {
                    if (kv.Value == null)
                    {
                        skip = "dynamic-argument";
                        return null;
                    }
                    foreach (string p in SplitValues(kv.Value))
                    {
                        if (p.IndexOfAny(new[] { '*', '?', '[' }) >= 0)
                        {
                            skip = "wildcard-path";
                            return null;
                        }
                        paths.Add(p);
                    }
                }
                else if (kv.Key == "literalpath")
                {
                    if (kv.Value == null)
                    {
                        skip = "dynamic-argument";
                        return null;
                    }
                    foreach (string p in SplitValues(kv.Value))
                    {
                        paths.Add(p);
                    }
                }
                else if (kv.Key == "simplematch")
                {
                    // 裸开关或 $true 置位；$false 不置位。
                    if (kv.Value == null || kv.Value == "$true") { fixedLit = true; }
                }
                // include/exclude/encoding/quiet/list/casesensitive/
                // allmatches：忽略（与矩阵口径一致）。
            }

            if (patterns.Count == 0)
            {
                skip = "no-pattern";
                return null;
            }
            foreach (string p in patterns)
            {
                foreach (char ch in p)
                {
                    if (ch < ' ' || ch == '\x007F')
                    {
                        skip = "control-char";
                        return null;
                    }
                }
                if (p.Length > 256)
                {
                    skip = "too-long";
                    return null;
                }
            }

            // 管道位置：非首段读上游管道，路径语义化为 stdin。
            bool stdinPipe = false;
            PipelineAst pipe = cmd.Parent as PipelineAst;
            if (pipe != null && pipe.PipelineElements.Count > 0 &&
                !ReferenceEquals(pipe.PipelineElements[0], cmd))
            {
                stdinPipe = true;
            }

            StringBuilder sb = new StringBuilder(128);
            sb.Append("{\"engine\":\"select-string\",\"patterns\":[");
            for (int i = 0; i < patterns.Count; i++)
            {
                if (i > 0) { sb.Append(','); }
                AppendJsonString(sb, patterns[i]);
            }
            sb.Append("],\"single\":");
            sb.Append(patterns.Count == 1 ? "true" : "false");
            sb.Append(",\"fixed\":");
            sb.Append(fixedLit ? "true" : "false");
            sb.Append(",\"paths\":[");
            for (int i = 0; i < paths.Count; i++)
            {
                if (i > 0) { sb.Append(','); }
                AppendJsonString(sb, paths[i]);
            }
            sb.Append("],\"path_kind\":\"");
            sb.Append(stdinPipe ? "stdin-pipe" : "none");
            sb.Append("\"}");
            return sb.ToString();
        }

        // 同串里重定义 Select-String/sls 别名：此后含义不可知，整串放弃。
        static bool AliasShadowed(ScriptBlockAst ast)
        {
            foreach (Ast a in ast.FindAll(IsCommand, true))
            {
                CommandAst cmd = (CommandAst)a;
                string name = cmd.GetCommandName();
                if (name == null) { continue; }
                string lower = name.ToLowerInvariant();
                if (lower != "set-alias" && lower != "new-alias") { continue; }
                foreach (CommandElementAst e in cmd.CommandElements)
                {
                    if (ReferenceEquals(e, cmd.CommandElements[0])) { continue; }
                    StringConstantExpressionAst s = e as StringConstantExpressionAst;
                    if (s == null) { continue; }
                    string v = s.Value.ToLowerInvariant();
                    if (v == CmdSls || v == CmdSelectString) { return true; }
                }
            }
            return false;
        }

        /**
         * 主入口：解析一段 PowerShell 命令文本，产出结构化查找请求
         * JSON（宿主侧只消费该 JSON 文本；ObjCall 分派名 "Extract"、
         * 命名实参键 "script"）。多命令时按序收集全部请求；首个命中
         * 硬线的命令携带 skip，已产出者保留。
         */
        public string Extract(string script)
        {
            Token[] tokens;
            ParseError[] errors;
            ScriptBlockAst ast = Parser.ParseInput(script, out tokens, out errors);
            StringBuilder sb = new StringBuilder(256);
            sb.Append("{\"requests\":[");
            if (errors != null && errors.Length > 0)
            {
                // 语法错误整串放弃（矩阵口径沿用"残缺引号"命名）。
                sb.Append("],\"skip\":\"unterminated-quote\"}");
                return sb.ToString();
            }
            if (AliasShadowed(ast))
            {
                sb.Append("],\"skip_all\":\"alias-shadow\"}");
                return sb.ToString();
            }
            bool anyRequest = false;
            string firstSkip = null;
            foreach (Ast a in ast.FindAll(IsCommand, true))
            {
                CommandAst cmd = (CommandAst)a;
                string name = cmd.GetCommandName();
                if (name == null) { continue; }
                string lower = name.ToLowerInvariant();
                if (lower != CmdSelectString && lower != CmdSls) { continue; }
                string skip;
                string body = ParseOne(cmd, out skip);
                if (body == null)
                {
                    if (!anyRequest && firstSkip == null) { firstSkip = skip; }
                    continue;
                }
                if (anyRequest) { sb.Append(','); }
                anyRequest = true;
                sb.Append(body);
            }
            sb.Append(']');
            if (!anyRequest && firstSkip != null)
            {
                sb.Append(",\"skip\":");
                AppendJsonString(sb, firstSkip);
            }
            sb.Append('}');
            return sb.ToString();
        }
    }
}

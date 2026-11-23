

/**
 * @module cg.clang.compile_db.expand
 *
 * @brief 命令行展开域：include/module 旗标推送、模块实参解析、rsp
 * 展开、模板模式与宏值归一、PCH 消费者判定。
 */
export module cg.clang.compile_db.expand;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.driver;
import cg.clang.compile_db.model;
import cg.clang.compile_db.tokens;

export namespace cg::clang
{

    namespace detail
    {

        /** @brief JSON 对象成员若为字符串则读出，否则无值。 */
        [[nodiscard]]
        inline auto json_string_field(
            const cg::util::json::ValueView& entry,
            std::string_view                 key) -> std::optional<std::string>
        {
            if (const auto v = entry.get(key); v && v->is_string())
            {
                return std::string{*v->as_string()};
            }
            return std::nullopt;
        }

        /** @brief token 是否为分写值的 GNU include 族旗标。 */
        [[nodiscard]]
        constexpr auto is_gnu_include_flag(std::string_view t) -> bool
        {
            return t == "-I" || t == "-isystem" || t == "-iquote"
                   || t == "-idirafter" || t == "-include";
        }

        /** @brief token 是否为连写值的 GNU include 旗标（"-Idir" 等）。 */
        [[nodiscard]]
        constexpr auto is_joined_include_flag(std::string_view t) -> bool
        {
            return (t.starts_with("-I") && t.size() > 2)
                   || (t.starts_with("-iquote") && t.size() > 7);
        }

        /** @brief 路径是否为 .pch/.gch 后缀。 */
        [[nodiscard]]
        constexpr auto is_pch_suffix(std::string_view p) -> bool
        {
            return p.ends_with(".pch") || p.ends_with(".gch");
        }

        /** @brief "@响应文件" token 记为条目的模块映射（首个 @ 生效）。 */
        inline void note_rsp_module_map(
            CompileCommand&    cc,
            const std::string& dir,
            std::string_view   t)
        {
            namespace fs = std::filesystem;
            if (cc.module_map.empty() && t.size() >= 2)
            {
                std::string rsp{t.substr(1)};
                if (fs::path{rsp}.is_relative())
                {
                    rsp = (fs::path{dir} / rsp)
                              .lexically_normal()
                              .generic_string();
                }
                cc.module_map = cg::util::norm_path(rsp);
            }
        }

        /** @brief "-fmodule-output=" 的值段解析为绝对路径后记入条目。 */
        inline void note_module_output(
            CompileCommand&    cc,
            const ArgResolver& resolve_arg,
            std::string_view   t)
        {
            cc.module_output =
                resolve_arg(std::string{t.substr(t.find('=') + 1)});
        }

        /**
         * @brief "-fmodule-file=NAME=path" 进结构化投喂并重拼进 argv。
         *
         * 名与路径进投喂表（重放去重），argv 重写解析后的路径段；名或
         * 路径为空时返回 false（token 不消费，落穿后续分支）。
         */
        [[nodiscard]]
        inline auto try_module_file_flag(
            CompileCommand&    cc,
            const ArgResolver& resolve_arg,
            const std::string& t) -> bool
        {
            if (!t.starts_with("-fmodule-file="))
            {
                return false;
            }
            const std::size_t eq1 = t.find('=');          // flag 之后
            const std::size_t eq2 = t.find('=', eq1 + 1); // 名|路径
            if (eq2 == std::string::npos || eq2 <= eq1 + 1
                || eq2 + 1 >= t.size())
            {
                return false;
            }
            const std::string name = t.substr(eq1 + 1, eq2 - eq1 - 1);
            const std::string pcm = resolve_arg(t.substr(eq2 + 1));
            const bool        seen = std::ranges::any_of(
                cc.module_feeds,
                [&](const ModuleFeed& feed)
                { return feed.name == name && feed.pcm == pcm; });
            if (!seen)
            {
                cc.module_feeds.push_back(ModuleFeed{.name = name, .pcm = pcm});
            }
            cc.argv.push_back(t.substr(0, eq2 + 1) + pcm);
            return true;
        }

        /** @brief 分写 include 旗标连值 token 一并消费（值按条目目录解析）。 */
        inline void push_include_token(
            CompileCommand&                 cc,
            const std::vector<std::string>& tokens,
            const ArgResolver&              resolve_arg,
            const std::string&              t,
            std::size_t&                    j)
        {
            cc.argv.push_back(t);
            if (j + 1 < tokens.size())
            {
                cc.argv.push_back(resolve_arg(tokens[++j]));
            }
        }

        /** @brief 连写 include 旗标（"-Idir"）解析值段后重拼进 argv。 */
        inline void push_joined_include(
            CompileCommand&    cc,
            const ArgResolver& resolve_arg,
            const std::string& t)
        {
            cc.argv.push_back(t.substr(0, 2) + resolve_arg(t.substr(2)));
        }

        /** @brief 消费并返回 k 位置的下一个 token（无后续 token 时不推进）。 */
        [[nodiscard]]
        inline auto next_token(
            const std::vector<std::string>& tokens,
            std::size_t&                    k) -> std::optional<std::string>
        {
            if (k + 1 >= tokens.size())
            {
                return std::nullopt;
            }
            ++k;
            return tokens[k];
        }

        /** @brief cl 旗标是否为分写值形态（值经守卫单点跟随）。 */
        [[nodiscard]]
        constexpr auto is_cl_separate_value_flag(std::string_view t) -> bool
        {
            return t == "/Fp" || t == "/Fo" || t == "/Fd" || t == "/I"
                   || t == "/external:I" || t == "/FI" || t == "/D" || t == "/U"
                   || t == "/FU" || t == "/AI" || t == "/arch"
                   || t == "/source-charset" || t == "/execution-charset"
                   || t == "/validate-charset";
        }

        /** @brief token 以 cl 旗标前缀连写时剥出非空值段；不匹配时无值。 */
        [[nodiscard]]
        inline auto cl_joined_value(std::string_view t, std::string_view prefix)
            -> std::optional<std::string>
        {
            if (!t.starts_with(prefix) || t.size() <= prefix.size())
            {
                return std::nullopt;
            }
            return std::string{t.substr(prefix.size())};
        }

        /**
         * @brief 响应文件 token 的模块旗标与路径解析。
         *
         * "-fmodule-file=" 前缀的 token：NAME=path 形态先登记投喂、解析
         * 路径段后重拼 token（结构化投喂的 pcm 同步为解析后拼写）；无名
         * 段的裸路径形态解析整段。其余非旗标 token 按路径解析，旗标
         * token 原样返回。
         */
        [[nodiscard]]
        auto resolve_module_arg(
            std::string         arg,
            CompileCommand&     cc,
            const ArgResolver&  resolve_arg,
            const FeedNotifier& note_feed) -> std::string
        {
            constexpr std::string_view module_file_prefix = "-fmodule-file=";
            if (arg.starts_with(module_file_prefix))
            {
                std::string tail = arg.substr(module_file_prefix.size());
                if (const std::size_t eq = tail.find('=');
                    eq != std::string::npos)
                {
                    note_feed(tail);
                    tail = tail.substr(0, eq + 1)
                           + resolve_arg(tail.substr(eq + 1));
                    cc.module_feeds.back().pcm = tail.substr(eq + 1);
                }
                else
                {
                    tail = resolve_arg(tail);
                }
                return std::string{module_file_prefix} + tail;
            }
            if (!arg.empty() && arg[0] != '-')
            {
                return resolve_arg(arg);
            }
            return arg;
        }

        /**
         * @brief 展开 argv 中的 "@响应文件" token（一层）。
         *
         * 非 @ token 原样透传；@ token 的相对路径按条目目录解析为绝对、
         * 首个 @ 记为 module_map，文件读取成功则按同一 tokenizer 切分逐
         * token 消化，读取失败则丢弃该 token。
         *
         * @internal
         * PCH 条目（pch_hint）的响应文件 token 逐字透传，由 PCH 感知解
         * 析器按 directory 绝对化路径值——此处绝不把 cl 的 `/D` 值误当路
         * 径，模块产物旗标（-fmodule-output=）仍按结构化字段吞掉。"-x"
         * 后恰好一个 token 是语言名而非路径，原样放行。展开只做一层：
         * 记录中的 @ 文件不再嵌套 @。
         */
        [[nodiscard]]
        auto expand_response_files(
            std::vector<std::string> in,
            const std::string&       dir,
            bool                     pch_hint,
            CompileCommand&          cc,
            const ArgResolver&       resolve_arg,
            const FeedNotifier&      note_feed) -> std::vector<std::string>
        {
            namespace fs = std::filesystem;
            std::vector<std::string> out;
            for (std::string& t : in)
            {
                if (!t.starts_with('@') || t.size() < 2)
                {
                    out.push_back(std::move(t));
                    continue;
                }
                std::string rsp = t.substr(1);
                if (fs::path{rsp}.is_relative())
                {
                    rsp = (fs::path{dir} / rsp)
                              .lexically_normal()
                              .generic_string();
                }
                if (cc.module_map.empty())
                {
                    cc.module_map = cg::util::norm_path(rsp);
                }
                const auto rsp_text = cg::util::read_text_file(rsp);
                if (!rsp_text.has_value())
                {
                    continue;
                }
                bool rsp_after_x = false;
                for (std::string& a : cg::util::tokenize_command(*rsp_text))
                {
                    // 产物旗标两个分支同判（构建产物，非解析输入）。
                    if (a.starts_with("-fmodule-output="))
                    {
                        cc.module_output =
                            resolve_arg(a.substr(a.find('=') + 1));
                        continue;
                    }
                    if (pch_hint)
                    {
                        out.push_back(std::move(a));
                        continue;
                    }
                    if (a == "-x")
                    {
                        rsp_after_x = true;
                        out.push_back(std::move(a));
                        continue;
                    }
                    if (rsp_after_x)
                    {
                        rsp_after_x = false;
                        out.push_back(std::move(a)); // -x 的语言值
                        continue;
                    }
                    out.push_back(resolve_module_arg(
                        std::move(a),
                        cc,
                        resolve_arg,
                        note_feed));
                }
            }
            return out;
        }

        /**
         * @brief 模板模式旗标是否已在条目 argv 里显式声明。
         *
         * @internal 与 cg.msbuild.translate 的同名判定同一规则（cl 前缀
         * '/' 与 '-' 皆认、该族选项大小写不敏感，按小写全量拼写比对）：
         * /permissive- 或 /Zc:twoPhase（严格）与 /Zc:twoPhase-（延迟）任一
         * 在场即已声明。
         */
        [[nodiscard]]
        inline auto template_mode_declared(
            const std::vector<std::string>& tokens) -> bool
        {
            for (const std::string& t : tokens)
            {
                const std::string low = cg::util::ascii_lower(t);
                if (low == "/permissive-" || low == "-permissive-"
                    || low == "/zc:twophase" || low == "-zc:twophase"
                    || low == "/zc:twophase-" || low == "-zc:twophase-")
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief 原地修剪宏定义值两端的空白。
         *
         * 工程文件里手滑的 "NAME= VALUE"（MSBuild 原样透传、MSVC 与
         * clang 的预处理器都宽容值的前导空白）在此归一为 "NAME=VALUE"。
         * 两段式（/D 与值分立）与连写（/DNAME=..）两种形态都处理，'/'
         * 与 '-' 前缀等价、大小写不敏感；只修剪值两端的空白，字符串字
         * 面量内部的空白（"a b"）原样保留。
         *
         * @internal cdb 载入与 pch sidecar 载入必须共用本函数：影子
         * PCH 烙进的宏值与消费者命令行（cdb 已归一化）须按字节一致，
         * clang 装载 PCH 按字节比对宏值，任一侧带原始空白即拒载、整组
         * TU 降级文本重放。
         */
        auto normalize_macro_values(std::vector<std::string>& tokens) -> void
        {
            for (std::size_t di = 0; di < tokens.size(); ++di)
            {
                std::string& t = tokens[di];
                const bool   two_token = t.size() == 2
                                         && (t[0] == '/' || t[0] == '-')
                                         && (t[1] == 'D' || t[1] == 'd');
                const std::string_view flag =
                    two_token ? std::string_view{} : std::string_view{t};
                if (!two_token
                    && !(
                        flag.size() > 2 && (flag[0] == '/' || flag[0] == '-')
                        && (flag[1] == 'D' || flag[1] == 'd')
                        && flag.contains('=')))
                {
                    continue;
                }
                std::string& def =
                    two_token && di + 1 < tokens.size() ? tokens[di + 1] : t;
                const std::size_t eq = def.find('=');
                if (eq == std::string::npos || eq + 1 >= def.size())
                {
                    continue;
                }
                std::string_view value{
                    def.c_str() + eq + 1,
                    def.size() - eq - 1};
                const std::size_t front = value.find_first_not_of(" \t");
                const std::size_t back = value.find_last_not_of(" \t");
                if (front == std::string_view::npos
                    || (front == 0 && back + 1 == value.size()))
                {
                    continue;
                }
                def = def.substr(0, eq + 1)
                      + std::string{value.substr(front, back - front + 1)};
            }
        }

        /**
         * @brief 旁车消费者记录的识别面。
         *
         * @internal pch.json consumers 一条记录中参与消费者身份判定的
         * 两个字段：与 producer 的关联键（绝对 /Fp）和 /Yu 伞头拼写。
         * file 是数据源组织信息（文件归属），不进判定。sidecar 的解析
         * 结构留在 cg.clang.pch，识别面以本结构过界，两模块不互相依赖。
         */
        struct SidecarPchFacts
        {
            std::string pch_key;    // 绝对 /Fp（与 producer 的关联键）
            std::string pch_header; // /Yu 的头拼写（原样）
        };

        /**
         * @brief PCH 消费者身份的最终判定。
         *
         * @internal sidecar_sourced 是来源注记：判定取自旁车记录为
         * true（调用方据此补 pch_involved），取自 cdb 令牌或判非消费
         * 者为 false。
         */
        struct PchConsumerVerdict
        {
            bool        use = false;
            bool        sidecar_sourced = false;
            std::string path;
            std::string header;
        };

        /**
         * @brief PCH 消费者身份的唯一判定点（cdb 令牌识别 vs pch.json
         * 旁车记录）。
         *
         * 输入两路各自的原始识别：cdb 侧是 loader 产出的结构化字段
         * （pch_use/pch_path/pch_header——装载层只做令牌提取，不判定）；
         * 旁车侧是调用方按文件归属查得的记录（无记录传 nullptr）。
         * 输出最终判定，规划器的两处消费（producer 引用收集、消费者
         * 改写）都必须经本函数，不再各自拼合并。
         *
         * 优先级（显式写死）：旁车为准，cdb 令牌补充——cdb 未判成消
         * 费者而旁车有该文件记录时以旁车为准；cdb 令牌已判成消费者的
         * 条目保持 cdb 结果，旁车不改写。工程管线里两源互斥：MSBuild
         * 适配器写的 cdb 剥离 /Yc /Yu /Fp（旁车是该场景唯一来源），
         * GNU 风格 cdb 没有旁车；两源同时命中同一 TU 只见于手工拼造
         * 的输入，此时保持 cdb 侧判定与历史合并行为逐条一致。
         */
        [[nodiscard]]
        auto resolve_pch_consumer(
            const CompileCommand&  cmd,
            const SidecarPchFacts* sidecar) -> PchConsumerVerdict
        {
            if (cmd.pch_use)
            {
                return PchConsumerVerdict{
                    .use = true,
                    .sidecar_sourced = false,
                    .path = cmd.pch_path,
                    .header = cmd.pch_header};
            }
            if (sidecar != nullptr)
            {
                return PchConsumerVerdict{
                    .use = true,
                    .sidecar_sourced = true,
                    .path = sidecar->pch_key,
                    .header = sidecar->pch_header};
            }
            return PchConsumerVerdict{};
        }

    } // namespace detail
} // namespace cg::clang

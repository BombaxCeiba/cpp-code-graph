/**
 * @module cg.cli.hook
 *
 * @brief `ccg hook` 家族：Claude Code 集成管理加上随附蹦床脚本调用的
 *        三个钩子事件入口。
 *
 * 三张面孔：
 * - `ccg hook install|uninstall|status --project|--user` 编辑
 *   settings.json 的 hooks 节。编辑器是保区间的 JSONC 重写器：被增删
 *   条目之外的每个字节都保持用户的原样（注释、排版、邻居条目），
 *   自有条目按针识别，uninstall 默认干跑并修剪清空的容器，无法解析
 *   的设置文件被拒绝、绝不覆写。
 * - `ccg hook posttooluse` / `ccg hook sessionstart` /
 *   `ccg hook userpromptsubmit` 从 stdin 读一份
 *   Claude Code 钩子事件 JSON，以文档化的
 *   {hookSpecificOutput:{hookEventName, additionalContext}} 形状应答于
 *   stdout——其余一切静默，退出恒 0（钩子绝不可使它观察的工具调用
 *   失败）。重活经派生的 ccg 子进程运行（update + changes / stats），
 *   使机器 JSON 从管道到达而不与本进程自身的 stdout 契约混淆
 *   （std::print 绕过任何 cout rdbuf 交换）。
 *
 * @internal db.tables.h 置于全局模块片段：本单元查询 cg.db 的影响面
 * 与统计，MSVC 要求 sqlpp23 表模型定义在此可见，以合并模块记录的
 * 模板特化（完整机理见 db.tables.h）。
 */
module;
#include "../db/db.tables.h"

export module cg.cli.hook;

import std;
import cg.util;
import cg.util.cstd;
import cg.util.shellscan;
import cg.db;
import cg.graph;
import cg.cli.parse;
import cg.cli.render;
import cg.cli.layout;

export namespace cg::cli
{
    namespace detail
    {
        namespace fs = std::filesystem;
        namespace json = cg::util::json;
        namespace shellscan = cg::util::shellscan;

        /**
         * @brief 一个已解析的 JSON(C) 值：记录其源区间；容器另记每个子项的
         *        区间。
         *
         * @internal
         * 编辑器对这些区间做外科手术式文本编辑；没有东西被重新序列化，故
         * 被编辑条目之外的注释、键序与排版逐字节存活。
         */
        struct SettingsNode
        {
            enum class Type : std::uint8_t
            {
                Object,
                Array,
                String,
                Literal
            };
            Type        type = Type::Literal;
            std::size_t begin = 0; // 值首字符的偏移
            std::size_t end = 0;   // 值末字符之后一格
            std::size_t after_open =
                0; // 容器：内部首位置，越过前导空白/注释（空时的安全插入点）
            std::string               text;   // 字符串：反转义后的内容
            std::vector<std::string>  keys;   // 对象
            std::vector<SettingsNode> values; // 对象
            std::vector<std::size_t>
                key_begins; // 对象：各键开引号的偏移（成员清除区间为
                            // 键始..值终）
            std::vector<SettingsNode> elements; // 数组

            [[nodiscard]]
            auto member(std::string_view key) const -> const SettingsNode*
            {
                for (std::size_t i = 0; i < keys.size(); ++i)
                {
                    if (keys[i] == key)
                    {
                        return &values[i];
                    }
                }
                return nullptr;
            }
        };

        /**
         * @brief 递归下降 JSONC 解析器。
         *
         * 支持 JSON 加行注释与块注释（斜杠斜杠、斜杠星形式）与尾逗号——用户
         * 维护的设置文件漂移成的方言。畸形输入时抛出点名偏移的
         * std::runtime_error；安装器对这种文件拒绝触碰而非写入。
         *
         * @internal
         * 字符串中的 \uXXXX 转义原样保留：此处比较的键与针都是 ASCII，解码形
         * 态永远无关紧要。
         */
        class SettingsParser
        {
        public:
            explicit SettingsParser(std::string_view text) : text_{text} {}

            /** @brief 解析整份文档；空/纯注释文件返回空。 */
            [[nodiscard]]
            auto parse_document() -> std::optional<SettingsNode>
            {
                skip_ws();
                if (pos_ >= text_.size())
                {
                    return std::nullopt;
                }
                SettingsNode root = parse_value();
                skip_ws();
                if (pos_ < text_.size())
                {
                    throw std::runtime_error{std::format(
                        "settings: trailing content at offset {}",
                        pos_)};
                }
                return root;
            }

        private:
            [[nodiscard]]
            auto error(std::string_view what) const -> std::runtime_error
            {
                return std::runtime_error{
                    std::format("settings: {} at offset {}", what, pos_)};
            }

            void skip_ws()
            {
                for (;;)
                {
                    while (pos_ < text_.size()
                           && (text_[pos_] == ' ' || text_[pos_] == '\t'
                               || text_[pos_] == '\r' || text_[pos_] == '\n'))
                    {
                        ++pos_;
                    }
                    if (pos_ + 1 < text_.size() && text_[pos_] == '/'
                        && text_[pos_ + 1] == '/')
                    {
                        pos_ += 2;
                        while (pos_ < text_.size() && text_[pos_] != '\n')
                        {
                            ++pos_;
                        }
                        continue;
                    }
                    if (pos_ + 1 < text_.size() && text_[pos_] == '/'
                        && text_[pos_ + 1] == '*')
                    {
                        pos_ += 2;
                        while (
                            pos_ + 1 < text_.size()
                            && !(text_[pos_] == '*' && text_[pos_ + 1] == '/'))
                        {
                            ++pos_;
                        }
                        pos_ = std::min(pos_ + 2, text_.size());
                        continue;
                    }
                    return;
                }
            }

            [[nodiscard]]
            auto parse_value() -> SettingsNode
            {
                if (pos_ >= text_.size())
                {
                    throw error("unexpected end of input");
                }
                switch (text_[pos_])
                {
                case '{':
                    return parse_object();
                case '[':
                    return parse_array();
                case '"':
                    return parse_string();
                default:
                    return parse_literal();
                }
            }

            [[nodiscard]]
            auto parse_string() -> SettingsNode
            {
                SettingsNode node;
                node.type = SettingsNode::Type::String;
                node.begin = pos_;
                ++pos_; // 越过起始引号（非内容字符）
                while (pos_ < text_.size())
                {
                    const char c = text_[pos_];
                    if (c == '"')
                    {
                        ++pos_;
                        node.end = pos_;
                        return node;
                    }
                    if (c == '\\' && pos_ + 1 < text_.size())
                    {
                        const char esc = text_[pos_ + 1];
                        switch (esc)
                        {
                        case 'n':
                            node.text += '\n';
                            break;
                        case 't':
                            node.text += '\t';
                            break;
                        case 'r':
                            node.text += '\r';
                            break;
                        case 'b':
                            node.text += '\b';
                            break;
                        case 'f':
                            node.text += '\f';
                            break;
                        default:
                            node.text += esc;
                            break;
                        }
                        if (esc == 'u' && pos_ + 5 < text_.size())
                        {
                            node.text.append(text_.substr(pos_ + 2, 4));
                            pos_ += 6;
                        }
                        else
                        {
                            pos_ += 2;
                        }
                        continue;
                    }
                    node.text += c;
                    ++pos_;
                }
                throw error("unterminated string");
            }

            [[nodiscard]]
            auto parse_object() -> SettingsNode
            {
                SettingsNode node;
                node.type = SettingsNode::Type::Object;
                node.begin = pos_;
                ++pos_;
                skip_ws();
                node.after_open = pos_;
                if (pos_ < text_.size() && text_[pos_] == '}')
                {
                    node.end = pos_ + 1;
                    ++pos_;
                    return node;
                }
                for (;;)
                {
                    skip_ws();
                    if (pos_ >= text_.size() || text_[pos_] != '"')
                    {
                        throw error("expected an object key");
                    }
                    const std::size_t key_begin = pos_;
                    SettingsNode      key = parse_string();
                    skip_ws();
                    if (pos_ >= text_.size() || text_[pos_] != ':')
                    {
                        throw error("expected ':' after an object key");
                    }
                    ++pos_;
                    skip_ws();
                    SettingsNode value = parse_value();
                    node.keys.push_back(std::move(key.text));
                    node.key_begins.push_back(key_begin);
                    node.values.push_back(std::move(value));
                    skip_ws();
                    if (pos_ < text_.size() && text_[pos_] == ',')
                    {
                        ++pos_;
                        skip_ws();
                        if (pos_ < text_.size() && text_[pos_] == '}')
                        {
                            break; // 容忍尾随逗号
                        }
                        continue;
                    }
                    break;
                }
                skip_ws();
                if (pos_ >= text_.size() || text_[pos_] != '}')
                {
                    throw error("expected '}' closing an object");
                }
                node.end = pos_ + 1;
                ++pos_;
                return node;
            }

            [[nodiscard]]
            auto parse_array() -> SettingsNode
            {
                SettingsNode node;
                node.type = SettingsNode::Type::Array;
                node.begin = pos_;
                ++pos_;
                skip_ws();
                node.after_open = pos_;
                if (pos_ < text_.size() && text_[pos_] == ']')
                {
                    node.end = pos_ + 1;
                    ++pos_;
                    return node;
                }
                for (;;)
                {
                    skip_ws();
                    node.elements.push_back(parse_value());
                    skip_ws();
                    if (pos_ < text_.size() && text_[pos_] == ',')
                    {
                        ++pos_;
                        skip_ws();
                        if (pos_ < text_.size() && text_[pos_] == ']')
                        {
                            break; // 容忍尾随逗号
                        }
                        continue;
                    }
                    break;
                }
                skip_ws();
                if (pos_ >= text_.size() || text_[pos_] != ']')
                {
                    throw error("expected ']' closing an array");
                }
                node.end = pos_ + 1;
                ++pos_;
                return node;
            }

            [[nodiscard]]
            auto parse_literal() -> SettingsNode
            {
                SettingsNode node;
                node.type = SettingsNode::Type::Literal;
                node.begin = pos_;
                while (pos_ < text_.size())
                {
                    const char c = text_[pos_];
                    if (c == ',' || c == '}' || c == ']' || c == ' '
                        || c == '\t' || c == '\r' || c == '\n' || c == '/')
                    {
                        break;
                    }
                    ++pos_;
                }
                if (pos_ == node.begin)
                {
                    throw error("unexpected character");
                }
                node.end = pos_;
                return node;
            }

            std::string_view text_;
            std::size_t      pos_ = 0;
        };

        /**
         * @brief 一次外科手术式文本替换；与其他已记录编辑作为一批施加，最晚
         *         偏移在前，使更早的偏移无需重解析即保持有效。
         */
        struct SettingsEdit
        {
            std::size_t begin = 0;
            std::size_t end = 0;
            std::string replacement;
        };

        /**
         * @brief settings 文本的外科手术式编辑器：记录清除与插入，一次性渲染。
         */
        class SettingsEditor
        {
        public:
            explicit SettingsEditor(std::string text) : text_{std::move(text)}
            {
            }

            /**
             * @brief 记录一个成员/元素区间的感知逗号清除。
             *
             * @internal
             * 前面的逗号胜出（它属于前一位邻居的尾部）；首子项的清除改取后面
             * 的逗号；完全没有逗号则容器变空、依旧合法。逗号周围的空白并入
             * 清除；注释不并入（用户放在被删条目旁的注释是用户内容，留下）。
             * 区间来自解析树，故待施清除要么嵌套要么不相交：已接受区间内部的
             * 清除被丢弃，包围已接受区间者先吞掉它们——施加的批次必须保持
             * 无重叠，否则后续偏移会作用在错位的文本上。残余的部分重叠意味
             * 共享此逗号的兄弟清除：退回用朴素区间顶住。
             */
            void erase(std::size_t begin, std::size_t end)
            {
                std::size_t b = begin;
                std::size_t e = end;
                std::size_t i = begin;
                while (i > 0
                       && (text_[i - 1] == ' ' || text_[i - 1] == '\t'
                           || text_[i - 1] == '\r' || text_[i - 1] == '\n'))
                {
                    --i;
                }
                if (i > 0 && text_[i - 1] == ',')
                {
                    b = i - 1;
                }
                else
                {
                    std::size_t j = end;
                    for (;;)
                    {
                        while (j < text_.size()
                               && (text_[j] == ' ' || text_[j] == '\t'
                                   || text_[j] == '\r' || text_[j] == '\n'))
                        {
                            ++j;
                        }
                        if (j + 1 < text_.size() && text_[j] == '/'
                            && (text_[j + 1] == '/' || text_[j + 1] == '*'))
                        {
                            if (text_[j + 1] == '/')
                            {
                                while (j < text_.size() && text_[j] != '\n')
                                {
                                    ++j;
                                }
                            }
                            else
                            {
                                while (
                                    j + 1 < text_.size()
                                    && !(
                                        text_[j] == '*' && text_[j + 1] == '/'))
                                {
                                    ++j;
                                }
                                j = std::min(j + 2, text_.size());
                            }
                            continue;
                        }
                        break;
                    }
                    if (j < text_.size() && text_[j] == ',')
                    {
                        e = j + 1;
                    }
                }
                for (const SettingsEdit& accepted : erases_)
                {
                    if (accepted.begin <= begin && end <= accepted.end)
                    {
                        return;
                    }
                }
                std::erase_if(
                    erases_,
                    [b, e](const SettingsEdit& a)
                    { return b <= a.begin && a.end <= e; });
                for (const SettingsEdit& accepted : erases_)
                {
                    if (b < accepted.end && accepted.begin < e)
                    {
                        b = begin;
                        e = end;
                        break;
                    }
                }
                erases_.push_back(SettingsEdit{b, e, ""});
            }

            /**
             * @brief 在 at（调用方从解析导出的逗号安全锚点）记录一次插入。
             *
             * @internal
             * first_in_container 表示锚点位于空容器的开口处；同一锚点的第二次
             * 插入以逗号链接（两个脚手架成员追加进同一个原本为空的对象绝不
             * 可裸拼接）。
             */
            void insert(
                std::size_t        at,
                const std::string& text,
                bool               first_in_container)
            {
                for (SettingsEdit& e : inserts_)
                {
                    if (e.begin == at && e.end == at)
                    {
                        e.replacement =
                            std::format("{},{}", e.replacement, text);
                        return;
                    }
                }
                inserts_.push_back(
                    SettingsEdit{
                        at,
                        at,
                        (first_in_container ? "" : ",") + text});
            }

            /**
             * @brief 施加一切编辑并返回重写后的文档。
             *
             * @internal
             * 与插入同偏移的清除在排序中胜出（更大 end 在前）——恰是"删旧
             * 自有条目、再把新条目放进原位"的情形；防御上绝不拼接过界。
             */
            [[nodiscard]]
            auto render() -> std::string
            {
                std::vector<SettingsEdit> all = erases_;
                all.insert(all.end(), inserts_.begin(), inserts_.end());
                std::ranges::sort(
                    all,
                    [](const SettingsEdit& a, const SettingsEdit& b)
                    {
                        if (a.begin != b.begin)
                        {
                            return a.begin > b.begin; // 最晚偏移在前
                        }
                        return a.end > b.end;
                    });
                std::string out = text_;
                for (const SettingsEdit& e : all)
                {
                    if (e.begin > e.end || e.end > out.size())
                    {
                        continue;
                    }
                    out.replace(e.begin, e.end - e.begin, e.replacement);
                }
                return out;
            }

        private:
            std::string               text_;
            std::vector<SettingsEdit> erases_;
            std::vector<SettingsEdit> inserts_;
        };

        /**
         * @brief 一条受管钩子条目的规格。
         *
         * @internal
         * script 是 .ccg/hooks 内的蹦床文件名（其词干兼作所有权针）；action
         * 是蹦床转发到的 `ccg hook <action>` 处理器（插件渠道以那种方式拼写
         * 命令——两种拼写都认作我们的，故 uninstall 会扫除两个渠道的设置
         * 条目）。
         */
        struct HookSpec
        {
            const char* event;
            const char* script;
            const char* action;
            const char* matcher; // nullptr = 事件无需 matcher
            int         timeout_seconds;
            const char* status_message; // nullptr = 无
        };

        inline constexpr HookSpec HOOK_SPECS[] = {
            // 宿主硬超时 10s：处理器内部最多等 update 9s，到点只 detach、绝不
            // kill——后台 update 继续跑完（租约协议保证 detach 后独立完成）。因此这里
            // 必须略大于 9s：hook 仍要先于宿主返回并注入那行"后台继续"。
            {"PostToolUse",
             "ccg-posttooluse",
             "posttooluse",
             "Edit|Write|MultiEdit|Update",
             10,
             "ccg: 正在刷新代码图..."},
            {"SessionStart",
             "ccg-sessionstart",
             "sessionstart",
             nullptr,
             15,
             "ccg: 正在读取代码数据库状态..."},
            // UserPromptSubmit 不支持 matcher（写了也会被静默忽略）；宿主的
            // 30s 默认上限不采用，处理器内部软截止 6s、仍在跑时毫秒级返回。
            {"UserPromptSubmit",
             "ccg-userpromptsubmit",
             "userpromptsubmit",
             nullptr,
             8,
             "ccg: 正在汇总索引结论..."},
            // Grep 期注入（五行注入口径）：命中才注入，
            // 处理器全程进程内查库（无子进程派发），典型耗时毫秒级。
            // 监听面六工具（Grep 原生 + Bash/PowerShell 命令行里的 rg 与
            // Select-String + Search 载荷直读 + Read/Glob 纯投递载体）：
            // Bash/PowerShell 先过微秒级触发词预筛，非查找命令不付解析
            // 成本；Read/Glob 不做意图提取不做符号查询，进处理器即走
            // digest 快速投递检查（无人值守会话的末批积压借此投递）；
            // matcher 多值写法与上面 PostToolUse 条目同款（宿主按
            // tool_name 正则评估）。
            {"PreToolUse",
             "ccg-pretooluse",
             "pretooluse",
             "Grep|Bash|PowerShell|Search|Read|Glob",
             10,
             "ccg: 正在查代码图..."},
        };

        /** @brief 命令文本是否属于本规格（按两种拼写之一的针匹配）。 */
        [[nodiscard]]
        auto is_our_command(std::string_view command, const HookSpec& spec)
            -> bool
        {
            const std::string direct = std::format("ccg hook {}", spec.action);
            return command.find(spec.script) != std::string_view::npos
                   || command.find(direct) != std::string_view::npos;
        }

        /** @brief 作用域选择所解析到的设置文件。 */
        [[nodiscard]]
        auto settings_path(const Invocation& inv) -> fs::path
        {
            if (inv.hook_user)
            {
                return cg::util::platform::home_directory() / ".claude"
                       / "settings.json";
            }
            return fs::path{".claude"} / "settings.json";
        }

        /**
         * @brief 在运行中的可执行文件旁定位规格的蹦床脚本（发布布局：
         *        `<dir>`/ccg(.exe) + `<dir>`/.ccg/hooks/）。
         *
         * @internal
         * 故障关闭：脚本缺失即中止注册，绝不把死路径写进设置文件。脚本变体
         * 跟随可执行文件自身的后缀：.exe 随附 .cmd 蹦床、普通二进制随附
         * .sh。从文件名而非平台 ifdef 推导，使该规则在任何宿主上可测试。
         */
        [[nodiscard]]
        auto script_path_for(const HookSpec& spec) -> fs::path
        {
            const fs::path exe = cg::util::platform::executable_path();
            if (exe.empty())
            {
                throw std::runtime_error(
                    "cannot determine this executable's location (platform lookup "
                    "failed); hook registration needs the .ccg directory beside "
                    "the ccg binary");
            }
            const std::string suffix =
                exe.extension() == ".exe" ? ".cmd" : ".sh";
            return exe.parent_path() / ".ccg" / "hooks"
                   / (std::string{spec.script} + suffix);
        }

        /**
         * @brief 引号包裹的命令文本。
         *
         * @internal 通用分隔符使条目在 Claude Code 派发钩子命令的每种宿主
         * shell 里都可读且有效。格式化构造一律 std::format，不手写拼接。
         */
        [[nodiscard]]
        auto quote_command(const fs::path& script) -> std::string
        {
            return std::format("\"{}\"", script.generic_string());
        }

        /** @brief 规格的 matcher 条目 JSON 文本。 */
        [[nodiscard]]
        auto entry_text(const HookSpec& spec, const std::string& command)
            -> std::string
        {
            json::Object entry;
            if (spec.matcher != nullptr)
            {
                entry.set("matcher", spec.matcher);
            }
            json::Object hook;
            hook.set("type", "command");
            hook.set("command", command);
            hook.set(
                "timeout",
                static_cast<std::int64_t>(spec.timeout_seconds));
            if (spec.status_message != nullptr)
            {
                hook.set("statusMessage", spec.status_message);
            }
            json::Array hooks;
            hooks.add(std::move(hook));
            entry.set("hooks", std::move(hooks));
            return json::stringify(std::move(entry));
        }

        /**
         * @brief 遍历 hooks.`<event>`，为每条命令属于我们的条目记录清除。
         *
         * 完全没有命令留下的条目（这里的每条命令都被移除）整体清除——无论
         * 它原是我们的还是现在空了；从来没有命令数组的条目是外来的、留下。
         * 逐规格报告事件数组最后幸存条目的区间——新条目追加在其后的逗号
         * 安全锚点——并在 prune_empty 时清除清空的事件数组/清空的 hooks
         * 对象/清空的 "hooks" 成员（uninstall 的零残留 pass；install 保留
         * 它即将复用的脚手架）。
         */
        struct SweepResult
        {
            std::size_t              found_commands = 0; // 自有命令条目总数
            std::vector<std::size_t> removed_per_event;  // 对齐 HOOK_SPECS
            std::vector<std::optional<std::pair<std::size_t, std::size_t>>>
                last_kept; // 对齐 HOOK_SPECS
        };

        auto sweep_own_entries(
            const SettingsNode& root,
            SettingsEditor&     editor,
            bool                prune_empty) -> SweepResult
        {
            SweepResult       result;
            const std::size_t specs = std::size(HOOK_SPECS);
            result.removed_per_event.assign(specs, 0);
            result.last_kept.assign(specs, std::nullopt);
            if (root.type != SettingsNode::Type::Object)
            {
                return result;
            }
            const SettingsNode* hooks = root.member("hooks");
            if (hooks == nullptr || hooks->type != SettingsNode::Type::Object)
            {
                return result;
            }
            std::size_t hooks_members_remaining = hooks->keys.size();
            for (std::size_t s = 0; s < specs; ++s)
            {
                const HookSpec&     spec = HOOK_SPECS[s];
                const SettingsNode* event = hooks->member(spec.event);
                if (event == nullptr
                    || event->type != SettingsNode::Type::Array)
                {
                    continue;
                }
                std::size_t entries_remaining = event->elements.size();
                for (const SettingsNode& entry : event->elements)
                {
                    const SettingsNode* commands =
                        entry.type == SettingsNode::Type::Object
                            ? entry.member("hooks")
                            : nullptr;
                    if (commands == nullptr
                        || commands->type != SettingsNode::Type::Array)
                    {
                        result.last_kept[s] =
                            std::make_pair(entry.begin, entry.end);
                        continue;
                    }
                    std::size_t commands_remaining = commands->elements.size();
                    std::size_t removed_here = 0;
                    for (const SettingsNode& command : commands->elements)
                    {
                        if (command.type != SettingsNode::Type::Object)
                        {
                            continue;
                        }
                        const SettingsNode* command_text =
                            command.member("command");
                        if (command_text == nullptr
                            || command_text->type != SettingsNode::Type::String
                            || !is_our_command(command_text->text, spec))
                        {
                            continue;
                        }
                        editor.erase(command.begin, command.end);
                        ++result.found_commands;
                        ++result.removed_per_event[s];
                        ++removed_here;
                        --commands_remaining;
                    }
                    if (commands_remaining > 0)
                    {
                        result.last_kept[s] =
                            std::make_pair(entry.begin, entry.end);
                        continue;
                    }
                    if (removed_here > 0)
                    {
                        editor.erase(entry.begin, entry.end);
                        --entries_remaining;
                    }
                }
                if (entries_remaining == 0 && prune_empty)
                {
                    const auto it = std::ranges::find(
                        hooks->keys,
                        std::string_view{spec.event});
                    editor.erase(
                        hooks->key_begins[static_cast<std::size_t>(
                            it - hooks->keys.begin())],
                        event->end);
                    --hooks_members_remaining;
                    result.last_kept[s] = std::nullopt; // 成员即将消失
                }
            }
            if (hooks_members_remaining == 0 && prune_empty)
            {
                const auto it =
                    std::ranges::find(root.keys, std::string_view{"hooks"});
                editor.erase(
                    root.key_begins[static_cast<std::size_t>(
                        it - root.keys.begin())],
                    hooks->end);
            }
            return result;
        }

        /**
         * @brief 为规格向 hooks.`<event>` 追加一条新 matcher 条目；hooks 对象
         *        存在而事件成员缺失时搭脚手架。
         *
         * @internal
         * 插入绝不打扰邻居：既有数组在其最后幸存条目之后增长（紧随锚点被
         * 清除的自有条目带走其前导逗号，而渲染 pass 先于本插入施加该清除，
         * 故本插入带来的逗号是唯一的一个）；两个规格向同一个（可能为空的）
         * hooks 对象搭脚手架时经编辑器的同锚合并链接成员。hooks 成员自身
         * 必须已存在——逐规格创建会铸造重复键，故 hook_install 为全部规格
         * 一次性组合它。外来的事件成员原样不动。
         */
        void append_entry(
            const SettingsNode&                                       root,
            SettingsEditor&                                           editor,
            const HookSpec&                                           spec,
            const std::string&                                        command,
            const std::optional<std::pair<std::size_t, std::size_t>>& last_kept)
        {
            if (root.type != SettingsNode::Type::Object)
            {
                return;
            }
            const std::string   entry = entry_text(spec, command);
            const SettingsNode* hooks = root.member("hooks");
            if (hooks == nullptr || hooks->type != SettingsNode::Type::Object)
            {
                return;
            }
            const SettingsNode* event = hooks->member(spec.event);
            if (event == nullptr)
            {
                const std::string member =
                    std::format("\"{}\": [{}]", spec.event, entry);
                editor.insert(
                    hooks->keys.empty() ? hooks->after_open
                                        : hooks->values.back().end,
                    member,
                    hooks->keys.empty());
                return;
            }
            if (event->type != SettingsNode::Type::Array)
            {
                return;
            }
            if (last_kept.has_value())
            {
                editor.insert(
                    last_kept->second,
                    entry,
                    /*first_in_container=*/false);
            }
            else
            {
                editor.insert(
                    event->after_open,
                    entry,
                    /*first_in_container=*/true);
            }
        }

        /**
         * @brief 读取并解析一个设置文件。
         *
         * root 为空 = 文件不存在；解析错误作为可行动的拒绝传播（无法理解时
         * 文件绝不被重写）。
         */
        struct SettingsFile
        {
            std::string                 text;
            std::optional<SettingsNode> root;
        };

        [[nodiscard]]
        auto load_settings(const fs::path& path) -> SettingsFile
        {
            std::error_code ec;
            if (!fs::exists(path, ec) || ec)
            {
                return {};
            }
            std::ifstream in{path, std::ios::binary};
            if (!in)
            {
                throw std::runtime_error(
                    std::format("cannot read '{}'", path.generic_string()));
            }
            SettingsFile file;
            file.text.assign(std::istreambuf_iterator<char>{in}, {});
            SettingsParser parser{file.text};
            file.root = parser.parse_document();
            return file;
        }

        void write_settings(const fs::path& path, const std::string& text)
        {
            std::error_code ec;
            const fs::path  parent = path.parent_path();
            if (!parent.empty())
            {
                fs::create_directories(parent, ec);
            }
            std::ofstream out{path, std::ios::binary | std::ios::trunc};
            if (!out)
            {
                throw std::runtime_error(
                    std::format("cannot write '{}'", path.generic_string()));
            }
            out << text;
            if (!text.empty() && text.back() != '\n')
            {
                out << '\n';
            }
        }

        /** @brief 为尚不存在的文件全新组合的设置文档。 */
        [[nodiscard]]
        auto fresh_document(const std::vector<std::string>& commands)
            -> std::string
        {
            std::string out = "{\n  \"hooks\": {\n";
            for (std::size_t i = 0; i < std::size(HOOK_SPECS); ++i)
            {
                out += std::format(
                    "    \"{}\": [{}]",
                    HOOK_SPECS[i].event,
                    entry_text(HOOK_SPECS[i], commands[i]));
                out += i + 1 < std::size(HOOK_SPECS) ? ",\n" : "\n";
            }
            out += "  }\n}\n";
            return out;
        }

        /**
         * @brief 其编辑值得走符号影响面注入的文件扩展（主文件、头、模块单元）。
         *
         * 项目文件（.sln/.vcxproj/.props/...，cg::util::is_project_file）另走
         * 重采集联动通道，不在此列。其余一切（配置、文档、脚本、锁文件）使钩
         * 子静默。
         *
         * @internal 判定收编至 util 的扩展分类单点（is_hook_corpus_file）：
         * 旧本地表做字节精确比较，大写 .CPP 会被静默漏判（风险不对称——
         * 漏判使钩子对整个文件失明，多收由下游未跟踪判定吸收），收编后按
         * ASCII 折叠后缀判定。
         */
        [[nodiscard]]
        auto is_corpus_file(std::string_view path) -> bool
        {
            return cg::util::is_hook_corpus_file(path);
        }

        /** @brief 注入上下文的字节预算（PostToolUse 与 SessionStart 各一档）。
         */
        inline constexpr std::size_t POST_TOOL_CONTEXT_BUDGET = 1200;
        inline constexpr std::size_t SESSION_START_CONTEXT_BUDGET = 600;

        // --------------------------------------------------------- hook 摘要
        // -- 批次期间积压的编辑结论：快速标脏只写 journal
        // 立即返回；批次收敛后由 UserPromptSubmit（主通道）或下一次
        // PostToolUse（机会式）一次性汇报。
        inline constexpr std::size_t DIGEST_BUDGET = 1200;
        // 与本次编辑的常规影响面行合并进同一个 1200B 信封时，留给积压的上限。
        inline constexpr std::size_t  DIGEST_PTU_BUDGET = 900;
        inline constexpr std::size_t  DIGEST_MAX_FILES = 8;
        inline constexpr std::int64_t FAST_PROBE_BUSY_MS = 150;
        inline constexpr std::int64_t FAST_ENQUEUE_BUSY_MS = 150;
        inline constexpr auto         FAST_PATH_ENQUEUE_DEADLINE =
            std::chrono::milliseconds{700};
        inline constexpr auto DIGEST_UPS_DEADLINE =
            std::chrono::milliseconds{6000};
        inline constexpr auto DIGEST_PTU_DEADLINE =
            std::chrono::milliseconds{2500};
        inline constexpr auto DIGEST_QUERY_TIMEOUT_UPS =
            std::chrono::milliseconds{1500};
        inline constexpr auto DIGEST_IMPACT_TIMEOUT_UPS =
            std::chrono::milliseconds{1200};
        inline constexpr auto DIGEST_QUERY_TIMEOUT_PTU =
            std::chrono::milliseconds{800};
        inline constexpr auto DIGEST_IMPACT_TIMEOUT_PTU =
            std::chrono::milliseconds{700};
        inline constexpr auto DIGEST_ENTRY_MAX_AGE = std::chrono::hours{24};
        inline constexpr auto DIGEST_CLAIMED_MAX_AGE = std::chrono::hours{1};
        inline constexpr auto DIGEST_FLUSH_LOCK_STALE =
            std::chrono::seconds{60};

        /**
         * @brief 把视图裁到 max 字节，裁剪时补省略号。
         *
         * @internal 绝不能按字节硬切：切点落在 UTF-8 多字节序列中间会产生
         * 非法编码，yyjson 写出会整体失败，整条注入退化成空行。切点先回退
         * 到码点边界，再优先在分隔符处下刀。
         */
        [[nodiscard]]
        auto clip(std::string_view s, std::size_t max) -> std::string
        {
            if (s.size() <= max)
            {
                return std::string{s};
            }
            std::size_t cut = max;
            while (cut > 0 && cut < s.size()
                   && cg::util::utf8_is_continuation(s[cut]))
            {
                --cut;
            }
            constexpr std::size_t LOOKBACK = 24;
            const std::size_t     floor = cut > LOOKBACK ? cut - LOOKBACK : 0;
            for (std::size_t i = cut; i > floor; --i)
            {
                const char c = s[i - 1];
                if (c == ';' || c == ',' || c == '\n' || c == ' ')
                {
                    cut = i;
                    break;
                }
                if (i >= 3)
                {
                    const std::string_view tail = s.substr(i - 3, 3);
                    if (tail == "；" || tail == "，" || tail == "、")
                    {
                        cut = i;
                        break;
                    }
                }
            }
            return std::string{s.substr(0, cut)} + "…";
        }

        /** @brief 路径用的尾裁变体：文件名是识别部分，故折叠长前缀而非名字。 */
        [[nodiscard]]
        auto clip_tail(std::string_view s, std::size_t max) -> std::string
        {
            if (s.size() <= max)
            {
                return std::string{s};
            }
            std::size_t start = s.size() - max;
            while (start < s.size() && cg::util::utf8_is_continuation(s[start]))
            {
                ++start;
            }
            return "…" + std::string{s.substr(start)};
        }

        /**
         * @brief 标识符用的中间省略：保留首尾，折叠中段。
         *
         * @internal 单片段的标识符（典型是 gtest 展开出的长夹具名）没有 `::`
         * 可折叠；只留尾部会把套件名整段丢掉，读者再也无法区分同一夹具下的
         * 用例。首尾都保留才能看出“哪个套件、哪个用例”。
         */
        [[nodiscard]]
        auto clip_middle(std::string_view s, std::size_t max) -> std::string
        {
            if (s.size() <= max)
            {
                return std::string{s};
            }
            if (max < 3)
            {
                return clip_tail(s, max);
            }
            const std::size_t budget = max - 1; // 留一个字符给省略号
            std::size_t       head = budget / 2;
            std::size_t       tail = budget - head;
            while (head > 0 && cg::util::utf8_is_continuation(s[head]))
            {
                --head;
            }
            std::size_t start = s.size() - tail;
            while (start < s.size() && cg::util::utf8_is_continuation(s[start]))
            {
                ++start;
            }
            if (start <= head)
            {
                return clip_tail(s, max);
            }
            return std::format("{}…{}", s.substr(0, head), s.substr(start));
        }

        /**
         * @brief 行尾的“等 N 项/个”计数后缀；没有则返回空视图。
         *
         * @internal 截断列表时把计数保留下来，读者仍知道真实规模，而不是只
         * 看到被砍掉的名单。
         */
        [[nodiscard]]
        auto trailing_count_suffix(std::string_view s) -> std::string_view
        {
            if (!s.ends_with("项") && !s.ends_with("个"))
            {
                return {};
            }
            const std::size_t unit_start = s.size() - 3;
            std::size_t       digits_end = unit_start;
            std::size_t       p = digits_end;
            while (p > 0 && std::isdigit(static_cast<unsigned char>(s[p - 1])))
            {
                --p;
            }
            if (p == digits_end || p < 2 || s[p - 1] != ' ')
            {
                return {};
            }
            --p;
            if (p >= 4 && s.substr(p - 3, 3) == "等" && s[p - 4] == ' ')
            {
                return s.substr(p - 4);
            }
            return {};
        }

        /** @brief 注入行裁剪：UTF-8 安全，并保留行尾的“等 N 项/个”计数。 */
        [[nodiscard]]
        auto clip_injection(std::string_view line, std::size_t max)
            -> std::string
        {
            if (line.size() <= max)
            {
                return std::string{line};
            }
            const std::string_view suffix = trailing_count_suffix(line);
            if (!suffix.empty() && suffix.size() + 4 < max)
            {
                const std::size_t head_budget = max - suffix.size() - 3;
                std::string       head = clip(
                    line.substr(0, line.size() - suffix.size()),
                    head_budget);
                head += suffix;
                return head;
            }
            return clip(line, max);
        }

        /**
         * @brief 限定名裁剪：优先在 `::` 边界下刀，保留最具体的尾部。
         *
         * @internal 从标识符中间截断（`ICSharpRuntimeBackend::Invo…`）对读者
         * 毫无用处；符号名的识别信息在尾部，故折叠前缀而不是尾巴。没有 `::`
         * 可折叠的单段长名（gtest 展开名）改用中间省略，避免把套件前缀整段
         * 丢掉。
         */
        [[nodiscard]]
        auto clip_qualified(std::string_view name, std::size_t max)
            -> std::string
        {
            if (name.size() <= max)
            {
                return std::string{name};
            }
            std::size_t start = 0;
            while (name.size() - start > max)
            {
                const std::size_t next = name.find("::", start + 1);
                if (next == std::string_view::npos)
                {
                    break;
                }
                start = next + 2;
            }
            if (start == 0 || name.size() - start > max)
            {
                return clip_middle(name, max);
            }
            return "…::" + std::string{name.substr(start)};
        }

        /**
         * @brief 路径比较键：反斜杠折成正斜杠，ASCII 小写。
         *
         * @internal 已收编至 util
         * 的比较规范化单点（cg::util::path_compare_key，
         * 路径规范化具名变体之一）；本名是钩子的既有调用面，转发保持调用点
         * 不动。
         */
        [[nodiscard]]
        auto path_compare_key(std::string_view path) -> std::string
        {
            return cg::util::path_compare_key(path);
        }

        /** @brief 路径末段（文件名与扩展）。 */
        [[nodiscard]]
        auto basename_of(std::string_view path) -> std::string
        {
            return std::string{cg::util::path_basename(path)};
        }

        /**
         * @brief 注入里展示的路径：优先相对当前工作目录，否则只留文件名。
         *
         * @internal Claude Code 以项目根为工作目录运行钩子；绝对路径的前缀对
         * 读者没有信息量，省略号还会被当成噪音。相对路径或末段文件名的长度
         * 可控，且仍能唯一定位到本次编辑的文件。
         */
        [[nodiscard]]
        auto display_path(std::string_view path) -> std::string
        {
            const auto normalized = [](std::string_view value)
            { return cg::util::to_forward_slashes(value); };
            // 已是相对路径（少见，但库内路径可能如此）时原样展示。
            const bool absolute =
                path.size() >= 2
                && ((std::isalpha(static_cast<unsigned char>(path.front()))
                     && path[1] == ':')
                    || path.front() == '/' || path.front() == '\\');
            if (!absolute)
            {
                return normalized(path);
            }
            std::error_code ec;
            const fs::path  cwd = fs::current_path(ec);
            if (!ec)
            {
                const std::u8string utf8 = cwd.generic_u8string();
                std::string         cwd_key = path_compare_key(
                    std::string_view{
                        reinterpret_cast<const char*>(utf8.data()),
                        utf8.size()});
                while (!cwd_key.empty() && cwd_key.back() == '/')
                {
                    cwd_key.pop_back();
                }
                const std::string path_key = path_compare_key(path);
                if (!cwd_key.empty() && path_key.size() > cwd_key.size() + 1
                    && path_key.compare(0, cwd_key.size(), cwd_key) == 0
                    && path_key[cwd_key.size()] == '/')
                {
                    return cg::util::to_forward_slashes(
                        path.substr(cwd_key.size() + 1));
                }
            }
            return basename_of(path);
        }

        /** @brief 源码扫描器切出的区间种类。 */
        enum class SourceSpan : std::uint8_t
        {
            Code,         ///< 普通代码（注释与字面量之外的部分）
            LineComment,  ///< 行注释（含起始 `//`，不含行尾换行）
            BlockComment, ///< 块注释（含 `/*` 与 `*/`）
            Literal,      ///< 字符串/字符字面量（含两侧引号与转义序列）
            RawLiteral,   ///< 原始字符串（含编码前缀、定界符与全部内容）
        };

        /**
         * @brief 五状态源码扫描器：把文本切成无缝衔接的区间流回调给 sink。
         *
         * @internal mask_literals 与 code_fingerprint 的共享骨架：状态转移与
         * 区间边界只此一份，两个调用方各自只写输出策略。raw_strings 显式保留
         * 两家的口径差异——掩码必须识别原始字符串（含 `u8R"delim(` 前缀），
         * 否则 `R"(` 会被当成普通字符串而把后续代码吞掉；指纹刻意不识别，
         * 原始字符串里的 `//` 顶多让一次纯注释编辑提示失效，误识别反而会吞
         * 掉真代码。sink(kind, begin, end) 收到的区间按升序覆盖全文且互不
         * 重叠；未闭合的注释/字面量照常切出到文本末尾。
         */
        template <typename Sink>
        auto scan_source_spans(
            std::string_view text,
            bool             raw_strings,
            Sink&&           sink) -> void
        {
            enum class State : std::uint8_t
            {
                Code,
                LineComment,
                BlockComment,
                String,
                Char,
            };
            const auto ident_char = [](char c)
            { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
            State       state = State::Code;
            std::size_t span_begin = 0; // 当前未发射区间的起点
            const auto  emit_code_before = [&](std::size_t at)
            {
                if (at > span_begin)
                {
                    sink(SourceSpan::Code, span_begin, at);
                }
            };
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                const char c = text[i];
                const char next = i + 1 < text.size() ? text[i + 1] : '\0';
                switch (state)
                {
                case State::Code:
                    if (c == '/' && next == '/')
                    {
                        emit_code_before(i);
                        span_begin = i;
                        ++i;
                        state = State::LineComment;
                    }
                    else if (c == '/' && next == '*')
                    {
                        emit_code_before(i);
                        span_begin = i;
                        ++i;
                        state = State::BlockComment;
                    }
                    else if (raw_strings && c == 'R' && next == '"')
                    {
                        // 前缀合法性：`R"`、`uR"`、`UR"`、`LR"`、`u8R"`。
                        const char prev = i > 0 ? text[i - 1] : '\0';
                        const bool bare = i == 0 || !ident_char(prev);
                        const bool u8_prefix =
                            i >= 2 && text[i - 1] == '8' && text[i - 2] == 'u'
                            && (i < 3 || !ident_char(text[i - 3]));
                        const bool simple_prefix =
                            i >= 1
                            && (prev == 'u' || prev == 'U' || prev == 'L')
                            && (i < 2 || !ident_char(text[i - 2]));
                        const std::size_t seg = text.find('(', i + 2);
                        bool raw = (bare || u8_prefix || simple_prefix)
                                   && seg != std::string_view::npos
                                   && seg - (i + 2) <= 16;
                        for (std::size_t k = i + 2; raw && k < seg; ++k)
                        {
                            const char d = text[k];
                            if (d == ' ' || d == '\\' || d == ')' || d == '('
                                || d == '\t' || d == '\n' || d == '\r')
                            {
                                raw = false;
                            }
                        }
                        if (raw)
                        {
                            const std::string_view delim =
                                text.substr(i + 2, seg - (i + 2));
                            std::string close{")"};
                            close.append(delim);
                            close.push_back('"');
                            const std::size_t end = text.find(close, seg + 1);
                            const std::size_t stop =
                                end == std::string_view::npos
                                    ? text.size()
                                    : end + close.size();
                            std::size_t begin = i;
                            if (u8_prefix)
                            {
                                begin = i - 2;
                            }
                            else if (simple_prefix)
                            {
                                begin = i - 1;
                            }
                            // 编码前缀字符（u8R/uR/UR/LR 的前半段）此前被当作
                            // 代码流过，这里把区间起点回拨，划进字面量。
                            emit_code_before(begin);
                            sink(SourceSpan::RawLiteral, begin, stop);
                            span_begin = stop;
                            i = stop > 0 ? stop - 1 : 0;
                        }
                        else
                        {
                            // 非法前缀/定界符：`R"` 恰好自成一个两字符字面量
                            // （R 进串、紧随的引号闭串），括号部分退回代码。
                            emit_code_before(i);
                            sink(SourceSpan::Literal, i, i + 2);
                            span_begin = i + 2;
                            ++i;
                        }
                    }
                    else if (c == '"')
                    {
                        emit_code_before(i);
                        span_begin = i;
                        state = State::String;
                    }
                    else if (c == '\'')
                    {
                        emit_code_before(i);
                        span_begin = i;
                        state = State::Char;
                    }
                    break;
                case State::LineComment:
                    if (c == '\n')
                    {
                        sink(SourceSpan::LineComment, span_begin, i);
                        span_begin = i;
                        state = State::Code;
                    }
                    break;
                case State::BlockComment:
                    if (c == '*' && next == '/')
                    {
                        sink(SourceSpan::BlockComment, span_begin, i + 2);
                        ++i;
                        span_begin = i + 1;
                        state = State::Code;
                    }
                    break;
                case State::String:
                case State::Char:
                {
                    const char quote = state == State::String ? '"' : '\'';
                    if (c == '\\' && next != '\0')
                    {
                        ++i;
                    }
                    else if (c == quote)
                    {
                        sink(SourceSpan::Literal, span_begin, i + 1);
                        span_begin = i + 1;
                        state = State::Code;
                    }
                    break;
                }
                }
            }
            // 截断文件留下的未闭合区间照实切出（孤引号/未终结注释）。
            if (state == State::Code)
            {
                emit_code_before(text.size());
            }
            else
            {
                const auto kind =
                    state == State::LineComment    ? SourceSpan::LineComment
                    : state == State::BlockComment ? SourceSpan::BlockComment
                                                   : SourceSpan::Literal;
                sink(kind, span_begin, text.size());
            }
        }

        /**
         * @brief 把注释与字符串/字符/原始字符串字面量内容替换为空格。
         *
         * @internal 保留换行与字节长度，故掩码文本与原文同下标。引用扫描若直接
         * 在原文上匹配，`R"(...)` 里的 `R`、日志文本里的字段名都会被当成代码
         * 引用；先掩码再匹配可以根除这类假命中。输出策略：代码区间原样保留，
         * 其余区间除换行外就地置空格（原始字符串识别开关见
         * scan_source_spans）。
         */
        [[nodiscard]]
        auto mask_literals(std::string_view text) -> std::string
        {
            std::string out{text};
            scan_source_spans(
                text,
                /*raw_strings=*/true,
                [&out](SourceSpan kind, std::size_t begin, std::size_t end)
                {
                    if (kind == SourceSpan::Code)
                    {
                        return;
                    }
                    for (std::size_t k = begin; k < end; ++k)
                    {
                        if (out[k] != '\n' && out[k] != '\r')
                        {
                            out[k] = ' ';
                        }
                    }
                });
            return out;
        }

        /** @brief name 在已掩码文本中以独立标识符出现的位置。 */
        [[nodiscard]]
        auto identifier_position_masked(
            std::string_view masked,
            std::string_view name) -> std::optional<std::size_t>
        {
            if (name.empty())
            {
                return std::nullopt;
            }
            for (std::size_t pos = masked.find(name);
                 pos != std::string_view::npos;
                 pos = masked.find(name, pos + 1))
            {
                const bool left_ok =
                    pos == 0
                    || !(
                        std::isalnum(
                            static_cast<unsigned char>(masked[pos - 1]))
                        || masked[pos - 1] == '_');
                const std::size_t end = pos + name.size();
                const bool        right_ok =
                    end >= masked.size()
                    || !(
                        std::isalnum(static_cast<unsigned char>(masked[end]))
                        || masked[end] == '_');
                if (left_ok && right_ok)
                {
                    return pos;
                }
            }
            return std::nullopt;
        }

        /** @brief name 以独立标识符出现的位置（跳过注释与字面量）。 */
        [[nodiscard]]
        auto identifier_position(std::string_view text, std::string_view name)
            -> std::optional<std::size_t>
        {
            return identifier_position_masked(mask_literals(text), name);
        }

        /** @brief name 是否以独立标识符（前后非标识符字符）出现在文本中。 */
        [[nodiscard]]
        auto line_has_identifier(std::string_view text, std::string_view name)
            -> bool
        {
            return identifier_position(text, name).has_value();
        }

        /** @brief 读取盘上文本；失败返回 nullopt（util
         * 单点转发的既有名字别名）。 */
        [[nodiscard]]
        auto read_text_file(const std::string& path)
            -> std::optional<std::string>
        {
            return cg::util::read_text_file(path);
        }

        /**
         * @brief 去掉注释与全部空白后的代码指纹。
         *
         * @internal 用于识别纯注释/空白编辑：token 序列未变即符号契约未变。
         * 字符串/字符字面量逐字保留（含其中的 `//`，故 `"//"` 不会被判成
         * 注释）；注释区间整段丢弃。原始字符串刻意不做特殊处理——最坏情况
         * 是失去这次提示，而不是误报（与掩码侧的口径差异见
         * scan_source_spans）。
         */
        [[nodiscard]]
        auto code_fingerprint(std::string_view code) -> std::string
        {
            std::string out;
            out.reserve(code.size());
            scan_source_spans(
                code,
                /*raw_strings=*/false,
                [&out,
                 code](SourceSpan kind, std::size_t begin, std::size_t end)
                {
                    switch (kind)
                    {
                    case SourceSpan::Code:
                        for (std::size_t i = begin; i < end; ++i)
                        {
                            if (!std::isspace(
                                    static_cast<unsigned char>(code[i])))
                            {
                                out.push_back(code[i]);
                            }
                        }
                        break;
                    case SourceSpan::Literal:
                        out.append(code.substr(begin, end - begin));
                        break;
                    case SourceSpan::LineComment:
                    case SourceSpan::BlockComment:
                    case SourceSpan::RawLiteral:
                        break;
                    }
                });
            return out;
        }

        /** @brief 一次编辑在“编辑后文件”中的位置（1 基行号）。 */
        struct EditSpan
        {
            bool        have_line = false;
            std::size_t line = 1;  // 首个新增/变更行
            std::size_t lines = 1; // 变更行数
            bool        comment_only = false;
        };

        /** @brief 把一段文本按行切分（保留空行）。 */
        [[nodiscard]]
        auto split_lines(std::string_view text) -> std::vector<std::string_view>
        {
            std::vector<std::string_view> lines;
            std::size_t                   begin = 0;
            while (begin <= text.size())
            {
                const std::size_t end = text.find('\n', begin);
                std::string_view  line = text.substr(
                    begin,
                    (end == std::string_view::npos ? text.size() : end)
                        - begin);
                if (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }
                lines.push_back(line);
                if (end == std::string_view::npos)
                {
                    break;
                }
                begin = end + 1;
            }
            return lines;
        }

        /** @brief 从 structuredPatch 的 hunks 取首个新增行；纯删除时取锚点。 */
        [[nodiscard]]
        auto span_from_patch(const json::ValueView& patch, EditSpan& span)
            -> bool
        {
            if (!patch.is_array())
            {
                return false;
            }
            std::optional<std::size_t> deleted_anchor;
            for (std::size_t h = 0; h < patch.size(); ++h)
            {
                const auto hunk = patch.at(h);
                if (!hunk.has_value() || !hunk->is_object())
                {
                    continue;
                }
                std::int64_t line = 1;
                if (const auto start = hunk->get("newStart");
                    start.has_value() && start->is_number())
                {
                    line = start->as_int().value_or(1);
                }
                if (line < 1)
                {
                    line = 1;
                }
                const auto lines = hunk->get("lines");
                if (!lines.has_value() || !lines->is_array())
                {
                    span.have_line = true;
                    span.line = static_cast<std::size_t>(line);
                    return true;
                }
                for (std::size_t j = 0; j < lines->size(); ++j)
                {
                    const auto entry = lines->at(j);
                    if (!entry.has_value() || !entry->is_string())
                    {
                        continue;
                    }
                    const std::string_view text{*entry->as_string()};
                    if (text.empty())
                    {
                        continue;
                    }
                    if (text.front() == '+')
                    {
                        std::size_t added = 0;
                        for (std::size_t k = j; k < lines->size(); ++k)
                        {
                            const auto next = lines->at(k);
                            if (!next.has_value() || !next->is_string())
                            {
                                break;
                            }
                            const std::string_view next_text{
                                *next->as_string()};
                            if (next_text.empty() || next_text.front() != '+')
                            {
                                break;
                            }
                            ++added;
                        }
                        span.have_line = true;
                        span.line = static_cast<std::size_t>(line);
                        span.lines = std::max<std::size_t>(added, 1);
                        return true;
                    }
                    if (text.front() == '-')
                    {
                        // 纯删除：锚点是“被删行之后那一行”（编辑后坐标）。必须
                        // 在遇到首个 '-' 时就地记录；尾部上下文行会继续推进
                        // line，不能用遍历结束后的值。
                        if (!deleted_anchor.has_value())
                        {
                            deleted_anchor = static_cast<std::size_t>(line);
                        }
                        continue;
                    }
                    if (text.front() == ' ')
                    {
                        ++line;
                    }
                }
            }
            if (deleted_anchor.has_value())
            {
                span.have_line = true;
                span.line = *deleted_anchor;
                span.lines = 1;
                return true;
            }
            return false;
        }

        /** @brief 文本在文件中的首次出现行（1 基）。 */
        [[nodiscard]]
        auto line_of_text(std::string_view file_text, std::string_view needle)
            -> std::optional<std::size_t>
        {
            if (needle.empty())
            {
                return std::nullopt;
            }
            const std::size_t pos = file_text.find(needle);
            if (pos == std::string_view::npos)
            {
                return std::nullopt;
            }
            return static_cast<std::size_t>(std::count(
                       file_text.begin(),
                       file_text.begin() + pos,
                       '\n'))
                   + 1;
        }

        /** @brief 两份文本的首个不同行（1 基）；相同返回 1。 */
        [[nodiscard]]
        auto first_different_line(
            std::string_view before,
            std::string_view after) -> std::size_t
        {
            const auto        a = split_lines(before);
            const auto        b = split_lines(after);
            const std::size_t limit = std::min(a.size(), b.size());
            for (std::size_t i = 0; i < limit; ++i)
            {
                if (a[i] != b[i])
                {
                    return i + 1;
                }
            }
            return limit + 1;
        }

        /**
         * @brief 从捕获的子进程输出提取一份 JSON 文档：机器模式恰在 stdout
         *        打印一行，故区间取首个 '{' 到末个 '}'，周围他物忽略。
         */
        [[nodiscard]]
        auto parse_child_json(const std::string& captured)
            -> std::optional<json::Document>
        {
            const std::size_t brace = captured.find('{');
            const std::size_t last = captured.rfind('}');
            if (brace == std::string::npos || last == std::string::npos
                || last <= brace)
            {
                return std::nullopt;
            }
            try
            {
                return json::Document::parse(
                    std::string_view{
                        captured.data() + brace,
                        last - brace + 1});
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        /**
         * @brief 测试注入：让本模块的全部子进程派生立即以"未启动"失败。
         *
         * @internal
         * 生产路径绝不设置该变量。单测用它覆盖 `started=false` 的诚实分支
         * （文案 + 直接入队兜底）而不真的派生 ccg——测试二进制不是 ccg，
         * 派生它只会递归跑测试。
         */
        [[nodiscard]]
        auto hook_child_start_failure_injected() -> bool
        {
            const auto env =
                cg::util::cstd::get_env("CCG_HOOK_TEST_FAIL_CHILD_START");
            return env.has_value() && *env == "1";
        }

        /**
         * @brief 测试注入：让带超时的子进程调用假装"一直在跑"。
         *
         * @internal
         * 返回的非空值表示要模拟的 update 运行时长：超过 timeout 即按超时
         * 分支返回（睡眠恰好 timeout，绝不真的等完），用于断言等待预算本身
         * ≤ 总预算减预留。
         */
        [[nodiscard]]
        auto hook_test_stall() -> std::optional<std::chrono::milliseconds>
        {
            if (const long long ms = cg::util::parse_bounded(
                    cg::util::cstd::get_env("CCG_HOOK_TEST_STALL_MS"),
                    0);
                ms > 0)
            {
                return std::chrono::milliseconds{ms};
            }
            return std::nullopt;
        }

        /**
         * @brief 以 args（UTF-8、无内嵌引号）把本可执行文件作为子进程运行，
         *        返回其解析后的机器模式 JSON 文档。
         *
         * @internal
         * 派生是直接 exec（无 shell），子进程继承工作目录与环境；库定位由
         * 父进程解析成显式的 `-o <文件夹> --config <配置>` 转发，两侧绝不
         * 各自猜。防御上绝不与无 shell 的派生纠缠引号问题。
         */
        [[nodiscard]]
        auto run_child_json(const std::vector<std::string>& args)
            -> std::optional<json::Document>
        {
            if (hook_child_start_failure_injected())
            {
                return std::nullopt;
            }
            const fs::path exe = cg::util::platform::executable_path();
            for (const std::string& arg : args)
            {
                if (arg.find('"') != std::string::npos)
                {
                    return std::nullopt;
                }
            }
            if (exe.empty())
            {
                return std::nullopt;
            }
            std::string captured;
            int         exit_code = -1;
            if (!cg::util::platform::capture_exec(
                    exe,
                    args,
                    captured,
                    exit_code))
            {
                return std::nullopt;
            }
            return parse_child_json(captured);
        }

/**
 * @brief PostToolUse 从进程真实创建时刻起算的总预算；宿主硬超时是
 *        10 秒。
 *
 * @internal
 * 只扣等待预算是不够的：等待结束后还要算影响面、派生查询、写注入，
 * 12k TU 库上这段固定开销实测约 1.3 秒。因此预算从进程创建起算，
 * 等待只拿"总预算 − 已存活 − 预留"的剩余部分，保证整个 hook 进程
 * 在宿主 timeout 之前结束（留至少 0.5 秒余量）。基准取自
 * cg::util::platform::process_elapsed_since_creation：Windows 上
 * 包含镜像加载与 DLL 初始化（实测冷启动可达数秒），不再从模块静态
 * 初始化才开始计时。
 *
 * @internal 非 Linux 的其它 POSIX（含 macOS）没有免权限的真实创建
 * 时刻，回退基准只能看到模块静态初始化之后的时长，镜像加载不可见；
 * 那里的默认总预算收紧到 8 秒，把这段不可观测的时间留进宿主余量。
 */
#if defined(_WIN32) || defined(__linux__)
        inline constexpr auto HOOK_TOTAL_BUDGET =
            std::chrono::milliseconds{9000};
#else
        inline constexpr auto HOOK_TOTAL_BUDGET =
            std::chrono::milliseconds{8000};
#endif

        /** @brief 等待结束后留给影响面计算与注入写出的余量。 */
        inline constexpr auto HOOK_POST_RESERVE =
            std::chrono::milliseconds{1000};

        /** @brief 读取毫秒环境变量；缺失或非法时用回退值。 */
        [[nodiscard]]
        auto env_millis(const char* name, std::chrono::milliseconds fallback)
            -> std::chrono::milliseconds
        {
            if (const auto env = cg::util::cstd::get_env(name))
            {
                const long long ms = std::atoll(env->c_str());
                if (ms >= 0)
                {
                    return std::chrono::milliseconds{ms};
                }
            }
            return fallback;
        }

        [[nodiscard]]
        auto hook_total_budget() -> std::chrono::milliseconds
        {
            return env_millis("CCG_HOOK_TOTAL_BUDGET_MS", HOOK_TOTAL_BUDGET);
        }

        [[nodiscard]]
        auto hook_post_reserve() -> std::chrono::milliseconds
        {
            return env_millis("CCG_HOOK_POST_RESERVE_MS", HOOK_POST_RESERVE);
        }

        /**
         * @brief 本次 update 等待可用的预算：总截止 − 进程已存活 − 预留。
         *
         * @internal
         * 绝对截止 = 进程真实创建时刻 + CCG_HOOK_TOTAL_BUDGET_MS（默认 9 秒；
         * 非 Linux 的其它 POSIX 为 8 秒，见 HOOK_TOTAL_BUDGET）。
         * 已存活时长取自 cg::util::platform::process_elapsed_since_creation，
         * Windows 上包含镜像加载/DLL 初始化；POSIX 为 best-effort（见
         * process_elapsed_since_creation 的平台差异注释）。`CCG_HOOK_UPDATE_TIMEOUT_MS` 仍可进一步收紧
         * （测试超时路径），但绝不能把预算推过绝对截止。结果非负；为 0 时
         * 子进程照样派生后立即 detach（绝不 kill）。
         */
        [[nodiscard]]
        auto hook_update_timeout() -> std::chrono::milliseconds
        {
            const auto elapsed =
                cg::util::platform::process_elapsed_since_creation();
            auto remaining =
                hook_total_budget() - elapsed - hook_post_reserve();
            if (remaining < std::chrono::milliseconds{0})
            {
                remaining = std::chrono::milliseconds{0};
            }
            return std::min(
                remaining,
                env_millis("CCG_HOOK_UPDATE_TIMEOUT_MS", remaining));
        }

        /**
         * @brief 绝对截止（进程创建起算）是否已到；margin 留给最终的 emit。
         *
         * @internal
         * 等待预算只约束 update 子进程。等待结束后的符号定位与影响面查询各自
         * 派生 ccg
         * 子进程，参考语料这类大库上单次可达数百毫秒；等待用满预算时这些
         * 步骤会把整个 hook 推过宿主 10 秒硬超时，注入因此丢失。每一步昂贵操作
         * 前用本探针给 emit 留出余量：宁可少报影响面，也必须在截止前把注入发
         * 出去。
         */
        [[nodiscard]]
        auto hook_deadline_reached(std::chrono::milliseconds margin) -> bool
        {
            // margin 之外还要算上 emit 预留：margin 只描述被启动的那一步能
            // 等多久，最终写注入同样不能越过宿主硬超时。
            return cg::util::platform::process_elapsed_since_creation() + margin
                       + hook_post_reserve()
                   >= hook_total_budget();
        }

        /**
         * @brief 派生查询子进程的单次上限。
         *
         * @internal
         * 数据库被后台 update 持有时，查询子进程会卡在 busy_timeout（默认
         * 5 秒）；给每类查询一个明确上限，并用同一个值当作"现在还能不能
         * 开始这一步"的截止余量——凡是要等超过余量的操作都不再启动。
         */
        inline constexpr auto HOOK_QUERY_TIMEOUT =
            std::chrono::milliseconds{4000};
        inline constexpr auto HOOK_IMPACT_TIMEOUT =
            std::chrono::milliseconds{2000};
        inline constexpr auto HOOK_CHANGES_TIMEOUT =
            std::chrono::milliseconds{1500};

        /**
         * @brief 进程内查询会话的忙等待上限（毫秒）。
         *
         * @internal
         * 库开在 WAL 下，读不受写者阻塞；这个上限只保险开库与极端窗口
         * （与 pretooluse 处理器的短忙超时同款）。撞上即让位，绝不拖住宿主。
         */
        inline constexpr std::int64_t HOOK_QUERY_BUSY_MS = 300;

        /** @brief 带超时的子进程 JSON 结果：超时时 doc 为空而子进程继续跑。 */
        struct TimedChildJson
        {
            std::optional<json::Document> doc;
            bool                          started = false;
            bool                          timed_out = false;
        };

        /**
         * @brief 以 args 运行本可执行文件，最多等 timeout；超时即放弃等待。
         *
         * @internal
         * stdout/stderr 被重定向到平台层的临时文件（子进程在父进程退出后
         * 继续写并继续完成更新）。子进程的 stderr 在完成时转回本进程的
         * stderr——与 capture_exec 的可见性一致；超时分支没有已捕获字节。
         */
        [[nodiscard]]
        auto run_child_json_timed(
            const std::vector<std::string>& args,
            std::chrono::milliseconds       timeout) -> TimedChildJson
        {
            TimedChildJson out;
            if (hook_child_start_failure_injected())
            {
                return out; // started=false：调用方走诚实文案与直接入队兜底
            }
            if (const auto stall = hook_test_stall())
            {
                // 测试注入：模拟一个跑不完的 update，但最多只睡等待预算本身。
                const auto waited = std::min(*stall, timeout);
                std::this_thread::sleep_for(waited);
                out.started = true;
                out.timed_out = *stall > timeout;
                return out;
            }
            const fs::path exe = cg::util::platform::executable_path();
            if (exe.empty())
            {
                return out;
            }
            for (const std::string& arg : args)
            {
                if (arg.find('"') != std::string::npos)
                {
                    return out;
                }
            }
            cg::util::platform::TimedExecResult result =
                cg::util::platform::capture_exec_timed(exe, args, timeout);
            out.started = result.started;
            out.timed_out = result.timed_out;
            if (result.started && !result.restricted_handles)
            {
                // 平台层的句柄白名单没建立起来，只能退回全继承：记录事实，
                // 别让宿主 stdio 被后台子进程拖住的回归无声无息。
                std::println(
                    std::cerr,
                    "ccg: note: child handle whitelist unavailable - "
                    "fell back to inherited-handle mode");
            }
            if (!result.err.empty())
            {
                std::print(std::cerr, "{}", result.err);
            }
            if (result.started && !result.timed_out)
            {
                // Document 只可移动构造（没有可用的赋值运算符）：emplace 而不是
                // 给 optional 赋值。
                auto parsed = parse_child_json(result.out);
                if (parsed.has_value())
                {
                    out.doc.emplace(std::move(*parsed));
                }
            }
            return out;
        }

        /**
         * @brief 把非法 UTF-8 字节替换为 U+FFFD，保证 JSON 写出不会整体失败。
         */
        [[nodiscard]]
        auto sanitize_utf8(std::string_view text) -> std::string
        {
            std::string out;
            out.reserve(text.size());
            std::size_t i = 0;
            while (i < text.size())
            {
                const std::size_t len = cg::util::utf8_seq_len(text[i]);
                bool              valid =
                    len != 1 || static_cast<unsigned char>(text[i]) < 0x80U;
                valid = valid && i + len <= text.size();
                for (std::size_t k = 1; valid && k < len; ++k)
                {
                    valid = cg::util::utf8_is_continuation(text[i + k]);
                }
                if (valid)
                {
                    out.append(text.substr(i, len));
                    i += len;
                }
                else
                {
                    out += "\xEF\xBF\xBD";
                    ++i;
                }
            }
            return out;
        }

        /** @brief 在 stdout 发出 Claude Code 钩子应答信封。 */
        void emit_hook_context(std::string_view event, std::string_view context)
        {
            const auto envelope = [&](std::string_view ctx)
            {
                json::Object inner;
                inner.set("hookEventName", event);
                inner.set("additionalContext", ctx);
                json::Object root;
                root.set("hookSpecificOutput", std::move(inner));
                return json::stringify(std::move(root));
            };
            std::string out = envelope(context);
            if (out.empty())
            {
                // 兜底：非法 UTF-8 只应来自意外的外部输入；替换后重试，
                // 绝不让整条注入退化成空行。
                out = envelope(sanitize_utf8(context));
            }
            if (out.empty())
            {
                out = std::format(
                    R"({{"hookSpecificOutput":{{"hookEventName":"{}",)"
                    R"("additionalContext":"[ccg] 注入生成失败（JSON 写出错误）"}}}})",
                    sanitize_utf8(event));
            }
            std::println("{}", out);
        }

        /** @brief 一次编辑的语义摘要：落点（编辑后行号）与是否只动了注释/空白。
         */
        struct EditInfo
        {
            EditSpan span;
            bool     comment_only = false;
        };

        /** @brief 从 JSON 对象成员取字符串；缺失/非字符串返回 ""。 */
        [[nodiscard]]
        auto member_string(const json::ValueView& object, std::string_view key)
            -> std::string
        {
            if (!object.is_object())
            {
                return {};
            }
            const auto value = object.get(key);
            if (!value.has_value() || !value->is_string())
            {
                return {};
            }
            return std::string{*value->as_string()};
        }

        /**
         * @brief 从工具调用载荷还原“改在哪一行、是否纯注释/空白”。
         *
         * @internal 首选 `tool_response.structuredPatch`（Edit/MultiEdit 的机器
         * 补丁，行号已是编辑后坐标）；缺失时退回把 `new_string` 定位到盘上
         * 文件；Write 则用 `originalFile` 与 `content` 的首个差异行。
         */
        [[nodiscard]]
        auto extract_edit_info(
            const json::ValueView&                tool_input,
            const std::optional<json::ValueView>& tool_response,
            const std::string&                    file_path,
            std::string_view                      tool_name) -> EditInfo
        {
            EditInfo info;
            bool     have_pair = false;
            bool     all_comment_only = true;

            const auto edits = tool_input.get("edits");
            if (edits.has_value() && edits->is_array() && edits->size() > 0)
            {
                for (std::size_t i = 0; i < edits->size(); ++i)
                {
                    const auto edit = edits->at(i);
                    if (!edit.has_value() || !edit->is_object())
                    {
                        all_comment_only = false;
                        continue;
                    }
                    const std::string old_text =
                        member_string(*edit, "old_string");
                    const std::string new_text =
                        member_string(*edit, "new_string");
                    const auto old_v = edit->get("old_string");
                    const auto new_v = edit->get("new_string");
                    if (!old_v.has_value() || !new_v.has_value())
                    {
                        all_comment_only = false;
                        continue;
                    }
                    have_pair = true;
                    if (code_fingerprint(old_text)
                        != code_fingerprint(new_text))
                    {
                        all_comment_only = false;
                    }
                }
            }
            else
            {
                const auto old_v = tool_input.get("old_string");
                const auto new_v = tool_input.get("new_string");
                if (old_v.has_value() && new_v.has_value() && old_v->is_string()
                    && new_v->is_string())
                {
                    have_pair = true;
                    const std::string old_text{*old_v->as_string()};
                    const std::string new_text{*new_v->as_string()};
                    all_comment_only = code_fingerprint(old_text)
                                       == code_fingerprint(new_text);
                }
            }
            info.comment_only = have_pair && all_comment_only;
            info.span.comment_only = info.comment_only;

            if (tool_response.has_value())
            {
                if (const auto patch = tool_response->get("structuredPatch");
                    patch.has_value() && span_from_patch(*patch, info.span))
                {
                    return info;
                }
            }

            if (tool_name == "Write")
            {
                const auto content = tool_input.get("content");
                const auto original = tool_response.has_value()
                                          ? tool_response->get("originalFile")
                                          : std::nullopt;
                if (content.has_value() && content->is_string()
                    && original.has_value() && original->is_string())
                {
                    const std::string before{*original->as_string()};
                    const std::string after{*content->as_string()};
                    info.span.have_line = true;
                    info.span.line = first_different_line(before, after);
                    info.span.lines = 1;
                    info.comment_only =
                        code_fingerprint(before) == code_fingerprint(after);
                    info.span.comment_only = info.comment_only;
                }
                return info;
            }

            std::string new_text;
            std::string old_text;
            if (edits.has_value() && edits->is_array() && edits->size() > 0)
            {
                const auto edit = edits->at(0);
                if (edit.has_value() && edit->is_object())
                {
                    new_text = member_string(*edit, "new_string");
                    old_text = member_string(*edit, "old_string");
                }
            }
            else
            {
                new_text = member_string(tool_input, "new_string");
                old_text = member_string(tool_input, "old_string");
            }
            if (!new_text.empty())
            {
                if (const auto file_text = read_text_file(file_path);
                    file_text.has_value())
                {
                    if (const auto line = line_of_text(*file_text, new_text);
                        line.has_value())
                    {
                        info.span.have_line = true;
                        info.span.line = *line;
                        info.span.lines = std::max<std::size_t>(
                            split_lines(new_text).size(),
                            1);
                    }
                    else if (!old_text.empty())
                    {
                        // 无 structuredPatch 的模拟/未落盘载荷：new_string
                        // 尚未出现 在盘上，但编辑点就是 old_string
                        // 当前所在行（纯注释/空白
                        // 插入不会把该行移走）。没有这一回退，锚点会整个丢失。
                        if (const auto old_line =
                                line_of_text(*file_text, old_text);
                            old_line.has_value())
                        {
                            info.span.have_line = true;
                            info.span.line = *old_line;
                            info.span.lines = std::max<std::size_t>(
                                split_lines(new_text).size(),
                                1);
                        }
                    }
                }
            }
            return info;
        }

        /** @brief 库里的一个符号（注入所需的字段）。 */
        struct FileSymbol
        {
            std::int64_t uid = 0;
            std::int64_t line = 0;
            std::int64_t col = 0;
            std::string  kind;
            std::string  name;
            std::string  qualified_name;
            std::string  file;
            std::string  parent;
        };

        /**
         * @brief PostToolUse 的进程内只读查询会话（进程内直连的混合形态）。
         *
         * @internal
         * 照 pretooluse 处理器的现成模式（只读开库 + 直接调 db 查询函数，毫秒
         * 级）取代逐查询派生 ccg 子进程：一次开库、一次语句表预编译，本次
         * 处理器的全部查询共用。增量 update 派生不在其内——写锁隔离 + 超时
         * 撒手（后台跑完不丢活）的语义保持子进程形态。alive=false 表示打开
         * 失败或某次查询抛出（忙/损坏）：之后所有查询按旧“子进程超时”的同一
         * 语义让位（调用方保留已收集的候选、停止派生），注入文案与旧超时路
         * 径逐字一致。
         */
        struct HookQueryDb
        {
            std::unique_ptr<cg::db::GraphStore> store;
            bool                                alive = false;
        };

        /** @brief 打开查询会话；打不开（忙/损坏/缺库）就带着 alive=false 返回。
         */
        [[nodiscard]]
        auto open_hook_query_db(const std::string& db_path) -> HookQueryDb
        {
            HookQueryDb out;
            try
            {
                out.store = std::make_unique<cg::db::GraphStore>(
                    db_path,
                    HOOK_QUERY_BUSY_MS);
                out.alive = true;
            }
            catch (const std::exception&)
            {
            }
            return out;
        }

        /**
         * @brief 完整符号行 -> 注入所需的轻量形状。
         *
         * @internal 字段集与旧 `ccg query --json`
         * 解析路径（parse_symbol_matches）
         * 完全一致，逐字节等价的注入输出由同一形状保证。
         */
        [[nodiscard]]
        auto file_symbol_of(const cg::db::SymbolRow& row) -> FileSymbol
        {
            FileSymbol s;
            s.uid = row.id;
            s.line = row.line;
            s.col = row.col;
            s.kind = std::string{cg::db::name_of(row.kind)};
            s.name = row.name;
            s.qualified_name = row.qualified_name;
            s.file = row.file;
            s.parent = row.parent;
            return s;
        }

        /**
         * @brief 进程内直连的按名查询：与 `ccg query <needle> --file <filter>
         *        --json` 同判据（分层查找、层合并、路径子串大小写敏感）。
         *
         * @internal `timed_out` 回填本次查询是否让位（会话失效/查询抛出）：调用
         * 方据此停止继续派生查询，而不是把"让位"误当成"没有匹配"——与旧子进
         * 程超时同一个旗语，行为等价。
         */
        [[nodiscard]]
        auto query_symbols(
            HookQueryDb&       db,
            const std::string& needle,
            const std::string& file_filter,
            bool*              timed_out = nullptr) -> std::vector<FileSymbol>
        {
            if (timed_out != nullptr)
            {
                *timed_out = false;
            }
            if (!db.alive)
            {
                if (timed_out != nullptr)
                {
                    *timed_out = true;
                }
                return {};
            }
            try
            {
                cg::db::SymbolFilter filter;
                filter.file_substring = file_filter;
                const cg::db::FindResult found =
                    cg::db::find_symbols(*db.store, needle, filter);
                std::vector<FileSymbol> out;
                out.reserve(found.matches.size());
                for (const cg::db::SymbolRow& row : found.matches)
                {
                    out.push_back(file_symbol_of(row));
                }
                return out;
            }
            catch (const std::exception&)
            {
                // 让位即弃用会话：后续查询全部按超时语义跳过（同旧级联）。
                db.alive = false;
                if (timed_out != nullptr)
                {
                    *timed_out = true;
                }
                return {};
            }
        }

        /** @brief C++ 关键字与常见噪声词（不可能是我们要找的符号名）。 */
        [[nodiscard]]
        auto is_noise_token(std::string_view token) -> bool
        {
            static constexpr std::string_view NOISE[] = {
                "auto",     "bool",     "break",     "case",      "catch",
                "char",     "class",    "const",     "constexpr", "continue",
                "default",  "delete",   "do",        "double",    "else",
                "enum",     "explicit", "extern",    "false",     "float",
                "for",      "friend",   "if",        "inline",    "int",
                "long",     "mutable",  "namespace", "new",       "noexcept",
                "nullptr",  "operator", "override",  "private",   "protected",
                "public",   "return",   "short",     "signed",    "sizeof",
                "static",   "std",      "struct",    "switch",    "template",
                "this",     "throw",    "true",      "try",       "typedef",
                "typename", "union",    "unsigned",  "using",     "virtual",
                "void",     "volatile", "while",     "include",   "define",
            };
            return std::ranges::find(NOISE, token) != std::ranges::end(NOISE);
        }

        /**
         * @brief 把 `operator` 与其后的符号拼成可查询的完整名字。
         *
         * @internal `operator` 单独出现时被噪音表过滤，`!=` 又不是标识符，
         * 于是 `bool operator!=(...)` 一行产不出任何候选 token。重载运算符
         * 在库里的名字就是 `operator!=`/`operator()`/`operator[]`，按完整
         * 拼写查询即可直接命中。`pos` 指向 `operator` 之后的第一个字符。
         */
        [[nodiscard]]
        auto operator_token_at(std::string_view line, std::size_t pos)
            -> std::string
        {
            if (pos >= line.size())
            {
                return {};
            }
            const auto skip_space = [&](std::size_t at)
            {
                while (at < line.size()
                       && std::isspace(static_cast<unsigned char>(line[at])))
                {
                    ++at;
                }
                return at;
            };
            pos = skip_space(pos);
            if (pos >= line.size())
            {
                return {};
            }
            if (line[pos] == '(')
            {
                const std::size_t close = skip_space(pos + 1);
                return close < line.size() && line[close] == ')'
                           ? "operator()"
                           : std::string{};
            }
            if (line[pos] == '[')
            {
                const std::size_t close = skip_space(pos + 1);
                return close < line.size() && line[close] == ']'
                           ? "operator[]"
                           : std::string{};
            }
            static constexpr std::string_view SYMBOLS = "=!<>+-*/%^&|~,";
            std::size_t                       end = pos;
            while (end < line.size()
                   && SYMBOLS.find(line[end]) != std::string_view::npos
                   && end - pos < 3)
            {
                ++end;
            }
            if (end > pos)
            {
                return "operator" + std::string{line.substr(pos, end - pos)};
            }
            const unsigned char c = static_cast<unsigned char>(line[pos]);
            if (std::isalpha(c) || c == '_')
            {
                std::size_t name_end = pos;
                while (
                    name_end < line.size()
                    && (std::isalnum(static_cast<unsigned char>(line[name_end]))
                        || line[name_end] == '_'))
                {
                    ++name_end;
                }
                // 转换运算符（`operator bool`）；查询不到时无害。
                return std::format(
                    "operator {}",
                    line.substr(pos, name_end - pos));
            }
            return {};
        }

        /** @brief 收集一行里的标识符 token（去重、过滤关键字）。 */
        void collect_tokens(
            std::string_view          line,
            std::vector<std::string>& out)
        {
            std::size_t i = 0;
            while (i < line.size())
            {
                const unsigned char c = static_cast<unsigned char>(line[i]);
                if (std::isalpha(c) || c == '_')
                {
                    const std::size_t begin = i;
                    while (i < line.size())
                    {
                        const unsigned char d =
                            static_cast<unsigned char>(line[i]);
                        if (!std::isalnum(d) && d != '_')
                        {
                            break;
                        }
                        ++i;
                    }
                    const std::string_view token =
                        line.substr(begin, i - begin);
                    if (token == "operator")
                    {
                        const std::string composed = operator_token_at(line, i);
                        const bool        seen = std::ranges::any_of(
                            out,
                            [&](const std::string& known)
                            { return known == composed; });
                        if (!composed.empty() && !seen)
                        {
                            out.emplace_back(composed);
                        }
                        continue;
                    }
                    const bool seen = std::ranges::any_of(
                        out,
                        [&](const std::string& known)
                        { return std::string_view{known} == token; });
                    if (token.size() >= 2 && !is_noise_token(token) && !seen)
                    {
                        out.emplace_back(token);
                    }
                }
                else
                {
                    ++i;
                }
            }
        }

        /** @brief 形如 ALL_CAPS(...) 的宏调用行（其符号来自展开，不在字面上）。
         */
        [[nodiscard]]
        auto looks_like_macro_call(std::string_view text) -> bool
        {
            std::size_t i = 0;
            while (i < text.size()
                   && (std::isalnum(static_cast<unsigned char>(text[i]))
                       || text[i] == '_'))
            {
                ++i;
            }
            if (i == 0 || i >= text.size() || text[i] != '(')
            {
                return false;
            }
            const std::string_view name = text.substr(0, i);
            bool                   saw_letter = false;
            for (const char c : name)
            {
                if (std::isalpha(static_cast<unsigned char>(c)))
                {
                    saw_letter = true;
                    if (std::islower(static_cast<unsigned char>(c)))
                    {
                        return false;
                    }
                }
            }
            return saw_letter;
        }

        /**
         * @brief 取 `using <name> = ...` 声明出的别名（不是别名声明则返回空）。
         *
         * @internal 别名行本身就是“编辑点之后最近的声明行”。单字符别名
         * （`using R = ...`）会被候选 token 的“长度 >= 2”规则丢弃，随后整文件
         * 回退又只按 `_`/`::` 枚举，名字都不含这两者的别名永远不可见；纯注释
         * 编辑因此会滑到后面的变量。这里按声明位置无条件保留别名名。
         */
        [[nodiscard]]
        auto using_alias_name(std::string_view line) -> std::string
        {
            std::string_view text = line;
            while (!text.empty()
                   && std::isspace(static_cast<unsigned char>(text.front())))
            {
                text.remove_prefix(1);
            }
            if (!text.starts_with("using "))
            {
                return {};
            }
            text.remove_prefix(6);
            while (!text.empty()
                   && std::isspace(static_cast<unsigned char>(text.front())))
            {
                text.remove_prefix(1);
            }
            std::size_t end = 0;
            while (end < text.size()
                   && (std::isalnum(static_cast<unsigned char>(text[end]))
                       || text[end] == '_'))
            {
                ++end;
            }
            if (end == 0)
            {
                return {};
            }
            std::size_t after = end;
            while (after < text.size()
                   && std::isspace(static_cast<unsigned char>(text[after])))
            {
                ++after;
            }
            // `using namespace x;` / `using std::foo;` 不是别名声明。
            if (after >= text.size() || text[after] != '=')
            {
                return {};
            }
            return std::string{text.substr(0, end)};
        }

        /**
         * @brief 把一行里的“声明名”token 提到本行候选的最前面。
         *
         * @internal `SampleImpl::update_cache()` 的声明名在最后一个
         * `::` 之后，字段的声明名在 `=` 之前，函数名在 `(` 之前。查询预算
         * 有限（见 locate 的 tried 上限），按词序硬扫会让外层类名先耗尽预算；
         * 先把这三个位置的名字排到前面，普通标识符保持原相对顺序。
         */
        void prioritize_declaration_tokens(
            std::string_view          line,
            std::vector<std::string>& tokens,
            std::size_t               begin)
        {
            const auto skip_space_back = [&](std::size_t pos)
            {
                while (
                    pos > 0
                    && std::isspace(static_cast<unsigned char>(line[pos - 1])))
                {
                    --pos;
                }
                return pos;
            };
            const auto skip_space_forward = [&](std::size_t pos)
            {
                while (pos < line.size()
                       && std::isspace(static_cast<unsigned char>(line[pos])))
                {
                    ++pos;
                }
                return pos;
            };
            const auto identifier_before = [&](std::size_t pos)
            {
                pos = skip_space_back(pos);
                const std::size_t end = pos;
                while (
                    pos > 0
                    && (std::isalnum(static_cast<unsigned char>(line[pos - 1]))
                        || line[pos - 1] == '_'))
                {
                    --pos;
                }
                return pos < end ? std::string{line.substr(pos, end - pos)}
                                 : std::string{};
            };
            const auto identifier_after = [&](std::size_t pos)
            {
                pos = skip_space_forward(pos);
                const std::size_t from = pos;
                while (pos < line.size()
                       && (std::isalnum(static_cast<unsigned char>(line[pos]))
                           || line[pos] == '_'))
                {
                    ++pos;
                }
                return from < pos ? std::string{line.substr(from, pos - from)}
                                  : std::string{};
            };
            std::vector<std::string> preferred;
            const auto               add = [&](std::string name)
            {
                if (name.empty() || is_noise_token(name))
                {
                    return;
                }
                cg::util::push_unique(preferred, std::move(name));
            };
            if (const std::size_t scope = line.rfind("::");
                scope != std::string_view::npos)
            {
                add(identifier_after(scope + 2));
            }
            if (const std::size_t paren = line.find('(');
                paren != std::string_view::npos)
            {
                add(identifier_before(paren));
            }
            for (std::size_t at = line.find('='); at != std::string_view::npos;
                 at = line.find('=', at + 1))
            {
                const char prev = at > 0 ? line[at - 1] : '\0';
                const char next = at + 1 < line.size() ? line[at + 1] : '\0';
                if (next == '=' || prev == '=' || prev == '!' || prev == '<'
                    || prev == '>')
                {
                    continue;
                }
                add(identifier_before(at));
                break;
            }
            std::size_t insert_at = begin;
            for (const std::string& want : preferred)
            {
                if (insert_at >= tokens.size())
                {
                    break;
                }
                const auto it = std::find(
                    tokens.begin() + static_cast<std::ptrdiff_t>(insert_at),
                    tokens.end(),
                    want);
                if (it == tokens.end())
                {
                    continue;
                }
                if (it
                    != tokens.begin() + static_cast<std::ptrdiff_t>(insert_at))
                {
                    std::rotate(
                        tokens.begin() + static_cast<std::ptrdiff_t>(insert_at),
                        it,
                        it + 1);
                }
                ++insert_at;
            }
        }

        /** @brief 行首空白剥除（isspace 口径）；返回剥除后的视图。 */
        [[nodiscard]]
        auto lstrip_spaces(std::string_view text) -> std::string_view
        {
            while (!text.empty()
                   && std::isspace(static_cast<unsigned char>(text.front())))
            {
                text.remove_prefix(1);
            }
            return text;
        }

        /**
         * @brief 剥除行首空白后是否注释/预处理行（// /* * # 四类前缀）。
         *
         * 行级扫描的仓内单点（候选 token、声明定位、落点附近的六处内联判定
         * 收编于此）；带额外前缀的调用方（如 "}" 起始行）自行补一个
         * starts_with 判定。
         */
        [[nodiscard]]
        auto is_comment_or_pp_line(std::string_view line) -> bool
        {
            const std::string_view text = lstrip_spaces(line);
            return text.starts_with("//") || text.starts_with("/*")
                   || text.starts_with("*") || text.starts_with("#");
        }

        /**
         * @brief 编辑落点附近最可能标识被编辑符号的 token。
         *
         * @internal 纯注释编辑时跳过注释/空行，从随后几行声明里取标识符
         * （`// 注释` 正下方那行就是目标声明）；语义编辑从落点行及其相邻行
         * 取。token 查询是索引命中的，比枚举整文件符号快一个数量级。遇到
         * `ALL_CAPS(...)` 宏调用则放弃——宏生成的符号名不在字面上，靠 token
         * 只会命中展开体里的字段，交给整文件回退按行判断。
         */
        [[nodiscard]]
        auto candidate_tokens(std::string_view file_text, const EditSpan& span)
            -> std::vector<std::string>
        {
            if (!span.have_line)
            {
                return {};
            }
            const auto lines = split_lines(file_text);
            if (lines.empty())
            {
                return {};
            }
            std::size_t begin = std::min(span.line - 1, lines.size() - 1);
            if (!span.comment_only && begin > 0)
            {
                --begin;
            }
            const std::size_t        end = std::min(lines.size(), begin + 18);
            std::vector<std::string> tokens;
            for (std::size_t i = begin; i < end && tokens.size() < 32; ++i)
            {
                const std::string_view text = lstrip_spaces(lines[i]);
                if (text.empty() || is_comment_or_pp_line(text))
                {
                    continue;
                }
                if (looks_like_macro_call(text))
                {
                    if (tokens.empty())
                    {
                        // gtest 风格宏（TEST/TEST_F）：库内符号名是宏参数的
                        // 拼接（Suite_Name_Test），参数本身在字面上出现，
                        // 按标识符收集即可被 LIKE 查询命中。旧行为在这里
                        // 直接放弃，导致整文件枚举成为唯一路径。
                        const std::size_t open = text.find('(');
                        if (open != std::string_view::npos
                            && open + 1 < text.size())
                        {
                            collect_tokens(text.substr(open + 1), tokens);
                        }
                        if (tokens.empty())
                        {
                            return {};
                        }
                    }
                    break;
                }
                // 最近的声明行若是 `using `<name>` = ...`，别名名优先于该行其它
                // token（且不受长度过滤），否则单字符别名会整行被跳过。
                const std::size_t line_begin = tokens.size();
                if (const std::string alias = using_alias_name(text);
                    !alias.empty())
                {
                    cg::util::push_unique(tokens, alias);
                }
                collect_tokens(text, tokens);
                prioritize_declaration_tokens(text, tokens, line_begin);
            }
            return tokens;
        }

        /**
         * @brief 同线候选的“声明标题”优先级（宏展开会在一行上叠多层结构）。
         *
         * @internal 集合经 db 层 kind 谓词族组合（原集合逐点保持）：档 1 =
         * 可调用 ∪ 类型别名，档 0 是类型标签（record/enum 无谓词承载，保持
         * 显式比较）。
         */
        [[nodiscard]]
        auto kind_rank(std::string_view kind) -> int
        {
            if (kind == "record" || kind == "enum")
            {
                return 0;
            }
            if (cg::db::is_callable(kind) || kind == "type_alias")
            {
                return 1;
            }
            if (kind == "enum_constant")
            {
                return 2;
            }
            if (kind == "variable")
            {
                return 3;
            }
            return 4;
        }

        /**
         * @brief 两个候选谁更值得报：行距优先；同距按“外层声明 > 字段”与
         *        最短限定名（宏展开的同一行会同时产出 tag、成员结构与字段）。
         */
        [[nodiscard]]
        auto better_candidate(
            const FileSymbol& a,
            std::size_t       a_dist,
            const FileSymbol& b,
            std::size_t       b_dist) -> bool
        {
            if (a_dist != b_dist)
            {
                return a_dist < b_dist;
            }
            const int rank_a = kind_rank(a.kind);
            const int rank_b = kind_rank(b.kind);
            if (rank_a != rank_b)
            {
                return rank_a < rank_b;
            }
            if (a.qualified_name.size() != b.qualified_name.size())
            {
                return a.qualified_name.size() < b.qualified_name.size();
            }
            if (a.col != b.col)
            {
                return a.col < b.col;
            }
            return a.qualified_name < b.qualified_name;
        }

        /** @brief 带盘上证据的落点候选：行距 + 声明核验 + 声明语法角色。 */
        struct NearCandidate
        {
            FileSymbol  symbol;
            std::size_t distance = 0;
            std::size_t disk_line = 0; // 盘上声明行；0 = 未找到（仅库内行号）
            bool        declarator =
                false; // `enum/struct ... { ... } name =` 的外层声明名
        };

        /**
         * @brief 某行是否像 name 的声明；declarator 回填“`}` 之后的声明名”。
         *
         * @internal 同行可能同时出现内联类型的 tag 与它声明的字段
         * （`enum class Kind { ... } kind = ...`）。`Kind` 后跟 `:` 是 tag，
         * `kind` 前面是 `}`、后面是 `=` 才是这行真正声明的对象。
         */
        [[nodiscard]]
        auto line_declares_symbol(
            std::string_view line,
            std::string_view name,
            std::string_view kind,
            bool*            declarator) -> bool
        {
            const auto pos = identifier_position(line, name);
            if (!pos.has_value())
            {
                return false;
            }
            std::size_t after = *pos + name.size();
            while (after < line.size()
                   && std::isspace(static_cast<unsigned char>(line[after])))
            {
                ++after;
            }
            std::size_t before = *pos;
            while (
                before > 0
                && std::isspace(static_cast<unsigned char>(line[before - 1])))
            {
                --before;
            }
            if (declarator != nullptr)
            {
                *declarator = before > 0 && line[before - 1] == '}';
            }
            const char next = after < line.size() ? line[after] : '\0';
            if (cg::db::is_callable(kind))
            {
                return next == '(';
            }
            if (cg::db::is_type_like(kind))
            {
                return next == '{' || next == ':' || next == ';' || next == '='
                       || next == '<' || next == '\0';
            }
            const bool member = before > 0
                                && (line[before - 1] == '.'
                                    || (before >= 2 && line[before - 1] == '>'
                                        && line[before - 2] == '-'));
            return !member
                   && (next == '=' || next == ';' || next == ',' || next == '{'
                       || next == '[' || next == '\0');
        }

        /**
         * @brief 在盘上附近找该符号的声明行（1 基）；找不到返回 0。
         *
         * 库内行号在“更新排队”或注释插入后会漂移；声明核验同时是选错符号
         * 的最后一道闸：名字若只是恰好出现在附近（构造函数的类名、同行的
         * 枚举 tag），行文本不会呈现声明形态。
         */
        [[nodiscard]]
        auto declaration_evidence_line(
            const std::vector<std::string_view>& lines,
            std::string_view                     name,
            std::string_view                     kind,
            std::size_t                          near,
            std::size_t                          back,
            std::size_t                          forward,
            bool*                                declarator) -> std::size_t
        {
            if (lines.empty() || name.empty() || near == 0)
            {
                return 0;
            }
            const std::size_t lo = near > back ? near - back : 1;
            const std::size_t hi = std::min(lines.size(), near + forward);
            std::size_t       best = 0;
            std::size_t best_distance = std::numeric_limits<std::size_t>::max();
            for (std::size_t line_no = lo; line_no <= hi; ++line_no)
            {
                const std::string_view text = lstrip_spaces(lines[line_no - 1]);
                if (text.empty() || is_comment_or_pp_line(text))
                {
                    continue;
                }
                bool role = false;
                if (!line_declares_symbol(text, name, kind, &role))
                {
                    continue;
                }
                const std::size_t distance =
                    line_no > near ? line_no - near : near - line_no;
                if (distance < best_distance)
                {
                    best = line_no;
                    best_distance = distance;
                    if (declarator != nullptr)
                    {
                        *declarator = role;
                    }
                }
            }
            return best;
        }

        /** @brief 两个近邻候选的偏好：行距 → 声明核验 → 声明角色 → 宏行标题。
         */
        [[nodiscard]]
        auto better_near_candidate(
            const NearCandidate& a,
            const NearCandidate& b) -> bool
        {
            if (a.distance != b.distance)
            {
                return a.distance < b.distance;
            }
            if ((a.disk_line != 0) != (b.disk_line != 0))
            {
                return a.disk_line != 0;
            }
            const auto is_value = [](std::string_view kind)
            { return cg::db::is_value_like(kind); };
            // 内联类型标签（record/enum/enum_constant）无谓词承载：集合语义
            // 是"同行噪音标签"而非 kind 身份档，保持显式比较。
            const auto is_inline_tag = [](std::string_view kind)
            {
                return kind == "record" || kind == "enum"
                       || kind == "enum_constant";
            };
            // 内联类型定义（`enum class Kind { ... } kind =`）里，被声明的是
            // `}` 之后那个名字；tag 与枚举常量都只是同行噪音，不能靠
            // kind_rank 压过字段（A#2 曾被 `Kind::None` 抢走）。
            const auto value_beats_tag =
                [&](const NearCandidate& value, const NearCandidate& tag)
            {
                return value.declarator && is_value(value.symbol.kind)
                       && is_inline_tag(tag.symbol.kind);
            };
            if (value_beats_tag(a, b))
            {
                return true;
            }
            if (value_beats_tag(b, a))
            {
                return false;
            }
            const int rank_a = kind_rank(a.symbol.kind);
            const int rank_b = kind_rank(b.symbol.kind);
            if (rank_a != rank_b)
            {
                return rank_a < rank_b;
            }
            if (a.symbol.qualified_name.size()
                != b.symbol.qualified_name.size())
            {
                return a.symbol.qualified_name.size()
                       < b.symbol.qualified_name.size();
            }
            if (a.symbol.col != b.symbol.col)
            {
                return a.symbol.col < b.symbol.col;
            }
            return a.symbol.qualified_name < b.symbol.qualified_name;
        }

        /** @brief 收集同文件窗口内的候选，并附上盘上声明证据。 */
        void collect_near_candidates(
            const std::vector<FileSymbol>&       matches,
            const std::string&                   want_file,
            const EditSpan&                      span,
            std::size_t                          window,
            const std::vector<std::string_view>& lines,
            std::vector<NearCandidate>*          out)
        {
            if (out == nullptr || !span.have_line)
            {
                return;
            }
            const std::string  want = path_compare_key(want_file);
            const std::int64_t anchor = static_cast<std::int64_t>(span.line);
            for (const FileSymbol& s : matches)
            {
                if (path_compare_key(s.file) != want || s.line <= 0)
                {
                    continue;
                }
                const std::size_t distance =
                    s.line >= anchor
                        ? static_cast<std::size_t>(s.line - anchor)
                        : static_cast<std::size_t>(anchor - s.line);
                if (span.comment_only)
                {
                    if (s.line < anchor || distance > window)
                    {
                        continue;
                    }
                }
                else if (distance > window)
                {
                    continue;
                }
                NearCandidate candidate;
                candidate.symbol = s;
                candidate.distance = distance;
                const std::string_view name =
                    s.name.empty() ? std::string_view{s.qualified_name}
                                   : std::string_view{s.name};
                candidate.disk_line = declaration_evidence_line(
                    lines,
                    name,
                    s.kind,
                    static_cast<std::size_t>(s.line),
                    /*back=*/2,
                    /*forward=*/6,
                    &candidate.declarator);
                out->push_back(std::move(candidate));
            }
        }

        /** @brief 从候选里取最优（含盘上声明核验）。 */
        [[nodiscard]]
        auto pick_best_near(
            const std::vector<FileSymbol>&       matches,
            const std::string&                   want_file,
            const EditSpan&                      span,
            std::size_t                          window,
            const std::vector<std::string_view>& lines)
            -> std::optional<NearCandidate>
        {
            std::vector<NearCandidate> candidates;
            collect_near_candidates(
                matches,
                want_file,
                span,
                window,
                lines,
                &candidates);
            if (candidates.empty())
            {
                return std::nullopt;
            }
            return *std::min_element(
                candidates.begin(),
                candidates.end(),
                better_near_candidate);
        }

        /** @brief 向上扫描到的声明候选：token 与其所在行。 */
        struct TokenHint
        {
            std::string token;
            std::size_t line = 0;
        };

        /** @brief 行首缩进（空格/制表符个数）。 */
        [[nodiscard]]
        auto indentation_of(std::string_view line) -> std::size_t
        {
            std::size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            {
                ++i;
            }
            return i;
        }

        /**
         * @brief 从编辑点向上找最近的声明行，给出 (token, 行) 提示。
         *
         * @internal 语义编辑常落在函数体内部，此时落点行的 token 是局部变量/
         * 被调函数，不是被编辑符号。向上找到最近的函数/记录/命名空间声明行
         * 再按 token 查库，命中要求符号行紧贴该声明行（±4 行，容纳多行签名）。
         * 缩进门控：函数体内语句的缩进不小于落点行，跳过它们直到缩进变浅的
         * 声明行，避免在函数体里空转。
         */
        [[nodiscard]]
        auto upward_declaration_hints(
            std::string_view file_text,
            std::size_t      anchor) -> std::vector<TokenHint>
        {
            const auto lines = split_lines(file_text);
            if (anchor == 0 || anchor > lines.size())
            {
                return {};
            }
            const std::size_t anchor_indent = indentation_of(lines[anchor - 1]);
            std::vector<TokenHint> hints;
            std::size_t            candidates = 0;
            for (std::size_t idx = anchor - 1; idx > 0 && candidates < 12;
                 --idx)
            {
                std::string_view text = lines[idx - 1];
                if (anchor_indent > 0
                    && indentation_of(lines[idx - 1]) >= anchor_indent)
                {
                    continue;
                }
                text = lstrip_spaces(text);
                if (text.empty() || is_comment_or_pp_line(text)
                    || text.starts_with("}"))
                {
                    continue;
                }
                const bool control =
                    text.starts_with("if ") || text.starts_with("if(")
                    || text.starts_with("for ") || text.starts_with("for(")
                    || text.starts_with("while ") || text.starts_with("while(")
                    || text.starts_with("switch ")
                    || text.starts_with("switch(") || text.starts_with("catch ")
                    || text.starts_with("catch(") || text.starts_with("return ")
                    || text.starts_with("else ");
                const bool declish =
                    text.starts_with("class ") || text.starts_with("struct ")
                    || text.starts_with("enum ") || text.starts_with("union ")
                    || text.starts_with("namespace ")
                    || text.starts_with("template")
                    || (!control && text.find('(') != std::string_view::npos);
                if (!declish)
                {
                    continue;
                }
                ++candidates;
                std::vector<std::string> tokens;
                collect_tokens(text, tokens);
                for (std::string& token : tokens)
                {
                    hints.push_back({std::move(token), idx});
                }
            }
            return hints;
        }

        /** @brief 在整文件符号集里按行号取“编辑点之前最近的”声明。 */
        [[nodiscard]]
        auto pick_preceding_symbol(
            std::vector<FileSymbol> matches,
            const std::string&      want_file,
            std::size_t             anchor,
            bool containers_only) -> std::optional<FileSymbol>
        {
            const std::string         want = path_compare_key(want_file);
            std::optional<FileSymbol> best;
            for (FileSymbol& s : matches)
            {
                if (path_compare_key(s.file) != want || s.line <= 0
                    || s.line > static_cast<std::int64_t>(anchor))
                {
                    continue;
                }
                if (containers_only)
                {
                    const bool container = cg::db::is_callable(s.kind)
                                           || cg::db::is_type_like(s.kind);
                    if (!container)
                    {
                        continue;
                    }
                }
                if (!best.has_value() || s.line > best->line
                    || (s.line == best->line
                        && better_candidate(s, 0, *best, 0)))
                {
                    best = std::move(s);
                }
            }
            return best;
        }

        /** @brief 在整文件符号集里按行号取“编辑点之后最近的”声明。 */
        [[nodiscard]]
        auto pick_following_symbol(
            std::vector<FileSymbol> matches,
            const std::string&      want_file,
            std::size_t             anchor) -> std::optional<FileSymbol>
        {
            const std::string         want = path_compare_key(want_file);
            std::optional<FileSymbol> best;
            for (FileSymbol& s : matches)
            {
                if (path_compare_key(s.file) != want || s.line <= 0
                    || s.line < static_cast<std::int64_t>(anchor))
                {
                    continue;
                }
                if (!best.has_value() || s.line < best->line
                    || (s.line == best->line
                        && better_candidate(s, 0, *best, 0)))
                {
                    best = std::move(s);
                }
            }
            return best;
        }

        /** @brief 收集与选中符号同文件同行的其它声明（宏展开会一行叠多个）。 */
        void collect_same_line(
            const FileSymbol&              chosen,
            const std::vector<FileSymbol>& source,
            std::vector<FileSymbol>*       out)
        {
            if (out == nullptr)
            {
                return;
            }
            out->clear();
            const std::string chosen_file = path_compare_key(chosen.file);
            for (const FileSymbol& s : source)
            {
                if (s.uid != chosen.uid && s.line == chosen.line
                    && path_compare_key(s.file) == chosen_file)
                {
                    out->push_back(s);
                }
            }
            std::ranges::sort(
                *out,
                [](const FileSymbol& a, const FileSymbol& b)
                {
                    const int rank_a = kind_rank(a.kind);
                    const int rank_b = kind_rank(b.kind);
                    return rank_a != rank_b
                               ? rank_a < rank_b
                               : a.qualified_name < b.qualified_name;
                });
        }

        /**
         * @brief 宏展开行（sqlpp 表宏、gtest TEST）会在一行叠出多个符号。
         *        如果恰好一个候选的非限定名以标识符出现在该行文本里，它才是
         *        这行字面声明的符号——优先于 kind_rank 的“外层 record”。
         */
        void prefer_textual_candidate(
            std::optional<FileSymbol>& chosen,
            std::vector<FileSymbol>&   same_line,
            std::string_view           line_text)
        {
            if (!chosen.has_value() || line_text.empty())
            {
                return;
            }
            std::vector<FileSymbol> candidates;
            candidates.push_back(*chosen);
            for (const FileSymbol& s : same_line)
            {
                if (s.uid != chosen->uid && s.line == chosen->line)
                {
                    candidates.push_back(s);
                }
            }
            std::optional<FileSymbol> textual;
            std::size_t               hits = 0;
            for (const FileSymbol& s : candidates)
            {
                const std::string_view name =
                    s.name.empty() ? std::string_view{s.qualified_name}
                                   : s.name;
                if (line_has_identifier(line_text, name))
                {
                    ++hits;
                    if (!textual.has_value())
                    {
                        textual = s;
                    }
                }
            }
            if (hits != 1 || !textual.has_value())
            {
                return;
            }
            std::erase_if(
                same_line,
                [&](const FileSymbol& s) { return s.uid == textual->uid; });
            chosen = std::move(*textual);
        }

        /**
         * @brief 锚行是否以 gtest 测试宏开始（TEST/TEST_F/TEST_P/TYPED_TEST
         * 等）。
         *
         * @internal 宏展开会在同一行合成 constructor/TestBody/destructor/
         * test_info_ 等符号；它们不是用户可读的“同线声明”，且主符号名
         * （`Suite_Name_Test`）不会字面出现在 `TEST(Suite, Name)` 里。
         */
        [[nodiscard]]
        auto is_test_macro_line(std::string_view line) -> bool
        {
            std::string_view text = line;
            while (!text.empty()
                   && std::isspace(static_cast<unsigned char>(text.front())))
            {
                text.remove_prefix(1);
            }
            std::size_t end = 0;
            while (end < text.size()
                   && (std::isalnum(static_cast<unsigned char>(text[end]))
                       || text[end] == '_'))
            {
                ++end;
            }
            const std::string_view word = text.substr(0, end);
            if (word != "TEST" && word != "TEST_F" && word != "TEST_P"
                && word != "TYPED_TEST" && word != "TYPED_TEST_P")
            {
                return false;
            }
            while (end < text.size()
                   && std::isspace(static_cast<unsigned char>(text[end])))
            {
                ++end;
            }
            return end < text.size() && text[end] == '(';
        }

        /**
         * @brief 定位被编辑符号：落点 token → 向上声明提示 → 枚举整文件。
         *
         * `same_line`（可空）回填同文件同行的其它声明：宏展开（gtest TEST、
         * sqlpp 表宏）会在同一行产出 record/method/field 多层符号，仅报一个
         * 会漏掉采样/阅读真正关心的另一个。
         */
        [[nodiscard]]
        auto locate_edited_symbol(
            HookQueryDb&             db,
            const std::string&       file_path,
            const EditInfo&          info,
            std::vector<FileSymbol>* same_line) -> std::optional<FileSymbol>
        {
            const std::string file_base = basename_of(file_path);
            if (file_base.empty())
            {
                return std::nullopt;
            }
            if (!info.span.have_line)
            {
                return std::nullopt;
            }
            std::vector<FileSymbol>  no_same_line;
            std::vector<FileSymbol>& out_same =
                same_line != nullptr ? *same_line : no_same_line;
            out_same.clear();
            const std::optional<std::string> file_text =
                read_text_file(file_path);
            const std::vector<std::string_view> lines =
                file_text.has_value() ? split_lines(*file_text)
                                      : std::vector<std::string_view>{};
            const auto finalize =
                [&](std::optional<FileSymbol> found,
                    std::size_t disk_line) -> std::optional<FileSymbol>
            {
                if (!found.has_value())
                {
                    return found;
                }
                // 盘上核验到的声明行比库内行号可信：更新排队时库内行号是编辑前
                // 的，注释插入还会把声明整体下推。
                const std::size_t line_no =
                    disk_line != 0
                        ? disk_line
                        : (found->line > 0
                               ? static_cast<std::size_t>(found->line)
                               : info.span.line);
                const std::string_view text =
                    line_no > 0 && line_no <= lines.size() ? lines[line_no - 1]
                                                           : std::string_view{};
                prefer_textual_candidate(found, out_same, text);
                const std::string_view chosen_name =
                    found->name.empty()
                        ? std::string_view{found->qualified_name}
                        : found->name;
                // 主符号名已在行文本中坐实时，同线符号多是宏展开噪音
                // （_sqlpp_name_tag、构造/析构、成员结构等），不再展示。
                // gtest 宏生成的主符号名不会字面出现，另按宏行/`_Test`
                // 后缀判定。
                const bool synthetic_test_symbol =
                    chosen_name.ends_with("_Test")
                    && !line_has_identifier(text, chosen_name);
                if (line_has_identifier(text, chosen_name)
                    || is_test_macro_line(text) || synthetic_test_symbol)
                {
                    out_same.clear();
                }
                return found;
            };
            // 语义编辑若从 doc 注释开始（例如整体替换一个带注释的函数），
            // 落点行本身没有代码 token，函数签名在注释块之后。把匹配锚点
            // 前移到注释块后的第一行代码，否则窗口内一个候选都没有，最终
            // 会退回“前一个符号”。
            EditSpan match_span = info.span;
            if (file_text.has_value())
            {
                if (!info.comment_only && match_span.have_line)
                {
                    std::size_t at =
                        match_span.line > 0 ? match_span.line - 1 : 0;
                    std::size_t advanced = at;
                    while (advanced < lines.size() && advanced - at < 16)
                    {
                        const std::string_view probe =
                            lstrip_spaces(lines[advanced]);
                        if (!probe.empty() && !is_comment_or_pp_line(probe))
                        {
                            break;
                        }
                        ++advanced;
                    }
                    if (advanced != at && advanced < lines.size())
                    {
                        match_span.line = advanced + 1;
                    }
                }
                // 纯注释编辑的目标声明就在注释之后：窗口收到 6 行；语义编辑
                // 仍取落点附近 ±3。24 行的旧窗口会让 20 行外的重载构造函数
                // （B#1）压过落点行上的真正声明。
                const std::size_t window = info.comment_only ? 6 : 3;
                std::size_t       tried = 0;
                // 不在第一个命中 token 处返回：同一行可能同时产出类名与成员名
                // （`SampleImpl::update_cache`），也可能同时产出内联
                // 枚举 tag 与字段（`Kind ... kind =`）。把所有 token 的候选汇总
                // 后按“行距 + 盘上声明核验 + 声明角色”全局选优。
                std::vector<NearCandidate> candidates;
                std::vector<FileSymbol>    pool;
                for (const std::string& token :
                     candidate_tokens(*file_text, match_span))
                {
                    if (++tried > 16)
                    {
                        break;
                    }
                    // 绝对截止前的余量不足时不再派生查询子进程：保留已收集的
                    // 候选，让调用方先把注入发出去（宿主 10 秒硬超时优先）。
                    if (hook_deadline_reached(HOOK_QUERY_TIMEOUT))
                    {
                        break;
                    }
                    bool       query_timed_out = false;
                    const auto matches =
                        query_symbols(db, token, file_base, &query_timed_out);
                    for (const FileSymbol& s : matches)
                    {
                        const bool seen = std::ranges::any_of(
                            pool,
                            [&](const FileSymbol& known)
                            { return known.uid == s.uid; });
                        if (!seen)
                        {
                            pool.push_back(s);
                        }
                    }
                    collect_near_candidates(
                        matches,
                        file_path,
                        match_span,
                        window,
                        lines,
                        &candidates);
                    if (query_timed_out)
                    {
                        // 一次查询已用满上限：继续派生只会把 hook 推过宿主
                        // 截止，保留已收集的候选立即收手。
                        break;
                    }
                    // 落点行上已有盘上核验的声明：更远的 token 不可能在
                    // better_near_candidate 的距离排序上赢过它，停止派生查询。
                    if (std::ranges::any_of(
                            candidates,
                            [](const NearCandidate& c)
                            { return c.distance == 0 && c.disk_line != 0; }))
                    {
                        break;
                    }
                }
                if (!candidates.empty())
                {
                    const NearCandidate& best = *std::min_element(
                        candidates.begin(),
                        candidates.end(),
                        better_near_candidate);
                    collect_same_line(best.symbol, pool, &out_same);
                    return finalize(best.symbol, best.disk_line);
                }
                // 语义编辑落在函数体内：向上找签名行，命中须紧贴该行。
                if (!info.comment_only)
                {
                    tried = 0;
                    for (const TokenHint& hint :
                         upward_declaration_hints(*file_text, match_span.line))
                    {
                        if (++tried > 8)
                        {
                            break;
                        }
                        if (hook_deadline_reached(HOOK_QUERY_TIMEOUT))
                        {
                            break;
                        }
                        bool       hint_timed_out = false;
                        const auto matches = query_symbols(
                            db,
                            hint.token,
                            file_base,
                            &hint_timed_out);
                        if (hint_timed_out)
                        {
                            break;
                        }
                        for (const FileSymbol& s : matches)
                        {
                            if (path_compare_key(s.file)
                                    != path_compare_key(file_path)
                                || s.line <= 0)
                            {
                                continue;
                            }
                            const std::size_t distance =
                                s.line >= static_cast<std::int64_t>(hint.line)
                                    ? static_cast<std::size_t>(
                                          s.line
                                          - static_cast<std::int64_t>(
                                              hint.line))
                                    : static_cast<std::size_t>(
                                          static_cast<std::int64_t>(hint.line)
                                          - s.line);
                            if (distance <= 4)
                            {
                                collect_same_line(s, matches, &out_same);
                                const std::string_view name =
                                    s.name.empty()
                                        ? std::string_view{s.qualified_name}
                                        : std::string_view{s.name};
                                const std::size_t disk_line =
                                    declaration_evidence_line(
                                        lines,
                                        name,
                                        s.kind,
                                        static_cast<std::size_t>(s.line),
                                        /*back=*/2,
                                        /*forward=*/6,
                                        nullptr);
                                return finalize(s, disk_line);
                            }
                        }
                    }
                }
            }
            // 最后回退：整文件符号集（昂贵，仅当前两条快路径都没命中）。
            // 空串参数在 Windows 命令行上会被丢弃，故用 `_`（SQL LIKE 的单字符
            // 通配，C++ 侧再按字面 "::" 复核）当“全部符号”针；全限定名含 `::`
            // 的符号也在其中。两轮合并——只查 `_` 会漏掉名字与限定名都不含
            // 下划线的全局符号。
            // 整文件枚举最昂贵（大库上一次可达数秒）：余量不足时不再启动。
            std::vector<FileSymbol> all;
            if (!hook_deadline_reached(HOOK_QUERY_TIMEOUT))
            {
                bool all_timed_out = false;
                all = query_symbols(db, "_", file_base, &all_timed_out);
                if (!all_timed_out
                    && !hook_deadline_reached(HOOK_QUERY_TIMEOUT))
                {
                    bool                    more_timed_out = false;
                    std::vector<FileSymbol> more =
                        query_symbols(db, "::", file_base, &more_timed_out);
                    for (FileSymbol& s : more)
                    {
                        const bool seen = std::ranges::any_of(
                            all,
                            [&](const FileSymbol& known)
                            { return known.uid == s.uid; });
                        if (!seen)
                        {
                            all.push_back(std::move(s));
                        }
                    }
                }
            }
            const auto finish = [&](std::optional<FileSymbol> chosen,
                                    std::size_t               disk_line =
                                        0) -> std::optional<FileSymbol>
            {
                if (chosen.has_value())
                {
                    collect_same_line(*chosen, all, &out_same);
                }
                return finalize(chosen, disk_line);
            };
            if (auto near = pick_best_near(
                    all,
                    file_path,
                    match_span,
                    info.comment_only ? 12 : 6,
                    lines);
                near.has_value())
            {
                return finish(near->symbol, near->disk_line);
            }
            if (info.comment_only)
            {
                if (auto next =
                        pick_following_symbol(all, file_path, match_span.line);
                    next.has_value())
                {
                    return finish(std::move(next));
                }
                return finish(pick_preceding_symbol(
                    all,
                    file_path,
                    match_span.line,
                    /*containers_only=*/false));
            }
            if (auto container = pick_preceding_symbol(
                    all,
                    file_path,
                    match_span.line,
                    /*containers_only=*/true);
                container.has_value())
            {
                return finish(std::move(container));
            }
            return finish(pick_preceding_symbol(
                all,
                file_path,
                match_span.line,
                /*containers_only=*/false));
        }

        /** @brief 符号级影响面的注入所需字段。 */
        struct ImpactInfo
        {
            bool ok = false;
            // 是否真的派生过 impact 查询。false 表示因预算/背景更新未查询，
            // 与"查询了但解析失败"必须区分：前者不能写成"无法精确解析"。
            bool attempted = false;
            // 失败原因：子进程失败 / 目标无法解析（歧义）/ 名字落到别的重载。
            std::string  reason;
            std::int64_t impacted = 0;
            std::int64_t direct = 0;
            std::int64_t transitive = 0;
            std::int64_t fan_in = 0;
            bool         truncated = false;
            std::int64_t family_size = 1;
            // 延迟收敛：impact JSON 顶层的 dispatchStaleFiles（派发数据滞
            // 后文件数；0 = 已收敛）。>0 时虚派发计数是编辑前事实，注入必须
            // 标注（注入路径绝不静默吃旧派发数据）。
            std::int64_t            dispatch_stale = 0;
            std::vector<FileSymbol> direct_symbols;
        };

        /** @brief 按直接消费者计的风险阶梯（与引擎的 score 公式同阈值）。
         * 权重 3 与三档阈值 120/30/8 由单一实验语料的实测分布反推而来，
         * 写死为默认档位。 */
        [[nodiscard]]
        auto direct_risk_name(std::int64_t direct, std::int64_t fan_in)
            -> std::string_view
        {
            if (direct == 0)
            {
                return "无";
            }
            const std::int64_t score = 3 * direct + fan_in;
            if (score > 120)
            {
                return "CRITICAL";
            }
            if (score > 30)
            {
                return "HIGH";
            }
            if (score > 8)
            {
                return "MEDIUM";
            }
            return "LOW";
        }

        /**
         * @brief impact 目标的唯一解析（与 cli.commands resolve_unique_target
         * 的 判据逐字一致）。
         *
         * @internal 去掉的是 CLI 的错误打印与建议——那是旧子进程的 stdout，钩子
         * 从未见过（错误 JSON 无 target 字段，旧解析路径落进“目标无法解析”文
         * 案）。层 1 精确子集恰一即用、多个即歧义；其余层唯一命中才用。
         */
        [[nodiscard]]
        auto resolve_impact_target(
            const cg::db::FindResult& found,
            std::string_view          name) -> std::optional<cg::db::SymbolRow>
        {
            if (found.tier == 0)
            {
                return std::nullopt;
            }
            if (found.tier == 1)
            {
                std::vector<cg::db::SymbolRow> exact;
                for (const cg::db::SymbolRow& m : found.matches)
                {
                    if (m.name == name || m.qualified_name == name
                        || m.unique_key == name)
                    {
                        exact.push_back(m);
                    }
                }
                if (exact.size() > 1)
                {
                    return std::nullopt;
                }
                if (exact.size() == 1)
                {
                    return exact.front();
                }
            }
            if (found.matches.size() > 1)
            {
                return std::nullopt;
            }
            return found.matches.front();
        }

        /**
         * @brief 对定位到的符号算上游影响面（深度 3，只列直接层）——进程内直连。
         *
         * @internal 与旧 `ccg impact --file --kind --direction up --depth 3
         * --json` 子进程的语义逐字对齐：限定名 + 文件过滤 + kind
         * 过滤做唯一目标解析， 目标 uid
         * 必须与查询到的一致（不一致说明名字解析落到了别的重载上，
         * 宁可不报也不报错人）；merge_family=false 对应旧 `--no-merge`——虚方法
         * 的默认视图会把整个覆盖族的调用方并进来，但纯注释编辑问的是本符号
         * 自身的影响面。计数字段与 byDepth 深度 1 消费者的取值同 impact_json 的
         * 写出口径；truncated 恒 false——旧 JSON 的 truncated 是 --top
         * 截断标志， 钩子从不传 --top。查询抛出（忙/损坏）按旧“超时”文案让位。
         */
        [[nodiscard]]
        auto symbol_impact(
            HookQueryDb&              db,
            const FileSymbol&         symbol,
            bool                      merge_family = true,
            std::chrono::milliseconds margin = HOOK_IMPACT_TIMEOUT)
            -> ImpactInfo
        {
            // 计算前的最后一道闸：调用方通常已经算过余量，这里再确认一次，
            // 任何路径都不能在截止之后才动手。attempted=false 让注入如实写
            // "未查询"而不是"无法精确解析"。
            if (hook_deadline_reached(margin))
            {
                return ImpactInfo{};
            }
            const auto gave_up = []
            {
                ImpactInfo failed;
                failed.attempted = true;
                failed.reason = "impact 查询超时";
                return failed;
            };
            if (!db.alive)
            {
                return gave_up();
            }
            try
            {
                cg::db::SymbolFilter filter;
                filter.file_substring = symbol.file;
                filter.kind = parse_kind(symbol.kind);
                const cg::db::FindResult found = cg::db::find_symbols(
                    *db.store,
                    symbol.qualified_name,
                    filter,
                    /*merge_lower_tiers=*/false);
                const auto target =
                    resolve_impact_target(found, symbol.qualified_name);
                if (!target.has_value())
                {
                    ImpactInfo failed;
                    failed.attempted = true;
                    failed.reason = "目标无法解析：同名重载/歧义";
                    return failed;
                }
                if (target->id != symbol.uid)
                {
                    ImpactInfo failed;
                    failed.attempted = true;
                    failed.reason = "名字解析到其它重载：uid 不一致";
                    return failed;
                }
                const cg::graph::ImpactResult result = cg::graph::impact(
                    *db.store,
                    target->id,
                    cg::graph::Direction::Up,
                    /*depth=*/3,
                    merge_family,
                    cg::graph::DispatchMode::Precise);
                ImpactInfo out;
                out.attempted = true;
                out.ok = true;
                out.impacted = result.impacted;
                out.direct = result.direct;
                out.transitive = result.transitive;
                out.fan_in = result.fan_in;
                out.truncated = false;
                out.family_size = 1;
                if (result.family.size() > 1)
                {
                    // 旧 JSON 的 family 数组只收可物化成员；同口径计数。
                    std::int64_t family = 0;
                    for (const cg::db::SymId member : result.family)
                    {
                        if (cg::db::get_symbol(*db.store, member).has_value())
                        {
                            ++family;
                        }
                    }
                    out.family_size = family;
                }
                out.dispatch_stale = result.dispatch_stale_files;
                for (const cg::graph::ImpactLevel& level : result.by_depth)
                {
                    if (level.depth != 1)
                    {
                        continue;
                    }
                    for (const cg::db::SymId id : level.ids)
                    {
                        if (const auto row = cg::db::get_symbol(*db.store, id))
                        {
                            out.direct_symbols.push_back(file_symbol_of(*row));
                        }
                    }
                }
                return out;
            }
            catch (const std::exception&)
            {
                db.alive = false;
                return gave_up();
            }
        }

        /** @brief 直接消费者的相关性分层：同文件 > 项目源码 > 测试 >
         * 框架/生成物。 */
        [[nodiscard]]
        auto consumer_rank(const FileSymbol& s, const std::string& edited_file)
            -> int
        {
            if (path_compare_key(s.file) == path_compare_key(edited_file))
            {
                return 0;
            }
            const std::string key = path_compare_key(s.file);
            if (key.find("gtest") != std::string::npos
                || key.find("googletest") != std::string::npos
                || key.find("3rdparty") != std::string::npos
                || key.find("_autogen") != std::string::npos)
            {
                return 3;
            }
            if (key.find("/tests/") != std::string::npos
                || key.find("test.") != std::string::npos
                || key.find("_test.") != std::string::npos)
            {
                return 2;
            }
            return 1;
        }

        /**
         * @brief 把 gtest 宏展开出的夹具名还原成可读的“套件::用例”。
         *
         * @internal TEST(Suite, Name) 展开为 `Suite_Name_Test::TestBody`。这类
         * 限定名的识别信息全在前缀，`clip_qualified` 保留尾部的策略会把同一
         * 夹具下的所有用例都折叠成 `…::TestBody`，多个消费者因此无法区分。
         */
        [[nodiscard]]
        auto readable_symbol_name(std::string_view name) -> std::string
        {
            static constexpr std::string_view TEST_BODY = "::TestBody";
            if (name.size() <= TEST_BODY.size() || !name.ends_with(TEST_BODY))
            {
                return std::string{name};
            }
            std::string_view stem =
                name.substr(0, name.size() - TEST_BODY.size());
            if (!stem.ends_with("_Test"))
            {
                return std::string{name};
            }
            stem.remove_suffix(5);
            // Suite_TestName；套件名本身以 _Test 结尾，故以最右的 "_Test_"
            // 为界。
            const std::size_t boundary = stem.rfind("_Test_");
            if (boundary != std::string_view::npos
                && boundary + 6 < stem.size())
            {
                return std::format(
                    "{}::{}",
                    stem.substr(0, boundary + 5),
                    stem.substr(boundary + 6));
            }
            // TEST(Suite, Name) 的套件名只有 "Test" 结尾而没有下划线（例如
            // MyGraphE2ETest）：展开名里只会出现 "Test_" 这个分界。分界必须
            // 不在开头，避免把套件名本身为 "Test_*" 的少数情况拆错。
            const std::size_t loose = stem.rfind("Test_");
            if (loose != std::string_view::npos && loose > 0
                && loose + 5 < stem.size())
            {
                return std::format(
                    "{}::{}",
                    stem.substr(0, loose + 4),
                    stem.substr(loose + 5));
            }
            return std::string{stem};
        }

        /**
         * @brief 注入里的符号名裁剪：gtest
         * 展开名保留套件前缀，其它按限定名折叠。
         *
         * @internal `readable_symbol_name` 还原出的 `Suite::Case` 里，套件名与
         * 用例名共同承担识别作用；`clip_qualified` 的"尾部更重要"规则会把
         * 套件整段折掉（`…::Case`），同一套件下的消费者于是又无法区分。
         * 普通限定名继续沿用尾部优先。
         */
        [[nodiscard]]
        auto clip_display_symbol(std::string_view name, std::size_t max)
            -> std::string
        {
            const std::string readable = readable_symbol_name(name);
            if (readable != name)
            {
                // 套件名与用例名都要保留：只留头部会把同一夹具下的用例
                // 折叠成同一个前缀。
                return clip_middle(readable, max);
            }
            return clip_qualified(readable, max);
        }

        /** @brief 注入里的一条消费者：限定名 + 定义位置。 */
        [[nodiscard]]
        auto describe_symbol(const FileSymbol& s) -> std::string
        {
            const std::string_view full =
                s.qualified_name.empty() ? std::string_view{s.name}
                                         : std::string_view{s.qualified_name};
            std::string out = clip_display_symbol(full, 96);
            if (s.file.empty() || s.line <= 0)
            {
                return out;
            }
            out += std::format(
                "（定义于 {}:{}）",
                clip(basename_of(s.file), 56),
                s.line);
            return out;
        }

        /**
         * @brief 就地按相关性排序消费者（注入与摘要两条呈现路径共用）。
         *
         * @internal 键序两级：先 consumer_rank 升序（同文件 > 项目源码 >
         *           测试 > 框架/生成物），平手再按限定名字典序。PostToolUse
         *           注入与摘要积压必须同口径，否则同一批消费者在两条路径
         *           的先后互相矛盾，故收在此单点。
         */
        void sort_consumers(
            std::vector<FileSymbol>& consumers,
            const std::string&       edited_file)
        {
            std::ranges::sort(
                consumers,
                [&](const FileSymbol& a, const FileSymbol& b)
                {
                    const int ra = consumer_rank(a, edited_file);
                    const int rb = consumer_rank(b, edited_file);
                    if (ra != rb)
                    {
                        return ra < rb;
                    }
                    return a.qualified_name < b.qualified_name;
                });
        }

        /** @brief 按相关性排序后把最相关的前 max_shown 个消费者追加到注入行。
         */
        void append_consumers(
            std::string&            line,
            std::vector<FileSymbol> consumers,
            const std::string&      edited_file,
            std::size_t             max_shown,
            std::string_view        prefix = "；最相关: ")
        {
            sort_consumers(consumers, edited_file);
            line += prefix;
            std::size_t              shown = 0;
            std::vector<std::string> seen;
            for (const FileSymbol& consumer : consumers)
            {
                if (std::ranges::find(seen, consumer.qualified_name)
                    != seen.end())
                {
                    continue;
                }
                seen.push_back(consumer.qualified_name);
                if (shown != 0)
                {
                    line += "、";
                }
                line += describe_symbol(consumer);
                if (++shown >= max_shown)
                {
                    break;
                }
            }
        }

        /** @brief pos 之前最后一个由标识符字符组成的 token（跳过空白）。 */
        [[nodiscard]]
        auto token_before(std::string_view line, std::size_t pos)
            -> std::string_view
        {
            std::size_t end = pos;
            while (end > 0
                   && std::isspace(static_cast<unsigned char>(line[end - 1])))
            {
                --end;
            }
            std::size_t begin = end;
            while (begin > 0
                   && (std::isalnum(static_cast<unsigned char>(line[begin - 1]))
                       || line[begin - 1] == '_'))
            {
                --begin;
            }
            return line.substr(begin, end - begin);
        }

        /** @brief 出现在名字前的这些词说明是表达式语境，不是声明。 */
        [[nodiscard]]
        auto is_call_context_keyword(std::string_view word) -> bool
        {
            static constexpr std::string_view KEYWORDS[] = {
                "return",
                "co_return",
                "throw",
                "delete",
                "case",
                "sizeof",
                "decltype",
                "new",
                "else",
                "do",
                "if",
                "while",
                "for",
                "switch",
                "co_await",
                "co_yield",
            };
            for (const std::string_view keyword : KEYWORDS)
            {
                if (word == keyword)
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief name 后接 `(` 时，name 前是否有声明所需的类型上下文。
         *
         * @internal 旧实现要求 name 前必须有标识符 token；`IWidgetBase** set(`
         * 的 token_before 为空，声明行因此被误判成引用，行号修正和排除都失效。
         * 这里把尾随的 `*`/`&`/`>`（模板实参）并入类型，并排除 return、赋值、
         * 成员访问等调用语境。
         */
        [[nodiscard]]
        auto callable_declaration_before(
            std::string_view line,
            std::size_t      pos,
            std::string_view kind) -> bool
        {
            std::size_t q = pos;
            while (q > 0
                   && std::isspace(static_cast<unsigned char>(line[q - 1])))
            {
                --q;
            }
            while (q > 0)
            {
                const char c = line[q - 1];
                if (c == '>' && q >= 2 && line[q - 2] == '-')
                {
                    return false; // ptr->name(：成员调用
                }
                if (c == '*' || c == '&' || c == '>')
                {
                    --q;
                }
                else if (c == ':' && q >= 2 && line[q - 2] == ':')
                {
                    // 连同限定符一起吃掉：`void Foo::bar(` 是声明，而
                    // 行首的 `std::move(` 是调用；只看 `::` 会混淆两者。
                    q -= 2;
                    while (q > 0
                           && (std::isalnum(
                                   static_cast<unsigned char>(line[q - 1]))
                               || line[q - 1] == '_'))
                    {
                        --q;
                    }
                }
                else
                {
                    break;
                }
                while (q > 0
                       && std::isspace(static_cast<unsigned char>(line[q - 1])))
                {
                    --q;
                }
            }
            const std::string_view before = token_before(line, q);
            if (is_call_context_keyword(before))
            {
                return false;
            }
            if (q > 0)
            {
                const char c = line[q - 1];
                if (c == '.' || c == '=' || c == '(' || c == ',' || c == '['
                    || c == '+' || c == '-' || c == '/' || c == '%' || c == '<'
                    || c == '!' || c == '?' || c == ';' || c == '{' || c == '}')
                {
                    return false;
                }
                return true;
            }
            // 行首的构造/析构声明：`Foo(` / `~Foo(`。
            return kind == "constructor" || kind == "destructor";
        }

        /** @brief 一次同名出现到底是读取、写入、调用、取址还是声明。 */
        enum class ReferenceNature : std::uint8_t
        {
            Read,
            Write,
            Call,
            Address,
            Other,
            Declaration,
        };

        /** @brief 一次同名出现的性质，以及它是否为 `.name`/`->name` 形态。 */
        struct ReferenceFacts
        {
            ReferenceNature nature = ReferenceNature::Other;
            bool            member_access = false;
            bool            found = false;
        };

        /**
         * @brief 可调用符号的 kind（用于区分声明与调用）。
         *
         * @internal 已收编至 db 层 kind 谓词族（cg::db::is_callable，kind 身份
         * 的单一判定点）；本名是钩子的既有调用面与镜像测试靶点，转发保持
         * 调用点不动。
         */
        [[nodiscard]]
        auto is_callable_kind(std::string_view kind) -> bool
        {
            return cg::db::is_callable(kind);
        }

        /** @brief 标识符字符判定。 */
        [[nodiscard]]
        auto is_ident_char(char c) -> bool
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        }

        /**
         * @brief 判断掩码行里 name 这次出现的性质。
         *
         * @internal 只看名字前一个字符会把 `out.found = true;` 标成“读取”、把
         * `p->ToString(...)` 标成“读取/取址”，读者会被错误标签带偏。这里按
         * 后缀区分：`=`（非 `==`）与复合赋值/自增自减是写入，`(` 是调用，
         * 成员访问链前有 `&`（或 `std::addressof`）是取址。声明必须排在写入
         * 之前判断，否则 `T field = ...;` 会被当成写字段。
         *
         * `masked` 必须是 `mask_literals` 的输出：位置与原文一致，且字面量里
         * 的假命中已被抹掉。
         */
        [[nodiscard]]
        auto reference_facts_masked(
            std::string_view masked,
            std::string_view name,
            std::string_view symbol_kind) -> ReferenceFacts
        {
            ReferenceFacts facts;
            const auto     found = identifier_position_masked(masked, name);
            if (!found.has_value())
            {
                facts.nature = ReferenceNature::Declaration;
                return facts;
            }
            facts.found = true;
            const std::size_t pos = *found;
            const auto        skip_space_forward = [&](std::size_t at)
            {
                while (at < masked.size()
                       && std::isspace(static_cast<unsigned char>(masked[at])))
                {
                    ++at;
                }
                return at;
            };
            std::size_t before = pos;
            while (
                before > 0
                && std::isspace(static_cast<unsigned char>(masked[before - 1])))
            {
                --before;
            }
            // `.name` / `->name` 才是成员访问；裸标识符可能是同名局部变量。
            if (before > 0 && masked[before - 1] == '.')
            {
                facts.member_access = true;
            }
            else if (
                before >= 2 && masked[before - 1] == '>'
                && masked[before - 2] == '-')
            {
                facts.member_access = true;
            }
            std::size_t after = skip_space_forward(pos + name.size());
            // `field[key] += 1` 的下标不改变语句性质：跳过配平的 `[...]` 再看
            // 真正的后缀（否则 B#5 的 map 写入会被标成“读取”）。
            if (after < masked.size() && masked[after] == '[')
            {
                int         bracket = 0;
                std::size_t scan = after;
                bool        closed = false;
                for (; scan < masked.size(); ++scan)
                {
                    if (masked[scan] == '[')
                    {
                        ++bracket;
                    }
                    else if (masked[scan] == ']')
                    {
                        if (--bracket == 0)
                        {
                            ++scan;
                            closed = true;
                            break;
                        }
                    }
                }
                if (closed)
                {
                    after = skip_space_forward(scan);
                }
            }
            const char suffix = after < masked.size() ? masked[after] : '\0';
            const char suffix2 =
                after + 1 < masked.size() ? masked[after + 1] : '\0';
            const bool assign = suffix == '=' && suffix2 != '=';
            const bool compound =
                (suffix == '+' || suffix == '-' || suffix == '*'
                 || suffix == '/' || suffix == '%' || suffix == '&'
                 || suffix == '|' || suffix == '^')
                && suffix2 == '=';
            const bool inc_dec = (suffix == '+' && suffix2 == '+')
                                 || (suffix == '-' && suffix2 == '-');
            const bool call = suffix == '(';
            // 成员链起点：`++counters_->session_calls` 的 `++` 并不紧贴名字，
            // 必须跳过 `.`/`->`/`::` 与接收者标识符后再判断。
            const auto chain_begin = [&](std::size_t at)
            {
                std::size_t chain = at;
                while (true)
                {
                    while (chain > 0
                           && std::isspace(
                               static_cast<unsigned char>(masked[chain - 1])))
                    {
                        --chain;
                    }
                    if (chain > 0 && masked[chain - 1] == '.')
                    {
                        --chain;
                    }
                    else if (
                        chain >= 2 && masked[chain - 1] == '>'
                        && masked[chain - 2] == '-')
                    {
                        chain -= 2;
                    }
                    else if (
                        chain >= 2 && masked[chain - 1] == ':'
                        && masked[chain - 2] == ':')
                    {
                        chain -= 2;
                    }
                    else
                    {
                        break;
                    }
                    while (chain > 0
                           && std::isspace(
                               static_cast<unsigned char>(masked[chain - 1])))
                    {
                        --chain;
                    }
                    while (chain > 0 && is_ident_char(masked[chain - 1]))
                    {
                        --chain;
                    }
                }
                while (chain > 0
                       && std::isspace(
                           static_cast<unsigned char>(masked[chain - 1])))
                {
                    --chain;
                }
                return chain;
            };
            const std::size_t chain = chain_begin(pos);
            const bool        prefix_inc_dec =
                chain >= 2
                && ((masked[chain - 1] == '+' && masked[chain - 2] == '+')
                    || (masked[chain - 1] == '-' && masked[chain - 2] == '-'));
            // 前缀为表达式语境时，后缀 `=`/`;` 不可能是声明（`x = 1;`）。
            bool expression_prefix = facts.member_access || prefix_inc_dec;
            if (before > 0)
            {
                const char c = masked[before - 1];
                if (c == '&' || c == '=' || c == '(' || c == ',' || c == '['
                    || c == '+' || c == '-' || c == '/' || c == '%' || c == '<'
                    || c == '!' || c == '?')
                {
                    expression_prefix = true;
                }
            }
            if (is_call_context_keyword(token_before(masked, pos)))
            {
                expression_prefix = true;
            }
            // 行首/语句起始处的 `x = 1;` 是赋值，不是声明；`T x = 1;`
            // 才是声明。
            const bool statement_start =
                before == 0 || masked[before - 1] == ';'
                || masked[before - 1] == '{' || masked[before - 1] == '}';
            const bool declaration_suffix = (assign || suffix == ';')
                                            && !expression_prefix
                                            && !statement_start;
            if (call)
            {
                if (is_callable_kind(symbol_kind)
                    && callable_declaration_before(masked, pos, symbol_kind))
                {
                    facts.nature = ReferenceNature::Declaration;
                }
                else
                {
                    facts.nature = ReferenceNature::Call;
                }
                return facts;
            }
            if (declaration_suffix)
            {
                facts.nature = ReferenceNature::Declaration;
                return facts;
            }
            if (assign || compound || inc_dec || prefix_inc_dec)
            {
                facts.nature = ReferenceNature::Write;
                return facts;
            }
            // 取址：`&x`，或 `&obj.x` / `std::addressof(obj.x)` 这类成员链。
            const bool ampersand = chain > 0 && masked[chain - 1] == '&'
                                   && !(chain >= 2 && masked[chain - 2] == '&');
            const std::size_t window_start = pos > 48 ? pos - 48 : 0;
            const bool        addressof =
                masked.substr(window_start, pos - window_start)
                    .find("addressof")
                != std::string_view::npos;
            if (ampersand || addressof)
            {
                facts.nature = ReferenceNature::Address;
                return facts;
            }
            facts.nature = ReferenceNature::Read;
            return facts;
        }

        /**
         * @brief 在 near 附近找 name 的声明行（编辑后盘上行号）；找不到返回 0。
         *
         * @internal 更新排队时库里的行号来自本次编辑之前：插入/删除注释会把
         * 声明行整体挪动，直接用旧行号排除会漏掉符号自身的声明。按名字出现
         * 且形似声明（字段/变量）或带返回类型后接 `(`（可调用符号）来定位，
         * 取距旧行号最近的一处。
         */
        [[nodiscard]]
        auto declaration_line_near(
            std::string_view text,
            std::string_view name,
            std::string_view kind,
            std::size_t      near,
            std::size_t      window) -> std::size_t
        {
            if (name.empty() || near == 0)
            {
                return 0;
            }
            const bool        callable = cg::db::is_callable(kind);
            const bool        type_like = cg::db::is_type_like(kind);
            const auto        lines = split_lines(text);
            const std::string masked_text = mask_literals(text);
            const auto        masked_lines = split_lines(masked_text);
            const std::size_t lo = near > window ? near - window : 1;
            const std::size_t hi = std::min(lines.size(), near + window);
            std::size_t       best = 0;
            std::size_t       best_distance = window + 1;
            // gtest 宏展开的符号名（`Suite_Case_Test`）不会字面出现在
            // `TEST(Suite, Case)` 行上；磁盘声明行只能按测试宏识别。
            const bool test_symbol_name =
                name.ends_with("_Test")
                || name.find("_Test::") != std::string_view::npos;
            std::size_t best_test_macro = 0;
            std::size_t best_test_distance = window + 1;
            for (std::size_t line_no = lo; line_no <= hi; ++line_no)
            {
                const std::string_view line = lines[line_no - 1];
                if (is_comment_or_pp_line(line))
                {
                    continue;
                }
                if (test_symbol_name && is_test_macro_line(line))
                {
                    const std::size_t distance =
                        line_no > near ? line_no - near : near - line_no;
                    if (distance < best_test_distance)
                    {
                        best_test_macro = line_no;
                        best_test_distance = distance;
                    }
                }
                const std::string_view masked_line = masked_lines[line_no - 1];
                const auto pos = identifier_position_masked(masked_line, name);
                if (!pos.has_value())
                {
                    continue;
                }
                bool declaration =
                    reference_facts_masked(masked_line, name, kind).nature
                    == ReferenceNature::Declaration;
                if (!declaration && callable)
                {
                    std::size_t after = *pos + name.size();
                    while (after < line.size()
                           && std::isspace(
                               static_cast<unsigned char>(line[after])))
                    {
                        ++after;
                    }
                    declaration =
                        after < line.size() && line[after] == '('
                        && callable_declaration_before(line, *pos, kind);
                }
                if (!declaration && type_like)
                {
                    std::size_t after = *pos + name.size();
                    while (after < line.size()
                           && std::isspace(
                               static_cast<unsigned char>(line[after])))
                    {
                        ++after;
                    }
                    // `using Str = ...` 的别名声明在名字后是 `=`；不认它会漏掉
                    // 磁盘声明行，符号自身随后被扫成“形似声明”引用。
                    declaration = after >= line.size() || line[after] == '{'
                                  || line[after] == ':' || line[after] == ';'
                                  || line[after] == '=';
                }
                if (!declaration && !callable && !type_like)
                {
                    // `enum/struct ... { ... } name = ...;` 的字段声明名在 `}`
                    // 之后；reference_facts 会把它判成“语句起始的写入”。
                    std::size_t before = *pos;
                    while (before > 0
                           && std::isspace(
                               static_cast<unsigned char>(line[before - 1])))
                    {
                        --before;
                    }
                    declaration = before > 0 && line[before - 1] == '}';
                }
                if (!declaration)
                {
                    continue;
                }
                const std::size_t distance =
                    line_no > near ? line_no - near : near - line_no;
                if (distance < best_distance)
                {
                    best = line_no;
                    best_distance = distance;
                }
            }
            return best != 0 ? best : best_test_macro;
        }

        /** @brief 同文件文本引用的一处命中：行号、性质与去缩进的源码片段。 */
        struct TextReference
        {
            std::size_t     line = 0;
            ReferenceNature nature = ReferenceNature::Other;
            std::string     snippet;
        };

        /** @brief 引用性质的展示顺序：写入/读取最具体，声明只做兜底。 */
        [[nodiscard]]
        auto reference_order(ReferenceNature nature) -> int
        {
            switch (nature)
            {
            case ReferenceNature::Write:
                return 0;
            case ReferenceNature::Read:
                return 1;
            case ReferenceNature::Call:
                return 2;
            case ReferenceNature::Address:
                return 3;
            case ReferenceNature::Other:
                return 4;
            default:
                return 5;
            }
        }

        /**
         * @brief 以命中位置为中心截取源码片段（前后各留少量上下文）。
         *
         * @internal 从行首硬裁会把行尾的命中整个切掉，读者看到的是一段不含
         * 目标名字的代码；`jobs` 的片段曾因此看起来像误报。
         */
        [[nodiscard]]
        auto clip_around(
            std::string_view line,
            std::size_t      pos,
            std::size_t      max) -> std::string
        {
            std::size_t lead = 0;
            while (lead < line.size()
                   && std::isspace(static_cast<unsigned char>(line[lead])))
            {
                ++lead;
            }
            const std::string_view trimmed = line.substr(lead);
            pos = pos >= lead ? pos - lead : 0;
            if (trimmed.size() <= max)
            {
                return std::string{trimmed};
            }
            const std::size_t before = max > 8 ? max / 4 : 0;
            std::size_t       begin = pos > before ? pos - before : 0;
            while (begin > 0 && begin < trimmed.size()
                   && cg::util::utf8_is_continuation(trimmed[begin]))
            {
                ++begin;
            }
            std::size_t end = begin + max;
            if (end >= trimmed.size())
            {
                end = trimmed.size();
            }
            else
            {
                while (end > begin
                       && cg::util::utf8_is_continuation(trimmed[end]))
                {
                    --end;
                }
            }
            std::string out;
            if (begin > 0)
            {
                out += "…";
            }
            out += std::string{trimmed.substr(begin, end - begin)};
            if (end < trimmed.size())
            {
                out += "…";
            }
            return out;
        }

        /**
         * @brief 声明区间末行（1 基）：多行 enum/record/初始化器都算声明本身。
         *
         * @internal 只排除声明起始行会把 `Kind::None;` 这样的续行当成“读取”
         * （A#2 的噪音）。按花括号/圆括号深度扫到语句结束：带函数体或聚合体
         * 的声明在配平 `}` 处结束，字段/别名在深度 0 的 `;` 处结束。
         */
        [[nodiscard]]
        auto declaration_range_end(
            const std::vector<std::string_view>& lines,
            std::size_t                          start,
            std::string_view                     symbol_kind) -> std::size_t
        {
            if (start == 0 || start > lines.size())
            {
                return start;
            }
            // 带体声明（可调用 ∪ record/enum——type_alias 不算，别名无体）：
            // 集合经 kind 谓词族组合，原集合逐点保持。
            const bool        body_like = cg::db::is_callable(symbol_kind)
                                          || symbol_kind == "record"
                                          || symbol_kind == "enum";
            int               brace = 0;
            int               paren = 0;
            bool              opened = false;
            const std::size_t limit = std::min(lines.size(), start + 63);
            for (std::size_t line_no = start; line_no <= limit; ++line_no)
            {
                const std::string masked = mask_literals(lines[line_no - 1]);
                for (const char c : masked)
                {
                    if (c == '{')
                    {
                        ++brace;
                        opened = true;
                    }
                    else if (c == '}')
                    {
                        if (brace > 0)
                        {
                            --brace;
                        }
                    }
                    else if (c == '(')
                    {
                        ++paren;
                    }
                    else if (c == ')')
                    {
                        if (paren > 0)
                        {
                            --paren;
                        }
                    }
                }
                if (body_like && opened && brace == 0)
                {
                    return line_no;
                }
                if (!body_like && brace == 0 && paren == 0
                    && masked.find(';') != std::string::npos)
                {
                    return line_no;
                }
            }
            return start;
        }

        /**
         * @brief 源文件里 name 以标识符出现的行（1 基，排除声明区间）。
         *
         * @internal 字段/变量的读取、函数取址都不产生图边；当 impact 报 0 时，
         * 同文件文本引用是最便宜、最不易误导的补充——它只陈述“名字在这里
         * 出现过”，不假装那是调用边。字面量先被 `mask_literals` 抹掉，所以
         * `R"(...)"` 这类原始字符串前缀不会再被当成同名别名；每条命中带上
         * 读取/写入/调用/取址/声明的性质，并按具体程度排序后截断。
         *
         * 字段（`kind == "field"`）开启 `member_access_only`：只有 `.name` /
         * `->name` 才算字段访问。同文件里裸 `name` 与同名局部变量、同名其它
         * 字段无法从文本区分，保留它们会把局部变量的操作误报成字段读取。
         *
         * `exclude_end` 是声明区间末行（含）；传入 0 时只排除 `exclude_line`。
         */
        [[nodiscard]]
        auto same_file_reference_lines(
            std::string_view text,
            std::string_view name,
            std::size_t      exclude_line,
            std::size_t      exclude_end,
            std::size_t      limit,
            std::string_view symbol_kind,
            bool             drop_declaration_like,
            bool             member_access_only) -> std::vector<TextReference>
        {
            std::vector<TextReference> hits;
            if (name.empty())
            {
                return {};
            }
            const auto        lines = split_lines(text);
            const std::string masked_text = mask_literals(text);
            const auto        masked_lines = split_lines(masked_text);
            const std::size_t range_end =
                exclude_end > exclude_line ? exclude_end : exclude_line;
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                const std::size_t line_no = i + 1;
                if (exclude_line != 0 && line_no >= exclude_line
                    && line_no <= range_end)
                {
                    continue;
                }
                const std::string_view trimmed = lstrip_spaces(lines[i]);
                if (is_comment_or_pp_line(trimmed))
                {
                    continue;
                }
                const ReferenceFacts facts =
                    reference_facts_masked(masked_lines[i], name, symbol_kind);
                if (!facts.found)
                {
                    continue;
                }
                // 纯注释编辑时磁盘声明行才是排除位；行号一旦漂移，符号
                // 自己的声明就会被扫成“形似声明”，对读者纯属噪音。
                if (drop_declaration_like
                    && facts.nature == ReferenceNature::Declaration)
                {
                    continue;
                }
                if (member_access_only && !facts.member_access)
                {
                    continue;
                }
                const auto pos =
                    identifier_position_masked(masked_lines[i], name);
                hits.push_back(
                    TextReference{
                        line_no,
                        facts.nature,
                        clip_around(trimmed, pos.value_or(0), 48)});
            }
            std::stable_sort(
                hits.begin(),
                hits.end(),
                [](const TextReference& a, const TextReference& b)
                {
                    return reference_order(a.nature)
                           < reference_order(b.nature);
                });
            if (hits.size() > limit)
            {
                hits.resize(limit);
            }
            return hits;
        }

        /** @brief 同文件引用命中的可读类别。 */
        [[nodiscard]]
        auto reference_nature_label(ReferenceNature nature) -> std::string_view
        {
            switch (nature)
            {
            case ReferenceNature::Write:
                return "写入";
            case ReferenceNature::Read:
                return "读取";
            case ReferenceNature::Call:
                return "调用";
            case ReferenceNature::Address:
                return "取址";
            case ReferenceNature::Declaration:
                return "形似声明";
            default:
                return "引用";
            }
        }

        /**
         * @brief 字段/变量所属类型的影响面（读取方的可用代理）。
         *
         * @internal 图不建模字段读取，但“谁在用这个类型”能覆盖大量跨文件
         * 读取场景（例如 IndexSummary::symbols 的读取者都在 cli.render）。
         */
        [[nodiscard]]
        auto parent_record_impact(
            HookQueryDb&       db,
            const FileSymbol&  symbol,
            const std::string& file_filter) -> std::optional<ImpactInfo>
        {
            if (symbol.parent.empty() || symbol.line <= 0)
            {
                return std::nullopt;
            }
            // 查询与随后的 impact 是两步：查询让位就不要再用剩余时间启动
            // impact，否则一次慢查询会把两步都推过截止。
            bool       query_timed_out = false;
            const auto matches = query_symbols(
                db,
                symbol.parent,
                basename_of(file_filter),
                &query_timed_out);
            if (query_timed_out || hook_deadline_reached(HOOK_IMPACT_TIMEOUT))
            {
                return std::nullopt;
            }
            std::optional<FileSymbol> owner;
            for (const FileSymbol& s : matches)
            {
                if ((s.kind != "record" && s.kind != "enum"
                     && s.kind != "type_alias")
                    || s.name != symbol.parent || s.line <= 0
                    || s.line > symbol.line)
                {
                    continue;
                }
                if (!owner.has_value() || s.line > owner->line)
                {
                    owner = s;
                }
            }
            if (!owner.has_value())
            {
                return std::nullopt;
            }
            return symbol_impact(db, *owner);
        }

        /** @brief `ccg changes` 的进程内直连结果（文件级回退与 digest
         * 的数据源）。 */
        struct HookChanges
        {
            /** @brief 喂入文件在库内（unknown 之外的状态；deleted/present
             * 同计）。 */
            bool any_known = false;
            /** @brief 首个已知文件的库内规范路径（未被索引时为空）。 */
            std::string  canonical;
            std::int64_t direct = 0;
            std::int64_t transitive = 0;
            std::int64_t fan_in = 0;
            /** @brief
             * 渲染层同款风险阶梯名（LOW/MEDIUM/HIGH/CRITICAL/UNKNOWN）。 */
            std::string risk;
            /** @brief 一个消费者：行物化失败时 qualified_name 为空（调用方跳过
             *         展示但保留计数——旧 JSON 的同类行没有 qualified_name
             * 字段）。 */
            struct Consumer
            {
                std::string  qualified_name;
                std::string  file;
                std::int64_t line = 0;
            };
            std::vector<Consumer> consumers; // 与 changes 的消费者序一致
        };

        /**
         * @brief 把喂入路径解析到 files 行（与 cli.commands resolve_input_path
         * 的 判据逐字一致）。
         *
         * @internal 钩子模块不能反向 import
         * commands（分层无环），此处是那份实现 的镜像：精确文本 ->
         * 大小写不敏感全路径（唯一命中）-> 唯一的 '/' 边界
         * 后缀命中；歧义或未匹配返回空。
         */
        [[nodiscard]]
        auto resolve_hook_input_file(
            cg::db::GraphStore& store,
            std::string_view    normalized) -> std::optional<cg::db::FileRow>
        {
            if (auto row = store.find_file(normalized))
            {
                return row;
            }
            const std::string want = cg::util::path_compare_key(normalized);
            const std::vector<cg::db::FileRow> files = cg::db::all_files(store);

            std::optional<cg::db::FileRow> hit;
            bool                           ambiguous = false;
            for (const cg::db::FileRow& f : files)
            {
                if (cg::util::path_compare_key(f.path) == want)
                {
                    if (hit.has_value())
                    {
                        ambiguous = true;
                    }
                    else
                    {
                        hit = f;
                    }
                }
            }
            if (hit.has_value() && !ambiguous)
            {
                return hit;
            }

            std::vector<cg::db::FileRow> matches;
            const auto add_unique = [&](const cg::db::FileRow& f)
            {
                if (std::ranges::find(matches, f.id, &cg::db::FileRow::id)
                    == matches.end())
                {
                    matches.push_back(f);
                }
            };
            for (std::size_t i = want.find('/'); i != std::string::npos;
                 i = want.find('/', i + 1))
            {
                const std::string suffix = want.substr(i + 1);
                if (suffix.empty())
                {
                    continue;
                }
                for (const cg::db::FileRow& f : files)
                {
                    const std::string have = cg::util::path_compare_key(f.path);
                    if (have == suffix
                        || (have.size() > suffix.size()
                            && have.ends_with(suffix)
                            && have[have.size() - suffix.size() - 1] == '/'))
                    {
                        add_unique(f);
                    }
                }
            }
            if (matches.size() == 1)
            {
                return matches.front();
            }
            return std::nullopt;
        }

        /** @brief 与 `normalize_update_files` 同规则的路径去重键。 */
        [[nodiscard]]
        inline auto normalize_hook_path(const std::string& file) -> std::string
        {
            std::error_code ec;
            fs::path        abs = fs::absolute(fs::path{file}, ec);
            if (ec)
            {
                abs = fs::path{file};
            }
            return cg::util::norm_path(abs.lexically_normal().generic_string());
        }

        /**
         * @brief `ccg changes <file> --depth 1` 的进程内直连。
         *
         * @internal 复刻 run_changes 的单文件路径：规范化 -> 喂入路径解析 ->
         * 文件全部符号作种子 -> cg::graph::changes（深度 1、派发默认
         * Precise）。 旧子进程还带 deleted/present 状态（fs::exists
         * 探测）——两个消费方都 只区分 unknown
         * 与否，该探测对注入输出不可观测，直连版不再做。
         * 会话失效或查询抛出返回空（对应旧“changes 查询失败/超时”分支）。
         */
        [[nodiscard]]
        auto hook_changes(HookQueryDb& db, const std::string& file)
            -> std::optional<HookChanges>
        {
            if (!db.alive)
            {
                return std::nullopt;
            }
            try
            {
                const std::string          norm = normalize_hook_path(file);
                HookChanges                out;
                std::vector<cg::db::SymId> seeds;
                if (const auto row = resolve_hook_input_file(*db.store, norm))
                {
                    out.any_known = true;
                    out.canonical = row->path;
                    for (const cg::db::SymbolRow& s :
                         cg::db::symbols_of_file(*db.store, row->id))
                    {
                        seeds.push_back(s.id);
                    }
                }
                if (!out.any_known)
                {
                    return out; // 未被索引：调用方的“本文件未被索引”分支
                }
                const cg::graph::ChangesResult result = cg::graph::changes(
                    *db.store,
                    seeds,
                    /*depth=*/1,
                    cg::graph::DispatchMode::Precise);
                out.direct = result.direct;
                out.transitive = result.transitive;
                out.fan_in = result.fan_in;
                out.risk = std::string{render::risk_name(result.risk)};
                out.consumers.reserve(result.consumers.size());
                for (const cg::graph::ChangesEntry& e : result.consumers)
                {
                    HookChanges::Consumer consumer;
                    if (const auto sym = cg::db::get_symbol(*db.store, e.id))
                    {
                        consumer.qualified_name = sym->qualified_name;
                        consumer.file = sym->file;
                        consumer.line = sym->line;
                    }
                    out.consumers.push_back(std::move(consumer));
                }
                return out;
            }
            catch (const std::exception&)
            {
                db.alive = false;
                return std::nullopt;
            }
        }

        /**
         * @brief 处理器共用的库定位：安静走布局发现（CCG_DB > -o/.ccg 发现；
         *        单配置自动选中），失败静默——钩子绝不可使它观察的工具调用失败。
         *
         * @internal 子进程参数经解析出的 `<文件夹>`/`<配置>` 显式转发（裁量 d：
         * posttooluse 的 update 定位与 SessionStart 注入同贯发现规则）。
         */
        [[nodiscard]]
        auto hook_resolved_db(const Invocation& inv)
            -> std::optional<LayoutChoice>
        {
            const auto choice = layout::try_resolve_query_layout(inv);
            if (!choice.has_value())
            {
                return std::nullopt;
            }
            std::error_code ec;
            if (!fs::is_regular_file(fs::path{choice->db}, ec))
            {
                return std::nullopt;
            }
            return choice;
        }

        /**
         * @brief `ccg update` 子进程连孵化都失败时的兜底：直接写
         * pending_updates。
         *
         * @internal
         * 一旦派生失败，`ccg update` 便无法替我们入队；直接在库的协调队列里
         * 插一行，让下一次任何更新者（后续 hook 事件、Stop 钩子的 --drain）
         * 收敛它。路径规范化与 run_update 完全一致（绝对化 + 词典序规范化）。
         * 所有失败静默：这是错误路径上的尽力而为，钩子绝不可让工具调用失败。
         *
         * @return 已写入（或该路径本来就在队列中）时为 true。
         */
        [[nodiscard]]
        auto enqueue_pending_direct(
            const Invocation&  inv,
            const std::string& file) -> bool
        {
            const auto choice = hook_resolved_db(inv);
            if (!choice.has_value())
            {
                return false;
            }
            try
            {
                cg::db::GraphStore store{choice->db};
                store.set_busy_timeout_ms(5000);
                const std::string norm = normalize_hook_path(file);
                store.enqueue_pending(std::vector<std::string>{norm});
                return true;
            }
            catch (const std::exception&)
            {
                return false;
            }
        }

        /**
         * @brief gtest 宏展开出的测试符号（用例夹具 record / TestBody）。
         *
         * @internal `TEST(Suite, Name)` 在图上表现为 `Suite_Name_Test` 记录，
         * 其名字一定以 `_Test` 结尾；这类符号图内没有外部消费者是构造使然，
         * 注入应把它作为预期事实陈述，而不是套用通用“无消费者”免责声明。
         */
        [[nodiscard]]
        auto looks_like_test_symbol(const FileSymbol& symbol) -> bool
        {
            const std::string_view name =
                symbol.name.empty() ? std::string_view{symbol.qualified_name}
                                    : std::string_view{symbol.name};
            return name.ends_with("_Test") || name.ends_with("_Test::TestBody")
                   || name.find("_Test::") != std::string_view::npos;
        }

        // ------------------------------------------------------- 摘要积压 --
        // 批次期间的编辑结论不走"边等边算"：快速路径只把（路径 + 编辑落点）写进
        // 每个编辑一个的 journal 文件（唯一名 +
        // 原子替换，写者之间零共享改写）， 消费者在"无活租约 + pending_updates
        // 为空"之后认领、基于最新库重算
        // 影响面，并一次性汇报。认领是改名，恰好一个消费者能成功；发送前已
        // 认领，故任何崩溃都只会丢一轮结论，绝不重复注入（需求 3）。不改 db
        // schema：全部状态在 <ccg.db 所在目录>/hook-digest/ 下的文件里。

        /** @brief 一次快路径编辑在 journal 里的一行。 */
        struct DigestEntry
        {
            std::string  path; // 展示用路径（工具载荷原样）
            std::string  norm; // 去重键：绝对 + 词典序规范化 + norm_path
            EditSpan     span; // 从 tool_input/tool_response 纯 JSON 解析
            bool         need_drain = false; // pending_updates 的 INSERT 没成功
            std::int64_t ts = 0; // system_clock 毫秒，仅用于排序/老化
        };

        /** @brief 一个文件的最终结论（渲染与预算的输入）。 */
        struct DigestRow
        {
            std::string              path_display; // 相对路径或原样路径
            std::string              display; // 最终展示名（basename 或尾裁）
            std::string              norm;
            std::string              symbol; // 限定名（已裁剪）
            std::string              kind;
            std::int64_t             uid = 0;
            std::int64_t             direct = 0, transitive = 0, fan_in = 0;
            std::string              risk;
            std::vector<std::string> consumers; // 最多 2 个 "file:line name"
            bool                     tracked = true;
            std::string              note; // "影响面查询超时" 等
            /** @brief 派发分析滞后文件数（>0 时结论行的派发计数是旧数据）。 */
            std::int64_t dispatch_stale = 0;
        };

        /** @brief 一次 flush 的产物：文本 + 待删除的 claimed 文件。 */
        struct DigestFlushResult
        {
            std::string           text;
            std::vector<fs::path> claimed;
        };

        enum class FlushMode
        {
            Poll,
            Opportunistic
        };

        /** @brief `<ccg.db 的父目录>/hook-digest`：与库同卷、同配置，天然隔离。
         */
        [[nodiscard]]
        inline auto digest_dir_for(const std::string& db) -> fs::path
        {
            return fs::path{db}.parent_path() / "hook-digest";
        }

        /**
         * @brief 字节上限的 UTF-8 序列安全前缀（不切断多字节序列中段）。
         *
         * @internal 与 utf8_prefix（码点数口径）互补：按字节预算判定的截
         *           断（注入硬顶与微小上限分支）必须把截点落在序列边界
         *           上，尾部残缺序列停在其起点；非法引导字节按 1 字节消
         *           费（utf8_seq_len 口径）。
         */
        [[nodiscard]]
        inline auto utf8_trunc_bytes(
            std::string_view text,
            std::size_t      max_bytes) -> std::string_view
        {
            if (text.size() <= max_bytes)
            {
                return text;
            }
            std::size_t keep = 0;
            std::size_t i = 0;
            while (i < text.size())
            {
                const std::size_t len = cg::util::utf8_seq_len(text[i]);
                if (i + len > text.size() || i + len > max_bytes)
                {
                    break;
                }
                i += len;
                keep = i;
            }
            return text.substr(0, keep);
        }

        /**
         * @brief 裁剪到至多 max 字节（UTF-8 码点安全，绝不产生非法序列）。
         *
         * @internal `clip` 会为省略号额外留 3 字节；预算约束下先让出这 3 字节，
         * 保证返回值严格 <= max。max 很小则直接按码点边界截断、不加省略号。
         */
        [[nodiscard]]
        inline auto clip_at_most(std::string_view s, std::size_t max)
            -> std::string
        {
            if (s.size() <= max)
            {
                return std::string{s};
            }
            if (max <= 3)
            {
                return std::string{utf8_trunc_bytes(s, max)};
            }
            return clip(s, max - 3);
        }

        /** @brief 从 JSON 成员读整数；缺失/非数字返回 0。 */
        [[nodiscard]]
        inline auto digest_number(
            const json::ValueView& object,
            std::string_view       key) -> std::int64_t
        {
            const auto value = object.get(key);
            return value.has_value() && value->is_number()
                       ? value->as_int().value_or(0)
                       : 0;
        }

        /** @brief 从 JSON 成员读布尔；缺失/非布尔返回 false。 */
        [[nodiscard]]
        inline auto digest_bool(
            const json::ValueView& object,
            std::string_view       key) -> bool
        {
            const auto value = object.get(key);
            return value.has_value() && value->is_bool()
                       ? value->as_bool().value_or(false)
                       : false;
        }

        /** @brief journal 条目的九字段原样值（读写两侧的单一事实源）。 */
        struct DigestEntryFields
        {
            std::int64_t v = 1;
            std::string  path;
            std::string  norm;
            bool         have_line = false;
            std::int64_t line = 0;
            std::int64_t lines = 0;
            bool         comment_only = false;
            bool         need_drain = false;
            std::int64_t ts = 0;
        };

        /**
         * @brief journal 条目 JSON 的唯一书写点。
         *
         * @internal 写侧（write_digest_entry）与改写侧（clear_need_drain）共用
         * 同一字段清单：增删字段只改这里，杜绝一边加了字段、另一边改写时丢掉
         * 的半套漂移。字段顺序即落盘顺序，与既有文件逐字节兼容。
         */
        [[nodiscard]]
        inline auto digest_entry_to_json(DigestEntryFields fields)
            -> std::string
        {
            json::Object o;
            o.set("v", fields.v);
            o.set("path", std::move(fields.path));
            o.set("norm", std::move(fields.norm));
            o.set("have_line", fields.have_line);
            o.set("line", fields.line);
            o.set("lines", fields.lines);
            o.set("comment_only", fields.comment_only);
            o.set("need_drain", fields.need_drain);
            o.set("ts", fields.ts);
            return json::stringify(std::move(o));
        }

        /** @brief journal 条目 JSON 的唯一读取点（缺失/类型不符取中性默认值）。
         */
        [[nodiscard]]
        inline auto digest_entry_from_json(const json::ValueView& root)
            -> DigestEntryFields
        {
            DigestEntryFields f;
            f.v = digest_number(root, "v");
            f.path = member_string(root, "path");
            f.norm = member_string(root, "norm");
            f.have_line = digest_bool(root, "have_line");
            f.line = digest_number(root, "line");
            f.lines = digest_number(root, "lines");
            f.comment_only = digest_bool(root, "comment_only");
            f.need_drain = digest_bool(root, "need_drain");
            f.ts = digest_number(root, "ts");
            return f;
        }

        /**
         * @brief 把一条快路径编辑写成唯一的 journal 文件。
         *
         * @internal 每个 hook 进程每次调用一个唯一名（pid + steady 滴答 +
         * 进程内 单调序），`atomic_write_file` 用同目录 tmp +
         * 原子替换，故并发写者之间
         * 无需任何共享读改写，读者永远看不到半写文件。目录不可写时静默放弃：
         * digest 只是汇报，索引入队可能已经成功。
         */
        inline void write_digest_entry(
            const std::string& db,
            const DigestEntry& entry)
        {
            std::error_code ec;
            const fs::path  dir = digest_dir_for(db);
            fs::create_directories(dir, ec);
            if (ec)
            {
                return;
            }
            DigestEntryFields fields;
            fields.path = entry.path;
            fields.norm = entry.norm;
            fields.have_line = entry.span.have_line;
            fields.line = static_cast<std::int64_t>(entry.span.line);
            fields.lines = static_cast<std::int64_t>(entry.span.lines);
            fields.comment_only = entry.span.comment_only;
            fields.need_drain = entry.need_drain;
            fields.ts = entry.ts;
            const std::string text = digest_entry_to_json(std::move(fields));
            if (text.empty())
            {
                return;
            }
            const fs::path file =
                dir
                / std::format(
                    "pending-{}-{}.json",
                    cg::util::platform::current_pid(),
                    cg::util::platform::make_nonce() & 0xFFFFFFFFFFFFULL);
            (void)cg::util::platform::atomic_write_file(file, text);
        }

        // 投递标记：一次捎带投递完成后落一个空标记文件
        // （`flushed-<来源>-<pid>-<nonce>`，与 claimed- 认领、pending-
        // journal 同目录同构）。没有共享读改写，并发消费者各写各的
        // 唯一名；`ccg hook status --json` 的 digest 节点按前缀计数，
        // 24h 老化后清零——观测口径与 pending/claimed 完全一致。

        /** @brief 投递标记文件名的固定前缀（status 计数与老化都锚定它）。 */
        inline constexpr std::string_view DIGEST_FLUSHED_PREFIX = "flushed-";

        /**
         * @brief 记一次经某事件捎带完成的 digest 投递（空标记文件）。
         *
         * @internal 计数是观测性状态而非正确性状态：目录不可写/创建失败
         * 一律静默（丢一个标记只是少计数，绝不影响投递本身）。文件名带
         * pid + 单调 nonce，并发投递者之间零碰撞、零共享。
         */
        inline void record_digest_delivery(
            const std::string& db,
            std::string_view   source)
        {
            std::error_code ec;
            fs::create_directories(digest_dir_for(db), ec);
            if (ec)
            {
                return;
            }
            const fs::path marker =
                digest_dir_for(db)
                / std::format(
                    "{}{}-{}-{}",
                    DIGEST_FLUSHED_PREFIX,
                    source,
                    cg::util::platform::current_pid(),
                    cg::util::platform::make_nonce() & 0xFFFFFFFFFFFFULL);
            std::ofstream out{marker, std::ios::binary | std::ios::trunc};
            (void)out;
        }

        /**
         * @brief 数出某来源的投递标记数（status 的 digest 观测节点）。
         *
         * @internal 只认 `flushed-<source>-` 整前缀，别的来源（或畸形名）
         * 不计入；目录不存在计 0。
         */
        [[nodiscard]]
        inline auto count_digest_deliveries(
            const fs::path&  dir,
            std::string_view source) -> std::int64_t
        {
            const std::string prefix =
                std::format("{}{}-", DIGEST_FLUSHED_PREFIX, source);
            std::error_code ec;
            std::int64_t    count = 0;
            for (const auto& item : fs::directory_iterator(dir, ec))
            {
                if (ec)
                {
                    break;
                }
                if (item.path().filename().string().starts_with(prefix))
                {
                    ++count;
                }
            }
            return count;
        }

        /** @brief 同一规范化路径的全部 journal 文件合并成的一组。 */
        struct DigestGroup
        {
            std::string           display;
            std::string           norm;
            EditSpan              span;
            bool                  need_drain = false;
            std::int64_t          first_ts = 0;
            std::int64_t          ts = 0;
            std::vector<fs::path> files;
        };

        /**
         * @brief 读全部 pending journal，按 norm 分组；损坏文件删除并写
         * stderr。
         *
         * @internal 解析失败绝不把垃圾拼进注入（也绝不留下永远重试的坏文件）。
         * 每组的展示路径与编辑落点取 ts 最大的一条（同一文件被多次编辑时，最新
         * 落点最可能对应读者关心的符号）；该组**全部**文件都记下来，认领时
         * 一起移走，避免旧条目下次又被读到。
         */
        [[nodiscard]]
        inline auto load_digest_groups(const fs::path& dir)
            -> std::vector<DigestGroup>
        {
            std::vector<DigestGroup>                     groups;
            std::unordered_map<std::string, std::size_t> index;
            std::error_code                              ec;
            for (const auto& item : fs::directory_iterator(dir, ec))
            {
                if (ec)
                {
                    break;
                }
                const std::string name = item.path().filename().string();
                if (!name.starts_with("pending-") || !name.ends_with(".json"))
                {
                    continue;
                }
                std::string text;
                {
                    std::ifstream in{item.path(), std::ios::binary};
                    if (!in)
                    {
                        continue;
                    }
                    text.assign(std::istreambuf_iterator<char>{in}, {});
                }
                std::string  norm;
                std::string  path_field;
                EditSpan     span;
                bool         need_drain = false;
                std::int64_t ts = 0;
                bool         ok = false;
                try
                {
                    const json::Document doc = json::Document::parse(text);
                    const auto           root = doc.root();
                    if (root.is_object())
                    {
                        // 行号在落盘值上钳到 >=1（脏数据不给渲染层出怪题）。
                        const DigestEntryFields f =
                            digest_entry_from_json(root);
                        path_field = std::move(f.path);
                        norm = std::move(f.norm);
                        span.have_line = f.have_line;
                        span.line = static_cast<std::size_t>(
                            std::max<std::int64_t>(f.line, 1));
                        span.lines = static_cast<std::size_t>(
                            std::max<std::int64_t>(f.lines, 1));
                        span.comment_only = f.comment_only;
                        need_drain = f.need_drain;
                        ts = f.ts;
                        ok = !path_field.empty() && !norm.empty();
                    }
                }
                catch (const std::exception&)
                {
                    ok = false;
                }
                if (!ok)
                {
                    std::println(
                        std::cerr,
                        "ccg hook: dropping unreadable digest entry '{}'",
                        name);
                    cg::util::platform::remove_file(item.path());
                    continue;
                }
                const auto found = index.find(norm);
                if (found == index.end())
                {
                    DigestGroup group;
                    group.display = path_field;
                    group.norm = norm;
                    group.span = span;
                    group.need_drain = need_drain;
                    group.first_ts = ts;
                    group.ts = ts;
                    group.files.push_back(item.path());
                    index.emplace(norm, groups.size());
                    groups.push_back(std::move(group));
                    continue;
                }
                DigestGroup& group = groups[found->second];
                group.files.push_back(item.path());
                group.first_ts = std::min(group.first_ts, ts);
                group.need_drain = group.need_drain || need_drain;
                if (ts >= group.ts)
                {
                    group.ts = ts;
                    group.display = path_field;
                    group.span = span;
                }
            }
            return groups;
        }

        /**
         * @brief 把已合并进 pending_updates 的组标回 `need_drain = false`。
         *
         * @internal 不这样做，下一次 flush 会永远看到"仍有未合并的旁路条目"，
         * 于是每轮都重新入队却永远不报告结论。重写是整个文件的原子替换；
         * 只在 enqueue 成功之后调用（失败保留脏标记，下一次重试）。
         */
        inline void clear_need_drain(const std::vector<DigestGroup>& groups)
        {
            for (const DigestGroup& group : groups)
            {
                if (!group.need_drain)
                {
                    continue;
                }
                for (const fs::path& file : group.files)
                {
                    std::string text;
                    {
                        std::ifstream in{file, std::ios::binary};
                        if (!in)
                        {
                            continue;
                        }
                        text.assign(std::istreambuf_iterator<char>{in}, {});
                    }
                    try
                    {
                        const json::Document doc = json::Document::parse(text);
                        const auto           root = doc.root();
                        if (!root.is_object())
                        {
                            continue;
                        }
                        DigestEntryFields fields = digest_entry_from_json(root);
                        fields.need_drain = false;
                        (void)cg::util::platform::atomic_write_file(
                            file,
                            digest_entry_to_json(std::move(fields)));
                    }
                    catch (const std::exception&)
                    {
                    }
                }
            }
        }

        /**
         * @brief 老化清理：pending 24h、claimed 1h、tmp 1h、投递标记 24h、
         *        无主的 flush.lock stale 窗口（目录不无限增长，崩溃残留不
         *        永久卡住注入）。
         */
        inline void digest_age_cleanup(const fs::path& dir)
        {
            const auto      now = fs::file_time_type::clock::now();
            std::error_code ec;
            for (const auto& item : fs::directory_iterator(dir, ec))
            {
                if (ec)
                {
                    break;
                }
                const std::string name = item.path().filename().string();
                if (name == "flush.lock")
                {
                    // 兜底回收：pid/stamp 都无法解析的锁（崩溃残留）在超龄后
                    // 删除。可读性本身就是 Windows 上的持有者探针（活持有者以
                    // 共享模式 0 打开，读取必失败）；有效 pid
                    // 仍存活的一律跳过。
                    std::int64_t pid = 0;
                    bool         readable = false;
                    {
                        std::ifstream in{item.path(), std::ios::binary};
                        if (in)
                        {
                            readable = true;
                            std::string line;
                            if (std::getline(in, line))
                            {
                                try
                                {
                                    pid = std::stoll(line);
                                }
                                catch (...)
                                {
                                    pid = 0;
                                }
                            }
                        }
                    }
                    if (!readable
                        || (pid > 0 && cg::util::platform::process_alive(pid)))
                    {
                        continue;
                    }
                    std::error_code tec;
                    const auto stamp = fs::last_write_time(item.path(), tec);
                    if (!tec && now - stamp > DIGEST_FLUSH_LOCK_STALE)
                    {
                        cg::util::platform::remove_file(item.path());
                    }
                    continue;
                }
                std::chrono::seconds limit{0};
                if (name.starts_with("claimed-"))
                {
                    limit = DIGEST_CLAIMED_MAX_AGE;
                }
                else if (name.starts_with(DIGEST_FLUSHED_PREFIX))
                {
                    // 投递标记比 claimed 活得久（24h 滑窗计数），但仍要
                    // 老化：持续无投递的目录不该背着历史标记无限增长。
                    limit = DIGEST_ENTRY_MAX_AGE;
                }
                else if (
                    name.starts_with("pending-") && name.ends_with(".json"))
                {
                    limit = DIGEST_ENTRY_MAX_AGE;
                }
                else if (name.ends_with(".tmp"))
                {
                    limit = DIGEST_CLAIMED_MAX_AGE;
                }
                else
                {
                    continue;
                }
                std::error_code tec;
                const auto      stamp = fs::last_write_time(item.path(), tec);
                if (tec)
                {
                    continue;
                }
                if (now - stamp > limit)
                {
                    cg::util::platform::remove_file(item.path());
                }
            }
        }

        /**
         * @brief 一条消费者标签：`file:line name`（名字中间省略保留套件）。
         *
         * @internal 影响面查询的两条来路（符号级 FileSymbol 与 changes JSON 的
         * 逐字段物化）共用同一裁剪口径；qualified 为空时退回 fallback 名。
         */
        [[nodiscard]]
        inline auto digest_consumer_label(
            std::string_view file,
            std::int64_t     line,
            std::string_view qualified,
            std::string_view fallback) -> std::string
        {
            std::string out;
            if (!file.empty() && line > 0)
            {
                out = std::format("{}:{} ", clip(basename_of(file), 32), line);
            }
            out += clip_middle(qualified.empty() ? fallback : qualified, 48);
            return out;
        }

        /** @brief FileSymbol 形态的消费者标签（限定名缺省退回短名）。 */
        [[nodiscard]]
        inline auto digest_consumer_label(const FileSymbol& symbol)
            -> std::string
        {
            return digest_consumer_label(
                symbol.file,
                symbol.line,
                symbol.qualified_name,
                symbol.name);
        }

        /** @brief 按相关性取最相关的前 max 个消费者标签（同文件优先）。 */
        [[nodiscard]]
        inline auto digest_consumers(
            std::vector<FileSymbol> consumers,
            const std::string&      edited_file,
            std::size_t             max) -> std::vector<std::string>
        {
            sort_consumers(consumers, edited_file);
            std::vector<std::string>        out;
            std::unordered_set<std::string> seen;
            for (const FileSymbol& consumer : consumers)
            {
                const std::string key = consumer.uid != 0
                                            ? std::format("u:{}", consumer.uid)
                                            : consumer.qualified_name;
                if (!seen.insert(key).second)
                {
                    continue;
                }
                out.push_back(digest_consumer_label(consumer));
                if (out.size() >= max)
                {
                    break;
                }
            }
            return out;
        }

        /**
         * @brief 文件级回退：`ccg changes --depth 1` 的直接/传递/风险/消费者
         *        （进程内直连）。
         */
        inline void digest_file_fallback(
            DigestRow&         row,
            HookQueryDb&       db,
            const std::string& display)
        {
            const auto changes = hook_changes(db, display);
            if (!changes.has_value())
            {
                row.note = "影响面查询超时";
                return;
            }
            if (!changes->any_known)
            {
                row.tracked = false;
                row.note.clear();
                return;
            }
            row.direct = changes->direct;
            row.transitive = changes->transitive;
            row.fan_in = changes->fan_in;
            row.risk = changes->risk;
            if (row.risk.empty() || row.risk == "UNKNOWN")
            {
                row.risk =
                    std::string{direct_risk_name(row.direct, row.fan_in)};
            }
            for (const HookChanges::Consumer& consumer : changes->consumers)
            {
                if (consumer.qualified_name.empty())
                {
                    continue;
                }
                row.consumers.push_back(digest_consumer_label(
                    consumer.file,
                    consumer.line,
                    consumer.qualified_name,
                    {}));
                if (row.consumers.size() >= 2)
                {
                    break;
                }
            }
        }

        /**
         * @brief 一个认领组的结论：符号级优先，定位失败退回文件级。
         *
         * @internal 只在批次收敛后调用（最新 db）；journal 从不缓存编辑时的
         * 影响面数字。符号级查询超时/失败也仍生成一行（note），消费掉该组，
         * 避免同一个文件被永久重试。
         */
        [[nodiscard]]
        inline auto compute_digest_row(
            const DigestGroup&        group,
            HookQueryDb&              db,
            std::chrono::milliseconds impact_margin) -> DigestRow
        {
            DigestRow row;
            row.norm = group.norm;
            row.path_display = display_path(group.display);
            EditInfo info;
            info.span = group.span;
            info.comment_only = group.span.comment_only;

            std::optional<FileSymbol> symbol;
            try
            {
                symbol = locate_edited_symbol(db, group.display, info, nullptr);
            }
            catch (const std::exception&)
            {
                symbol = std::nullopt;
            }
            if (symbol.has_value())
            {
                ImpactInfo impact;
                try
                {
                    impact = symbol_impact(
                        db,
                        *symbol,
                        /*merge_family=*/false,
                        impact_margin);
                }
                catch (const std::exception&)
                {
                }
                const bool method_like = cg::db::is_method_like(symbol->kind);
                if (impact.ok && impact.direct == 0 && method_like)
                {
                    try
                    {
                        ImpactInfo merged = symbol_impact(
                            db,
                            *symbol,
                            /*merge_family=*/true,
                            impact_margin);
                        if (merged.ok && merged.direct > impact.direct)
                        {
                            impact = std::move(merged);
                        }
                    }
                    catch (const std::exception&)
                    {
                    }
                }
                if (impact.ok)
                {
                    const std::string_view full =
                        symbol->qualified_name.empty()
                            ? std::string_view{symbol->name}
                            : std::string_view{symbol->qualified_name};
                    row.symbol = clip_middle(full, 60);
                    row.kind = symbol->kind;
                    row.uid = symbol->uid;
                    row.direct = impact.direct;
                    row.transitive = impact.transitive;
                    row.fan_in = impact.fan_in;
                    row.dispatch_stale = impact.dispatch_stale;
                    row.risk = std::string{
                        direct_risk_name(impact.direct, impact.fan_in)};
                    row.consumers = digest_consumers(
                        impact.direct_symbols,
                        group.display,
                        2);
                    return row;
                }
            }
            try
            {
                digest_file_fallback(row, db, group.display);
            }
            catch (const std::exception&)
            {
                row.note = "影响面查询超时";
            }
            return row;
        }

        /** @brief 风险排序权重：CRITICAL > HIGH > MEDIUM > LOW > 无。 */
        [[nodiscard]]
        inline auto digest_risk_rank(std::string_view risk) -> int
        {
            if (risk == "CRITICAL")
            {
                return 4;
            }
            if (risk == "HIGH")
            {
                return 3;
            }
            if (risk == "MEDIUM")
            {
                return 2;
            }
            if (risk == "LOW")
            {
                return 1;
            }
            return 0;
        }

        /** @brief 一行结论的渲染（中文紧凑格式）。 */
        [[nodiscard]]
        inline auto render_digest_row(const DigestRow& row) -> std::string
        {
            std::string line = row.display + ": ";
            if (!row.tracked)
            {
                return line + "未被索引";
            }
            if (!row.note.empty())
            {
                return line + row.note;
            }
            if (!row.symbol.empty())
            {
                line += std::format(
                    "{}（{}）直接 {} / 传递 {} / 风险 {}",
                    row.symbol,
                    clip(row.kind, 20),
                    row.direct,
                    row.transitive,
                    row.risk);
            }
            else
            {
                line += std::format(
                    "文件级 直接 {} / 传递 {} / 风险 {}",
                    row.direct,
                    row.transitive,
                    row.risk);
            }
            if (!row.consumers.empty())
            {
                line = std::format("{}；最相关：", line);
                for (std::size_t i = 0; i < row.consumers.size(); ++i)
                {
                    if (i != 0)
                    {
                        line += ", ";
                    }
                    line += row.consumers[i];
                }
            }
            if (row.dispatch_stale > 0)
            {
                line += std::format(
                    "；派发分析滞后 {} 文件（虚调用计数含旧数据）",
                    row.dispatch_stale);
            }
            return line;
        }

        /**
         * @brief 去重（符号级 + 摘要级）并为幸存行计算展示名。
         *
         * @internal 展示名必须在摘要级去重之前算好：`render_digest_row` 以
         * `row.display` 开头，若此时仍为空，不同文件的“未被索引/文件级”结论
         * 会渲染成完全相同的文本而互相吞掉。摘要 key 再带上规范化路径，保证
         * “同一摘要”只在同一文件内合并，不同文件绝不会共享一条结论。
         *
         * @return 幸存行与被合并掉的重复行数（计入“等 N 项”）。
         */
        [[nodiscard]]
        inline auto finalize_digest_rows(std::vector<DigestRow> rows)
            -> std::pair<std::vector<DigestRow>, std::size_t>
        {
            // 符号级去重：uid 优先，其次限定名|kind，最后路径。
            std::vector<DigestRow>                       unique;
            std::unordered_map<std::string, std::size_t> seen;
            std::size_t                                  duplicates = 0;
            for (DigestRow& row : rows)
            {
                std::string key;
                if (row.uid > 0)
                {
                    key = std::format("u:{}", row.uid);
                }
                else if (!row.symbol.empty())
                {
                    key = std::format("s:{}|{}", row.symbol, row.kind);
                }
                else
                {
                    key = std::format("f:{}", row.norm);
                }
                const auto found = seen.find(key);
                if (found == seen.end())
                {
                    seen.emplace(std::move(key), unique.size());
                    unique.push_back(std::move(row));
                    continue;
                }
                ++duplicates;
                DigestRow& kept = unique[found->second];
                if (row.direct > kept.direct)
                {
                    kept = std::move(row);
                }
            }

            // 展示名：basename 重名时改用尾裁的相对路径。
            std::unordered_map<std::string, std::size_t> base_counts;
            for (const DigestRow& row : unique)
            {
                ++base_counts[basename_of(row.path_display)];
            }
            for (DigestRow& row : unique)
            {
                const std::string base = basename_of(row.path_display);
                row.display = base_counts[base] > 1
                                  ? clip_tail(row.path_display, 40)
                                  : base;
            }

            // 摘要级去重：同一路径上渲染文本完全相同只留一行。
            std::vector<DigestRow>          final_rows;
            std::unordered_set<std::string> texts;
            for (DigestRow& row : unique)
            {
                const std::string key = std::format(
                    "{}\n{}",
                    row.norm.empty() ? row.path_display : row.norm,
                    render_digest_row(row));
                if (!texts.insert(key).second)
                {
                    ++duplicates;
                    continue;
                }
                final_rows.push_back(std::move(row));
            }
            return {std::move(final_rows), duplicates};
        }

        /**
         * @brief 组装一条有界的批量结论。
         *
         * @internal 先留出尾部“等 N 项”的字节，再逐行追加；放不下的行计入
         * omitted，读者仍知道真实规模。所有裁剪走码点边界（`clip_at_most`），
         * 非法 UTF-8 会让整条 JSON 注入失败。omitted 的位数会改变后缀长度，
         * 因此迭代到稳定（最多 4 轮）。
         */
        [[nodiscard]]
        inline auto assemble_digest(
            const std::vector<DigestRow>& rows,
            std::size_t                   omitted,
            std::size_t                   budget) -> std::string
        {
            const std::string header =
                std::format("[ccg] 批次完成：{} 个文件", rows.size());
            std::size_t total_omitted = omitted;
            std::size_t used = 0;
            std::string body;
            for (int pass = 0; pass < 4; ++pass)
            {
                const std::string suffix =
                    total_omitted > 0 ? std::format(" 等 {} 项", total_omitted)
                                      : std::string{};
                const std::size_t head_budget =
                    budget > suffix.size() ? budget - suffix.size() : 0;
                body = clip_at_most(
                    header,
                    std::min<std::size_t>(160, head_budget));
                used = 0;
                for (const DigestRow& row : rows)
                {
                    std::string line = "\n" + render_digest_row(row);
                    line = clip_at_most(line, 260);
                    if (body.size() + line.size() > head_budget)
                    {
                        break;
                    }
                    body += line;
                    ++used;
                }
                const std::size_t next = omitted + (rows.size() - used);
                if (next == total_omitted)
                {
                    break;
                }
                total_omitted = next;
            }
            const std::string suffix =
                total_omitted > 0 ? std::format(" 等 {} 项", total_omitted)
                                  : std::string{};
            // 最后一轮之后 body 可能与最终 suffix 的预算差一个数量级（位数
            // 增长）；再按最终预算裁一次，保证总字节严格 <= budget。
            const std::size_t head_budget =
                budget > suffix.size() ? budget - suffix.size() : 0;
            if (body.size() > head_budget)
            {
                body = clip_at_most(body, head_budget);
            }
            return body + suffix;
        }

        /**
         * @brief 延迟收敛的触发探针：队列已收敛但派发数据滞后时派生一次
         *        detach 的 `ccg update --drain`（其尾部跑收敛批）。
         *
         * @internal 独立于 hook_digest_flush：digest journal 目录不存在时 flush
         * 在入口就静默返回，而派发收敛的触发绝不能依赖 journal 目录是否曾经有
         * 过 PostToolUse 事件。活租约在场（正在跑的 update/drain）即静默返回
         * ——持有者无论如何会覆盖；重复派生至多浪费一次毫秒级的空转
         * （claim_update 失败即退出 0）。所有失败路径静默（探针绝不阻塞会话）。
         */
        inline void maybe_detach_dispatch_drain(const LayoutChoice& choice)
        {
            try
            {
                cg::db::GraphStore probe{choice.db, FAST_PROBE_BUSY_MS};
                if (probe.claim_snapshot().live)
                {
                    return;
                }
                const std::int64_t parsed =
                    cg::util::parse_bounded(probe.meta("dispatch_stale"), 0);
                if (parsed <= 0)
                {
                    return;
                }
                std::vector<std::string> args{
                    "update",
                    "--drain",
                    "--json",
                    "-o",
                    choice.root,
                    "--config",
                    choice.config};
                (void)run_child_json_timed(args, std::chrono::milliseconds{0});
            }
            catch (const std::exception&)
            {
            }
        }

        /**
         * @brief 积压 flush：认领 journal、基于最新库重算、组装一条结论。
         *
         * @param shared 调用方已有的查询会话（posttooluse 传入，全处理器共用一
         *               次开库）；空则认领成功后自开一份。
         *
         * @return nullopt = 静默返回（调用方必须零 stdout）。以下情形静默：
         * 目录不存在、取不到消费者锁、仍有活租约、队列尚未收敛、没有积压。
         *
         * @internal 步骤严格有序：取锁 → 快照 → 活租约即退出 → 读 journal →
         * 队列未收敛则合并 need_drain 并（Poll）派生一次 drain 后退出 → 按 FIFO
         * 认领至多 DIGEST_MAX_FILES 组（原子改名，恰好一个消费者赢）→ 在截止
         * 前计算 → 去重 → 组装。认领先于发送，claimed 残留永不回读，只老化
         * 删除：需求 3 的"绝不重复注入"优先于"绝不丢失"。
         */
        [[nodiscard]]
        inline auto hook_digest_flush(
            const LayoutChoice& choice,
            FlushMode           mode,
            std::size_t         budget,
            HookQueryDb* shared = nullptr) -> std::optional<DigestFlushResult>
        {
            try
            {
                const fs::path  dir = digest_dir_for(choice.db);
                std::error_code ec;
                if (!fs::is_directory(dir, ec) || ec)
                {
                    return std::nullopt;
                }
                cg::util::platform::FileLock lock =
                    cg::util::platform::try_acquire_file_lock(
                        dir / "flush.lock",
                        std::format(
                            "pid:{}:t{}",
                            cg::util::platform::current_pid(),
                            std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count()),
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            DIGEST_FLUSH_LOCK_STALE)
                            .count());
                if (!lock.held())
                {
                    return std::nullopt;
                }

                std::optional<cg::db::ClaimSnapshot> snapshot;
                try
                {
                    cg::db::GraphStore probe{choice.db, FAST_PROBE_BUSY_MS};
                    snapshot = probe.claim_snapshot();
                    // coord_events 老化（7 天/万行）：挂在 digest 老化同一点
                    // （flush 是 PostToolUse/UPS/SessionStart 共用的低频收
                    // 口，且必在无活租约时到达）；复用本条 probe 连接，
                    // DELETE 空扫描微秒级。失败静默——老化下一轮再来。
                    (void)probe.coord_events_age(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::hours{24 * 7})
                            .count(),
                        10'000);
                }
                catch (const std::exception&)
                {
                    return std::nullopt;
                }
                if (snapshot->live)
                {
                    return std::nullopt; // 需求 5：仍在跑则静默
                }

                digest_age_cleanup(dir);
                std::vector<DigestGroup> groups = load_digest_groups(dir);
                std::vector<std::string> drain_paths;
                for (const DigestGroup& group : groups)
                {
                    if (group.need_drain)
                    {
                        drain_paths.push_back(group.norm);
                    }
                }
                if (!snapshot->pending.empty() || !drain_paths.empty())
                {
                    // 队列还没被任何更新者收敛：本轮不注入，先让它变干净。
                    // need_drain 的路径以幂等 INSERT 合并进 pending_updates；
                    // 失败就留到下一次（journal 仍在）。
                    if (!drain_paths.empty())
                    {
                        try
                        {
                            cg::db::GraphStore store{choice.db, 300};
                            store.enqueue_pending(drain_paths);
                            // 合并成功才清脏标记：否则下一轮会永远重复"合并
                            // 但不报告"，结论永远出不来。
                            clear_need_drain(groups);
                        }
                        catch (const std::exception&)
                        {
                        }
                    }
                    if (mode == FlushMode::Poll)
                    {
                        // 立即 detach：不等待、不 kill，后台 update 自己跑完。
                        std::vector<std::string> args{
                            "update",
                            "--drain",
                            "--json",
                            "-o",
                            choice.root,
                            "--config",
                            choice.config};
                        (void)run_child_json_timed(
                            args,
                            std::chrono::milliseconds{0});
                    }
                    return std::nullopt;
                }
                if (groups.empty())
                {
                    return std::nullopt;
                }

                // FIFO：最早进入积压的组先汇报，保证最终收敛。
                std::ranges::sort(
                    groups,
                    [](const DigestGroup& a, const DigestGroup& b)
                    { return a.first_ts < b.first_ts; });

                struct Selected
                {
                    DigestGroup group;
                    std::vector<std::pair<fs::path, fs::path>>
                        files; // orig -> claimed
                };
                std::vector<Selected> selected;
                std::vector<fs::path> claimed_paths;
                const std::size_t     take =
                    std::min(groups.size(), DIGEST_MAX_FILES);
                std::uint64_t ticks =
                    static_cast<std::uint64_t>(std::chrono::steady_clock::now()
                                                   .time_since_epoch()
                                                   .count());
                for (std::size_t i = 0; i < take; ++i)
                {
                    Selected pick;
                    pick.group = groups[i];
                    bool ok = true;
                    for (const fs::path& source : pick.group.files)
                    {
                        const fs::path target =
                            dir
                            / std::format(
                                "claimed-{}-{}-{}",
                                cg::util::platform::current_pid(),
                                ticks++,
                                source.filename().string());
                        std::error_code rec;
                        fs::rename(source, target, rec);
                        if (rec)
                        {
                            ok = false;
                            break;
                        }
                        pick.files.emplace_back(source, target);
                    }
                    if (!ok)
                    {
                        // 有一个没抢到：把已改名的放回
                        // pending，整组留给下一轮。
                        for (auto& [source, target] : pick.files)
                        {
                            std::error_code rec;
                            fs::rename(target, source, rec);
                        }
                        continue;
                    }
                    for (const auto& entry : pick.files)
                    {
                        claimed_paths.push_back(entry.second);
                    }
                    selected.push_back(std::move(pick));
                }
                if (selected.empty())
                {
                    return std::nullopt;
                }

                std::size_t omitted = groups.size() - selected.size();
                const auto  impact_margin = mode == FlushMode::Poll
                                                ? DIGEST_IMPACT_TIMEOUT_UPS
                                                : DIGEST_IMPACT_TIMEOUT_PTU;
                const auto  deadline = mode == FlushMode::Poll
                                           ? DIGEST_UPS_DEADLINE
                                           : DIGEST_PTU_DEADLINE;
                const auto  started =
                    cg::util::platform::process_elapsed_since_creation();
                // 结论计算共用调用方的查询会话（无则自开）；一次开库服务全组。
                HookQueryDb own;
                if (shared == nullptr)
                {
                    own = open_hook_query_db(choice.db);
                }
                HookQueryDb& db = shared != nullptr ? *shared : own;

                std::vector<DigestRow> rows;
                for (std::size_t i = 0; i < selected.size(); ++i)
                {
                    if (cg::util::platform::process_elapsed_since_creation()
                            - started
                        >= deadline)
                    {
                        // 截止：本组与其余已认领组改名回 pending，下一轮再报。
                        for (std::size_t j = i; j < selected.size(); ++j)
                        {
                            for (auto& [source, target] : selected[j].files)
                            {
                                const fs::path back =
                                    dir
                                    / std::format(
                                        "pending-{}-{}-{}",
                                        cg::util::platform::current_pid(),
                                        ticks++,
                                        source.filename().string());
                                std::error_code rec;
                                fs::rename(target, back, rec);
                            }
                        }
                        omitted += selected.size() - i;
                        break;
                    }
                    rows.push_back(compute_digest_row(
                        selected[i].group,
                        db,
                        impact_margin));
                }

                auto [final_rows, duplicates] =
                    finalize_digest_rows(std::move(rows));
                omitted += duplicates;

                std::ranges::sort(
                    final_rows,
                    [](const DigestRow& a, const DigestRow& b)
                    {
                        const int ra = digest_risk_rank(a.risk);
                        const int rb = digest_risk_rank(b.risk);
                        if (ra != rb)
                        {
                            return ra > rb;
                        }
                        if (a.direct != b.direct)
                        {
                            return a.direct > b.direct;
                        }
                        return a.path_display < b.path_display;
                    });

                DigestFlushResult result;
                result.text = assemble_digest(final_rows, omitted, budget);
                result.claimed = std::move(claimed_paths);
                if (result.text.empty())
                {
                    return result;
                }
                return result;
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        /** @brief 发送完成后的清理：删除已认领文件（失败静默，只老化兜底）。 */
        inline void digest_flush_done(const DigestFlushResult& result)
        {
            for (const fs::path& file : result.claimed)
            {
                cg::util::platform::remove_file(file);
            }
        }

        /**
         * @brief 文件级门的语料面点查：被编辑文件是否已在目标库的 files 表
         *        （头文件同计——files 表收录全部语料文件，非仅 TU 主文件）。
         *
         * @internal 精确点查走 files.path_id 唯一索引（毫秒级）；未命中再付
         * 一次大小写折叠的全路径匹配（Claude 传来的 file_path 可能是模型
         * 自行拼的小写形态）。**刻意不做**查询侧的后缀兜底：后缀解析服务
         * 的是"用户输入的模糊定位"，而门判的是文件的字面身份——裸名后缀
         * 会把任何跨目录同名文件（别库的 main.cpp）误放行，门就失效了。
         */
        [[nodiscard]]
        auto corpus_gate_pass(
            cg::db::GraphStore& probe,
            const std::string&  norm) -> bool
        {
            if (probe.find_file(norm).has_value())
            {
                return true;
            }
            const std::string want = cg::util::path_compare_key(norm);
            for (const cg::db::FileRow& f : cg::db::all_files(probe))
            {
                if (cg::util::path_compare_key(f.path) == want)
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief PostToolUse 的快速标脏：有活租约时只把文件入队并写一条
         *        journal，立即收工。
         *
         * @internal 先用一次短超时的只读快照问"批次是否在跑"。SQLite 单写者
         * + 引擎整批单事务意味着批次期间 INSERT 会一直等到 busy_timeout；把
         * 连接压到 150ms，失败就写 need_drain 旁路，绝不阻塞会话。
         * 入队之前先过文件级门（干活前先查语料面，而不是 update 跑完之后
         * 才发现文件不在库里白跑一遍）：库定位按会话 cwd 布局发现，跨仓库
         * 工作时被编辑文件可能根本不属于这个库（某废弃库实例积压 120 行
         * 孤儿队列的实证）——语料面外的文件立即拒收（一行 stderr +
         * enqueue-reject 审计）；工程文件跳过门走重采集联动（新入工程的
         * 文件经重捕获后由 update 侧的重查轮收编，见 cli.update 的
         * captured 分支）。门自身探测不到库（probe_failed）时保守放行：
         * 活库被长写者占住时绝不能把可能在语料面的编辑拒之门外。
         * @return true = 已处理完毕（调用方直接静默返回）；false = 无活租约
         *         且探测成功，走常规 update 路径。探测失败（打不开/读不了：
         *         可能正撞上长写者，也可能库损坏）时宁可诚实标脏也不赌
         *         "没有活租约"去派生一次可能长达 9 秒的 update——同样返回
         *         true（need_drain 会在无活租约时合并并 drain）。
         */
        [[nodiscard]]
        auto pt_fast_dirty(
            const std::string&  db,
            const std::string&  file,
            const std::string&  norm,
            const EditSpan&     span,
            const LayoutChoice& choice,
            bool                project_file) -> bool
        {
            const auto fast_start =
                cg::util::platform::process_elapsed_since_creation();
            bool probe_failed = false;
            try
            {
                cg::db::GraphStore probe{db, FAST_PROBE_BUSY_MS};
                if (!project_file && !corpus_gate_pass(probe, norm))
                {
                    // 立即拒收：一行 stderr（进宿主 hook 日志，不进注入
                    // 信封）+ enqueue-reject 审计行——误投当场留痕，绝不
                    // 入队白跑。审计失败静默（旁路尽力而为通道）。
                    std::println(
                        std::cerr,
                        "ccg hook: {} not in corpus of {} - event skipped",
                        clip_tail(display_path(file), 72),
                        choice.config);
                    (void)probe.log_coord_event(
                        cg::db::CoordEventInput{
                            .event = cg::db::CoordEvent::EnqueueReject,
                            .actor_holder = std::format(
                                "ccg-hook:pid:{}",
                                cg::util::platform::current_pid()),
                            .reason = "not-in-corpus",
                            .detail = std::format(
                                R"({{"config":"{}","file":"{}"}})",
                                choice.config,
                                norm),
                        });
                    return true;
                }
                const cg::db::ClaimSnapshot snapshot = probe.claim_snapshot();
                if (snapshot.live)
                {
                    bool queued = false;
                    if (cg::util::platform::process_elapsed_since_creation()
                            - fast_start
                        < FAST_PATH_ENQUEUE_DEADLINE)
                    {
                        try
                        {
                            probe.set_busy_timeout_ms(FAST_ENQUEUE_BUSY_MS);
                            const std::string* one = &norm;
                            probe.enqueue_pending(
                                std::span<const std::string>{one, 1});
                            queued = true;
                        }
                        catch (const std::exception&)
                        {
                            queued = false; // 绝不谎称已入队
                        }
                    }
                    write_digest_entry(
                        db,
                        DigestEntry{
                            file,
                            norm,
                            span,
                            !queued,
                            cg::util::cstd::epoch_ms()});
                    return true;
                }
            }
            catch (const std::exception&)
            {
                probe_failed = true;
            }
            if (probe_failed)
            {
                write_digest_entry(
                    db,
                    DigestEntry{
                        file,
                        norm,
                        span,
                        true,
                        cg::util::cstd::epoch_ms()});
                return true;
            }
            return false;
        }

        /** @brief 纯注释编辑的预定位产物（update 占库前算好的全部查询）。 */
        struct PtPreLocate
        {
            std::optional<FileSymbol> symbol;
            std::vector<FileSymbol>   same_line;
            std::optional<ImpactInfo> impact;
            std::optional<ImpactInfo> merged_impact;
            std::optional<ImpactInfo> owner_impact;
        };

        /**
         * @brief 纯注释编辑的预定位与预计算。
         *
         * @internal 注释编辑不动 uid/边：趁 update 尚未占用数据库，先用既有
         * 索引定位被编辑符号，并把影响面、字段所属类型代理、虚方法族视图
         * 一并算好。update 一旦超时 detach，后台写者会把后续每次查询拖到
         * busy_timeout（大库上可达数秒），那时不能再派生查询；这些预计算
         * 让注入在 detach 分支仍能指对符号并带上真实的直接消费者。
         */
        [[nodiscard]]
        auto pt_prelocate(
            HookQueryDb&       db,
            const std::string& file,
            const EditInfo&    edit) -> PtPreLocate
        {
            PtPreLocate pre;
            if (!edit.comment_only)
            {
                return pre;
            }
            pre.symbol = locate_edited_symbol(db, file, edit, &pre.same_line);
            if (pre.symbol.has_value()
                && !hook_deadline_reached(HOOK_IMPACT_TIMEOUT))
            {
                pre.impact = symbol_impact(
                    db,
                    *pre.symbol,
                    /*merge_family=*/false);
                const bool field_like = cg::db::is_value_like(pre.symbol->kind);
                const bool method_like =
                    cg::db::is_method_like(pre.symbol->kind);
                if (pre.impact->ok && pre.impact->direct == 0 && field_like
                    && !pre.symbol->parent.empty()
                    && !hook_deadline_reached(
                        HOOK_QUERY_TIMEOUT + HOOK_IMPACT_TIMEOUT))
                {
                    // 与主路径一致，用库内规范路径过滤：用户传来的
                    // 小写路径在 `query --file` 上是大小写敏感的。
                    pre.owner_impact = parent_record_impact(
                        db,
                        *pre.symbol,
                        pre.symbol->file.empty() ? file : pre.symbol->file);
                }
                // 虚方法/接口实现的消费者经基类派发，自身直接边为 0；
                // 这次族视图同样要在 detach 前算好。
                if (pre.impact->ok && pre.impact->direct == 0 && method_like
                    && !hook_deadline_reached(HOOK_IMPACT_TIMEOUT))
                {
                    pre.merged_impact = symbol_impact(db, *pre.symbol, true);
                }
            }
            return pre;
        }

        /** @brief PostToolUse 的 update 派生结论（含项目文件联动状态）。 */
        struct PtUpdate
        {
            bool started = true;
            bool queued = false;
            bool background = false;
            bool fallback_queued = false;
            /** @brief 重析规模注记（"重析 N TU（M TU 失败）…"，空 = 无）。 */
            std::string stale_note;
            /** @brief 项目文件联动结论（captured/busy/failed/untracked；空 =
             * 未触发）。
             */
            std::string refresh_state;
        };

        /**
         * @brief 派生一次 `ccg update` 并解析其 JSON 结论。
         *
         * @internal 预算只算一次：等待结束后再算会因已用时间增长而变小，
         * 日志也会与实际等待不符。宿主在 10s 会砍 hook；预算到点即放手，
         * 子进程不被杀死——索引更新在后台继续，注入只能基于既有索引。
         */
        auto pt_run_update(
            const Invocation&               inv,
            const std::string&              file,
            const std::vector<std::string>& db_args) -> PtUpdate
        {
            PtUpdate                 u;
            std::vector<std::string> args{"update", file, "--json"};
            args.insert(args.end(), db_args.begin(), db_args.end());
            const auto update_budget = hook_update_timeout();
            // ASCII 诊断（stderr 不进注入）：把实际等待预算与其基准
            // 一起打出，冷启动/负载下的预算收缩可直接核对。
            std::println(
                std::cerr,
                "ccg hook: wait budget {} ms (process age {} ms)",
                update_budget.count(),
                cg::util::platform::process_elapsed_since_creation().count());
            TimedChildJson update = run_child_json_timed(args, update_budget);
            if (!update.started)
            {
                // 子进程根本没孵化（CreateProcess/fork 失败、可执行路径
                // 不可得）。绝不谎报"已入库"：直接写队列让下一次 drain
                // 兜底，并在注入里明说索引更新未能启动。
                u.started = false;
                u.queued = true;
                u.fallback_queued = enqueue_pending_direct(inv, file);
                std::println(
                    std::cerr,
                    "ccg hook: index update could not be started - {}",
                    u.fallback_queued ? "file queued for the next drain"
                                      : "direct enqueue failed too");
            }
            else if (update.timed_out)
            {
                u.background = true;
                // 与"排队"同一后果：行号/引用须按盘上文本校正。
                u.queued = true;
                // ASCII 诊断（stderr 不进注入）：测试与运维都靠它确认
                // 走的是 detach 分支，而注入文本保持面向人的中文。
                std::println(
                    std::cerr,
                    "ccg hook: update still running after {} ms - "
                    "detached; refresh continues in the background",
                    update_budget.count());
            }
            else if (update.doc.has_value())
            {
                const auto claimed = update.doc->root().get("claimed");
                const auto queued = update.doc->root().get("queued");
                if (claimed.has_value() && claimed->is_bool()
                    && !claimed->as_bool().value_or(true) && queued.has_value()
                    && queued->is_number() && queued->as_int().value_or(0) > 0)
                {
                    // 活持有者/过期租约下 update 只入队；此时绝不能
                    // 声称“已入库”（注入基于既有索引）。
                    u.queued = true;
                }
                const auto stale = update.doc->root().get("stale");
                if (stale.has_value() && stale->is_number())
                {
                    u.stale_note =
                        std::format("重析 {} TU", stale->as_int().value_or(0));
                }
                const auto failed = update.doc->root().get("failed");
                if (failed.has_value() && failed->is_number()
                    && failed->as_int().value_or(0) > 0)
                {
                    u.stale_note += std::format(
                        "（{} TU 失败）",
                        failed->as_int().value_or(0));
                }
                // 延迟收敛：快速 update 把派发分析留给了收敛批，
                // 注入必须如实带上滞后事实（绝不静默）。
                const auto deferred =
                    update.doc->root().get("dispatchStaleFiles");
                if (deferred.has_value() && deferred->is_number()
                    && deferred->as_int().value_or(0) > 0)
                {
                    u.stale_note += std::format(
                        "，派发分析延后（滞后 {} 文件，ccg update "
                        "--drain 收敛）",
                        deferred->as_int().value_or(0));
                }
                // 项目文件联动结论（仅项目文件轮出现）。
                const auto refresh = update.doc->root().get("projectRefresh");
                if (refresh.has_value() && refresh->is_string())
                {
                    u.refresh_state = std::string{*refresh->as_string()};
                }
            }
            return u;
        }

        /** @brief 符号级注入的同文件文本证据（引用行与同名归属）。 */
        struct PtSymbolRefs
        {
            std::vector<TextReference> refs;
            std::vector<std::string>   same_name_owners;
            bool                       test_like = false;
        };

        /**
         * @brief 收集被编辑符号的同文件文本证据：引用行、同名多归属、
         *        gtest 用例判定。
         *
         * @internal 库内行号可能滞后于本次编辑：排除位（声明自身）按盘上
         *           实际声明行修正，多行声明的续行一并排除。
         */
        [[nodiscard]]
        auto collect_pt_refs(
            HookQueryDb&       db,
            const FileSymbol&  symbol,
            const EditInfo&    edit,
            std::int64_t       shown_line,
            const std::string& source_file,
            std::string_view   symbol_name,
            bool               field_like,
            bool               method_like,
            bool               update_queued) -> PtSymbolRefs
        {
            PtSymbolRefs out;
            // 名字以 `_Test` 结尾只是必要条件；只有磁盘声明行确实是
            // gtest 测试宏时才按“测试用例”措辞，避免误标手写类。
            out.test_like = looks_like_test_symbol(symbol);
            if (const auto text = read_text_file(source_file); text.has_value())
            {
                const auto        decl_lines = split_lines(*text);
                const std::size_t decl_line = static_cast<std::size_t>(
                    shown_line > 0 ? shown_line : symbol.line);
                if (out.test_like)
                {
                    out.test_like =
                        decl_line > 0 && decl_line <= decl_lines.size()
                        && is_test_macro_line(decl_lines[decl_line - 1]);
                }
                std::size_t exclude_line =
                    symbol.line > 0 ? static_cast<std::size_t>(symbol.line) : 0;
                if ((update_queued || edit.comment_only) && exclude_line > 0)
                {
                    // 库里的行号可能来自本次编辑之前：注释插入/删除
                    // 会把声明行整体挪动。按盘上实际声明行修正排除
                    // 位，否则符号自身的声明会被列成一条“引用”。
                    const std::size_t window =
                        3 + (edit.span.have_line ? edit.span.lines : 0);
                    if (const std::size_t actual = declaration_line_near(
                            *text,
                            symbol_name,
                            symbol.kind,
                            exclude_line,
                            window);
                        actual != 0)
                    {
                        exclude_line = actual;
                    }
                }
                // 只排除起始行会漏掉多行 enum/初始化器的续行（A#2 的
                // `Kind::None;`），必须排除整个声明区间。
                const std::size_t exclude_end = exclude_line != 0
                                                    ? declaration_range_end(
                                                          decl_lines,
                                                          exclude_line,
                                                          symbol.kind)
                                                    : 0;
                out.refs = same_file_reference_lines(
                    *text,
                    symbol_name,
                    exclude_line,
                    exclude_end,
                    4,
                    symbol.kind,
                    /*drop_declaration_like=*/edit.comment_only,
                    /*member_access_only=*/symbol.kind == "field");
            }
            // 同文件同名成员可能分属多个 owner（B#5 的两个
            // session_calls）；文本扫描不解析接收者类型，只能如实标注。
            if ((field_like || method_like) && !out.refs.empty()
                && !hook_deadline_reached(HOOK_QUERY_TIMEOUT))
            {
                const auto declarations = query_symbols(
                    db,
                    std::string{symbol_name},
                    basename_of(source_file));
                for (const FileSymbol& d : declarations)
                {
                    if (d.name != symbol_name || d.line <= 0
                        || path_compare_key(d.file)
                               != path_compare_key(source_file))
                    {
                        continue;
                    }
                    // 同族同档：字段侧看值样集合，方法侧看可调用全集
                    // （自由函数与方法同档——同文件同名调用不区分归属）。
                    const bool same_kind = field_like
                                               ? cg::db::is_value_like(d.kind)
                                               : cg::db::is_callable(d.kind);
                    if (!same_kind)
                    {
                        continue;
                    }
                    const std::string owner =
                        !d.parent.empty() ? d.parent : d.qualified_name;
                    if (!owner.empty()
                        && std::ranges::find(out.same_name_owners, owner)
                               == out.same_name_owners.end())
                    {
                        out.same_name_owners.push_back(owner);
                    }
                }
            }
            return out;
        }

        /**
         * @brief 把同文件引用行段追加进注入行（逐行标注引用性质并附短片段）。
         *
         * @internal 读者不会再把行号误当成计数，也不必回文件确认那一行
         *           到底是什么；同名多归属的接收者类型未解析须如实标注。
         */
        void append_pt_symbol_refs(
            std::string&                      line,
            const std::vector<TextReference>& refs,
            const std::vector<std::string>&   same_name_owners,
            const FileSymbol&                 symbol,
            bool                              field_like,
            bool                              method_like)
        {
            if (refs.empty())
            {
                // 未建模的边界已在 direct==0 的主句里说过一次，
                // 这里不再重复同一句限定。
                if (symbol.kind == "field")
                {
                    line = std::format("{}；同文件未发现成员访问", line);
                }
                else if (field_like)
                {
                    line = std::format("{}；同文件未发现文本引用", line);
                }
                return;
            }
            const auto append_owner_names = [&]()
            {
                std::size_t shown_owner = 0;
                for (const std::string& owner : same_name_owners)
                {
                    if (shown_owner != 0)
                    {
                        line += "、";
                    }
                    line += clip(owner, 32);
                    if (++shown_owner >= 3)
                    {
                        break;
                    }
                }
                if (same_name_owners.size() > shown_owner)
                {
                    line += std::format(" 等 {} 个", same_name_owners.size());
                }
            };
            if (symbol.kind == "field")
            {
                if (same_name_owners.size() > 1)
                {
                    line += std::format(
                        "；同文件同名成员访问（同名 {} 处：",
                        same_name_owners.size());
                    append_owner_names();
                    line = std::format(
                        "{}；接收者类型未解析，以下可能属于其它结构体）: ",
                        line);
                }
                else
                {
                    line = std::format(
                        "{}；同文件成员访问（.name/->name）: ",
                        line);
                }
            }
            else if (field_like)
            {
                line = std::format(
                    "{}；同文件同名标识符（可能含同名局部变量）: ",
                    line);
            }
            else if (method_like && same_name_owners.size() > 1)
            {
                // B#10：同文件多个 LoadPlugin，`runtime.LoadPlugin`
                // 属于另一个类型；文本扫描不解析接收者，如实标注。
                line += std::format(
                    "；同文件同名调用（同名 {} 处：",
                    same_name_owners.size());
                append_owner_names();
                line = std::format(
                    "{}；接收者类型未解析，以下可能属于其它类型）: ",
                    line);
            }
            else if (method_like)
            {
                // 即使本文件只声明了一个同名方法，`obj.Name(...)`
                // 的接收者仍可能指向别的类型（B#10 的 runtime.）。
                line = std::format(
                    "{}；同文件引用行（方法名；接收者类型未解析，可能属于其它"
                    "类型）: ",
                    line);
            }
            else
            {
                line = std::format("{}；同文件引用行: ", line);
            }
            for (std::size_t i = 0; i < refs.size(); ++i)
            {
                line += std::format(
                    "{}{}（{}",
                    i == 0 ? "" : "、",
                    refs[i].line,
                    reference_nature_label(refs[i].nature));
                if (i < 3 && !refs[i].snippet.empty())
                {
                    line = std::format("{}：{}", line, refs[i].snippet);
                }
                line = std::format("{}）", line);
            }
        }

        /**
         * @brief 追加字段所属类型的调用方代理段（类型级参考，非字段消费者）。
         *
         * @internal 字段的读取方不可直接解析；所属类型的调用方是一个诚实
         *           且常有用的代理（例如 IndexSummary 的读者）。预定位阶段
         *           已算好的结果直接复用：detach 之后派生查询只会撞上后台
         *           写者的 busy_timeout。
         */
        void append_pt_owner_proxy(
            std::string&                     line,
            HookQueryDb&                     db,
            const FileSymbol&                symbol,
            const std::string&               source_file,
            const std::optional<ImpactInfo>& pre_owner_impact,
            const std::optional<FileSymbol>& pre_symbol,
            bool                             field_like)
        {
            if (!field_like || symbol.parent.empty())
            {
                return;
            }
            std::optional<ImpactInfo> owner;
            if (pre_owner_impact.has_value() && pre_symbol.has_value()
                && pre_symbol->uid == symbol.uid)
            {
                owner = pre_owner_impact;
            }
            else if (!hook_deadline_reached(
                         HOOK_QUERY_TIMEOUT + HOOK_IMPACT_TIMEOUT))
            {
                owner = parent_record_impact(db, symbol, source_file);
            }
            if (owner.has_value() && owner->ok && owner->direct > 0)
            {
                line += std::format(
                    "；所属类型 {} 的调用方 {}（直接 {}；类型级参考，非字段消费"
                    "者）",
                    clip(symbol.parent, 48),
                    owner->impacted,
                    owner->direct);
                append_consumers(
                    line,
                    owner->direct_symbols,
                    source_file,
                    5,
                    "；类型级消费者: ");
                if (owner->direct > 5)
                {
                    line += std::format(" 等 {} 项", owner->direct);
                }
            }
        }

        /**
         * @brief PostToolUse 处理器：刷新被编辑文件的库，再把该文件的影响面
         *        浓缩为一行有界上下文。
         *
         * 每个跳过与失败路径都静默（零输出、退出 0）——钩子绝不可使它观察的
         * 工具调用失败。
         *
         * @internal
         * 手工控制台运行时绝不吊在 stdin 上；此处尚无索引时不作声（技能里
         * 教了 `ccg index`）。第一步把编辑折入库：`ccg update` 经库协调队列
         * 入队并小批排空；活持有者在场时这是一次仍把文件留在队列里的空操作。
         * 第二步从 tool_input/tool_response 还原编辑落点（structuredPatch 的
         * 新增行优先），在落点附近按标识符 token 查到被编辑符号，再经
         * `ccg impact --direction up` 报它自己的上游影响面（直接层优先）。
         * 只有定位失败或同名重载无法唯一解析时，才退回文件级 `ccg changes
         * --depth 1`——整文件种子集与传递扇出正是旧文本 CRITICAL 噪音的主因。
         * 纯注释/空白编辑会显式标注“符号契约未变”。未被本库跟踪的文件无言
         * 可发。
         */
        auto hook_posttooluse(const Invocation& inv) -> int
        {
            try
            {
                if (!cg::util::platform::stdin_is_piped())
                {
                    return 0;
                }
                const std::string input(
                    std::istreambuf_iterator<char>{std::cin},
                    {});
                const json::Document doc = json::Document::parse(input);
                const auto           root = doc.root();
                const auto           tool = root.get("tool_name");
                if (!tool.has_value() || !tool->is_string())
                {
                    return 0;
                }
                const auto tool_name = tool->as_string();
                // 工具名白名单与 HOOK_SPECS 的 matcher 镜像。Update 载荷
                // 同为 file_path。
                if (!tool_name.has_value()
                    || (*tool_name != "Edit" && *tool_name != "Write"
                        && *tool_name != "MultiEdit" && *tool_name != "Update"))
                {
                    return 0;
                }
                const auto tool_input = root.get("tool_input");
                if (!tool_input.has_value() || !tool_input->is_object())
                {
                    return 0;
                }
                const auto file_member = tool_input->get("file_path");
                if (!file_member.has_value() || !file_member->is_string())
                {
                    return 0;
                }
                const auto file_value = file_member->as_string();
                // 语料文件与项目文件都放行：前者走符号影响面注入，后者只入队
                // 触发重采集联动（见下方 project_file 分支）——项目文件的编辑
                // 改的是语料来源（TU 清单/包含路径），不在库里产生任何符号；
                // 其余一切（配置、文档、脚本、锁文件）照旧静默。
                if (!file_value.has_value() || file_value->empty()
                    || file_value->find('"') != std::string_view::npos
                    || (!is_corpus_file(*file_value)
                        && !cg::util::is_project_file(*file_value)))
                {
                    return 0;
                }
                const std::string file{*file_value};
                // 项目文件（.sln/.vcxproj/.props/...）：编辑改的是语料来源，
                // 不产生符号——影响面查询全部跳过，注入只报联动事实（下方
                // project_file 分支）。
                const bool project_file = cg::util::is_project_file(file);
                const auto choice = hook_resolved_db(inv);
                if (!choice.has_value())
                {
                    return 0;
                }
                const std::vector<std::string> db_args{
                    "-o",
                    choice->root,
                    "--config",
                    choice->config};

                const auto     tool_response = root.get("tool_response");
                const EditInfo edit = extract_edit_info(
                    *tool_input,
                    tool_response,
                    file,
                    *tool_name);

                // 快速标脏（需求 1）：活租约在场就只入队写 journal 立即收工；
                // 探测失败宁可诚实标脏也不赌一次长 update。入队前先过
                // 文件级门：语料面外的编辑当场拒收，工程文件走联动通道
                // （细节见 pt_fast_dirty 与 corpus_gate_pass）。
                const std::string norm = normalize_hook_path(file);
                if (pt_fast_dirty(
                        choice->db,
                        file,
                        norm,
                        edit.span,
                        *choice,
                        project_file))
                {
                    return 0;
                }

                // 进程内直连的混合形态：本次处理器的全部查询（定位/探
                // 针/枚举/影响面/
                // changes 回退/digest 重算）共用这一条进程内会话；db_args 只剩
                // update 子进程还在用（写锁隔离 + 超时撒手保持子进程形态）。
                HookQueryDb qdb = open_hook_query_db(choice->db);

                // 无活租约：机会式汇报积压——批次刚结束而会话仍在工具循环时，
                // 下一次编辑即可看到结论，不必等用户输入。它与本次编辑的常规
                // 影响面行合并进同一个 1200B 信封（积压上限 900B）。
                std::optional<DigestFlushResult> digest = hook_digest_flush(
                    *choice,
                    FlushMode::Opportunistic,
                    DIGEST_PTU_BUDGET,
                    &qdb);
                // 纯注释编辑不动符号契约：趁 update 尚未占用数据库先完成预定位
                // 与影响面/所属类型/族视图的预计算，detach 分支免查询（细节见
                // pt_prelocate）。
                PtPreLocate pre;
                if (!project_file)
                {
                    pre = pt_prelocate(qdb, file, edit);
                }
                const std::optional<FileSymbol>& pre_symbol = pre.symbol;
                const std::vector<FileSymbol>&   pre_same_line = pre.same_line;
                const std::optional<ImpactInfo>& pre_impact = pre.impact;
                const std::optional<ImpactInfo>& pre_merged_impact =
                    pre.merged_impact;
                const std::optional<ImpactInfo>& pre_owner_impact =
                    pre.owner_impact;

                // 联动结论（update --json 的 projectRefresh）：captured / busy
                // / failed / untracked；空 = 本轮没触发（普通 TU 批或超时
                // detach）。
                const PtUpdate upd = pt_run_update(inv, file, db_args);
                std::string    stale_note = std::move(upd.stale_note);
                std::string    refresh_state = std::move(upd.refresh_state);
                const bool     update_queued = upd.queued;
                const bool     update_background = upd.background;
                const bool     update_started = upd.started;
                const bool     update_fallback_queued = upd.fallback_queued;

                std::string line;
                if (!update_started)
                {
                    // 诚实提示：更新没有启动，绝不写"已入库"。入队兜底成功时
                    // 说明下一次刷新会补上，失败时连这句也不承诺。
                    line = std::format(
                        "[ccg] {} 未能启动索引更新{}（影响面基于编辑前索引）",
                        clip_tail(display_path(file), 72),
                        update_fallback_queued ? "，已入队待下次刷新" : "");
                }
                else if (update_background)
                {
                    // 只有预定位阶段真的算过影响面，才能说"基于既有索引"；
                    // 预算连预计算都没留下时如实写"未查询"，避免暗示下面
                    // 会有一份省略了的影响面。
                    line = std::format(
                        "[ccg] 已记录 {}（索引更新在后台继续；{}）",
                        clip_tail(display_path(file), 72),
                        (pre_impact.has_value() && pre_impact->attempted)
                            ? "影响面基于既有索引"
                            : "影响面未查询（索引更新占用，已跳过）");
                }
                else
                {
                    line = std::format(
                        "[ccg] {} {}",
                        update_queued ? "已记录" : "已入库",
                        clip_tail(display_path(file), 72));
                }
                if (!stale_note.empty())
                {
                    line += std::format("（{}）", stale_note);
                }
                // 排队状态是校验事实，不是影响面数据；压到行尾，避免挤占开头
                // 最需要被读到的符号与计数。后台继续的提示已在首行写明，不重复。
                const auto emit_line = [&](std::string text)
                {
                    if (update_started && update_queued && !update_background
                        && !project_file)
                    {
                        text = std::format(
                            "{}；影响面基于编辑前索引（更新排队）",
                            text);
                    }
                    const std::size_t digest_len =
                        digest.has_value() ? digest->text.size() : 0;
                    const std::size_t normal_max =
                        POST_TOOL_CONTEXT_BUDGET > digest_len + (digest ? 1 : 0)
                            ? POST_TOOL_CONTEXT_BUDGET - digest_len
                                  - (digest ? 1 : 0)
                            : 0;
                    std::string context = clip_injection(text, normal_max);
                    if (digest.has_value())
                    {
                        context = std::format("{}\n", context);
                        context += digest->text;
                    }
                    if (context.size() > POST_TOOL_CONTEXT_BUDGET)
                    {
                        context =
                            clip_at_most(context, POST_TOOL_CONTEXT_BUDGET);
                    }
                    emit_hook_context("PostToolUse", context);
                    // 先认领、后发送、再删除：到这里注入已写出，claimed 文件
                    // 才可以消失。发送前崩溃只会丢一轮，绝不重复。
                    if (digest.has_value())
                    {
                        digest_flush_done(*digest);
                    }
                };

                // 项目文件分支：符号定位/影响面/同文件引用对项目文件全部无意
                // 义（它们不在库里、没有符号）；注入只报联动事实。文案按联动
                // 结论区分：captured（语料已刷新）、busy（重采集被互斥/未启动/
                // 超时让行，本事件留队列）、failed（采集失败，库保持上次完整
                // 语料）、untracked（无注册表联动，按未跟踪出队）、以及超时
                // detach（refresh_state 为空但 update 仍在后台跑）。
                if (project_file)
                {
                    if (!update_started)
                    {
                        // update 派生失败：上方通用失败文案已构造（含兜底入队
                        // 状态），再点明这是项目文件、刷新未发生。
                        line += "；项目文件（工程重采集未能启动，语料未刷新）";
                    }
                    else if (refresh_state == "captured")
                    {
                        line = std::format(
                            "[ccg] {}（项目文件；工程已重采集，新 TU/移除 TU 已"
                            "随增量索引入库）",
                            clip_tail(display_path(file), 72));
                    }
                    else if (refresh_state == "failed")
                    {
                        line = std::format(
                            "[ccg] {}（项目文件；工程重采集失败，库保持上次完整"
                            "语料，下次编辑自动重试）",
                            clip_tail(display_path(file), 72));
                    }
                    else if (refresh_state == "busy")
                    {
                        line = std::format(
                            "[ccg] 已记录 {}（项目文件；另一采集正在进行，本事件"
                            "已入队待收敛）",
                            clip_tail(display_path(file), 72));
                    }
                    else if (refresh_state == "untracked")
                    {
                        line = std::format(
                            "[ccg] {}（项目文件；本库无 msbuild 注册表，按未跟踪"
                            "处理——如需联动请先 `ccg msbuild <solution>`）",
                            clip_tail(display_path(file), 72));
                    }
                    else
                    {
                        // 超时 detach（或占位轮）：重采集在后台继续。
                        line = std::format(
                            "[ccg] 已记录 {}（项目文件；工程重采集与增量索引在"
                            "后台进行）",
                            clip_tail(display_path(file), 72));
                    }
                    emit_line(std::move(line));
                    return 0;
                }

                // `ccg changes` 的进程内直连（懒加载）一次取两样东西：库内规范
                // 路径（`query --file` 的路径过滤是大小写敏感的，而 Claude Code
                // 传来的 file_path 可能是模型自行拼的小写形态）和文件级回退所
                // 需的消费者——主路径失败或走到回退时才真正需要。
                std::optional<HookChanges> changes_result;
                bool                       changes_tried = false;
                const auto                 fetch_changes =
                    [&]() -> const std::optional<HookChanges>&
                {
                    if (!changes_tried)
                    {
                        changes_tried = true;
                        if (update_background
                            || hook_deadline_reached(HOOK_CHANGES_TIMEOUT))
                        {
                            return changes_result; // 截止/后台写者优先：放弃回退
                        }
                        changes_result = hook_changes(qdb, file);
                    }
                    return changes_result;
                };

                // 符号级主路径：报告被编辑符号自己的上游影响面，而不是整文件
                // 全部符号的扇出。纯注释编辑另有标注（符号契约未变）。
                std::vector<FileSymbol>   same_line;
                std::optional<FileSymbol> symbol;
                if (pre_symbol.has_value())
                {
                    // 纯注释编辑的符号契约未变：update 前定位到的符号就是答案，
                    // 不再重复查询。这同时覆盖 detach 分支（后台写者会让新查询
                    // 阻塞）与"update 用满预算、截止余量已不够再查询"的分支。
                    symbol = pre_symbol;
                    same_line = pre_same_line;
                }
                else
                {
                    symbol = locate_edited_symbol(qdb, file, edit, &same_line);
                }
                if (!symbol.has_value() && !update_background)
                {
                    const auto& changes = fetch_changes();
                    if (changes.has_value() && !changes->canonical.empty()
                        && basename_of(changes->canonical) != basename_of(file))
                    {
                        same_line.clear();
                        symbol = locate_edited_symbol(
                            qdb,
                            changes->canonical,
                            edit,
                            &same_line);
                    }
                }
                if (symbol.has_value())
                {
                    // 纯注释编辑只需要本符号自身的影响面：虚方法默认的族合并
                    // 会把整个覆盖族的调用方写成"直接消费者"，与本次编辑无关。
                    // 预定位阶段已经算好的结果优先；否则在截止余量内现查。
                    // detach 本身不再是跳过查询的理由——预计算结果已就绪时，
                    // 后台写者根本不会被碰到。
                    ImpactInfo impact;
                    if (pre_impact.has_value() && pre_symbol.has_value()
                        && pre_symbol->uid == symbol->uid)
                    {
                        impact = pre_impact.value();
                    }
                    else if (!hook_deadline_reached(HOOK_IMPACT_TIMEOUT))
                    {
                        impact = symbol_impact(
                            qdb,
                            *symbol,
                            /*merge_family=*/!edit.comment_only);
                    }
                    // 非注释编辑保留族合并视图（契约改动确实波及族内实现），
                    // 但另取一次 no-merge
                    // 计数，避免把族合计写成自身直接消费者。
                    std::int64_t self_direct = -1;
                    if (!edit.comment_only && impact.ok
                        && impact.family_size > 1
                        && !hook_deadline_reached(HOOK_IMPACT_TIMEOUT))
                    {
                        if (const ImpactInfo own =
                                symbol_impact(qdb, *symbol, false);
                            own.ok)
                        {
                            self_direct = own.direct;
                        }
                    }
                    const std::string shown_file =
                        symbol->file.empty() ? file : symbol->file;
                    // 库内行号可能滞后于本次编辑；盘上声明行更可信。纯注释编辑
                    // 会让声明整体下移，即使更新已执行也要按磁盘校正，否则注入
                    // 会展示旧行号并把自己的声明当成引用。
                    std::int64_t shown_line = symbol->line;
                    if ((update_queued || edit.comment_only)
                        && symbol->line > 0)
                    {
                        if (const auto text = read_text_file(shown_file);
                            text.has_value())
                        {
                            const std::string_view name =
                                symbol->name.empty()
                                    ? std::string_view{symbol->qualified_name}
                                    : std::string_view{symbol->name};
                            const std::size_t window =
                                3 + (edit.span.have_line ? edit.span.lines : 0);
                            if (const std::size_t actual =
                                    declaration_line_near(
                                        *text,
                                        name,
                                        symbol->kind,
                                        static_cast<std::size_t>(symbol->line),
                                        window);
                                actual != 0)
                            {
                                shown_line = static_cast<std::int64_t>(actual);
                            }
                        }
                    }
                    line += std::format(
                        "；被编辑符号 {}（{}，{}:{}）",
                        clip_display_symbol(symbol->qualified_name, 80),
                        symbol->kind,
                        clip(basename_of(shown_file), 40),
                        shown_line);
                    if (!same_line.empty())
                    {
                        line = std::format("{}；同线符号: ", line);
                        std::size_t extra = 0;
                        for (const FileSymbol& sibling : same_line)
                        {
                            if (extra != 0)
                            {
                                line += "、";
                            }
                            line += clip(
                                sibling.name.empty() ? sibling.qualified_name
                                                     : sibling.name,
                                40);
                            line += std::format("（{}）", sibling.kind);
                            if (++extra >= 5)
                            {
                                break;
                            }
                        }
                        if (same_line.size() > extra)
                        {
                            line += std::format(" 等 {} 个", same_line.size());
                        }
                    }
                    if (edit.comment_only)
                    {
                        // 只在此处说一次：注释/空白编辑不动契约，风险也不变。
                        line = std::format(
                            "{}；仅注释/空白改动（符号契约不变，风险: 不变）",
                            line);
                    }
                    const bool field_like = cg::db::is_value_like(symbol->kind);
                    const bool method_like =
                        cg::db::is_method_like(symbol->kind);
                    const bool type_like = cg::db::is_type_like(symbol->kind);
                    const std::string source_file =
                        symbol->file.empty() ? file : symbol->file;
                    const std::string_view symbol_name =
                        symbol->name.empty()
                            ? std::string_view{symbol->qualified_name}
                            : std::string_view{symbol->name};
                    // 同文件文本证据（引用行/同名多归属/gtest 判定）整体在
                    // collect_pt_refs 收集。
                    const PtSymbolRefs srefs = collect_pt_refs(
                        qdb,
                        *symbol,
                        edit,
                        shown_line,
                        source_file,
                        symbol_name,
                        field_like,
                        method_like,
                        update_queued);
                    const bool test_like = srefs.test_like;
                    const std::vector<TextReference>& refs = srefs.refs;
                    const std::vector<std::string>&   same_name_owners =
                        srefs.same_name_owners;
                    if (!impact.ok)
                    {
                        if (!impact.attempted)
                        {
                            // 没查询与查询失败是两件事：预算/背景更新让它没
                            // 发生，就不能写成"无法精确解析"。
                            line = std::format(
                                "{}；影响面未查询（索引更新占用，已跳过）",
                                line);
                            append_pt_symbol_refs(
                                line,
                                refs,
                                same_name_owners,
                                *symbol,
                                field_like,
                                method_like);
                            append_pt_owner_proxy(
                                line,
                                qdb,
                                *symbol,
                                source_file,
                                pre_owner_impact,
                                pre_symbol,
                                field_like);
                            emit_line(std::move(line));
                            return 0;
                        }
                        // 同名重载/名字歧义时 impact 绑定不到 uid。文件级
                        // `changes` 报出的可能是整文件数百个消费者（B#1 的
                        // 498），那不是这个符号的消费者；宁可不给大数。
                        line = std::format("{}；符号级影响无法精确解析", line);
                        if (!impact.reason.empty())
                        {
                            line += std::format("（{}）", impact.reason);
                        }
                        line = std::format(
                            "{}；已省略文件级消费者（文件级计数不代表该"
                            "符号）",
                            line);
                        append_pt_symbol_refs(
                            line,
                            refs,
                            same_name_owners,
                            *symbol,
                            field_like,
                            method_like);
                        append_pt_owner_proxy(
                            line,
                            qdb,
                            *symbol,
                            source_file,
                            pre_owner_impact,
                            pre_symbol,
                            field_like);
                        emit_line(std::move(line));
                        return 0;
                    }
                    else if (impact.direct == 0)
                    {
                        // 数字与边界说明各只出现一次：字段的“0”必须紧跟未建模
                        // 说明，否则会被读成“该字段无人使用”；其余类型同理，
                        // 不先写一个孤立的“直接 0，传递 0”再补一句解释。
                        if (test_like)
                        {
                            // gtest 用例由测试框架注册，图内 0 外部消费者是
                            // 构造使然；如实说明，避免读者把它当成遗漏。
                            line = std::format(
                                "{}；上游消费者 0（直接 0）；该符号是 gtest "
                                "测试用例（仅由测试框架注册，图内无外部消费"
                                "者属预期）",
                                line);
                        }
                        else if (field_like)
                        {
                            // 图只记录 calls 与类型 uses；字段/变量的读写没有
                            // 边。这里如实说明未建模，而不是断言“无消费者”。
                            line = std::format(
                                "{}；上游消费者 0（直接 0；图未建模字段读取，"
                                "uses 只指向类型声明，不能据此断言无消费者）",
                                line);
                        }
                        else if (method_like)
                        {
                            // 接口方法、虚函数与导出符号的调用者常在图外；
                            // 只能陈述“图内无调用边”，不能断言无人调用。
                            line = std::format(
                                "{}；上游消费者 0（直接 0；图内无调用边，虚派"
                                "发、取址与接口/导出消费者未建模，不能据此"
                                "断言无消费者）",
                                line);
                        }
                        else if (symbol->kind == "enum")
                        {
                            // uses 边只接纳 record/struct/union（extract 侧
                            // is_record_kind），枚举的入边恒为 0；不能写成
                            // “uses 已计入”来断言无使用。
                            line = std::format(
                                "{}；上游消费者 0（直接 0；图未建模枚举作为"
                                "类型的 uses，uses 只指向 record，不能据此断"
                                "言无使用）",
                                line);
                        }
                        else if (type_like)
                        {
                            line = std::format(
                                "{}；上游消费者 0（直接 0；类型 uses 已计入，"
                                "模板实例化/文本引用未建模）",
                                line);
                        }
                        else
                        {
                            line = std::format(
                                "{}；上游消费者 0（直接 0；同文件 CALLS 已计"
                                "入，取址/字段访问未建模）",
                                line);
                        }
                        bool refs_have_call = false;
                        for (const TextReference& ref : refs)
                        {
                            if (ref.nature == ReferenceNature::Call)
                            {
                                refs_have_call = true;
                                break;
                            }
                        }
                        if (method_like && refs_have_call)
                        {
                            // 调用点就在同文件，但图里没有到本符号的边：这是
                            // 接口/虚派发解析边界，不是“无人调用”。
                            line = std::format(
                                "{}；文本调用点未解析到本符号（接口/虚派发）"
                                "，图内 0 不代表无人调用",
                                line);
                        }
                        if (method_like && edit.comment_only)
                        {
                            // 纯注释编辑默认用 --no-merge；虚方法/接口实现的
                            // 消费者经基类派发，自身直接边为 0。用预定位阶段
                            // 算好的族视图，使 detach
                            // 后也能看到“接口契约消费者”。
                            std::optional<ImpactInfo> merged =
                                pre_merged_impact;
                            if (!merged.has_value()
                                && !hook_deadline_reached(HOOK_IMPACT_TIMEOUT))
                            {
                                merged = symbol_impact(qdb, *symbol, true);
                            }
                            if (merged.has_value() && merged->ok
                                && merged->direct > 0)
                            {
                                line += std::format(
                                    "；虚函数族/接口派发视角直接消费者 {}（经基类"
                                    "派发；本符号自身直接 0）",
                                    merged->direct);
                                append_consumers(
                                    line,
                                    merged->direct_symbols,
                                    source_file,
                                    5,
                                    "；族消费者: ");
                            }
                        }
                        append_pt_symbol_refs(
                            line,
                            refs,
                            same_name_owners,
                            *symbol,
                            field_like,
                            method_like);
                        append_pt_owner_proxy(
                            line,
                            qdb,
                            *symbol,
                            source_file,
                            pre_owner_impact,
                            pre_symbol,
                            field_like);
                        emit_line(std::move(line));
                        return 0;
                    }
                    else if (edit.comment_only)
                    {
                        // 注释/空白编辑不动契约：传递扇出对本次编辑没有行动
                        // 价值，只报直接消费者，也不带截断提示。
                        line += std::format("；直接消费者 {}", impact.direct);
                        append_consumers(line, impact.direct_symbols, file, 5);
                        if (impact.direct > 5)
                        {
                            line += std::format(" 等 {} 项", impact.direct);
                        }
                        emit_line(std::move(line));
                        return 0;
                    }
                    else
                    {
                        // 深度 3 被截断时 impacted/transitive 只是下界，不能
                        // 写成精确计数。虚函数族合并必须标注：合并后的 direct
                        // 不是本符号自身的直接消费者数。
                        std::string family_note;
                        if (impact.family_size > 1)
                        {
                            family_note = std::format(
                                "，含虚函数家族 {} 个实现",
                                impact.family_size);
                            if (self_direct == 0)
                            {
                                family_note = std::format(
                                    "{}；本符号自身直接 0",
                                    family_note);
                            }
                            else if (self_direct > 0)
                            {
                                family_note += std::format(
                                    "；本符号自身直接 {}",
                                    self_direct);
                            }
                        }
                        line += std::format(
                            "；上游消费者 {}{}（直接 {}{}，传递 {}）",
                            impact.truncated ? "≥" : "",
                            impact.impacted,
                            impact.direct,
                            family_note,
                            impact.transitive);
                        line += std::format(
                            "，直接风险 {}",
                            direct_risk_name(impact.direct, impact.fan_in));
                        if (impact.truncated)
                        {
                            line = std::format("{}（深度 3 内）", line);
                        }
                        if (impact.dispatch_stale > 0)
                        {
                            line += std::format(
                                "；派发分析滞后 {} 文件（虚调用计数含旧数据，ccg "
                                "update --drain 收敛）",
                                impact.dispatch_stale);
                        }
                        append_consumers(line, impact.direct_symbols, file, 5);
                        if (impact.direct > 5)
                        {
                            line += std::format(" 等 {} 项", impact.direct);
                        }
                        emit_line(std::move(line));
                        return 0;
                    }
                }

                // 文件级回退：未能定位符号，或同名重载令符号级计数不可用。
                // 深度 1 只取直接消费者——传递扇出正是旧文本噪音的主因。
                const auto& changes = fetch_changes();
                if (!changes.has_value())
                {
                    // 回退查询失败（后台写者/截止）时仍要说话：更新本身已经
                    // 给出可核查的事实（已入库 / 排队 / 后台继续），吞掉它
                    // 只会让读者把 hook 失效与"无影响面"混为一谈。符号定位
                    // 成功时 line 里已带符号；失败时至少保留首行状态。
                    emit_line(std::move(line));
                    return 0;
                }
                if (!changes->any_known)
                {
                    // 新建/未跟踪文件此前完全静默；一行说明比“无输出”更有用，
                    // 也避免读者把缺失当成 hook 失效。
                    line = std::format(
                        "{}；本文件未被索引（新建或不在 compile_commands），"
                        "暂无影响面",
                        line);
                    emit_line(std::move(line));
                    return 0;
                }
                if (!symbol.has_value())
                {
                    line = std::format("{}；未能从本次编辑定位符号", line);
                }
                const std::int64_t count =
                    static_cast<std::int64_t>(changes->consumers.size());
                if (count == 0)
                {
                    line += "；文件级直接消费者 0（可能仅测试内部/未解析调用；"
                            "同文件调用者见 depth 1）";
                }
                else
                {
                    line +=
                        std::format("；文件级直接消费者 {}（depth 1）", count);
                    line = std::format("{}；最相关: ", line);
                    std::size_t shown = 0;
                    for (std::size_t i = 0;
                         i < changes->consumers.size() && shown < 3;
                         ++i)
                    {
                        const HookChanges::Consumer& consumer =
                            changes->consumers[i];
                        if (consumer.qualified_name.empty())
                        {
                            continue;
                        }
                        if (shown != 0)
                        {
                            line += "、";
                        }
                        line +=
                            clip_display_symbol(consumer.qualified_name, 60);
                        // 旧 JSON 的 file/line 与 qualified_name 同条件在场（行
                        // 物化成功即三字段齐写），此处非空名字即同此条件。
                        line += std::format(
                            "（定义于 {}:{}）",
                            clip(basename_of(consumer.file), 32),
                            consumer.line);
                        ++shown;
                    }
                    if (count > 3)
                    {
                        line += std::format(" 等 {} 项", count);
                    }
                }
                emit_line(std::move(line));
                return 0;
            }
            catch (const std::exception&)
            {
                return 0;
            }
        }

        /**
         * @brief UserPromptSubmit 处理器：用户下一次输入前汇报积压的批次结论。
         *
         * @internal
         * 官方契约：该事件支持 `hookSpecificOutput.additionalContext`，且 exit
         * 0 的纯文本 stdout 也会进上下文——因此静默路径必须零 stdout，诊断只走
         * stderr。批次仍在跑时 handler 毫秒级返回（需求 5）。
         */
        auto hook_userpromptsubmit(const Invocation& inv) -> int
        {
            try
            {
                // 手工控制台运行时绝不能吊在 stdin 上；宿主的 UserPromptSubmit
                // 输入与本 hook 无关，但必须读空管道，避免宿主写端阻塞。
                if (!cg::util::platform::stdin_is_piped())
                {
                    return 0;
                }
                const std::string input(
                    std::istreambuf_iterator<char>{std::cin},
                    {});
                (void)input;
                const auto choice = hook_resolved_db(inv);
                if (!choice.has_value())
                {
                    return 0;
                }
                const auto digest =
                    hook_digest_flush(*choice, FlushMode::Poll, DIGEST_BUDGET);
                if (!digest.has_value() || digest->text.empty())
                {
                    // 收敛时机：用户下一次输入前，若快速批留下了未收敛的
                    // 派发滞后（队列已空、无活租约），后台派生一次 --drain。
                    // flush 没汇报结论不代表库干净——journal 目录可以不存在。
                    maybe_detach_dispatch_drain(*choice);
                    return 0; // 静默路径：零 stdout（exit 0
                              // 的文本也会进上下文）
                }
                // 诊断只许写 stderr：UserPromptSubmit 的 stdout 会被宿主当作
                // 上下文注入，任何调试打印都会变成垃圾。
                emit_hook_context("UserPromptSubmit", digest->text);
                digest_flush_done(*digest);
                maybe_detach_dispatch_drain(*choice);
                return 0;
            }
            catch (const std::exception&)
            {
                return 0;
            }
        }

        /**
         * @brief "YYYY-MM-DDTHH:MM:SSZ" 距当前 UTC 的小时数（解析失败返回空）。
         *        库内 indexedAt 的写入口径即该格式（cli.render 侧 set）。
         */
        [[nodiscard]]
        auto iso8601_age_hours(std::string_view iso)
            -> std::optional<std::int64_t>
        {
            // from_stream 一把解析六字段并逐字核对分隔符，ispanstream
            // 直接吃 string_view（不再经 std::string 中转拷贝）。MSVC
            // 实测（14.51）会把 2 月 30 日这类日历上不存在的日子静默
            // 规范化成下月日期而不是置失败，所以解析成功后再用 format
            // 还原比对：规范化必然对不上原串，往返一致顶替 ymd.ok()
            // 的日历合法性检查。
            std::chrono::sys_seconds indexed{};
            std::ispanstream         in{
                std::span<const char>{iso.data(), iso.size()}};
            if (!std::chrono::from_stream(
                    in,
                    "%Y-%m-%dT%H:%M:%SZ",
                    indexed))
            {
                return std::nullopt;
            }
            char trailing = '\0';
            if (in.get(trailing))
            {
                // "Z" 之后还有字符：不是该口径的时间戳。
                return std::nullopt;
            }
            if (std::format("{:%Y-%m-%dT%H:%M:%SZ}", indexed) != iso)
            {
                // 静默规范化留下的痕迹：按解析失败处理，不产出假
                // 新鲜度（陈旧警示宁可缺省不可误报）。
                return std::nullopt;
            }
            const auto epoch =
                static_cast<long long>(indexed.time_since_epoch().count());
            if (epoch < 0)
            {
                // 1970 前的时间戳不参与新鲜度判定（与旧实现的
                // y < 1970 同判失败）。
                return std::nullopt;
            }
            const auto now = static_cast<long long>(std::time(nullptr));
            const auto diff_s = now - epoch;
            return diff_s / 3600
                   + ((diff_s % 3600 + 3600) % 3600 >= 1800 ? 1 : 0);
        }

        /** @brief 小时数的中文时段表述（SessionStart 陈旧分支用）。 */
        [[nodiscard]]
        auto age_phrase(std::int64_t hours) -> std::string
        {
            if (hours < 1)
            {
                return "1 小时前";
            }
            if (hours < 48)
            {
                return std::format("{} 小时前", hours);
            }
            return std::format("{} 天前", hours / 24);
        }

        /** @brief PATH 里是否存在 ccg.exe（命中则注入短写 "ccg"）。 */
        [[nodiscard]]
        auto ccg_on_path() -> bool
        {
            const auto path_env = cg::util::cstd::get_env("PATH");
            if (!path_env.has_value() || path_env->empty())
            {
                return false;
            }
            std::error_code ec;
            std::size_t     at = 0;
            while (at <= path_env->size())
            {
                const std::size_t      semi = path_env->find(';', at);
                const std::string_view dir =
                    std::string_view{*path_env}.substr(at, semi - at);
                if (!dir.empty() && fs::exists(fs::path{dir} / "ccg.exe", ec)
                    && !ec)
                {
                    return true;
                }
                if (semi == std::string_view::npos)
                {
                    break;
                }
                at = semi + 1;
            }
            return false;
        }

        /**
         * @brief SessionStart 处理器：两行有界的库状态（就绪+新鲜度、CLI 可达
         *        与场景映射），使会话一开始就知道代码图在场与何时该用它。
         *        工作上下文无库时静默。
         *
         * @internal 文案要点：
         * - 不放"N 符号/N 边/N 文件"仪表盘数字与"常用查询 query/context/impact"
         *   泛点名——泛点名会把会话带偏去跑 impact 向上视图（答非所问的
         *   家族枚举），数字对模型无行动价值；
         * - 需求表述与"同名消歧/找调用者/影响面/调用链"场景高度重合，故采用
         *   场景→命令精确映射，只点名确定正确的命令；
         * - CLI 路径取自身 exe（模型此前要翻 settings.json 挖路径才调得通）；
         *   PATH 里探测到 ccg.exe 时只写 "ccg"。
         */
        auto hook_sessionstart(const Invocation& inv) -> int
        {
            try
            {
                const auto choice = hook_resolved_db(inv);
                if (!choice.has_value())
                {
                    return 0;
                }
                const std::vector<std::string> db_args{
                    "-o",
                    choice->root,
                    "--config",
                    choice->config};
                std::vector<std::string> args{"stats", "--json"};
                args.insert(args.end(), db_args.begin(), db_args.end());
                auto stats = run_child_json(args);
                if (!stats.has_value())
                {
                    return 0;
                }
                // 新鲜度：无 pending 且索引时间在 24h 内视为最新；陈旧时给
                // "行号可能漂移"警示与收敛命令（此时点名 update 是对的）。
                std::int64_t pending = 0;
                if (const auto files = stats->root().get("files");
                    files.has_value() && files->is_object())
                {
                    if (const auto v = files->get("pending");
                        v.has_value() && v->is_number())
                    {
                        pending = v->as_int().value_or(0);
                    }
                }
                std::optional<std::int64_t> age_hours;
                if (const auto db = stats->root().get("db");
                    db.has_value() && db->is_object())
                {
                    if (const auto at = db->get("indexedAt");
                        at.has_value() && at->is_string())
                    {
                        age_hours = iso8601_age_hours(*at->as_string());
                    }
                }
                std::string line = "[ccg] 代码图就绪";
                const bool  fresh =
                    pending == 0 && age_hours.has_value() && *age_hours <= 24;
                if (fresh)
                {
                    line = std::format("{}，索引最新。", line);
                }
                else if (age_hours.has_value())
                {
                    line += std::format(
                        "，索引于 {}（行号可能漂移，必要时 "
                        "ccg update --drain）。",
                        age_phrase(*age_hours));
                }
                else
                {
                    line =
                        std::format("{}（索引时间未知，行号可能漂移）。", line);
                }
                // CLI 可达：自身 exe 路径；PATH 命中时短写。
                std::string cli = "ccg";
                if (!ccg_on_path())
                {
                    const fs::path exe = cg::util::platform::executable_path();
                    if (!exe.empty())
                    {
                        cli = exe.string();
                    }
                }
                line += std::format("CLI：{}。", cli);
                const std::string context_map =
                    "[ccg] 同名消歧/找调用者/影响面/调用链 → ccg "
                    "query/callers/impact/trace（--help 详见）";
                std::string status = std::format("{}\n{}", line, context_map);
                if (status.size() > SESSION_START_CONTEXT_BUDGET)
                {
                    status = clip_at_most(status, SESSION_START_CONTEXT_BUDGET);
                }
                // 兜底通道：只清崩溃/会话结束残留，不承担实时性；预算服从既有
                // 600B（状态与场景映射两行之后剩下的字节）。
                std::optional<DigestFlushResult> digest;
                if (status.size() + 1 < SESSION_START_CONTEXT_BUDGET)
                {
                    digest = hook_digest_flush(
                        *choice,
                        FlushMode::Poll,
                        SESSION_START_CONTEXT_BUDGET - status.size() - 1);
                }
                std::string context = status;
                if (digest.has_value())
                {
                    context = std::format("{}\n", context);
                    context += digest->text;
                }
                if (context.size() > SESSION_START_CONTEXT_BUDGET)
                {
                    context =
                        clip_at_most(context, SESSION_START_CONTEXT_BUDGET);
                }
                emit_hook_context("SessionStart", context);
                if (digest.has_value())
                {
                    digest_flush_done(*digest);
                }
                // 会话开始的收敛时机与 UserPromptSubmit 同理（上个会话遗留的
                // 快速批滞后在会话开始即后台收敛）。
                maybe_detach_dispatch_drain(*choice);
                return 0;
            }
            catch (const std::exception&)
            {
                return 0;
            }
        }

        // ------------------------------------------------- Grep
        // 期注入 -- 注入规格（已锁定）：注入格式为
        // 五行口径： 身份行（限定名 + 参数表 + 种类 + 父链）/ 定义坐标 /
        // 同名消歧 top3 / 调用者 top3（零调用者走措辞
        // v2，后缀按同名可调用数分支）/ 派发置信
        // 分档。有值才注入（无命中零输出），文本预算 1400 字节（超预算走规格
        // 的四级降级梯）。渲染器是纯函数：事实结构与字节锁测试共用同一入口。
        //
        // 目标选择（已定稿）：把剥装饰后的 pattern 当选择器在符号表
        // 上执行——唯一命中（裸名恰一个 / 限定链唯一）才定调五行；多命中（裸
        // 名撞多个同名、重载族）信息不足，注入候选清单（对照评测显示清单式
        // 提示优于错定调，也优于无注入）；零命中静默；交替 | 是
        // 多意图探索不定调；Grep 带 path 时先按定义位置预筛候选（可能把多命
        // 中塌缩成唯一 → 转五行）。

        /** @brief 注入文本的字节预算（规格 budget_bytes；降级梯的判定线）。 */
        inline constexpr std::size_t PTU_INJECTION_BUDGET = 1400;

        /** @brief 各查询阶段前给截止探针留的余量（毫秒）。 */
        inline constexpr auto PTU_QUERY_MARGIN = std::chrono::milliseconds{800};

        /** @brief 交替拆分与多 pattern 合并的分支数上限（超限整体静默）。 */
        inline constexpr std::size_t PTU_ALTERNATION_MAX_BRANCHES = 8;

        /** @brief 多词合成信封的合计命中上限（超过只报计数披露行）。 */
        inline constexpr std::size_t PTU_LOOKUP_HITS_MAX = 5;

        /** @brief 受限正则路径接受的 pattern 字节长度上限（复杂度代理）。 */
        inline constexpr std::size_t PTU_REGEX_PATTERN_MAX = 64;

        /** @brief 受限正则路径扫描时跳过的名字字节长度上限（超长模板实例名
         *         防护：实测无上限时超长名可把回溯推成栈溢出）。 */
        inline constexpr std::size_t PTU_REGEX_NAME_MAX = 128;

        /** @brief 受限正则路径全表物化的每段行数（段间查截止探针,可弃置
         *         后续段——每段 SQL 独立跑完，满足结果循环跑到 DONE 的全店
         *         不变式）。 */
        inline constexpr std::size_t PTU_REGEX_SPAN_ROWS = 20000;

        /** @brief 受限正则路径（CCG_PRETOOLUSE_REGEX）的内部默认开关值。
         *
         * 环境变量未设置或取值不是 "1"/"0" 时回落到这里。定版为启用：
         * 正则路径开启与关闭的两组对照验证均无回归——命中
         * 质量、影响分、真值、噪声四个子项上均无塌陷，正则全表扫描
         * 实测延迟也远低于截止余量，故默认放行；需要保守行为的环境
         * 仍可用 "0" 显式关闭。
         */
        inline constexpr bool PTU_REGEX_DEFAULT_ENABLED = true;

        /**
         * @brief PreToolUse 受限正则路径的运行期开关（环境变量
         *        CCG_PRETOOLUSE_REGEX）。
         *
         * 取值 "1" 启用、"0" 禁用；未设置或任何其它值（含空串、垃圾值）
         * 回落内部默认 PTU_REGEX_DEFAULT_ENABLED。解析在首次调用时完成并
         * 经函数局部 static 固化（C++ magic static，线程安全）——钩子每个
         * 工具事件新起一个进程，但单事件内该判定出现在判定树的多个分支
         * 上，首次之后的重复调用必须是纯内存读、零系统调用。开关只闸正
         * 则路径；交替拆分与多 pattern 并查不经此开关。
         */
        [[nodiscard]]
        auto pretooluse_regex_enabled() -> bool
        {
            static const bool enabled = []
            {
                const auto env =
                    cg::util::cstd::get_env("CCG_PRETOOLUSE_REGEX");
                if (env.has_value())
                {
                    if (*env == "1")
                    {
                        return true;
                    }
                    if (*env == "0")
                    {
                        return false;
                    }
                }
                return PTU_REGEX_DEFAULT_ENABLED;
            }();
            return enabled;
        }

        /**
         * @brief UTF-8 码点数（预算与 Python 侧字符数口径对齐时使用）。
         */
        [[nodiscard]]
        auto utf8_length(std::string_view text) -> std::size_t
        {
            std::size_t n = 0;
            for (std::size_t i = 0; i < text.size();)
            {
                i += cg::util::utf8_seq_len(text[i]);
                ++n;
            }
            return n;
        }

        /**
         * @brief 保留前 `max_chars` 个码点的字节前缀（不切断多字节序列）。
         */
        [[nodiscard]]
        auto utf8_prefix(std::string_view text, std::size_t max_chars)
            -> std::string_view
        {
            std::size_t n = 0;
            std::size_t bytes = 0;
            for (std::size_t i = 0; i < text.size() && n < max_chars;)
            {
                const std::size_t len = cg::util::utf8_seq_len(text[i]);
                if (i + len > text.size())
                {
                    break; // 尾部残缺序列：停在其起点
                }
                i += len;
                bytes = i;
                ++n;
            }
            return text.substr(0, bytes);
        }

        /**
         * @brief 规格的 shorten：去首尾空白，超长截到 limit-1 码点再加省略号。
         */
        [[nodiscard]]
        auto shorten_cp(std::string_view text, std::size_t limit) -> std::string
        {
            const auto spaces = [](char c)
            {
                return c == ' ' || c == '\t' || c == '\n' || c == '\r'
                       || c == '\f' || c == '\v';
            };
            while (!text.empty() && spaces(text.front()))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && spaces(text.back()))
            {
                text.remove_suffix(1);
            }
            if (utf8_length(text) <= limit)
            {
                return std::string{text};
            }
            return std::string{utf8_prefix(text, limit - 1)} + "…";
        }

        /**
         * @brief 限定名尾部 `keep` 段（规格的 short_qual；段数不足时原样）。
         */
        [[nodiscard]]
        auto short_qual(std::string_view qname, std::size_t keep) -> std::string
        {
            const std::vector<std::string_view> segs =
                cg::util::split_qualified(qname);
            if (segs.size() <= keep)
            {
                return std::string{qname};
            }
            return cg::util::join(
                std::span<const std::string_view>{segs}.last(keep),
                "::");
        }

        /** @brief 渲染事实：同名消歧行的一个他者（已按展示序排好）。 */
        struct InjectionSameName
        {
            std::string qualified;
            std::string rel_file;
            int         line = 0;
        };

        /** @brief 渲染事实：一个调用者（occ 已聚合，first_* 已是最高频站点）。
         */
        struct InjectionCaller
        {
            std::string  qualified;
            std::string  first_rel_file;
            int          first_line = 0;
            std::int64_t occ = 0;
        };

        /** @brief 渲染事实：一个派发实现候选（display_source 为目标限定名或原始
         *         key 文本；loc 为 "rel:line" 或 "?"；confidence 原样字符串）。
         */
        struct InjectionCandidate
        {
            std::string display_source;
            std::string loc;
            std::string confidence;
            std::string reason;
            int         rank = 1;
        };

        /** @brief 渲染事实：派发档（候选已按置信降序稳定排好）。 */
        struct InjectionDispatch
        {
            bool                            present = false;
            int                             sites = 0;
            int                             resolved = 0;
            std::vector<InjectionCandidate> candidates;
        };

        /** @brief 五行注入渲染的全部事实（收集与渲染解耦，测试直喂本结构）。 */
        struct InjectionFacts
        {
            std::string                    name;
            std::string                    kind;
            std::string                    qualified;
            std::string                    parent;
            std::vector<std::string>       params;
            std::string                    rel_file;
            int                            line = 0;
            int                            col = 0;
            std::size_t                    same_name_callable = 0;
            std::vector<InjectionSameName> same_name_others; // 已截前 6、已排序
            std::vector<InjectionCaller>   callers;          // 已截前 3、已排序
            // 调用者总数（截断前；>3 时渲染尾注指向 ccg callers 全量命令）。
            std::size_t       callers_total = 0;
            InjectionDispatch dispatch;
        };

        /** @brief 清单模式的一个候选（refs = 调用边 occ 之和）。 */
        struct InjectionListEntry
        {
            std::string  qualified;
            std::string  kind;
            std::int64_t refs = 0;
            std::string  rel_file;
            int          line = 0;
        };

        /** @brief 候选清单的事实：entries 为全部可调用同名者，已按 refs 降序。
         */
        struct CandidateListFacts
        {
            std::string                     name;
            std::vector<InjectionListEntry> entries; // 已排序（渲染不再动序）
        };

        /** @brief 置信档位次（规格 CONF_RANK：high>medium>low，其余按 low）。
         */
        [[nodiscard]]
        auto confidence_rank(std::string_view confidence) -> int
        {
            if (confidence == "high")
            {
                return 3;
            }
            if (confidence == "medium")
            {
                return 2;
            }
            return 1;
        }

        /** @brief 参数表：前 4 个各截 24 码点、逗号紧连；空为 void。 */
        [[nodiscard]]
        auto render_params(const std::vector<std::string>& params)
            -> std::string
        {
            std::string       out;
            const std::size_t take = std::min<std::size_t>(params.size(), 4);
            for (std::size_t i = 0; i < take; ++i)
            {
                if (!out.empty())
                {
                    out += ",";
                }
                out += shorten_cp(params[i], 24);
            }
            return out.empty() ? std::string{"void"} : out;
        }

        /** @brief 以 "; " 连接（消歧/调用者列表的分隔口径）。 */
        [[nodiscard]]
        auto join_semicolon(const std::vector<std::string>& parts)
            -> std::string
        {
            std::string out;
            for (const std::string& p : parts)
            {
                if (!out.empty())
                {
                    out += "; ";
                }
                out += p;
            }
            return out;
        }

        /** @brief 调用者条目：short2 (rel:line[, xN])；N 封顶 999。 */
        [[nodiscard]]
        auto render_caller(const InjectionCaller& caller) -> std::string
        {
            const std::string tail =
                caller.occ > 1 ? std::format(
                                     ", x{}",
                                     std::min<std::int64_t>(caller.occ, 999))
                               : std::string{};
            return std::format(
                "{} ({}:{}{})",
                short_qual(caller.qualified, 2),
                caller.first_rel_file,
                caller.first_line,
                tail);
        }

        /**
         * @brief 注入五行渲染（规格逐字模板；超预算时走降级梯）。
         *
         * @internal 行序与措辞与已锁定的注入规格逐字节一致；降级梯四
         * 步（换短名 → 删 "(+N more)" 尾注 → 从最后一个 ';' 截为 " …)" → 硬截
         * 加截断标记）也按规格顺序实现。
         */
        [[nodiscard]]
        auto render_injection_v71(const InjectionFacts& f) -> std::string
        {
            std::vector<std::string> lines;
            // 第 1 行：身份（限定名 + 参数表 + 种类 [+ of 父链]）。
            lines.push_back(
                std::format(
                    "[ccg] {}({}) — {}{}",
                    f.qualified,
                    render_params(f.params),
                    f.kind,
                    f.parent.empty() ? std::string{}
                                     : std::format(" of {}", f.parent)));
            // 第 2 行：定义坐标。
            lines.push_back(
                std::format("[ccg] def: {}:{}:{}", f.rel_file, f.line, f.col));
            // 第 3 行：同名消歧（无他者则整行省略）。
            if (!f.same_name_others.empty())
            {
                std::vector<std::string> shown;
                const std::size_t        keep =
                    std::min<std::size_t>(f.same_name_others.size(), 3);
                for (std::size_t i = 0; i < keep; ++i)
                {
                    const InjectionSameName& o = f.same_name_others[i];
                    shown.push_back(
                        std::format(
                            "{} @{}:{}",
                            short_qual(o.qualified, 3),
                            o.rel_file,
                            o.line));
                }
                const std::int64_t extra =
                    static_cast<std::int64_t>(f.same_name_others.size())
                    - static_cast<std::int64_t>(keep);
                std::string tail = extra > 0 ? std::format(" (+{} more)", extra)
                                             : std::string{};
                lines.push_back(
                    std::format(
                        "[ccg] same-name({}): {}{} — hits at those coords belong to "
                        "other symbols, not this one",
                        f.same_name_callable,
                        join_semicolon(shown),
                        tail));
            }
            // 第 4 行：调用者 top3（空则零调用者措辞 v2，后缀按同名可调用数）。
            if (!f.callers.empty())
            {
                std::vector<std::string> shown;
                for (const InjectionCaller& c : f.callers)
                {
                    shown.push_back(render_caller(c));
                }
                std::string line = std::format(
                    "[ccg] callers(top3): {}",
                    join_semicolon(shown));
                // 截断处补全量命令尾注：模型在"找谁调用 X"时不易想起
                // callers 命令，尾注直接给出拿全表的一条命令。
                if (f.callers_total > f.callers.size())
                {
                    line += std::format(" — all: ccg callers {}", f.name);
                }
                lines.push_back(std::move(line));
            }
            else
            {
                std::string line =
                    "[ccg] callers(top3): 0 edges in call graph for this exact "
                    "symbol; answer with no callers for it";
                if (f.same_name_callable > 0)
                {
                    line += std::format(
                        ". Grep call-shape rows like x->{}(...) or y.{}(...) invoke "
                        "the {} other same-name symbols' impls (see same-name "
                        "line), not this one",
                        f.name,
                        f.name,
                        f.same_name_callable);
                }
                else
                {
                    line = std::format(
                        "{}. Grep hits are only this symbol's own definition/"
                        "declaration lines — a header declaration is not a call "
                        "site",
                        line);
                }
                lines.push_back(std::move(line));
            }
            // 第 5 行：派发分档（无虚站点则 direct-only 一句）。
            if (!f.dispatch.present)
            {
                lines.push_back("[ccg] dispatch: none (direct calls only)");
            }
            else
            {
                // 直方图按候选列表的首见顺序（列表已按置信降序稳定排好）。
                std::vector<std::pair<std::string, std::size_t>> tiers;
                for (const InjectionCandidate& c : f.dispatch.candidates)
                {
                    const auto it = std::ranges::find_if(
                        tiers,
                        [&](const auto& t) { return t.first == c.confidence; });
                    if (it != tiers.end())
                    {
                        ++it->second;
                    }
                    else
                    {
                        tiers.emplace_back(c.confidence, 1);
                    }
                }
                std::string tier_str;
                for (const auto& [conf, n] : tiers)
                {
                    if (!tier_str.empty())
                    {
                        tier_str += ", ";
                    }
                    tier_str += std::format("{} x{}", conf, n);
                }
                std::string top;
                if (!f.dispatch.candidates.empty())
                {
                    const InjectionCandidate& c = f.dispatch.candidates.front();
                    top = std::format(
                        "; top: {} [{}] ({}) @{}",
                        short_qual(c.display_source, 2),
                        c.confidence,
                        shorten_cp(c.reason, 24),
                        c.loc);
                }
                std::string family_note;
                // 家族>1 时尾注全量命令（家族枚举的正解是
                // query，注入不指路时用户会被泛点名带偏去跑 impact
                // 向上视图）。
                if (f.dispatch.candidates.size() > 1)
                {
                    family_note =
                        std::format(" — family list: ccg query {}", f.name);
                }
                lines.push_back(
                    std::format(
                        "[ccg] dispatch: {} virtual sites, {} resolved; {} impl "
                        "candidates by confidence [{}]{}{}",
                        f.dispatch.sites,
                        f.dispatch.resolved,
                        f.dispatch.candidates.size(),
                        tier_str,
                        top,
                        family_note));
            }
            std::string text;
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                if (i > 0)
                {
                    text += "\n";
                }
                text += lines[i];
            }
            // 降级梯（正常事实不会触发；保留以兑现规格的预算承诺）。
            const auto over = [&]
            { return text.size() > PTU_INJECTION_BUDGET; };
            if (over())
            {
                // 1) 全限定名换两段短名（全文替换）。
                const std::string full =
                    f.qualified.empty() ? f.name : f.qualified;
                const std::string short_name = short_qual(full, 2);
                if (full != short_name)
                {
                    std::string replaced;
                    std::size_t at = 0;
                    while (true)
                    {
                        const std::size_t hit = text.find(full, at);
                        if (hit == std::string::npos)
                        {
                            replaced.append(text, at, std::string::npos);
                            break;
                        }
                        replaced.append(text, at, hit - at);
                        replaced += short_name;
                        at = hit + full.size();
                    }
                    text = std::move(replaced);
                }
            }
            while (over() && text.find(" (+") != std::string::npos)
            {
                // 2) 删除首个 "(+N more)" 尾注（从 " (+" 到其后首个 ')'）。
                const std::size_t cut = text.find(" (+");
                const std::size_t end = text.find(')', cut);
                if (end == std::string::npos)
                {
                    break;
                }
                text.erase(cut, end - cut + 1);
            }
            while (over() && text.find(';') != std::string::npos)
            {
                // 3) 最后一个 ';' 起截为 " …)"。
                const std::size_t cut = text.rfind(';');
                text = text.substr(0, cut) + " …)";
            }
            if (over())
            {
                // 4) 字节硬截（不切断多字节序列）加截断标记。
                constexpr std::size_t tail_room = 20;
                const std::size_t cap = PTU_INJECTION_BUDGET > tail_room
                                            ? PTU_INJECTION_BUDGET - tail_room
                                            : 0;
                text =
                    std::string{utf8_trunc_bytes(text, cap)} + " …[ccg 截断]";
            }
            return text;
        }

        /**
         * @brief 路径过滤串归一化：分隔符统一正斜杠 + 小写 + 去尾斜杠。
         *
         * @internal "core/" 与 "core"、"core\\" 同形——尾斜杠若不去，路径预筛的
         *           锚定匹配（等于 / 前缀 / 后缀）三形态全落空而整筛成空
         *           = 静默，与用户收窄意图相反。库内路径无尾斜杠，过滤串与
         *           被比路径两侧同过此函数等价。
         */
        [[nodiscard]]
        inline auto norm_path_filter(std::string s) -> std::string
        {
            s = cg::util::ascii_lower(s);
            std::ranges::replace(s, '\\', '/');
            while (s.ends_with('/'))
            {
                s.pop_back();
            }
            return s;
        }

        /**
         * @brief 候选清单渲染（多命中形态）。
         *
         * @internal 结构：
         * 首行 "matches N same-name symbols (no dominant one)"；候选行按 refs
         * 降序最多 5 个，尾注 " (+M more)" 挂在最后一个候选行上；末行教收敛
         * 动作（限定名 / 路径过滤，示例名取头部候选的限定名）。预算沿用
         * 1400B：超限时从尾部裁候选行
         * （尾注计数随之增大），首行与末行永不裁。
         */
        [[nodiscard]]
        auto render_candidate_list(const CandidateListFacts& f) -> std::string
        {
            const std::size_t shown_max = 5;
            std::size_t       shown = std::min(f.entries.size(), shown_max);
            const auto        build = [&](std::size_t count)
            {
                std::vector<std::string> lines;
                lines.push_back(
                    std::format(
                        "[ccg] \"{}\" matches {} same-name symbols (no dominant one) "
                        "— candidates by references:",
                        f.name,
                        f.entries.size()));
                for (std::size_t i = 0; i < count; ++i)
                {
                    const InjectionListEntry& e = f.entries[i];
                    std::string               line = std::format(
                        "[ccg] - {} — {}, {} refs @{}:{}",
                        e.qualified,
                        e.kind,
                        e.refs,
                        e.rel_file,
                        e.line);
                    if (i + 1 == count && f.entries.size() > count)
                    {
                        line += std::format(
                            "  (+{} more)",
                            f.entries.size() - count);
                    }
                    lines.push_back(std::move(line));
                }
                lines.push_back(
                    std::format(
                        "[ccg] re-grep with a qualified name (e.g. {}) or a path "
                        "filter to pin one symbol",
                        f.entries.empty() ? f.name
                                          : f.entries.front().qualified));
                // 一发全量提示：同名符号靠换 pattern 硬拼代价高，而 query
                // 一条命令即得全表——此行只点名这条确定正确的命令。
                lines.push_back(
                    std::format(
                        "[ccg] full same-name list in one shot: ccg query {}",
                        f.name));
                std::string text;
                for (std::size_t i = 0; i < lines.size(); ++i)
                {
                    if (i > 0)
                    {
                        text += "\n";
                    }
                    text += lines[i];
                }
                return text;
            };
            std::string text = build(shown);
            while (text.size() > PTU_INJECTION_BUDGET && shown > 1)
            {
                --shown;
                text = build(shown);
            }
            return text;
        }

        /**
         * @brief 显示截断：超预算时截到上限内（UTF-8 安全）并加省略号后缀。
         *
         * @internal cap 按"截后内容 + 3 字节省略号 ≤ cap"执行（U+2026 为
         *           3 字节 UTF-8 序列），保证显示串自身恒有界。
         */
        [[nodiscard]]
        auto display_clip(std::string_view text, std::size_t cap) -> std::string
        {
            if (text.size() <= cap)
            {
                return std::string{text};
            }
            return std::string{utf8_trunc_bytes(text, cap - 3)} + "…";
        }

        /** @brief 多词清单的一个符号条目（term 序 + 现行清单条目域）。 */
        struct MultiLookupEntry
        {
            std::string  term;      ///< 命中该符号的有效分支 ident / 命中名
            std::string  qualified; ///< 限定名（qualified_of 口径）
            std::string  kind;      ///< 种类名（name_of 口径）
            std::int64_t refs = 0;  ///< calls 边 occ 之和（非可调用 0）
            std::string  rel_file;  ///< 展示相对路径（display_path 口径）
            int          line = 0;
        };

        /**
         * @brief 多词候选清单渲染（交替分支 / 多 pattern / 正则多名共用）。
         *
         * 首行披露模式原文与合计命中数；条目按 term 出现顺序优先、term 内
         * refs 降序（并列走路径/行号升序，与现行清单排序口径一致）；末行
         * 教收敛动作（单分支重查钉住事实）。预算沿用 PTU_INJECTION_BUDGET：
         * 条目域（qualified 96B / rel_file 64B）与首行 pattern（64B）先做
         * 显示截断，再走构建降级（超预算从尾部裁条目并挂 (+K more) 尾注，
         * 首行与末行永不裁）；shown==1 仍超预算时按五行块第 4 级同款做字节
         * 硬截档收尾——恒 ≤ 预算返回，不依赖"实际触不到"的经验断言。
         *
         * @param display_pattern 首行披露的模式显示串（内部再做 64B 截断）。
         * @param terms_note     首行的词口径披露（如 "2 terms" / "3 names"；
         *                        调用方合成，本函数不截断——合成侧恒有界）。
         * @param entries        全部条目（渲染器自行按 term 序 + refs 序排）。
         */
        [[nodiscard]]
        auto render_multi_lookup_list(
            std::string_view                     display_pattern,
            std::string_view                     terms_note,
            const std::vector<MultiLookupEntry>& entries) -> std::string
        {
            // term 首见次序即主键（稳定、与 pattern 意图对齐——全局 refs
            // 排序 + 截断会把尾部分支整段裁掉，用户白查）。
            std::vector<std::string_view> term_order;
            for (const MultiLookupEntry& e : entries)
            {
                if (std::ranges::find(term_order, e.term) == term_order.end())
                {
                    term_order.push_back(e.term);
                }
            }
            const auto term_rank = [&](std::string_view term)
            {
                return static_cast<std::size_t>(
                    std::ranges::find(term_order, term) - term_order.begin());
            };
            std::vector<MultiLookupEntry> sorted = entries;
            std::ranges::stable_sort(
                sorted,
                [&](const MultiLookupEntry& a, const MultiLookupEntry& b)
                {
                    const std::size_t ra = term_rank(a.term);
                    const std::size_t rb = term_rank(b.term);
                    if (ra != rb)
                    {
                        return ra < rb;
                    }
                    if (a.refs != b.refs)
                    {
                        return a.refs > b.refs;
                    }
                    if (a.rel_file != b.rel_file)
                    {
                        return a.rel_file < b.rel_file;
                    }
                    return a.line < b.line;
                });
            const std::string pattern_disp = display_clip(display_pattern, 64);
            const auto        build = [&](std::size_t count)
            {
                std::vector<std::string> lines;
                lines.push_back(
                    std::format(
                        "[ccg] \"{}\" matched {} symbols across {} — candidates "
                        "by references:",
                        pattern_disp,
                        sorted.size(),
                        terms_note));
                for (std::size_t i = 0; i < count; ++i)
                {
                    const MultiLookupEntry& e = sorted[i];
                    std::string             line = std::format(
                        "[ccg] - {} — {}, {} refs @{}:{}",
                        display_clip(e.qualified, 96),
                        e.kind,
                        e.refs,
                        display_clip(e.rel_file, 64),
                        e.line);
                    if (i + 1 == count && sorted.size() > count)
                    {
                        line +=
                            std::format("  (+{} more)", sorted.size() - count);
                    }
                    lines.push_back(std::move(line));
                }
                lines.push_back("[ccg] re-grep with one term to pin its facts");
                std::string text;
                for (std::size_t i = 0; i < lines.size(); ++i)
                {
                    if (i > 0)
                    {
                        text += "\n";
                    }
                    text += lines[i];
                }
                return text;
            };
            std::size_t shown = std::min(sorted.size(), PTU_LOOKUP_HITS_MAX);
            std::string text = build(shown);
            while (text.size() > PTU_INJECTION_BUDGET && shown > 1)
            {
                --shown;
                text = build(shown);
            }
            if (text.size() > PTU_INJECTION_BUDGET)
            {
                // 硬截档（保险丝）：与 render_injection_v71 第 4 级同款——
                // 预算内做 UTF-8 序列安全截断并以截断标记收尾。条目域显示
                // 截断后正常路径走不到这里；机械可证的预算硬顶不依赖该
                // 经验断言。
                constexpr std::size_t tail_room = 20;
                const std::size_t cap = PTU_INJECTION_BUDGET > tail_room
                                            ? PTU_INJECTION_BUDGET - tail_room
                                            : 0;
                text =
                    std::string{utf8_trunc_bytes(text, cap)} + " …[ccg 截断]";
            }
            return text;
        }

        /**
         * @brief 超限计数披露行（合计命中超过 PTU_LOOKUP_HITS_MAX 时替代
         *        清包清单的单行出口）。
         *
         * @param display_pattern 模式显示串（内部 64B 截断，同多词清单首行）。
         * @param count           合计命中数（交替/多 pattern 路径为符号数，
         *                        正则路径装载前为命中名数）。
         * @param unit            计数单位措辞（"symbols" / "names"）。
         */
        [[nodiscard]]
        auto render_overmatch_disclosure(
            std::string_view display_pattern,
            std::size_t      count,
            std::string_view unit = "symbols") -> std::string
        {
            return std::format(
                "[ccg] \"{}\" matched {} {} — too many to list; narrow the "
                "pattern",
                display_clip(display_pattern, 64),
                count,
                unit);
        }

        /** @brief 同名集合里一个符号的轻量行（一条 SQL 拿齐渲染与排序要的域）。
         */
        struct PreToolUseBrief
        {
            cg::db::SymId      id = 0;
            cg::db::SymbolKind kind = cg::db::SymbolKind::Record;
            std::string        unique_key;
            std::string        name;
            std::string        file; // 库内规范化路径
            int                line = 0;
        };

        /**
         * @brief unique_key（kind|parent|name|path|phash[|const]）的父链段。
         */
        [[nodiscard]]
        auto key_parent_chain(std::string_view unique_key) -> std::string_view
        {
            const std::size_t first = unique_key.find('|');
            if (first == std::string_view::npos)
            {
                return {};
            }
            const std::size_t second = unique_key.find('|', first + 1);
            return unique_key.substr(first + 1, second - first - 1);
        }

        /** @brief 限定名 = 父链 + "::" + 名（与索引侧 unique_key 的拼法一致）。
         */
        [[nodiscard]]
        auto qualified_of(const PreToolUseBrief& brief) -> std::string
        {
            const std::string_view parent = key_parent_chain(brief.unique_key);
            return parent.empty() ? brief.name
                                  : std::format("{}::{}", parent, brief.name);
        }

        /**
         * @brief 生成物路径判定的名字兜底（同名排序里"手写在前"的口径
         *        之一）。
         *
         * @internal 实验库的 in_handwritten 是 core/、include/core/
         * 前缀白名单；通用化后取其补集近似：build/ 与 _deps/
         * 之下的文件视为生成物/三方。主判定 is_generated 优先用编译数据
         * 库 directory 推导的根集合；本兜底在推导不可用时独立生效，
         * 推导守卫也以它豁免已知三方目录（见 collect_generated_roots）。
         */
        [[nodiscard]]
        auto is_generated_path(std::string_view normalized_path) -> bool
        {
            const std::string p = cg::util::to_forward_slashes(normalized_path);
            return p.starts_with("build/") || p.starts_with("_deps/")
                   || p.contains("/build/") || p.contains("/_deps/");
        }

        /** @brief 生成物根集合：编译数据库 directory 推导出的构建目录
         *         前缀（路径比较键形态、以 '/' 收尾）。
         *
         * @internal keys 为空表示推导不可用（无索引档案、档案未记录
         * 编译数据库、文件缺失或畸形），消费方回退名字兜底——钩子语境
         * 里一切失败都静默退到既有行为。
         */
        struct GeneratedRoots
        {
            std::vector<std::string> keys;
        };

        /**
         * @brief 从配置目录的索引档案读编译数据库，收集其条目
         *        "directory" 字段并筛出可当生成物根的子集。
         *
         * @internal 档案记录的路径是索引这份库时实际生效的语料，与库内
         * 文件严格配套，比现场重扫目录更真实。守卫：某 directory 之下
         * 存在主源文件（条目 "file"，按其 directory 解析为绝对路径）且
         * 该文件不被名字兜底命中时，说明该 directory 是源码目录而非
         * 纯构建目录（MSBuild 适配器记 cl 的工作目录、源码树内构建
         * 均属此类），否决之；build/_deps 之下的三方源码被兜底豁免，
         * 不会连坐否决 ninja 场景的构建根。畸形条目（缺 directory 或
         * file）只放弃自身参与。任何异常静默返回空。
         */
        [[nodiscard]]
        auto collect_generated_roots(const std::string& config_dir)
            -> GeneratedRoots
        {
            const auto profile = layout::load_profile(config_dir);
            if (!profile.has_value() || profile->compile_commands.empty())
            {
                return {};
            }
            // 档案记录的路径可能保持索引时的相对拼写：按产出文件夹
            // （配置目录的父目录）解析一次；解析结果不存在时保留原样，
            // 交由进程工作目录兜底——都读不到即回退。
            std::string cdb_file = profile->compile_commands;
            const bool  cdb_absolute =
                cdb_file.size() >= 2
                && ((std::isalpha(static_cast<unsigned char>(cdb_file.front()))
                     && cdb_file[1] == ':')
                    || cdb_file.front() == '/' || cdb_file.front() == '\\');
            if (!cdb_absolute)
            {
                const std::string by_root = cg::util::norm_path(
                    (fs::path{config_dir}.parent_path() / fs::path{cdb_file})
                        .lexically_normal()
                        .generic_string());
                std::error_code ec;
                if (fs::is_regular_file(fs::path{by_root}, ec))
                {
                    cdb_file = std::move(by_root);
                }
            }
            const auto text = cg::util::read_text_file(cdb_file);
            if (!text.has_value())
            {
                return {};
            }
            try
            {
                const json::Document  doc = json::Document::parse(*text);
                const json::ValueView arr = doc.root();
                if (!arr.is_array())
                {
                    return {};
                }
                const fs::path cdb_dir = fs::path{cdb_file}.parent_path();
                std::vector<std::string> dir_keys;  // 候选（可能被否决）
                std::vector<std::string> file_keys; // 守卫材料（比较键形态）
                for (std::size_t i = 0; i < arr.size(); ++i)
                {
                    const auto entry_view = arr.at(i);
                    if (!entry_view || !entry_view->is_object())
                    {
                        continue;
                    }
                    const json::ValueView entry = *entry_view;
                    const auto            field = [&entry](std::string_view key)
                        -> std::optional<std::string>
                    {
                        if (const auto v = entry.get(key);
                            v.has_value() && v->is_string())
                        {
                            if (const auto s = v->as_string(); s.has_value())
                            {
                                return std::string{*s};
                            }
                        }
                        return std::nullopt;
                    };
                    const std::optional<std::string> dir = field("directory");
                    if (!dir.has_value() || dir->empty())
                    {
                        continue;
                    }
                    // 相对 directory 按数据库文件所在目录解析；绝对
                    // 拼接时右侧整体替换，两种形态一次到位。拼接只用
                    // 原始串，规范化输出绝不再喂给文件系统拼接。
                    const fs::path dir_abs =
                        (cdb_dir / fs::path{*dir}).lexically_normal();
                    std::string dir_key = path_compare_key(
                        cg::util::norm_path(dir_abs.generic_string()));
                    while (!dir_key.empty() && dir_key.back() == '/')
                    {
                        dir_key.pop_back();
                    }
                    if (dir_key.empty())
                    {
                        continue;
                    }
                    dir_key += '/';
                    if (std::ranges::find(dir_keys, dir_key) == dir_keys.end())
                    {
                        dir_keys.push_back(dir_key);
                    }
                    const std::optional<std::string> file = field("file");
                    if (!file.has_value() || file->empty())
                    {
                        continue;
                    }
                    file_keys.push_back(path_compare_key(
                        cg::util::norm_path((dir_abs / fs::path{*file})
                                                .lexically_normal()
                                                .generic_string())));
                }
                GeneratedRoots out;
                for (const std::string& dir_key : dir_keys)
                {
                    const bool vetoed = std::ranges::any_of(
                        file_keys,
                        [&dir_key](const std::string& file_key)
                        {
                            return file_key.starts_with(dir_key)
                                   && !is_generated_path(file_key);
                        });
                    if (!vetoed)
                    {
                        out.keys.push_back(dir_key);
                    }
                }
                return out;
            }
            catch (const std::exception&)
            {
                return {};
            }
        }

        /**
         * @brief 生成物判定主口径：名字兜底命中，或路径落进任一推导根
         *        前缀。
         *
         * @param normalized_path 库内规范化路径。
         * @param roots           collect_generated_roots 的推导结果（可空，
         *                        空时等价于名字兜底）。
         */
        [[nodiscard]]
        auto is_generated(
            std::string_view      normalized_path,
            const GeneratedRoots& roots) -> bool
        {
            if (is_generated_path(normalized_path))
            {
                return true;
            }
            if (roots.keys.empty())
            {
                return false;
            }
            const std::string key = path_compare_key(normalized_path);
            for (const std::string& dir_key : roots.keys)
            {
                if (key.starts_with(dir_key))
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief
         * 该种类是否计入"可调用"（function/method/constructor/destructor）。
         *
         * @internal 已收编至 db 层 kind 谓词族（cg::db::is_callable）——两版口径
         * 从此同源，不再靠注释互锚；本名保持既有调用面与镜像测试靶点不动。
         * 析构函数有显式调用形态（`p->~T()`）且声明行与调用行同样需要区分，
         * 两版都收。
         */
        [[nodiscard]]
        auto is_callable_kind(cg::db::SymbolKind kind) -> bool
        {
            return cg::db::is_callable(kind);
        }

        /**
         * @brief 把 Grep 的正则意图剥回标识符链（A::b 形态），剥不动则放弃。
         *
         * @internal 只认"整体就是一个标识符链"的模式：两端可带锚点/词边界装饰
         * （^ $ \b \B 以及 (?<!\w)/(?!\w) 与字面 [A-Za-z0-9_] 形态）与调用形
         * 尾巴（\(\) / \( —— 定调动作的常见写法，剥掉后按链解析）；其余
         * （交替、通配、点号、成员访问 x->f）一律视为非符号查询静默跳过
         * ——PreToolUse 注入只做零噪音的精确补充，不做猜测。
         */
        [[nodiscard]]
        auto grep_pattern_identifier(std::string_view pattern) -> std::string
        {
            std::string s{pattern};
            const auto  spaces = [](char c)
            { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
            while (!s.empty() && spaces(s.front()))
            {
                s.erase(s.begin());
            }
            while (!s.empty() && spaces(s.back()))
            {
                s.pop_back();
            }
            constexpr std::string_view heads[] =
                {"^", "\\b", "\\B", "(?<!\\w)", "(?<![A-Za-z0-9_])"};
            constexpr std::string_view tails[] =
                {"$", "\\b", "\\B", "(?!\\w)", "(?![A-Za-z0-9_])"};
            bool changed = true;
            while (changed)
            {
                changed = false;
                for (const std::string_view h : heads)
                {
                    if (s.starts_with(h))
                    {
                        s.erase(0, h.size());
                        changed = true;
                    }
                }
                for (const std::string_view t : tails)
                {
                    if (s.ends_with(t))
                    {
                        s.erase(s.size() - t.size());
                        changed = true;
                    }
                }
            }
            // 调用形尾巴：\(\)、\)、\(、) 依次剥掉（锚点剥离之后），让
            // "SampleError::what\(" 这类定调写法落到链解析。开头/中段的
            // 括号不剥——那是复杂正则，整条放弃。
            while (!s.empty())
            {
                if (s.ends_with("\\(\\)"))
                {
                    s.erase(s.size() - 4);
                }
                else if (s.ends_with("\\(") || s.ends_with("\\)"))
                {
                    s.erase(s.size() - 2);
                }
                else if (s.ends_with("(") || s.ends_with(")"))
                {
                    s.pop_back();
                }
                else
                {
                    break;
                }
            }
            const auto ident = [](std::string_view seg)
            {
                if (seg.empty()
                    || !(
                        std::isalpha(static_cast<unsigned char>(seg[0]))
                        || seg[0] == '_'))
                {
                    return false;
                }
                return std::ranges::all_of(
                    seg.substr(1),
                    [](char c)
                    {
                        return std::isalnum(static_cast<unsigned char>(c))
                               || c == '_';
                    });
            };
            std::size_t start = 0;
            bool        ok = !s.empty();
            while (ok)
            {
                const std::size_t      at = s.find("::", start);
                const std::string_view seg =
                    at == std::string::npos
                        ? std::string_view{s}.substr(start)
                        : std::string_view{s}.substr(start, at - start);
                if (!ident(seg))
                {
                    ok = false;
                    break;
                }
                if (at == std::string::npos)
                {
                    break;
                }
                start = at + 2;
            }
            return ok ? s : std::string{};
        }

        /**
         * @brief 把查找模式按顶层交替符拆成分支列表（括号深度 0 的裸 |）。
         *
         * 转义形态 \| 在正则语义里是字面竖线不是交替，不拆；分支内的括号
         * （分组）中的 | 属于该分支自身，不产生新分支。无顶层 | 的输入返回
         * 仅含原文的单元素列表（精确链快路径由此保持与现行逐字节等价）。
         * 出现空分支、括号不平衡（含未闭合分组）、分支总数超过上限，或输
         * 入本身为空串/纯空白时返回空列表——整体视为非符号查找或病态形
         * 态，与多意图拒（宁漏勿猜）同口径静默；空输入的拒同时封死"多
         * pattern 借空串绕过空分支拒"的通路。
         */
        [[nodiscard]]
        auto split_top_alternation(std::string_view pattern)
            -> std::vector<std::string>
        {
            const auto blank = [](char c)
            {
                return c == ' ' || c == '\t' || c == '\n' || c == '\r'
                       || c == '\f' || c == '\v';
            };
            bool all_blank = true;
            for (const char c : pattern)
            {
                if (!blank(c))
                {
                    all_blank = false;
                    break;
                }
            }
            if (all_blank)
            {
                return {};
            }
            std::vector<std::string> branches;
            std::size_t              depth = 0;
            std::size_t              start = 0;
            bool                     escaped = false;
            for (std::size_t i = 0; i < pattern.size(); ++i)
            {
                const char c = pattern[i];
                if (escaped)
                {
                    escaped = false;
                    continue;
                }
                if (c == '\\')
                {
                    // 连续两个反斜杠是转义的反斜杠，其后字符未转义。
                    escaped = true;
                    continue;
                }
                if (c == '(')
                {
                    ++depth;
                    continue;
                }
                if (c == ')')
                {
                    if (depth == 0)
                    {
                        return {};
                    }
                    --depth;
                    continue;
                }
                if (c == '|' && depth == 0)
                {
                    branches.emplace_back(pattern.substr(start, i - start));
                    start = i + 1;
                }
            }
            if (depth != 0)
            {
                return {};
            }
            branches.emplace_back(pattern.substr(start));
            for (const std::string& b : branches)
            {
                bool only_blank = true;
                for (const char c : b)
                {
                    if (!blank(c))
                    {
                        only_blank = false;
                        break;
                    }
                }
                if (only_blank)
                {
                    return {};
                }
            }
            if (branches.size() > PTU_ALTERNATION_MAX_BRANCHES)
            {
                return {};
            }
            return branches;
        }

        /**
         * @brief 受限正则路径的形态护栏：长度、纯装饰与病态量词形态的机械
         *        判定。
         *
         * 只放行长度不超过 PTU_REGEX_PATTERN_MAX、含实质内容（剥锚点与空
         * 白后非空）、且不含病态量词形态的模式。量词起始字符集为 * + ? {
         * （转义形态按保守口径一并计入——误拒的方向是静默，与现行行为相
         * 同，零损失）；机械检测两道：任意两个量词起始字符直接相邻（挡掉
         * 占有/惰性量词与平铺重复量词）、右括号后紧跟量词起始字符的一切
         * 组合（挡掉 `(a*)*` 嵌套量词与 `(a|aa)+` 这类构造合法但回溯指数
         * 级的形态——后者分组内无量词、量词不相邻，唯有本道不可绕过；代
         * 价是 `(ab)+` 无害形态同拒，其现行本就静默，零行为损失）。构造
         * 期抛出的 std::regex_error 由调用点 try/catch 兜底（双保险，任何
         * 异常形态整体静默）。
         */
        [[nodiscard]]
        auto regex_pattern_allowed(std::string_view pattern) -> bool
        {
            if (pattern.size() > PTU_REGEX_PATTERN_MAX)
            {
                return false;
            }
            // 纯装饰拒：剥锚点与空白后无实质内容（空正则、^$、^^^ 类），
            // 不为其白付一次开库与全表扫描。
            const auto is_space = [](char c)
            {
                return c == ' ' || c == '\t' || c == '\n' || c == '\r'
                       || c == '\f' || c == '\v';
            };
            bool has_content = false;
            for (std::size_t i = 0; i < pattern.size() && !has_content; ++i)
            {
                const char c = pattern[i];
                if (is_space(c) || c == '^' || c == '$')
                {
                    continue;
                }
                if (c == '\\' && i + 1 < pattern.size()
                    && (pattern[i + 1] == 'b' || pattern[i + 1] == 'B'))
                {
                    ++i; // \b / \B 锚点一并剥掉
                    continue;
                }
                has_content = true;
            }
            if (!has_content)
            {
                return false;
            }
            const auto quantifier_start = [](char c)
            { return c == '*' || c == '+' || c == '?' || c == '{'; };
            for (std::size_t i = 0; i < pattern.size(); ++i)
            {
                const char c = pattern[i];
                if (quantifier_start(c))
                {
                    // 第一道：相邻量词（a**、a*+、a+?、a?{2} …）。
                    if (i + 1 < pattern.size()
                        && quantifier_start(pattern[i + 1]))
                    {
                        return false;
                    }
                }
                // 第二道（含"量词修饰含量词分组"细分口径的全集）：`)` 后
                // 紧跟量词起始字符的一切组合。
                if (c == ')' && i + 1 < pattern.size()
                    && quantifier_start(pattern[i + 1]))
                {
                    return false;
                }
            }
            return true;
        }

        /** @brief 汇合层判定树的分支分类结果。 */
        struct LookupBranchIntent
        {
            /// 噪声过滤后存活的标识符链（去重保序；单元素即现行单链快路径）。
            std::vector<std::string> alive;
            /// 非噪声分支的原始文本（保序；正则路径 pattern 与显示串的重拼源，
            /// 噪声分支剔除——正则路径绝不让噪声词复活匹配）。
            std::vector<std::string> non_noise_raw;
            /// 全部分支都是噪声词时为真（不给正则路径机会）。
            bool noise_only = true;
        };

        /**
         * @brief 汇合层的查找意图分类：逐 pattern 顶层交替拆分、去重保序、
         *        标识符判定与噪声过滤。
         *
         * 四臂汇合后统一走本函数（Grep/Search 恒单 pattern，Bash/
         * PowerShell 可带多 pattern）。返回 nullopt 表示整体静默（空输入、
         * 病态拆分形态、分支超限）；否则 alive 为空时进入正则路径判定（仅
         * 当非纯噪声且开关开启），alive 单元素走现行单链出口（字节零变化），
         * 多元素走多词并查。分支去重键：标识符链形态取其 ident，其余取原
         * 始文本（`pi|pi` 与 `rg -e pi -e pi` 都收敛为单分支）。
         */
        [[nodiscard]]
        auto classify_lookup_patterns(const std::vector<std::string>& patterns)
            -> std::optional<LookupBranchIntent>
        {
            const auto blank = [](std::string_view s)
            {
                for (const char c : s)
                {
                    if (!(c == ' ' || c == '\t' || c == '\n' || c == '\r'
                          || c == '\f' || c == '\v'))
                    {
                        return false;
                    }
                }
                return true;
            };
            if (patterns.empty()
                || std::ranges::all_of(
                    patterns,
                    [&](const std::string& p) { return blank(p); }))
            {
                return std::nullopt;
            }
            std::vector<std::string> branches;
            std::vector<std::string> keys;
            for (const std::string& p : patterns)
            {
                const std::vector<std::string> parts = split_top_alternation(p);
                if (parts.empty())
                {
                    return std::nullopt;
                }
                for (const std::string& part : parts)
                {
                    std::string key = grep_pattern_identifier(part);
                    if (key.empty())
                    {
                        key = part;
                    }
                    if (std::ranges::find(keys, key) != keys.end())
                    {
                        continue;
                    }
                    keys.push_back(std::move(key));
                    branches.push_back(part);
                }
            }
            if (branches.size() > PTU_ALTERNATION_MAX_BRANCHES)
            {
                return std::nullopt;
            }
            LookupBranchIntent intent;
            for (const std::string& b : branches)
            {
                const std::string ident = grep_pattern_identifier(b);
                if (ident.empty())
                {
                    intent.non_noise_raw.push_back(b);
                    intent.noise_only = false;
                    continue;
                }
                if (is_noise_token(ident))
                {
                    continue;
                }
                intent.alive.push_back(ident);
                intent.non_noise_raw.push_back(b);
                intent.noise_only = false;
            }
            return intent;
        }

        /** @brief 一个符号的 calls 边 occ 之和（清单的 refs 口径——只算
         *         直接调用，不含 virtual_dispatch）。 */
        [[nodiscard]]
        auto call_occ_of(cg::db::GraphStore& store, const cg::db::SymId id)
            -> std::int64_t
        {
            using cg::db::EdgeType;
            const std::vector<EdgeType> call_types{EdgeType::Calls};
            std::int64_t                occ = 0;
            for (const cg::db::EdgeAggRow& row :
                 cg::db::edges_to(store, id, call_types))
            {
                occ += row.occurrences_sum;
            }
            return occ;
        }

        /**
         * @brief 多命中形态的事实收集：候选清单（清单口径排序）。
         *
         * @internal 条目取可调用同名者（清单的 N 即可调用计数：what 13/check
         * 128）；并列 refs 按展示路径、行号升序稳定排序。全非可调用的同名集
         * （如两个同名 record）退回全集——类型同样是合法搜索目标，以 0 refs
         * 如实列出。收集受截止探针约束，超时返回 nullopt（调用方静默）。
         */
        [[nodiscard]]
        auto collect_candidate_list(
            cg::db::GraphStore&                 store,
            const std::string&                  name,
            const std::vector<PreToolUseBrief>& briefs)
            -> std::optional<CandidateListFacts>
        {
            const bool any_callable = std::ranges::any_of(
                briefs,
                [](const PreToolUseBrief& b)
                { return is_callable_kind(b.kind); });
            std::vector<InjectionListEntry> entries;
            for (const PreToolUseBrief& b : briefs)
            {
                if (any_callable && !is_callable_kind(b.kind))
                {
                    continue;
                }
                InjectionListEntry e;
                e.qualified = qualified_of(b);
                e.kind = std::string{cg::db::name_of(b.kind)};
                e.refs =
                    is_callable_kind(b.kind) ? call_occ_of(store, b.id) : 0;
                e.rel_file = display_path(b.file);
                e.line = b.line;
                entries.push_back(std::move(e));
                if (hook_deadline_reached(PTU_QUERY_MARGIN))
                {
                    return std::nullopt;
                }
            }
            std::ranges::stable_sort(
                entries,
                [](const InjectionListEntry& a, const InjectionListEntry& b)
                {
                    if (a.refs != b.refs)
                    {
                        return a.refs > b.refs;
                    }
                    if (a.rel_file != b.rel_file)
                    {
                        return a.rel_file < b.rel_file;
                    }
                    return a.line < b.line;
                });
            CandidateListFacts f;
            f.name = name;
            f.entries = std::move(entries);
            return f;
        }

        /**
         * @brief 收集目标的渲染事实（只读查库；处处受截止探针约束）。
         *
         * @param roots 生成物根推导结果（同名排序"手写在前"的主判定）。
         * @return 事实齐全返回之；无同名集/超预算返回 nullopt（调用方静默）。
         */
        [[nodiscard]]
        auto collect_injection_facts(
            cg::db::GraphStore&    store,
            const PreToolUseBrief& target,
            const GeneratedRoots&  roots) -> std::optional<InjectionFacts>
        {
            InjectionFacts f;
            f.name = target.name;
            f.kind = std::string{cg::db::name_of(target.kind)};
            f.qualified = qualified_of(target);
            f.parent = std::string{key_parent_chain(target.unique_key)};
            f.rel_file = display_path(target.file);
            f.line = target.line;

            // 目标本体的列号与参数表要整行符号记录。
            const auto self_row = cg::db::get_symbol(store, target.id);
            if (!self_row.has_value())
            {
                return std::nullopt;
            }
            f.col = self_row->col;
            if (self_row->param_types.has_value())
            {
                try
                {
                    const json::Document doc =
                        json::Document::parse(*self_row->param_types);
                    const auto arr = doc.root();
                    if (arr.is_array())
                    {
                        for (std::size_t i = 0; i < arr.size(); ++i)
                        {
                            if (const auto v = arr.at(i);
                                v.has_value() && v->is_string())
                            {
                                if (const auto s = v->as_string();
                                    s.has_value())
                                {
                                    f.params.emplace_back(*s);
                                }
                            }
                        }
                    }
                }
                catch (const std::exception&)
                {
                    f.params.clear(); // 残缺参数表按 void 呈现
                }
            }

            // 同名集合：可调用计数（不截断）+ 他者展示行（手写在前、路径序、
            // 截前 6）。briefs 按声明在前的收集序传入即可。批量轻量投影
            // 一次拉齐七个字段（不做限定名），不再逐符号全套物化。
            const std::vector<cg::db::SymId> same_ids =
                cg::db::symbol_ids_by_name(store, target.name);
            if (same_ids.empty())
            {
                return std::nullopt;
            }
            std::vector<PreToolUseBrief> others;
            for (const cg::db::SymbolBrief& brief :
                 cg::db::symbol_briefs(store, same_ids))
            {
                if (brief.id == target.id)
                {
                    continue;
                }
                PreToolUseBrief b;
                b.id = brief.id;
                b.kind = brief.kind;
                b.unique_key = brief.unique_key;
                b.name = brief.name;
                b.file = brief.file;
                b.line = brief.line;
                if (is_callable_kind(b.kind))
                {
                    ++f.same_name_callable;
                }
                others.push_back(std::move(b));
            }
            if (hook_deadline_reached(PTU_QUERY_MARGIN))
            {
                return std::nullopt;
            }
            std::ranges::stable_sort(
                others,
                [&roots](const PreToolUseBrief& a, const PreToolUseBrief& b)
                {
                    const int a_gen = is_generated(a.file, roots) ? 1 : 0;
                    const int b_gen = is_generated(b.file, roots) ? 1 : 0;
                    if (a_gen != b_gen)
                    {
                        return a_gen < b_gen;
                    }
                    return display_path(a.file) < display_path(b.file);
                });
            const std::size_t keep = std::min<std::size_t>(others.size(), 6);
            for (std::size_t i = 0; i < keep; ++i)
            {
                f.same_name_others.push_back(
                    {qualified_of(others[i]),
                     display_path(others[i].file),
                     others[i].line});
            }

            // 调用者：calls + virtual_dispatch 原始边按 edge_id 升序聚合（并列
            // occ 的次序与"先见到者先列"的口径一致），站点按 occ 降序取首。
            struct CallerSite
            {
                std::string  rel_file;
                int          line = 0;
                std::int64_t occ = 0;
            };
            struct CallerAgg
            {
                cg::db::SymId           id = 0;
                std::int64_t            occ = 0;
                std::vector<CallerSite> sites; // 先截前 12 个，再按 occ 降序
            };
            std::vector<CallerAgg> agg;
            const auto             agg_of = [&](cg::db::SymId id) -> CallerAgg&
            {
                for (CallerAgg& a : agg)
                {
                    if (a.id == id)
                    {
                        return a;
                    }
                }
                agg.push_back(CallerAgg{id, 0, {}});
                return agg.back();
            };
            for (const cg::db::EdgeRow& e :
                 cg::db::caller_edge_rows(store, target.id))
            {
                CallerAgg& a = agg_of(e.src);
                a.occ += e.occurrences;
                if (a.sites.size() < 12)
                {
                    const auto fr = store.file(e.site_file);
                    a.sites.push_back(
                        CallerSite{
                            display_path(fr ? fr->path : "?"),
                            e.site_line,
                            e.occurrences});
                }
            }
            std::ranges::stable_sort(
                agg,
                [](const CallerAgg& a, const CallerAgg& b)
                { return a.occ > b.occ; });
            f.callers_total = agg.size();
            const std::size_t top_callers =
                std::min<std::size_t>(agg.size(), 3);
            for (std::size_t i = 0; i < top_callers; ++i)
            {
                CallerAgg& a = agg[i];
                std::ranges::stable_sort(
                    a.sites,
                    [](const CallerSite& x, const CallerSite& y)
                    { return x.occ > y.occ; });
                const auto row = cg::db::get_symbol(store, a.id);
                if (!row.has_value())
                {
                    continue;
                }
                PreToolUseBrief caller;
                caller.id = row->id;
                caller.kind = row->kind;
                caller.unique_key = row->unique_key;
                caller.name = row->name;
                f.callers.push_back(
                    {qualified_of(caller),
                     a.sites.empty() ? std::string{"?"}
                                     : a.sites.front().rel_file,
                     a.sites.empty() ? 0 : a.sites.front().line,
                     a.occ});
            }

            // 派发档：该符号名下的虚调用点与逐点候选（按 target 键最高置信）。
            const std::vector<cg::db::CallSiteRow> sites =
                cg::db::virtual_sites_of_method(store, target.id);
            if (!sites.empty())
            {
                f.dispatch.present = true;
                f.dispatch.sites = static_cast<int>(sites.size());
                struct CandAgg
                {
                    std::string target_key;
                    std::string confidence;
                    std::string reason;
                    int         rank = 1;
                };
                std::vector<CandAgg> cands;
                for (const cg::db::CallSiteRow& site : sites)
                {
                    if (site.resolved)
                    {
                        ++f.dispatch.resolved;
                    }
                    try
                    {
                        const json::Document doc =
                            json::Document::parse(site.candidates);
                        const auto arr = doc.root();
                        if (!arr.is_array())
                        {
                            continue;
                        }
                        for (std::size_t i = 0; i < arr.size(); ++i)
                        {
                            const auto entry = arr.at(i);
                            if (!entry.has_value() || !entry->is_object())
                            {
                                continue;
                            }
                            CandAgg c;
                            if (const auto t = entry->get("target");
                                t.has_value() && t->is_string())
                            {
                                c.target_key = std::string{*t->as_string()};
                            }
                            if (c.target_key.empty())
                            {
                                continue;
                            }
                            if (const auto cf = entry->get("confidence");
                                cf.has_value() && cf->is_string())
                            {
                                c.confidence = std::string{*cf->as_string()};
                            }
                            else
                            {
                                c.confidence = "low";
                            }
                            if (const auto rs = entry->get("reason");
                                rs.has_value() && rs->is_string())
                            {
                                c.reason = std::string{*rs->as_string()};
                            }
                            c.rank = confidence_rank(c.confidence);
                            const auto it = std::ranges::find_if(
                                cands,
                                [&](const CandAgg& x)
                                { return x.target_key == c.target_key; });
                            if (it == cands.end())
                            {
                                cands.push_back(std::move(c));
                            }
                            else if (c.rank > it->rank)
                            {
                                // 同一目标只保留最高置信的一份。
                                *it = std::move(c);
                            }
                        }
                    }
                    catch (const std::exception&)
                    {
                    }
                }
                std::ranges::stable_sort(
                    cands,
                    [](const CandAgg& a, const CandAgg& b)
                    { return a.rank > b.rank; });
                for (const CandAgg& c : cands)
                {
                    InjectionCandidate               out;
                    std::optional<cg::db::SymbolRow> sym;
                    if (const auto id =
                            cg::db::symbol_id_by_key(store, c.target_key);
                        id.has_value())
                    {
                        sym = cg::db::get_symbol(store, *id);
                    }
                    if (sym.has_value())
                    {
                        out.display_source = std::format(
                            "{}::{}",
                            key_parent_chain(sym->unique_key),
                            sym->name);
                        if (key_parent_chain(sym->unique_key).empty())
                        {
                            out.display_source = sym->name;
                        }
                        out.loc = std::format(
                            "{}:{}",
                            display_path(sym->file),
                            sym->line);
                    }
                    else
                    {
                        out.display_source = c.target_key;
                        out.loc = "?";
                    }
                    out.confidence = c.confidence;
                    out.reason = c.reason;
                    out.rank = c.rank;
                    f.dispatch.candidates.push_back(std::move(out));
                }
            }
            return f;
        }

        /**
         * @brief 归一化后的查找意图：pattern 原样 + 路径过滤串（可空）。
         *
         * 多 pattern 形态（rg -e a -e b、Select-String -Pattern a,b）把其
         * 余 pattern 放进 extra_patterns——多 pattern 与顶层交替同属"多词
         * 并查"（OR 语义同构），由汇合判定层按交替分支同款护栏合并处理；
         * fixed 字面形态永不产生 extra_patterns（见 normalize_shell_lookup）。
         */
        struct ShellLookup
        {
            std::string
                pattern; ///< 首 pattern（单 pattern 形态即唯一 pattern）
            std::vector<std::string>
                        extra_patterns; ///< 其余 pattern（空 = 单 pattern）
            std::string path_filter;
        };

        /**
         * @brief 结构化查找请求的归一化闸门（shell 查找形态的汇合点）。
         *
         * 多个查找单元（rg foo; rg bar）、多路径、通配路径仍是多意图或范
         * 围不明的形态，一律静默放弃（宁漏勿猜）。多 pattern（-e a -e b、
         * -Pattern a,b）是 OR 语义的"多词并查"，与顶层交替同构：非 fixed
         * 形态 2..8 个 pattern 转发进 extra_patterns（合计分支上限与交替
         * 同口径），fixed 形态绝不拆（多 pattern 直接拒）。引擎显式否决单
         * 意图（single=false 的单 pattern 请求——各提取层对多 pattern 均按
         * 数量派生 single，该形态只能来自引擎 JSON 直写）仍拒，作为引擎
         * 语义漂移的防线。fixed 字面串（rg -F / -SimpleMatch）必须本身就
         * 是裸标识符链——剥装饰为恒等——才转发；含元字符/竖线的字面（如
         * -F 'check()'、-F 'a|b'）在找非符号文本，同样放弃。AfterCd 的
         * cwd 提示收窄路径过滤：拼接只会让过滤更准或筛空静默，两个方向
         * 都安全（错误方向永远是漏注入而非错注入）。
         */
        [[nodiscard]]
        auto normalize_shell_lookup(
            const shellscan::ShellExtractOutcome& outcome)
            -> std::optional<ShellLookup>
        {
            if (!outcome.skip_all.empty() || outcome.requests.size() != 1)
            {
                return std::nullopt;
            }
            const shellscan::ShellRequest& request = outcome.requests.front();
            if (request.patterns.empty())
            {
                return std::nullopt;
            }
            if (!request.single && request.patterns.size() == 1)
            {
                return std::nullopt;
            }
            if (request.fixed && request.patterns.size() > 1)
            {
                return std::nullopt;
            }
            if (!request.fixed
                && request.patterns.size() > PTU_ALTERNATION_MAX_BRANCHES)
            {
                return std::nullopt;
            }
            const std::string& pattern = request.patterns.front();
            if (request.fixed && grep_pattern_identifier(pattern) != pattern)
            {
                return std::nullopt;
            }
            if (request.paths.size() > 1)
            {
                return std::nullopt;
            }
            std::string path_filter;
            if (request.paths.size() == 1)
            {
                const std::string& only = request.paths.front();
                if (only.find_first_of("*?[") != std::string::npos)
                {
                    return std::nullopt;
                }
                path_filter = only;
            }
            if (request.path_kind == shellscan::PathKind::AfterCd
                && !request.cwd_hint.empty())
            {
                if (path_filter.empty())
                {
                    path_filter = request.cwd_hint;
                }
                else if (
                    !path_filter.starts_with("/")
                    && path_filter.find(':') == std::string::npos)
                {
                    path_filter = request.cwd_hint + "/" + path_filter;
                }
                // 绝对路径过滤串与 cd 提示无关，保持原样。
            }
            ShellLookup lookup;
            lookup.pattern = pattern;
            lookup.extra_patterns.assign(
                request.patterns.begin() + 1,
                request.patterns.end());
            lookup.path_filter = std::move(path_filter);
            return lookup;
        }

        /**
         * @brief PreToolUse 静默出口的机会式捎带：借一次本来零输出的调用
         *        把编辑积压的 digest 投递出去。
         *
         * 无人值守会话里 digest 的投递时机原本只有三处（下一次
         * PostToolUse / UserPromptSubmit / SessionStart），agent 做完末轮
         * 编辑转纯查询或总结后，末批积压永不投递——而查询与读文件恰是
         * 那之后最高频的事件，本函数把它们变成第四个投递点。复用
         * PostToolUse 同款 hook_digest_flush(Opportunistic)：journal 目录
         * 不存在 / 锁竞争 / 活租约 / 队列未收敛 / 无积压全部零成本静默，
         * 绝不派生子进程（drain 是 Poll 模式的行为）、绝不等待。
         *
         * @param choice 已定位的库（调用方解析过的出口直接复用，零重复
         *               解析）；无库可用的出口不调本函数。
         * @return 恒 0（调用方原样返回）。
         */
        auto pretooluse_opportunistic_flush(const LayoutChoice& choice) -> int
        {
            try
            {
                // 截止探针：本事件离宿主硬超时不足一次 flush 的计算预算
                // （2.5s）时放弃捎带，积压留给下一个时机——绝不为了投递
                // 把一次查询调用推过 10s 硬超时。
                if (hook_deadline_reached(DIGEST_PTU_DEADLINE))
                {
                    return 0;
                }
                const std::optional<DigestFlushResult> digest =
                    hook_digest_flush(
                        choice,
                        FlushMode::Opportunistic,
                        DIGEST_PTU_BUDGET);
                if (digest.has_value())
                {
                    // 静默出口本无注入，digest 独占信封；发送后清理与
                    // PostToolUse 同序（先认领、后发送、再删除）。
                    emit_hook_context("PreToolUse", digest->text);
                    digest_flush_done(*digest);
                    record_digest_delivery(choice.db, "pretooluse");
                }
            }
            catch (const std::exception&)
            {
            }
            return 0;
        }

        /**
         * @brief inv 版捎带入口：静默出口发生在库定位之前时自行解析库。
         *
         * @internal 与 choice 版的分工：run_pretooluse_symbol_query 内的
         *           出口已持有 choice，走 choice 版零重复解析；处理器顶层
         *           的早退出口（工具载荷解析失败、意图弃权等）走本版。
         *           无库 = 无从投递，静默返回。
         */
        auto pretooluse_opportunistic_flush(const Invocation& inv) -> int
        {
            try
            {
                const auto choice = hook_resolved_db(inv);
                if (!choice.has_value())
                {
                    return 0;
                }
                return pretooluse_opportunistic_flush(*choice);
            }
            catch (const std::exception&)
            {
                return 0;
            }
        }

        /**
         * @brief 按标识符链装载同名候选（限定链优先精确匹配，筛空退全集）。
         *
         * @internal 自查询接口层抽出的纯搬移：限定链（A::b 形态）先按
         *           unique_key 父链段精确过滤，筛空退回裸名全集（链猜错
         *           时的宁漏整集兜底）；多词并查与正则路径复用同一装载口
         *           径，保证四臂同名集合逐字节同源。
         */
        [[nodiscard]]
        auto load_briefs_for_ident(
            cg::db::GraphStore& store,
            const std::string&  ident) -> std::vector<PreToolUseBrief>
        {
            const std::size_t sep = ident.rfind("::");
            const std::string name =
                sep == std::string::npos ? ident : ident.substr(sep + 2);
            const std::string chain =
                sep == std::string::npos ? std::string{} : ident.substr(0, sep);
            const auto load = [&]()
            {
                // 批量轻量投影一次拉齐（七字段、不做限定名）。
                std::vector<PreToolUseBrief> out;
                for (const cg::db::SymbolBrief& brief : cg::db::symbol_briefs(
                         store,
                         cg::db::symbol_ids_by_name(store, name)))
                {
                    if (brief.name != name)
                    {
                        continue;
                    }
                    PreToolUseBrief b;
                    b.id = brief.id;
                    b.kind = brief.kind;
                    b.unique_key = brief.unique_key;
                    b.name = brief.name;
                    b.file = brief.file;
                    b.line = brief.line;
                    out.push_back(std::move(b));
                }
                return out;
            };
            // 链过滤在内存副本上做；筛空退回用全集重试，不回数据库重查。
            const std::vector<PreToolUseBrief> all = load();
            std::vector<PreToolUseBrief>       briefs = all;
            if (!chain.empty())
            {
                std::erase_if(
                    briefs,
                    [&](const PreToolUseBrief& b)
                    { return key_parent_chain(b.unique_key) != chain; });
                if (briefs.empty())
                {
                    briefs = all;
                }
            }
            return briefs;
        }

        /**
         * @brief 路径预筛：锚定匹配（等于/前缀/后缀），大小写与分隔符不
         *        敏感。
         *
         * @internal 自查询接口层抽出的纯搬移：带路径过滤时（Grep 的 path
         *           输入或 shell 请求的路径提示/cd 归一）先按定义位置收窄
         *           候选——多命中可能塌缩成唯一（转五行）；筛空由调用方
         *           静默（用户显式收窄了范围，范围外的事实不属于这次查
         *           询）。库内路径是绝对形态：相对过滤串与展示相对路径比
         *           （cwd 在工程内时即工作区相对路径），绝对过滤串与原始
         *           路径比。匹配锚定（等于 / 以"过滤串/"开头 / 以"/过滤
         *           串"结尾），大小写与分隔符不敏感——纯子串会把
         *           core/…/include/core/… 误判进 include/core 过滤。空过滤串
         *           是无操作（全库口径）。
         */
        auto apply_path_prefilter(
            std::vector<PreToolUseBrief>& briefs,
            const std::string&            path_filter) -> void
        {
            if (path_filter.empty())
            {
                return;
            }
            const auto norm_lower = [](std::string s)
            { return norm_path_filter(std::move(s)); };
            const std::string f = norm_lower(path_filter);
            const bool        abs_filter =
                f.find(':') != std::string::npos || f.starts_with("/");
            std::erase_if(
                briefs,
                [&](const PreToolUseBrief& b)
                {
                    const std::string cmp =
                        norm_lower(abs_filter ? b.file : display_path(b.file));
                    return !(
                        cmp == f || cmp.starts_with(f + "/")
                        || cmp.ends_with("/" + f));
                });
        }

        /**
         * @brief 查询接口层：标识符链进、注入出（四工具共享的主体）。
         *
         * @internal 原是 Grep 专用处理器 7395 行起的整段主体，抽函数为纯
         *           搬移：库定位到注入渲染零行为差异，唯一变化是路径过滤
         *           串从直接读 tool_input.path 改为参数传入（Grep 臂传入的
         *           值与原取值相同；shell 臂传入归一化后的请求路径或 cd
         *           提示）。意图判定（grep_pattern_identifier + 噪声过滤）
         *           在调用方完成。静默出口（截止/零命中/事实收集失败）经
         *           pretooluse_opportunistic_flush 捎带积压 digest；注入
         *           命中的出口保持纯注入。
         */
        auto run_pretooluse_symbol_query(
            const Invocation&  inv,
            const std::string& ident,
            const std::string& path_filter) -> int
        {
            const auto choice = hook_resolved_db(inv);
            if (!choice.has_value())
            {
                return 0;
            }
            if (hook_deadline_reached(PTU_QUERY_MARGIN))
            {
                // 静默出口捎带：截止探针触发时 flush 自身的入口探针
                // （DIGEST_PTU_DEADLINE 余量）会直接放弃，不追加成本。
                return pretooluse_opportunistic_flush(*choice);
            }
            // 只读开库：短 busy 超时让"撞上写者"直接走静默路径。
            cg::db::GraphStore store{choice->db, 300};
            // 同名候选装载 + 路径预筛（抽出的共享辅助，纯搬移）。
            std::vector<PreToolUseBrief> briefs =
                load_briefs_for_ident(store, ident);
            apply_path_prefilter(briefs, path_filter);
            // 选择器定调：剥装饰后的 pattern 在符号表上唯一命中才注入
            // 五行；多命中（裸名撞同名、重载族）信息不足，注入候选清单；
            // 零命中静默。
            if (briefs.empty() || hook_deadline_reached(PTU_QUERY_MARGIN))
            {
                return pretooluse_opportunistic_flush(*choice);
            }
            if (briefs.size() == 1)
            {
                const GeneratedRoots roots =
                    collect_generated_roots(choice->config_dir);
                const auto facts =
                    collect_injection_facts(store, briefs.front(), roots);
                if (!facts.has_value())
                {
                    return pretooluse_opportunistic_flush(*choice);
                }
                emit_hook_context("PreToolUse", render_injection_v71(*facts));
                return 0;
            }
            const std::size_t tail_sep = ident.rfind("::");
            const std::string bare_name = tail_sep == std::string::npos
                                              ? ident
                                              : ident.substr(tail_sep + 2);
            const auto list = collect_candidate_list(store, bare_name, briefs);
            if (!list.has_value())
            {
                return pretooluse_opportunistic_flush(*choice);
            }
            emit_hook_context("PreToolUse", render_candidate_list(*list));
            return 0;
        }

        /**
         * @brief 多词并查（交替分支 / 多 pattern / 正则多名共用）：逐分支
         *        装载、去重合并，按命中形态分流到既有或新渲染出口。
         *
         * 单有效分支（其余分支零命中或噪声丢弃）退化为现行单链出口——
         * 信封与该分支单独查询逐字节相同；多有效分支走多词候选清单；合
         * 计命中超上限只给计数披露行。注入出口保持纯注入（不捎带
         * digest），一切静默出口照常捎带。
         *
         * @param inv             处理器调用现场（库定位用）。
         * @param terms           有效分支 ident / 正则命中名（保序）。
         * @param display_pattern 首行披露用模式串（已 64B 显示截断）。
         * @param path_filter     路径过滤串（可空，锚定匹配口径同单链）。
         * @param term_unit       首行词口径措辞（"terms" / "names"）。
         */
        auto run_pretooluse_multi_lookup(
            const Invocation&               inv,
            const std::vector<std::string>& terms,
            const std::string&              display_pattern,
            const std::string&              path_filter,
            std::string_view                term_unit = "terms") -> int
        {
            const auto choice = hook_resolved_db(inv);
            if (!choice.has_value())
            {
                return 0;
            }
            if (hook_deadline_reached(PTU_QUERY_MARGIN))
            {
                return pretooluse_opportunistic_flush(*choice);
            }
            cg::db::GraphStore store{choice->db, 300};
            // 逐 term 装载 + 预筛 + SymId 去重（保 term 顺序、同 term 内
            // 保装载序，重复命中归首个 term）；每 term 装载后查一次截止
            // 探针（超时整体静默捎带）。
            std::vector<std::size_t> effective; // 有命中的 term 下标
            std::vector<std::vector<PreToolUseBrief>> per_term;
            std::vector<cg::db::SymId>                seen_ids;
            std::size_t                               total = 0;
            for (std::size_t t = 0; t < terms.size(); ++t)
            {
                std::vector<PreToolUseBrief> briefs =
                    load_briefs_for_ident(store, terms[t]);
                apply_path_prefilter(briefs, path_filter);
                std::erase_if(
                    briefs,
                    [&](const PreToolUseBrief& b)
                    {
                        return std::ranges::find(seen_ids, b.id)
                               != seen_ids.end();
                    });
                if (!briefs.empty())
                {
                    for (const PreToolUseBrief& b : briefs)
                    {
                        seen_ids.push_back(b.id);
                    }
                    effective.push_back(t);
                    total += briefs.size();
                }
                per_term.push_back(std::move(briefs));
                if (hook_deadline_reached(PTU_QUERY_MARGIN))
                {
                    return pretooluse_opportunistic_flush(*choice);
                }
            }
            if (total == 0)
            {
                return pretooluse_opportunistic_flush(*choice);
            }
            // 单有效 term：退化为该 term 的现行单链出口（信封字节一致）。
            // 零命中分支不计入、不披露（零命中静默哲学的自然延伸）。
            if (effective.size() == 1)
            {
                const std::size_t                   t = effective.front();
                const std::vector<PreToolUseBrief>& hit = per_term[t];
                if (hit.size() == 1)
                {
                    const GeneratedRoots roots =
                        collect_generated_roots(choice->config_dir);
                    const auto facts =
                        collect_injection_facts(store, hit.front(), roots);
                    if (!facts.has_value())
                    {
                        return pretooluse_opportunistic_flush(*choice);
                    }
                    emit_hook_context(
                        "PreToolUse",
                        render_injection_v71(*facts));
                    return 0;
                }
                const std::size_t tail_sep = terms[t].rfind("::");
                const std::string bare_name =
                    tail_sep == std::string::npos
                        ? terms[t]
                        : terms[t].substr(tail_sep + 2);
                const auto list = collect_candidate_list(store, bare_name, hit);
                if (!list.has_value())
                {
                    return pretooluse_opportunistic_flush(*choice);
                }
                emit_hook_context("PreToolUse", render_candidate_list(*list));
                return 0;
            }
            // 合计超上限：不列清单，只报计数披露行（有输出即注入出口，
            // 不捎带）。
            if (total > PTU_LOOKUP_HITS_MAX)
            {
                emit_hook_context(
                    "PreToolUse",
                    render_overmatch_disclosure(display_pattern, total));
                return 0;
            }
            // 多有效 term 且合计 ≤ 上限：多词候选清单。
            std::vector<MultiLookupEntry> entries;
            for (const std::size_t t : effective)
            {
                for (const PreToolUseBrief& b : per_term[t])
                {
                    MultiLookupEntry e;
                    e.term = terms[t];
                    e.qualified = qualified_of(b);
                    e.kind = std::string{cg::db::name_of(b.kind)};
                    e.refs =
                        is_callable_kind(b.kind) ? call_occ_of(store, b.id) : 0;
                    e.rel_file = display_path(b.file);
                    e.line = b.line;
                    entries.push_back(std::move(e));
                }
            }
            const std::string terms_note =
                std::format("{} {}", effective.size(), term_unit);
            emit_hook_context(
                "PreToolUse",
                render_multi_lookup_list(display_pattern, terms_note, entries));
            return 0;
        }

        /**
         * @brief 受限正则路径：一次全表符号名扫描（std::regex 全匹配）+
         *        既有出口注入。
         *
         * 只在零存活分支、非纯噪声、开关开启且形态过护栏时被调用。截止
         * 探针先于一切扫描（含名字物化 SQL）生效；物化按段拉取（段间探
         * 针可弃置后续段），扫描循环内每 256 名再查一次活探针（内存
         * vector 上的探针不受"SQL 结果循环跑到 DONE"不变式约束——单次
         * regex_match 若因护栏漏网形态挂死，只靠段间探针永远执行不到）；
         * 超过名字长度上限的名字直接跳过（防超长模板实例名把回溯推成
         * 栈溢出）。命中名走既有查询/渲染出口：唯一名 → 现行唯一/清单
         * 分流；多名（≤上限）→ 多词清单；超限 → 计数披露行。全部出口
         * 与交替路径共用（四臂同语义由此保证）。
         */
        auto run_pretooluse_regex_lookup(
            const Invocation&  inv,
            const std::string& pattern,
            const std::string& path_filter) -> int
        {
            const auto choice = hook_resolved_db(inv);
            if (!choice.has_value())
            {
                return 0;
            }
            if (hook_deadline_reached(PTU_QUERY_MARGIN))
            {
                return pretooluse_opportunistic_flush(*choice);
            }
            cg::db::GraphStore store{choice->db, 300};
            std::regex         re;
            try
            {
                re = std::regex{
                    pattern,
                    std::regex::ECMAScript}; // 双保险：护栏漏网的异常形态
            } // 构造抛错即静默
            catch (const std::regex_error&)
            {
                return pretooluse_opportunistic_flush(*choice);
            }
            // 分段物化 + 全名扫描（regex_match 全匹配语义，不用
            // regex_search：性能快一个量级、回溯浅，且与"整条就是一个
            // 标识符"的准入口径同构）。命中名按 str.id 升序天然去重有序。
            std::vector<std::string> matched;
            cg::db::StrId            cursor = 0;
            cg::db::StrId            prev_id = -1;
            std::size_t              probed = 0;
            while (true)
            {
                // 段间截止探针：先于每一段物化 SQL，触发即放弃整个正则
                // 查询（宁漏勿拖）。
                if (hook_deadline_reached(PTU_QUERY_MARGIN))
                {
                    return pretooluse_opportunistic_flush(*choice);
                }
                const std::vector<std::pair<cg::db::StrId, std::string>> span =
                    cg::db::symbol_names_span(
                        store,
                        cursor,
                        PTU_REGEX_SPAN_ROWS);
                if (span.empty())
                {
                    break;
                }
                bool advanced = false;
                for (const auto& [id, name] : span)
                {
                    cursor = id;
                    advanced = true;
                    if (id == prev_id)
                    {
                        continue; // 同名符号共享 str 行：连接产出的相邻重复
                    }
                    prev_id = id;
                    if (name.size() <= PTU_REGEX_NAME_MAX
                        && std::regex_match(name, re))
                    {
                        matched.push_back(name);
                    }
                    // 扫描循环内活探针（每 256 名）：护栏漏网的挂死形态
                    // 也要在探针余量内被放弃。
                    if (++probed >= 256)
                    {
                        probed = 0;
                        if (hook_deadline_reached(PTU_QUERY_MARGIN))
                        {
                            return pretooluse_opportunistic_flush(*choice);
                        }
                    }
                }
                if (!advanced)
                {
                    break;
                }
            }
            if (matched.empty())
            {
                return pretooluse_opportunistic_flush(*choice);
            }
            if (matched.size() == 1)
            {
                // 唯一命中名：现行单链出口的同款装载与分流（正则命中的
                // 是裸名，无链过滤）。
                std::vector<PreToolUseBrief> briefs =
                    load_briefs_for_ident(store, matched.front());
                apply_path_prefilter(briefs, path_filter);
                if (briefs.empty() || hook_deadline_reached(PTU_QUERY_MARGIN))
                {
                    return pretooluse_opportunistic_flush(*choice);
                }
                if (briefs.size() == 1)
                {
                    const GeneratedRoots roots =
                        collect_generated_roots(choice->config_dir);
                    const auto facts =
                        collect_injection_facts(store, briefs.front(), roots);
                    if (!facts.has_value())
                    {
                        return pretooluse_opportunistic_flush(*choice);
                    }
                    emit_hook_context(
                        "PreToolUse",
                        render_injection_v71(*facts));
                    return 0;
                }
                const auto list =
                    collect_candidate_list(store, matched.front(), briefs);
                if (!list.has_value())
                {
                    return pretooluse_opportunistic_flush(*choice);
                }
                emit_hook_context("PreToolUse", render_candidate_list(*list));
                return 0;
            }
            if (matched.size() > PTU_LOOKUP_HITS_MAX)
            {
                emit_hook_context(
                    "PreToolUse",
                    render_overmatch_disclosure(
                        pattern,
                        matched.size(),
                        "names"));
                return 0;
            }
            return run_pretooluse_multi_lookup(
                inv,
                matched,
                pattern,
                path_filter,
                "names");
        }

        /**
         * @brief PreToolUse 处理器：六工具监听（Grep|Bash|PowerShell|Search
         *        的四查找臂 + Read/Glob 纯投递载体），查找意图命中已索引符
         *        号时注入五行事实或候选清单（additionalContext）。
         *
         * Grep 臂直读 pattern/path（原生路径零改动）；Bash/PowerShell 臂先过
         * 微秒级触发词预筛再进方言提取层（Bash 走 tree-sitter 官方语法，
         * PowerShell 走进程内真引擎——命中才付那一次毫秒级解析），产出结构
         * 化查找请求后经归一化闸门；Search 臂按候选字段直读。四方在汇合判
         * 定树处汇合：pattern 按顶层交替（及多 pattern 的 OR 语义）拆分，
         * 逐分支走标识符链判定 + 噪声过滤——存活单分支直通现行单链查询
         * （字节零变化），多分支走多词并查，零存活的非噪声形态在开关开
         * 启时走受限正则路径；查询接口层整段共享。
         *
         * 每个跳过与失败路径都静默（零输出、退出 0）——钩子绝不可拖慢或否
         * 决它观察的工具调用。全程进程内只读查库（不派发子进程），并在每个
         * 昂贵阶段前用截止探针保证毫秒级返回。静默出口顺带一次机会式
         * digest 捎带（pretooluse_opportunistic_flush）：无人值守会话里
         * 查询/读文件是编辑阶段之后最高频的事件，末批积压借此投递；Read/
         * Glob 是纯投递载体——不付任何意图提取与查图成本，进处理器即走
         * 快速投递检查。符号注入命中的出口不捎带：1400B 注入信封不与
         * digest 混排，积压留给下一个静默出口。
         */
        auto hook_pretooluse(const Invocation& inv) -> int
        {
            try
            {
                if (!cg::util::platform::stdin_is_piped())
                {
                    return 0;
                }
                const std::string input(
                    std::istreambuf_iterator<char>{std::cin},
                    {});
                const json::Document doc = json::Document::parse(input);
                const auto           root = doc.root();
                const auto           tool = root.get("tool_name");
                if (!tool.has_value() || !tool->is_string())
                {
                    return pretooluse_opportunistic_flush(inv);
                }
                const auto tool_name = tool->as_string();
                const auto tool_input = root.get("tool_input");
                if (!tool_input.has_value() || !tool_input->is_object())
                {
                    return pretooluse_opportunistic_flush(inv);
                }
                // 纯投递载体：Read/Glob 不做意图提取、不做符号查询，进处理
                // 器即走快速投递检查——这两个工具是无人值守阶段的最高频事
                // 件，绝不能让它们付四臂解析成本。
                if (*tool_name == "Read" || *tool_name == "Glob")
                {
                    return pretooluse_opportunistic_flush(inv);
                }
                // 四工具分支：解析层各自消化方言，统一归一为 pattern 列表
                // （多 pattern 形态并带 extra_patterns）+ 路径过滤串后进共
                // 享的意图判定与查询接口层。
                std::vector<std::string> patterns;
                std::string              path_filter;
                if (*tool_name == "Grep")
                {
                    patterns.push_back(member_string(*tool_input, "pattern"));
                    path_filter = member_string(*tool_input, "path");
                }
                else if (*tool_name == "Bash" || *tool_name == "PowerShell")
                {
                    const std::string command =
                        member_string(*tool_input, "command");
                    if (command.empty())
                    {
                        return pretooluse_opportunistic_flush(inv);
                    }
                    // 加载前预筛（微秒级）：绝大多数 shell 命令不是查找，先
                    // 以触发词扫描挡掉——Bash 臂不付 tree-sitter 解析成本；
                    // PowerShell 臂的预筛在提取入口内部完成，未命中不付引
                    // 擎成本，命中才付一次进程内解析（毫秒级）。
                    shellscan::ShellExtractOutcome outcome;
                    if (*tool_name == "Bash")
                    {
                        if (!shellscan::bash_mentions_search(command))
                        {
                            return pretooluse_opportunistic_flush(inv);
                        }
                        outcome = shellscan::extract_bash_queries(command);
                    }
                    else
                    {
                        outcome = shellscan::extract_ps_queries(command);
                    }
                    const auto lookup = normalize_shell_lookup(outcome);
                    if (!lookup.has_value())
                    {
                        return pretooluse_opportunistic_flush(inv);
                    }
                    patterns.push_back(std::move(lookup->pattern));
                    for (const std::string& extra : lookup->extra_patterns)
                    {
                        patterns.push_back(extra);
                    }
                    path_filter = std::move(lookup->path_filter);
                }
                else if (*tool_name == "Search")
                {
                    const auto outcome =
                        shellscan::search_tool_queries(*tool_input);
                    const auto lookup = normalize_shell_lookup(outcome);
                    if (!lookup.has_value())
                    {
                        return pretooluse_opportunistic_flush(inv);
                    }
                    patterns.push_back(std::move(lookup->pattern));
                    for (const std::string& extra : lookup->extra_patterns)
                    {
                        patterns.push_back(extra);
                    }
                    path_filter = std::move(lookup->path_filter);
                }
                else
                {
                    return pretooluse_opportunistic_flush(inv);
                }
                // 汇合判定树：逐 pattern 顶层交替拆分 + 去重保序 + 逐分支
                // 标识符判定与噪声过滤（classify_lookup_patterns）。存活
                // 单分支直通现行单链出口（快路径字节零变化）；多分支走多
                // 词并查；零存活且非纯噪声时，受限正则路径仅在开关开启且
                // 形态过护栏时进入（开关只闸正则路径，交替拆分不经开关）。
                const auto intent = classify_lookup_patterns(patterns);
                if (!intent.has_value())
                {
                    return pretooluse_opportunistic_flush(inv);
                }
                if (intent->alive.empty())
                {
                    if (intent->noise_only || !pretooluse_regex_enabled())
                    {
                        return pretooluse_opportunistic_flush(inv);
                    }
                    // 正则 pattern 一律取"非噪声分支原始文本重拼"，绝不取
                    // 用户原文整条——噪声词分支已被丢弃，原文整条会让库
                    // 里恰好叫该名的自定义符号绕过噪声护栏复活。
                    const std::string joined_pattern = cg::util::join(
                        std::span<const std::string>{
                            intent->non_noise_raw.data(),
                            intent->non_noise_raw.size()},
                        "|");
                    if (!regex_pattern_allowed(joined_pattern))
                    {
                        return pretooluse_opportunistic_flush(inv);
                    }
                    return run_pretooluse_regex_lookup(
                        inv,
                        joined_pattern,
                        path_filter);
                }
                if (intent->alive.size() == 1)
                {
                    return run_pretooluse_symbol_query(
                        inv,
                        intent->alive.front(),
                        path_filter);
                }
                // 多词清单首行披露与实际参与查询的模式串一致（非噪声分
                // 支拼接），显示截断保证首行有界。
                const std::string display_pattern = display_clip(
                    cg::util::join(
                        std::span<const std::string>{
                            intent->non_noise_raw.data(),
                            intent->non_noise_raw.size()},
                        "|"),
                    64);
                return run_pretooluse_multi_lookup(
                    inv,
                    intent->alive,
                    display_pattern,
                    path_filter);
            }
            catch (const std::exception&)
            {
                return 0;
            }
        }

        /**
         * @brief 安装完成后的 PATH shim 提示行。
         *
         * @internal 背景：hook 装好只覆盖 Claude Code 场景，用户在普通终端里
         *           仍要写全路径才能调 ccg——shim 安装器就躺在同一目录，但不
         *           提示就没人发现。
         *           故障关闭：安装器随 cmake --install 落在二进制旁，但 dev
         *           构建树的 POST_BUILD 不拷贝它——不存在就闭嘴，绝不宣传
         *           死路径（与 script_path_for 的缺脚本即中止同纪律）。脚本
         *           变体跟随可执行文件后缀（.exe -> .cmd），文案与
         *           CLI 规格文档 的 Docs and PATH 小节、安装文档 同源。
         */
        void print_path_shim_tip()
        {
            const fs::path exe = cg::util::platform::executable_path();
            if (exe.empty())
            {
                return;
            }
            const std::string flavor =
                exe.extension() == ".exe" ? ".cmd" : ".sh";
            const fs::path installer =
                exe.parent_path() / ("ccg-install" + flavor);
            std::error_code ec;
            if (!fs::exists(installer, ec) || ec)
            {
                return;
            }
            std::println(
                std::cerr,
                "ccg: tip: for a bare `ccg` on PATH from any directory, run the "
                "shim installer beside this executable ({}); without it, add "
                "this directory to PATH by hand, or point CCG_BIN at the "
                "binary for the hooks",
                installer.filename().generic_string());
        }

        /** @brief `ccg hook install`：安装或刷新自有钩子条目。 */
        auto hook_install(const Invocation& inv, const fs::path& settings)
            -> int
        {
            std::vector<std::string> commands;
            for (const HookSpec& spec : HOOK_SPECS)
            {
                const fs::path  script = script_path_for(spec);
                std::error_code ec;
                if (!fs::exists(script, ec) || ec)
                {
                    throw std::runtime_error(
                        std::format(
                            "hook script not found at '{}' - expected the .ccg "
                            "directory shipped beside this executable (the build "
                            "copies publish/ there)",
                            script.generic_string()));
                }
                commands.push_back(quote_command(script));
            }

            const SettingsFile file = load_settings(settings);
            if (!file.root.has_value())
            {
                write_settings(settings, fresh_document(commands));
                std::println(
                    std::cerr,
                    "ccg: hooks installed into {}",
                    settings.generic_string());
                for (std::size_t i = 0; i < std::size(HOOK_SPECS); ++i)
                {
                    std::println(
                        std::cerr,
                        "  {} -> {}",
                        HOOK_SPECS[i].event,
                        commands[i]);
                }
                print_path_shim_tip();
                return 0;
            }
            if (file.root->type != SettingsNode::Type::Object)
            {
                throw std::runtime_error(
                    std::format(
                        "'{}' has a non-object root; refusing to rewrite it",
                        settings.generic_string()));
            }
            SettingsEditor    editor{file.text};
            const SweepResult sweep = sweep_own_entries(
                *file.root,
                editor,
                /*prune_empty=*/false);
            if (file.root->member("hooks") == nullptr)
            {
                std::string events;
                for (std::size_t i = 0; i < std::size(HOOK_SPECS); ++i)
                {
                    events += std::format(
                        "{}\"{}\": [{}]",
                        i == 0 ? "" : ",",
                        HOOK_SPECS[i].event,
                        entry_text(HOOK_SPECS[i], commands[i]));
                }
                editor.insert(
                    file.root->keys.empty() ? file.root->after_open
                                            : file.root->values.back().end,
                    std::format("\"hooks\": {{{}}}", events),
                    file.root->keys.empty());
            }
            else
            {
                for (std::size_t i = 0; i < std::size(HOOK_SPECS); ++i)
                {
                    append_entry(
                        *file.root,
                        editor,
                        HOOK_SPECS[i],
                        commands[i],
                        sweep.last_kept[i]);
                }
            }
            write_settings(settings, editor.render());
            const char* verb =
                sweep.found_commands > 0 ? "refreshed in" : "installed into";
            std::println(
                std::cerr,
                "ccg: hooks {} {}",
                verb,
                settings.generic_string());
            print_path_shim_tip();
            if (inv.json)
            {
                json::Object o;
                o.set("settings", settings.generic_string());
                o.set("refreshed", sweep.found_commands > 0);
                json::Array entries;
                for (std::size_t i = 0; i < std::size(HOOK_SPECS); ++i)
                {
                    json::Object e;
                    e.set("event", HOOK_SPECS[i].event);
                    e.set("command", commands[i]);
                    e.set("replaced", sweep.removed_per_event[i] > 0);
                    entries.add(std::move(e));
                }
                o.set("entries", std::move(entries));
                std::println("{}", json::stringify(std::move(o)));
            }
            return 0;
        }

        /** @brief `ccg hook uninstall`：干跑默认地移除自有条目并修剪空容器。 */
        auto hook_uninstall(const Invocation& inv, const fs::path& settings)
            -> int
        {
            const SettingsFile file = load_settings(settings);
            if (!file.root.has_value())
            {
                std::println(
                    std::cerr,
                    "ccg: {} does not exist; nothing to uninstall",
                    settings.generic_string());
                return 0;
            }
            if (file.root->type != SettingsNode::Type::Object)
            {
                throw std::runtime_error(
                    std::format(
                        "'{}' has a non-object root; refusing to rewrite it",
                        settings.generic_string()));
            }
            SettingsEditor    editor{file.text};
            const SweepResult sweep = sweep_own_entries(
                *file.root,
                editor,
                /*prune_empty=*/true);
            if (sweep.found_commands == 0)
            {
                std::println(
                    std::cerr,
                    "ccg: no ccg hook entries in {}",
                    settings.generic_string());
                return 0;
            }
            if (!inv.force)
            {
                std::println(
                    std::cerr,
                    "ccg: would remove {} hook command{} from {} (dry run; "
                    "rerun with --force)",
                    sweep.found_commands,
                    sweep.found_commands == 1 ? "" : "s",
                    settings.generic_string());
                return 0;
            }
            write_settings(settings, editor.render());
            std::println(
                std::cerr,
                "ccg: removed {} hook command{} from {}",
                sweep.found_commands,
                sweep.found_commands == 1 ? "" : "s",
                settings.generic_string());
            if (inv.json)
            {
                json::Object o;
                o.set("settings", settings.generic_string());
                o.set(
                    "removed",
                    static_cast<std::int64_t>(sweep.found_commands));
                std::println("{}", json::stringify(std::move(o)));
            }
            return 0;
        }

        /** @brief `ccg hook status`：报告自有条目与脚本在位情况。 */
        auto hook_status(const Invocation& inv, const fs::path& settings) -> int
        {
            std::error_code    ec;
            const bool         file_exists = fs::exists(settings, ec) && !ec;
            const SettingsFile file =
                file_exists ? load_settings(settings) : SettingsFile{};
            std::vector<std::size_t> counts(std::size(HOOK_SPECS), 0);
            std::vector<std::string> commands(std::size(HOOK_SPECS));
            if (file.root.has_value()
                && file.root->type == SettingsNode::Type::Object)
            {
                const SettingsNode* hooks = file.root->member("hooks");
                if (hooks != nullptr
                    && hooks->type == SettingsNode::Type::Object)
                {
                    for (std::size_t i = 0; i < std::size(HOOK_SPECS); ++i)
                    {
                        const SettingsNode* event =
                            hooks->member(HOOK_SPECS[i].event);
                        if (event == nullptr
                            || event->type != SettingsNode::Type::Array)
                        {
                            continue;
                        }
                        for (const SettingsNode& entry : event->elements)
                        {
                            if (entry.type != SettingsNode::Type::Object)
                            {
                                continue;
                            }
                            const SettingsNode* cmds = entry.member("hooks");
                            if (cmds == nullptr
                                || cmds->type != SettingsNode::Type::Array)
                            {
                                continue;
                            }
                            for (const SettingsNode& command : cmds->elements)
                            {
                                if (command.type != SettingsNode::Type::Object)
                                {
                                    continue;
                                }
                                const SettingsNode* command_text =
                                    command.member("command");
                                if (command_text != nullptr
                                    && command_text->type
                                           == SettingsNode::Type::String
                                    && is_our_command(
                                        command_text->text,
                                        HOOK_SPECS[i]))
                                {
                                    ++counts[i];
                                    commands[i] = command_text->text;
                                }
                            }
                        }
                    }
                }
            }
            const std::size_t installed = static_cast<std::size_t>(
                std::ranges::fold_left(counts, std::size_t{0}, std::plus{}));
            if (inv.json)
            {
                json::Object o;
                o.set("settings", settings.generic_string());
                o.set("exists", file_exists);
                json::Array entries;
                for (std::size_t i = 0; i < std::size(HOOK_SPECS); ++i)
                {
                    json::Object e;
                    e.set("event", HOOK_SPECS[i].event);
                    e.set("entries", static_cast<std::int64_t>(counts[i]));
                    if (counts[i] > 0)
                    {
                        e.set("command", commands[i]);
                    }
                    entries.add(std::move(e));
                }
                o.set("entries", std::move(entries));
                // 积压摘要（运维/e2e 断言用；不改变退出码）。无库时省略。
                if (const auto choice = hook_resolved_db(inv);
                    choice.has_value())
                {
                    const fs::path  dir = digest_dir_for(choice->db);
                    std::size_t     pending = 0;
                    std::size_t     claimed = 0;
                    std::size_t     need_drain = 0;
                    std::error_code dec;
                    for (const auto& item : fs::directory_iterator(dir, dec))
                    {
                        if (dec)
                        {
                            break;
                        }
                        const std::string name =
                            item.path().filename().string();
                        if (name.starts_with("pending-")
                            && name.ends_with(".json"))
                        {
                            ++pending;
                            std::ifstream in{item.path(), std::ios::binary};
                            if (!in)
                            {
                                continue;
                            }
                            const std::string text{
                                std::istreambuf_iterator<char>{in},
                                {}};
                            try
                            {
                                const json::Document entry =
                                    json::Document::parse(text);
                                if (digest_bool(entry.root(), "need_drain"))
                                {
                                    ++need_drain;
                                }
                            }
                            catch (const std::exception&)
                            {
                            }
                        }
                        else if (name.starts_with("claimed-"))
                        {
                            ++claimed;
                        }
                    }
                    json::Object digest;
                    digest.set("dir", dir.generic_string());
                    digest.set("pending", static_cast<std::int64_t>(pending));
                    digest.set("claimed", static_cast<std::int64_t>(claimed));
                    digest.set(
                        "needDrain",
                        static_cast<std::int64_t>(need_drain));
                    digest.set(
                        "preToolUseFlushes",
                        count_digest_deliveries(dir, "pretooluse"));
                    // 协调事件审计的读侧：最近 24h 各类型条数——"昨晚发生
                    // 过几次 steal/rollback"一眼可答（表缺席/读失败省略，
                    // 与 digest 其余字段的尽力而为口径一致）。
                    try
                    {
                        const std::int64_t since_ms =
                            cg::util::cstd::epoch_ms()
                            - std::chrono::duration_cast<
                                  std::chrono::milliseconds>(
                                  std::chrono::hours{24})
                                  .count();
                        cg::db::GraphStore store{choice->db, 300};
                        const auto         coord_counts =
                            store.coord_event_counts_since(since_ms);
                        if (!coord_counts.empty())
                        {
                            json::Object coord;
                            for (const auto& [name, count] : coord_counts)
                            {
                                coord.set(name, count);
                            }
                            digest.set("coordEvents", std::move(coord));
                        }
                        // 最近 run 的生命体征：最新一条 run-finish 与（若
                        // 更晚）一条悬挂 run-start（在跑或被弃——被弃者由
                        // 下一个接管者补记 abandoned 后自愈消失）。
                        const std::vector<cg::db::CoordEventRow> runs =
                            store.coord_events_since(since_ms);
                        const cg::db::CoordEventRow* last_finish = nullptr;
                        const cg::db::CoordEventRow* last_any = nullptr;
                        for (const cg::db::CoordEventRow& row : runs)
                        {
                            if (row.event != "run-start"
                                && row.event != "run-finish")
                            {
                                continue;
                            }
                            last_any = &row;
                            if (row.event == "run-finish")
                            {
                                last_finish = &row;
                            }
                        }
                        if (last_finish != nullptr)
                        {
                            json::Object lr;
                            lr.set("tsMs", last_finish->ts_ms);
                            lr.set("reason", last_finish->reason);
                            lr.set("actorPid", last_finish->actor_pid);
                            lr.set("detail", last_finish->detail);
                            digest.set("lastRun", std::move(lr));
                        }
                        if (last_any != nullptr
                            && last_any->event == "run-start")
                        {
                            json::Object running;
                            running.set("tsMs", last_any->ts_ms);
                            running.set("actorPid", last_any->actor_pid);
                            running.set("detail", last_any->detail);
                            digest.set("running", std::move(running));
                        }
                    }
                    catch (const std::exception&)
                    {
                    }
                    o.set("digest", std::move(digest));
                }
                std::println("{}", json::stringify(std::move(o)));
                return 0;
            }
            std::println(
                "settings: {}{}",
                settings.generic_string(),
                file_exists ? "" : " (missing)");
            for (std::size_t i = 0; i < std::size(HOOK_SPECS); ++i)
            {
                std::println(
                    "  {:18} {} entr{}",
                    HOOK_SPECS[i].event,
                    counts[i],
                    counts[i] == 1 ? "y" : "ies");
                if (counts[i] > 0)
                {
                    std::println("               {}", commands[i]);
                }
            }
            if (installed > 0)
            {
                const fs::path  script = script_path_for(HOOK_SPECS[0]);
                std::error_code sec;
                std::println(
                    "scripts:     {} ({})",
                    script.generic_string(),
                    fs::exists(script, sec) && !sec ? "present" : "MISSING");
            }
            return 0;
        }

        /**
         * @brief `ccg hook <action>` 入口：作用域命令加蹦床脚本转发来的两个
         *        处理器动作。
         */
        auto run_hook(const Invocation& inv) -> int
        {
            const std::string& action = inv.positional.front();
            const auto         usage = usage_for(Invocation::Cmd::Hook);
            const bool         handler =
                action == "posttooluse" || action == "sessionstart"
                || action == "userpromptsubmit" || action == "pretooluse";
            if (handler)
            {
                if (inv.hook_user || inv.hook_project || inv.force)
                {
                    throw UsageError{
                        std::format(
                            "hook {} takes no scope or force flags",
                            action),
                        usage};
                }
                if (action == "posttooluse")
                {
                    return hook_posttooluse(inv);
                }
                if (action == "pretooluse")
                {
                    return hook_pretooluse(inv);
                }
                return action == "sessionstart" ? hook_sessionstart(inv)
                                                : hook_userpromptsubmit(inv);
            }
            if (action != "install" && action != "uninstall"
                && action != "status")
            {
                throw UsageError{
                    std::format(
                        "hook action must be install, uninstall, or "
                        "status (got '{}')",
                        action),
                    usage};
            }
            if (inv.hook_user == inv.hook_project)
            {
                throw UsageError{
                    std::format(
                        "hook {} needs exactly one scope: --project "
                        "(the current directory's .claude/settings."
                        "json) or --user",
                        action),
                    usage};
            }
            const fs::path settings = settings_path(inv);
            if (action == "install")
            {
                return hook_install(inv, settings);
            }
            if (action == "uninstall")
            {
                return hook_uninstall(inv, settings);
            }
            return hook_status(inv, settings);
        }

    } // namespace detail
} // namespace cg::cli

/**
 * @module cg.graph.traverse.include
 *
 * @brief 包含闭包解析器 IncludeResolver：include 边的传递解析与路径归一。
 */
export module cg.graph.traverse.include;

import std;
import cg.util;
import cg.db;
import cg.graph.traverse.model;

export namespace cg::graph
{

    namespace detail
    {

        /**
         * @brief 文件表之上的包含目标解析器。
         *
         * 引号目标先对包含者目录解析，再按路径后缀解析；匹配多个文件的目标
         * 解析到其全部（对包含图过近似只多花一次冗余解析，欠近似则会静默漏掉
         * 脏 TU）。查找大小写不敏感：文件系统（在 Windows 上）如此，而存储的
         * 拼写来自各 TU 各自的包含搜索，两侧差异必须折掉。
         *
         * @internal
         * 增量引擎（cg.graph.build）的包含解析即走本类 resolve()。包含环检测
         * 额外使用 resolve_strict()——同一规则集的无歧义子集——因为上报的环
         * 必须真实，不能是保守扇出的伪影（见 resolve_strict 的说明）。
         */
        class IncludeResolver
        {
        public:
            void add(const std::string& path, cg::db::FileId fid)
            {
                const std::string lower = lowercase(cg::util::norm_path(path));
                by_path_[lower].push_back(fid);
                for (std::size_t i = lower.find('/'); i != std::string::npos;
                     i = lower.find('/', i + 1))
                {
                    by_suffix_[lower.substr(i + 1)].push_back(fid);
                }
            }

            /**
             * @brief 解析一条包含目标（保守版）。
             *
             * @return 全部候选文件 id；目标为空或无命中时为空集。
             *
             * @internal
             * 后缀回退：全路径未命中（定位文件的是包含搜索路径而非包含者
             * 目录）时，依次尝试拼接路径在 '/' 边界上的每个后缀；匹配多个
             * 文件的目标解析到其全部（过近似只多花冗余解析）。
             */
            [[nodiscard]]
            auto resolve(
                std::string_view target,
                std::string_view includer_path) const
                -> std::vector<cg::db::FileId>
            {
                namespace fs = std::filesystem;
                if (target.empty())
                {
                    return {};
                }
                fs::path t{std::string_view{target}};
                if (t.is_relative())
                {
                    fs::path base{std::string_view{includer_path}};
                    base.remove_filename();
                    t = base / t;
                }
                const std::string full = lowercase(
                    cg::util::norm_path(t.lexically_normal().generic_string()));
                if (const auto it = by_path_.find(full); it != by_path_.end())
                {
                    return it->second;
                }
                std::vector<cg::db::FileId> out;
                for (std::size_t i = full.find('/'); i != std::string::npos;
                     i = full.find('/', i + 1))
                {
                    const std::string suffix = full.substr(i + 1);
                    if (suffix.empty())
                    {
                        continue;
                    }
                    if (const auto it = by_suffix_.find(suffix);
                        it != by_suffix_.end())
                    {
                        for (const cg::db::FileId fid : it->second)
                        {
                            cg::util::push_unique(out, fid);
                        }
                    }
                }
                return out;
            }

            /**
             * @brief resolve() 的严格孪生，服务环检测（CI 门禁）。
             *
             * 判定顺序：引号相对目标先对包含者目录（编译器对引号包含先搜当前
             * 目录，该处的精确命中即无歧义事实）；否则以完整包含拼写作路径
             * 后缀——定位文件的是包含搜索路径而非包含者目录——仅当恰好一个
             * 已索引文件携带该后缀时接受。
             *
             * @internal
             * resolve() 会把多匹配后缀扇出到每个候选：对新鲜度检查是对的
             * （冗余解析廉价），对门禁是错的——捏造的边会变成任何源码编辑
             * 都无法消除的幻影环。此处绝不尝试更短的后缀（裸 "logger.h"
             * 拼写可匹配末段恰好折叠为同名的无关文件）。
             */
            struct StrictTarget
            {
                enum class Kind : std::uint8_t
                {
                    None,
                    Unique,
                    Ambiguous
                } kind = Kind::None;
                cg::db::FileId file = 0; // Kind::Unique 时有效
            };

            /**
             * @brief 以严格子集规则解析一条包含目标。
             *
             * @return 无命中为 Kind::None；唯一命中为 Kind::Unique 并携带文件；
             *         多命中为 Kind::Ambiguous（不贡献边）。
             *
             * @internal 对拼写本身做包含搜索路径仿真（完整拼写作后缀键）。
             */
            [[nodiscard]]
            auto resolve_strict(
                std::string_view target,
                std::string_view includer_path,
                bool             quoted) const -> StrictTarget
            {
                namespace fs = std::filesystem;
                if (target.empty())
                {
                    return {};
                }
                fs::path t{std::string_view{target}};
                if (t.is_absolute())
                {
                    const std::string full =
                        lowercase(cg::util::norm_path(t.generic_string()));
                    const auto it = by_path_.find(full);
                    return it == by_path_.end() || it->second.size() != 1
                               ? StrictTarget{}
                               : StrictTarget{
                                     .kind = StrictTarget::Kind::Unique,
                                     .file = it->second.front()};
                }
                if (quoted)
                {
                    fs::path base{std::string_view{includer_path}};
                    base.remove_filename();
                    const std::string joined = lowercase(
                        cg::util::norm_path(
                            (base / t).lexically_normal().generic_string()));
                    const auto it = by_path_.find(joined);
                    if (it != by_path_.end() && it->second.size() == 1)
                    {
                        return {
                            .kind = StrictTarget::Kind::Unique,
                            .file = it->second.front()};
                    }
                }
                const std::string spell = lowercase(
                    cg::util::norm_path(t.lexically_normal().generic_string()));
                const auto it = by_suffix_.find(spell);
                if (it == by_suffix_.end() || it->second.empty())
                {
                    return {};
                }
                return it->second.size() == 1
               ? StrictTarget{.kind = StrictTarget::Kind::Unique,
                              .file = it->second.front()}
               : StrictTarget{.kind = StrictTarget::Kind::Ambiguous};
            }

        private:
            [[nodiscard]]
            static auto lowercase(std::string s) -> std::string
            {
                return cg::util::ascii_lower(s);
            }

            std::unordered_map<std::string, std::vector<cg::db::FileId>>
                by_path_;
            std::unordered_map<std::string, std::vector<cg::db::FileId>>
                by_suffix_;
        };

    } // namespace detail
} // namespace cg::graph

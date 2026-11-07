/**
 * @module cg.util
 *
 * @brief 核心工具模块：版本串、散列、限定名处理、路径规范化、Windows
 *        命令串分词、字符串驻留、解析状态枚举。
 *
 * 核心刻意保持零依赖（仅 `import std;`），使其他每个模块都能建于其上
 * （分层最底层）。cg.util.json（yyjson 门面）、
 * cg.util.platform（OS 探针）、cg.util.guard（硬件故障防护）、
 * cg.util.pool（通用工人池）、cg.util.treesit（tree-sitter 门面）、
 * cg.util.cshost（C# 对象宿主：Select-String 引擎面与 MSBuild 求值
 * 面）、cg.util.msvcswitch（MSVC 模块开关规范拼写清单）与
 * cg.util.shellscan（shell 查找意图提取层）在此再导出，令伞面
 * `import cg.util;` 的用法原样保持可用。
 */
export module cg.util;

import std;

export import cg.util.json;
export import cg.util.platform;
export import cg.util.guard;
export import cg.util.pool;
export import cg.util.treesit;
export import cg.util.cshost;
export import cg.util.shellscan;
export import cg.util.msvcswitch;

export namespace cg::util
{

    /** @brief 应用版本号，逐字存入 db 的 meta 表。 */
    inline constexpr std::string_view version = "0.1.0";

    /**
     * @brief FNV-1a 散列（64 位）。
     *
     * 支撑 symbols 表的 sig_hash（免存签名文本的重载同一性）。
     * 非密码学：只求稳定、廉价、对字节序敏感。
     */
    inline constexpr auto fnv1a64(std::string_view s) noexcept -> std::uint64_t
    {
        constexpr std::uint64_t basis = 0xcbf29ce484222325ULL;
        constexpr std::uint64_t prime = 0x100000001b3ULL;
        std::uint64_t           h = basis;
        for (const char c : s)
        {
            h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
            h *= prime;
        }
        return h;
    }

    /**
     * @brief 把限定名按 "::" 切成组件。
     *
     * 空输入产出零个组件；相邻分隔符产出空组件（"a::::b" → {"a", "", "b"}），
     * 与 join_qualified 的往返契约一致。
     */
    [[nodiscard]]
    inline auto split_qualified(std::string_view qualified)
        -> std::vector<std::string_view>
    {
        std::vector<std::string_view> parts;
        if (qualified.empty())
        {
            return parts;
        }
        std::size_t pos = 0;
        for (;;)
        {
            const std::size_t sep = qualified.find("::", pos);
            if (sep == std::string_view::npos)
            {
                parts.push_back(qualified.substr(pos));
                return parts;
            }
            parts.push_back(qualified.substr(pos, sep - pos));
            pos = sep + 2;
        }
    }

    /**
     * @brief 按单字符分隔符切分（相邻分隔符产出空段，保留空段语义）。
     *
     * 环境变量（INCLUDE 的 ';'）与点分版本（'.'）等逐段扫描的仓内单点；
     * 空段是否跳过由调用方按各自输入域决定（Windows 路径列表的空段无
     * 意义，版本串的空段即畸形）。空输入产出零个组件。
     */
    [[nodiscard]]
    inline auto split(std::string_view text, char sep)
        -> std::vector<std::string_view>
    {
        std::vector<std::string_view> parts;
        if (text.empty())
        {
            return parts;
        }
        std::size_t pos = 0;
        for (;;)
        {
            const std::size_t end = text.find(sep, pos);
            parts.push_back(text.substr(
                pos,
                end == std::string_view::npos ? std::string_view::npos
                                              : end - pos));
            if (end == std::string_view::npos)
            {
                return parts;
            }
            pos = end + 1;
        }
    }

    /**
     * @brief 点分数字版本串（如 "14.51.36231"）解析为整数段。
     *
     * 任一段为空或含非数字字符时整体拒绝（返回空）；段数不限，段值以
     * int 承载（MSVC 工具集四段均远小于上限）。msbuild 管线与翻译层三
     * 份逐段扫描的仓内单点；各自的路径标记定位（/MSVC/ 等）留在调用方。
     */
    [[nodiscard]]
    inline auto parse_dotted_version(std::string_view text) -> std::vector<int>
    {
        std::vector<int> parts;
        std::size_t      start = 0;
        while (start <= text.size())
        {
            const std::size_t      dot = text.find('.', start);
            const std::string_view part = text.substr(
                start,
                dot == std::string_view::npos ? std::string_view::npos
                                              : dot - start);
            if (part.empty()
                || !std::ranges::all_of(
                    part,
                    [](char c) { return c >= '0' && c <= '9'; }))
            {
                return {};
            }
            std::int32_t value = 0;
            if (std::from_chars(part.data(), part.data() + part.size(), value)
                    .ec
                != std::errc{})
            {
                return {}; // 段值溢出 int（畸形数据）：整体拒绝
            }
            parts.push_back(value);
            if (dot == std::string_view::npos)
            {
                return parts;
            }
            start = dot + 1;
        }
        return parts;
    }

    /**
     * @brief 把一段区间用分隔符接成字符串。
     *
     * 元素须可显式转换为 std::string_view（std::string / std::string_view
     * 直用；整型段先经 views::transform(std::to_string) 再入）。空区间产
     * 出空串。join_qualified 是 "::" 特例的既有具名形态。
     *
     * @internal 刻意不做 length-prefix（长度前缀）形态：防歧义拼接
     * （哈希键构造）在别处自成单点，绝不与此混流。
     */
    template <std::ranges::input_range Range>
    [[nodiscard]]
    auto join(const Range& parts, std::string_view sep) -> std::string
    {
        std::string out;
        for (auto it = std::ranges::begin(parts); it != std::ranges::end(parts);
             ++it)
        {
            if (it != std::ranges::begin(parts))
            {
                out += sep;
            }
            out += std::string_view{*it};
        }
        return out;
    }

    /**
     * @brief 尾部追加且保序去重：元素已在区间中时不追加。
     *
     * 有序小向量的"首次出现胜出"收集模板（include 目录、token 清单、
     * 金丝雀路径）——线性查重在这里比哈希集合便宜（向量常只有几个元素，
     * 且保持首见顺序是渲染/日志语义）。大集合去重请用 set/unordered_set。
     */
    template <typename Vec, typename Value>
    void push_unique(Vec& out, Value&& value)
    {
        if (std::ranges::find(out, value) == out.end())
        {
            out.push_back(std::forward<Value>(value));
        }
    }

    /** @brief 用 "::" 把组件接回；split_qualified 的逆操作。 */
    [[nodiscard]]
    inline auto join_qualified(std::span<const std::string_view> parts)
        -> std::string
    {
        return join(parts, "::");
    }

    /**
     * @brief 签名哈希：对规范参数列表做 FNV-1a64 后的
     *        有符号 64 位重解释。
     *
     * 该值同时进入 unique_key 的 `p<hash>` 段与 symbols 表的 sig_hash 列
     * （重载身份），故是持久化身份的一部分——本函数是全仓唯一实现，既往
     * extract/analysis 两份逐字节雷同的拷贝已收编于此（tests/schema-mirror
     * 有已知值对照断言钉死逐位不变）。
     *
     * @internal 参数拼写以 ',' 连接（无分隔符转义——参数类型拼写不含逗号
     * 的前提由抽取侧的规范化保证，与 compose_key 同口径）。
     */
    [[nodiscard]]
    inline auto sig_hash_of(const std::vector<std::string>& param_types)
        -> std::int64_t
    {
        return static_cast<std::int64_t>(fnv1a64(join(param_types, ",")));
    }

    /**
     * @brief 剥除两端的空白（空格、水平制表符、回车、换行）。
     *
     * 仓内 trim 的唯一定义（替换昔日三份字符集各异的分叉实现：msbuild
     * 行扫描的头 `" \t"` 尾 `" \t\r"`、驱动器输出的 `" \t\r\n"`、类型拼写的
     * `" \t"`——三者的实际输入域都不含行首 `\r` 或行内 `\n`，统一到标准空白集
     * 行为等价）。全空白或空输入返回空串。
     */
    [[nodiscard]]
    inline auto trim(std::string_view value) -> std::string
    {
        constexpr std::string_view whitespace{" \t\r\n"};
        const std::size_t          first = value.find_first_not_of(whitespace);
        if (first == std::string_view::npos)
        {
            return {};
        }
        const std::size_t last = value.find_last_not_of(whitespace);
        return std::string{value.substr(first, last - first + 1)};
    }

    /**
     * @brief 剥掉串尾的 CR（getline 消费 '\n' 后在 CRLF 输入上留下的残余）。
     *
     * 逐行扫描的仓内单点；空串与尾字符非 CR 均为无操作。
     */
    inline void strip_cr(std::string& line)
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
    }

    /**
     * @brief 按行回调消费文本：行不含行尾符，"\r\n" 的 CR 一并剥掉。
     *
     * 与 std::getline 逐行语义一致：末尾无换行的残段仍作为最后一行回调、
     * 文本以 '\n' 结束时不再多出空行、空文本零次回调。
     */
    template <typename Fn>
    void for_each_line(std::string_view text, Fn&& fn)
    {
        std::size_t pos = 0;
        while (pos < text.size())
        {
            const std::size_t      nl = text.find('\n', pos);
            const std::string_view line = text.substr(
                pos,
                nl == std::string_view::npos ? std::string_view::npos
                                             : nl - pos);
            if (line.ends_with('\r'))
            {
                fn(line.substr(0, line.size() - 1));
            }
            else
            {
                fn(line);
            }
            if (nl == std::string_view::npos)
            {
                return;
            }
            pos = nl + 1;
        }
    }

    /**
     * @brief 有界整数解析：来源缺失、非数字或解析值非正时回退默认值。
     *
     * db meta 键与环境变量 knobs 的仓内同模板单点（from_chars 族）。与被
     * 收编的 atoll 实现同口径：数字前缀即取值（尾随垃圾不整段拒绝）、空
     * 串与非数字前缀解析为失败；值由本工具经 to_string 写入，无空白/正
     * 号形态。非正数（含 0）同样回退——knobs 的语义是"时长/上限必须为
     * 正，否则用默认"。
     */
    [[nodiscard]]
    inline auto parse_bounded(
        const std::optional<std::string>& raw,
        std::int64_t                      fallback) -> std::int64_t
    {
        if (!raw.has_value())
        {
            return fallback;
        }
        std::int64_t value = 0;
        const char*  begin = raw->c_str();
        const char*  end = begin + raw->size();
        if (std::from_chars(begin, end, value).ec != std::errc{} || value <= 0)
        {
            return fallback;
        }
        return value;
    }

    /**
     * @brief ASCII 大小写折叠：'A'-'Z' 映射为小写，其余字节原样。
     *
     * 刻意只做 ASCII——字节映射在每个 locale 下都确定，不像
     * std::tolower（路径与扩展名比较的既定选择）。仓内 ASCII 折叠的唯一定义
     * （替换昔日路径折叠、方言 flag 折叠、db 文件路径匹配各自的手写版本）。
     */
    /** @brief ascii_lower 的单字符形态：逐字符比较处的折叠单点。 */
    [[nodiscard]]
    inline auto ascii_lower_char(char c) -> char
    {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    [[nodiscard]]
    inline auto ascii_lower(std::string_view text) -> std::string
    {
        std::string out;
        out.reserve(text.size());
        for (const char c : text)
        {
            out.push_back(ascii_lower_char(c));
        }
        return out;
    }

    /**
     * @brief ASCII 大小写不敏感的全串相等。
     *
     * 与 ascii_lower 同一折叠口径（仅 'A'-'Z'，locale 无关）。仓内 ASCII
     * 比较判定的唯一定义（收敛昔日 msbuild 管线与 db 检索各自的本地版本）。
     */
    [[nodiscard]]
    inline auto ascii_iequals(std::string_view a, std::string_view b) -> bool
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (ascii_lower_char(a[i]) != ascii_lower_char(b[i]))
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief ASCII 大小写不敏感的子串判定（hay 是否包含 needle）。
     *
     * @internal 只折叠 ASCII 字母：标识符与查询都是 ASCII 为主的 C++ 拼
     * 写，locale 无关的折叠保证跨环境确定性；非 ASCII 字节按原值比较
     * （UTF-8 标识符走字节精确匹配，不做 Unicode 折叠）。空 needle 恒真；
     * hay 短于 needle 恒假。
     */
    [[nodiscard]]
    inline auto ascii_icontains(std::string_view hay, std::string_view needle)
        -> bool
    {
        if (needle.empty())
        {
            return true;
        }
        if (hay.size() < needle.size())
        {
            return false;
        }
        const auto fold = [](char c) { return ascii_lower_char(c); };
        for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
        {
            std::size_t j = 0;
            while (j < needle.size() && fold(hay[i + j]) == fold(needle[j]))
            {
                ++j;
            }
            if (j == needle.size())
            {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief \x1f 复合键的两段拼接（槽键/站点键等持久化与内存键共用形态）。
     *
     * @internal 分隔符取 \x1f（标识符与路径字符域之外的控制字符）。刻意
     * 不做任何成分转义：各成分的字符域由调用方保证不含分隔符，而键已进
     * 持久化（analysis_slots 等），逐字节形态是兼容红线——转义会改变在库
     * 键的相等性。与昔日各处的手写 += / std::format 拼法逐字节等价。
     */
    [[nodiscard]]
    inline auto compose_key(std::string_view a, std::string_view b)
        -> std::string
    {
        std::string out;
        out.reserve(a.size() + b.size() + 1);
        out += a;
        out.push_back('\x1f');
        out += b;
        return out;
    }

    /**
     * @brief 透明字符串散列：配合 std::equal_to<> 让 unordered_map/set 支持
     *        string_view 异构直查。
     *
     * 查表方持有 string_view（游标路径、meta 键）时不再为构造键临时拷贝
     * 整个字符串；映射声明形如
     * `std::unordered_map<std::string, V, cg::util::string_hash,
     * std::equal_to<>>`。
     */
    struct string_hash
    {
        using is_transparent = void;

        [[nodiscard]]
        std::size_t operator()(std::string_view s) const noexcept
        {
            return std::hash<std::string_view>{}(s);
        }
    };

    /**
     * @brief UTF-8 引导字节给出的码点序列长度（1-4）。
     *
     * 非法引导字节（0x80-0xBF 或 0xF8 以上）按 1 处理：调用方对残缺/畸形
     * 序列的既有口径是"逐字节消费、以替换字符补位"，本函数不改变该策略，
     * 只把长度分类收成单点。
     */
    [[nodiscard]]
    inline auto utf8_seq_len(char lead) -> std::size_t
    {
        const auto c = static_cast<unsigned char>(lead);
        if ((c & 0xE0U) == 0xC0U)
        {
            return 2;
        }
        if ((c & 0xF0U) == 0xE0U)
        {
            return 3;
        }
        if ((c & 0xF8U) == 0xF0U)
        {
            return 4;
        }
        return 1;
    }

    /** @brief 字节是否 UTF-8 续字节（10xxxxxx）。 */
    [[nodiscard]]
    inline auto utf8_is_continuation(char c) -> bool
    {
        return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U;
    }

    /**
     * @brief MSBuild/VS 项目与属性文件的扩展分类。
     *
     * 这些文件的编辑改的是"语料从哪来"（TU 清单、包含路径、属性继承），
     * 而不是任何 TU 的字节；它们触发的是重采集联动，绝不进入常规 TU 解析。
     * 清单与提取输入及属性链的一切形态对齐：.sln（解决方案）、.vcxproj
     * （工程）、.vcxitems（共享项工程）、.props（属性表）、.targets（生成
     * 目标）、.filters（IDE 过滤器树——不动构建语义但与项目文件同生同灭，
     * 一并入队交给持有者裁决），外加 CMake 的 CMakeLists.txt（无扩展名
     * 惯例的完整文件名，按折叠 basename 特判——后缀 .txt 会把一切文本
     * 文件吸进联动通道，绝不能走后缀判定）。平台中立：纯文本比较，无
     * 平台分支。
     *
     * @internal 比较按 ASCII 折叠后缀进行（与仓内路径比较纪律一致）：漏掉
     * 一个 .VCXPROJ 事件意味着整条联动链失灵，而多收一个文件的代价只是
     * 持有者按未跟踪出队——风险不对称，故不做字节精确比较。
     */
    [[nodiscard]]
    inline auto is_project_file(std::string_view path) -> bool
    {
        static constexpr std::string_view EXTS[] = {
            ".sln",
            ".vcxproj",
            ".vcxitems",
            ".props",
            ".targets",
            ".filters",
        };
        const std::string lower = ascii_lower(path);
        for (const std::string_view ext : EXTS)
        {
            if (lower.size() >= ext.size()
                && lower.compare(lower.size() - ext.size(), ext.size(), ext)
                       == 0)
            {
                return true;
            }
        }
        const std::size_t      slash = lower.find_last_of("/\\");
        const std::string_view base =
            lower.substr(slash == std::string::npos ? 0 : slash + 1);
        return base == "cmakelists.txt";
    }

    /**
     * @brief C/C++ 源文件扩展名的分类（单一事实源）。
     *
     * 既往五张分散的扩展名表（compile_db×2、translate、pipeline、钩子）
     * 大小写语义不一，且钩子处大写 .CPP 静默漏判。本枚举把全仓用到的
     * 源扩展名切成语义类，各消费点以"类的具名子集"表达自己的集合（见
     * 下方 is_* 谓词），扩展名拼写与折叠规则只此一处。
     *
     * @internal 与 is_project_file 同先例：按 ASCII 折叠后缀判定（风险
     * 不对称——漏判 .CPP 意味着整条管线对它失明，多收一个文件的代价由
     * 各消费点自己的下游判定吸收）。
     */
    enum class SourceFileClass : std::uint8_t
    {
        None,          ///< 非源文件（配置/文档/脚本等）
        CSource,       ///< .c——C 翻译单元
        CppSource,     ///< .cpp/.cxx/.cc/.c++——C++ 翻译单元
        ModuleUnit,    ///< .cppm/.cxxm/.ixx/.mpp/.mxx/.c++m——模块单元
        Header,        ///< .h/.hh/.hpp/.hxx/.inl——常规头文件
        ForcedInclude, ///< .inc——强制包含体（Unity 拼接块）
        ComGenerated,  ///< .tlh/.tli——MSVC COM 类型库生成头
    };

    /**
     * @brief 按折叠后缀把路径归类到源文件类；非源扩展给 None。
     *
     * 输入可以是完整路径或裸 token（PCH 旗标值等）：判定只看折叠后的
     * 尾段。
     */
    [[nodiscard]]
    inline auto classify_source_path(std::string_view path) -> SourceFileClass
    {
        const std::string lower = ascii_lower(path);
        struct Entry
        {
            std::string_view ext;
            SourceFileClass  cls;
        };
        static constexpr Entry TABLE[] = {
            {".cpp", SourceFileClass::CppSource},
            {".cxx", SourceFileClass::CppSource},
            {".cc", SourceFileClass::CppSource},
            {".c++", SourceFileClass::CppSource},
            {".c", SourceFileClass::CSource},
            {".cppm", SourceFileClass::ModuleUnit},
            {".cxxm", SourceFileClass::ModuleUnit},
            {".ixx", SourceFileClass::ModuleUnit},
            {".mpp", SourceFileClass::ModuleUnit},
            {".mxx", SourceFileClass::ModuleUnit},
            {".c++m", SourceFileClass::ModuleUnit},
            {".h", SourceFileClass::Header},
            {".hh", SourceFileClass::Header},
            {".hpp", SourceFileClass::Header},
            {".hxx", SourceFileClass::Header},
            {".inl", SourceFileClass::Header},
            {".inc", SourceFileClass::ForcedInclude},
            {".tlh", SourceFileClass::ComGenerated},
            {".tli", SourceFileClass::ComGenerated},
        };
        for (const Entry& e : TABLE)
        {
            if (lower.size() >= e.ext.size()
                && lower.compare(
                       lower.size() - e.ext.size(),
                       e.ext.size(),
                       e.ext)
                       == 0)
            {
                return e.cls;
            }
        }
        return SourceFileClass::None;
    }

    /**
     * @brief 翻译单元输入扩展（cl 分写旗标值的"源文件不是头名"判定）。
     *
     * 子集 = CSource ∪ CppSource ∪ ModuleUnit。消费点：compile_db 的
     * PCH 旗标值判定、msbuild 适配器（translate）与 pch 重放剥离。既往
     * translate 侧少收 .mpp/.mxx/.c++m 三拼（compile_db 注释自认"与
     * msbuild 适配器同语义"，集合却漂移）——收编时统一到全集合。
     */
    [[nodiscard]]
    inline auto is_tu_input_file(std::string_view path) -> bool
    {
        using C = SourceFileClass;
        switch (classify_source_path(path))
        {
        case C::CSource:
        case C::CppSource:
        case C::ModuleUnit:
            return true;
        default:
            return false;
        }
    }

    /** @brief 模块单元扩展（compile_db 的 is_module_tu 判定面）。 */
    [[nodiscard]]
    inline auto is_module_unit_file(std::string_view path) -> bool
    {
        return classify_source_path(path) == SourceFileClass::ModuleUnit;
    }

    /**
     * @brief 索引输入类源码扩展（生成器白名单的准入面）。
     *
     * 子集 = 翻译单元 ∪ Header ∪ ComGenerated。消费点：msbuild 管线的
     * 自定义生成步骤输出判定（.tlh/.tli 是 #import 的生成头，属索引输入；
     * .inc 不在其列）。
     */
    [[nodiscard]]
    inline auto is_indexable_source_file(std::string_view path) -> bool
    {
        using C = SourceFileClass;
        switch (classify_source_path(path))
        {
        case C::CSource:
        case C::CppSource:
        case C::ModuleUnit:
        case C::Header:
        case C::ComGenerated:
            return true;
        default:
            return false;
        }
    }

    /**
     * @brief 钩子语料扩展（编辑值得走符号影响面注入的文件）。
     *
     * 子集 = 翻译单元 ∪ Header ∪ ForcedInclude（.inc 与 .inl 同为文本
     * 拼接进 TU 的头形态；.tlh/.tli 是生成物，编辑它不等于编辑源）。
     * 项目文件（is_project_file）另走重采集联动通道，不在此列。
     */
    [[nodiscard]]
    inline auto is_hook_corpus_file(std::string_view path) -> bool
    {
        using C = SourceFileClass;
        switch (classify_source_path(path))
        {
        case C::CSource:
        case C::CppSource:
        case C::ModuleUnit:
        case C::Header:
        case C::ForcedInclude:
            return true;
        default:
            return false;
        }
    }

    /** @brief 纯 C++ 源单元扩展（graph 层"无包含者的源文件即删除"判定）。 */
    [[nodiscard]]
    inline auto is_cpp_source_file(std::string_view path) -> bool
    {
        return classify_source_path(path) == SourceFileClass::CppSource;
    }

    /**
     * @brief 规范化路径以供存储与比较。
     *
     * 反斜杠变正斜杠；分隔符连跑折成一个（前导 "//" 折成 "/"——语料路径都
     * 基于盘符）；盘符小写。其余字符逐字保留大小写。
     */
    [[nodiscard]]
    inline auto norm_path(std::string_view path) -> std::string
    {
        std::string out;
        out.reserve(path.size());
        for (const char raw : path)
        {
            const char c = (raw == '\\') ? '/' : raw;
            if (c == '/' && !out.empty() && out.back() == '/')
            {
                continue;
            }
            out.push_back(c);
        }
        if (out.size() >= 2 && out[1] == ':' && out[0] >= 'A' && out[0] <= 'Z')
        {
            out[0] = static_cast<char>(out[0] - 'A' + 'a');
        }
        return out;
    }

    /**
     * @brief 仅把反斜杠换成正斜杠，其余逐字保留（含大小写与分隔符连跑）。
     *
     * 与 norm_path 的分工：本函数是纯字符替换，语义严格收窄——调用方只
     * 要分隔符统一（比较、标记扫描），不要折叠连跑、不要小写盘符时用它；
     * 那些是存储规范化的额外含义，误用会改变键的身份。原地形态直接用
     * `std::ranges::replace(s, '\\', '/')`。
     */
    [[nodiscard]]
    inline auto to_forward_slashes(std::string_view path) -> std::string
    {
        std::string out;
        out.reserve(path.size());
        for (const char c : path)
        {
            out.push_back(c == '\\' ? '/' : c);
        }
        return out;
    }

    /**
     * @brief 路径比较键：分隔符折成正斜杠 + ASCII 小写。
     *
     * 路径规范化两个具名变体之一（另一个是 norm_path 的"存储规范化"）：
     * 比较规范化只服务"两个拼写是否指同一路径"的等值判定，不做存储键的
     * 身份整形（不折叠分隔符连跑、不动盘符），因此对比已存储规范化的
     * 路径与任意原始拼写都成立——两侧同经本函数折叠后比较。
     *
     * @internal 各处"ascii_lower + 手写反斜杠替换（或只 lower 已规范化
     * 路径）"的折叠比较拼法收编于此；语义是二者的并集（多折分隔符对已
     * 规范化输入是空操作，对原始拼写是对的方向）。
     */
    [[nodiscard]]
    inline auto path_compare_key(std::string_view path) -> std::string
    {
        return ascii_lower(to_forward_slashes(path));
    }

    /**
     * @brief 路径末段（文件名与扩展名），'/' 与 '\' 都认作分隔符。
     *
     * 无分隔符时整串即末段；尾部分隔符给出空段（与既有各手写版本一致，
     * 调用方按输入域保证不以分隔符结尾）。要小写形态配
     * ascii_lower(path_basename(...))。
     */
    [[nodiscard]]
    inline auto path_basename(std::string_view path) -> std::string_view
    {
        const std::size_t slash = path.find_last_of("/\\");
        return slash == std::string_view::npos ? path : path.substr(slash + 1);
    }

    /**
     * @brief 在已规范化路径上按词典法折叠 "." 与 "seg/.." 段。
     *
     * 纯文本操作，不碰文件系统；前导 ".." 段存活，使相对爬出保持可辨认。
     * clang 以 include 搜索的拼写（"a/../b/x.h"）报告头文件位置，故游标
     * 路径必须先折叠再成为文件身份（存储规则：一个物理文件恰一行
     * files 记录）。
     *
     * @internal
     * 经 norm 的视图切分：std::string::substr 会返回临时 string，其 SSO
     * 缓冲随所在语句结束死亡，栈上每一段都会悬垂（实测在 ".." 拼写的引号
     * include 上表现为交错的乱码文件名）。
     */
    [[nodiscard]]
    inline auto collapse_dot_segments(std::string_view path) -> std::string
    {
        std::string norm = norm_path(path);
        if (norm.find("..") == std::string::npos
            && norm.find("/./") == std::string::npos)
        {
            return norm;
        }
        const std::string_view        nv{norm};
        std::vector<std::string_view> stack;
        std::size_t                   start = 0;
        while (start <= nv.size())
        {
            const std::size_t      end = nv.find('/', start);
            const std::string_view seg = nv.substr(
                start,
                end == std::string_view::npos ? std::string_view::npos
                                              : end - start);
            if (seg == "..")
            {
                if (!stack.empty() && stack.back() != "..")
                {
                    stack.pop_back(); // "x/.." 相消
                }
                else
                {
                    stack.push_back(seg); // 前导爬出段存活
                }
            }
            else if (!seg.empty() && seg != ".")
            {
                stack.push_back(seg);
            }
            if (end == std::string_view::npos)
            {
                break;
            }
            start = end + 1;
        }
        return join(stack, "/");
    }

    /**
     * @brief canonical_case_spelling 的缓存对象。
     *
     * 每个基目录一份目录列表（fold 表），外加整路径结果，显式归调用方持有。
     *
     * @internal
     * 这份状态曾是拼写探针内部函数局部 thread_local 的隐藏缓存；现在是每个
     * 抽取 worker（及任何其他探针调用方）在运行期间持有的 per-worker 状态
     * ——一个 worker 一份缓存把语料走一遍，任何地方都没有隐藏全局或函数静态
     * 生命周期。
     */
    class PathCaseCache
    {
    public:
        PathCaseCache() = default;

        /**
         * @brief 用段在磁盘上携带的大小写重拼已规范化路径的每一段。
         *
         * include 搜索按 -I 标志当时的拼写引用头文件，因此混用同一目录
         * "src" 与 "SRC" 两种拼写的 compile db 会给每个文件铸出两个身份
         * （唯一键内嵌路径），在真实子图旁边复制出一份重复子图；磁盘拼写
         * 是唯一稳定的。大小写敏感的文件系统保持同 fold 文件相区分：目录
         * 内两个 fold 相同的入口记为歧义、两种拼写都原样保留；没有任何
         * 匹配的段也原样保留（路径在该处要么精确存在，要么不存在）。探测
         * 结果按目录与整路径缓存在本实例中。
         *
         * @internal
         * 每个基目录恰一份目录列表，供后续所有段复用。先收集、后打标记：
         * 进行中扫描的半成品状态（非局部退出——crash-guard 路径上迭代内的
         * 故障会恢复到此帧之后——跳过循环的剩余部分）绝不能被观察成已完成
         * 的扫描，否则后续每次探测都会对着残缺列表反复重拼段。未打标记的
         * 入口只会触发重新扫描。探测基取到此为止的路径（仍相对时为 "."，
         * 盘符前缀之后取驱动器根——对 "e:" 调 directory_iterator 读到的
         * 是该驱动器的工作目录而非其根）。不可读的基目录：缓存保持空、
         * 各段保持原样。
         */
        [[nodiscard]]
        auto canonical_case_spelling(std::string_view path) -> std::string
        {
            const std::string key{path};
            if (const auto hit = whole_.find(key); hit != whole_.end())
            {
                return hit->second;
            }

            const auto entries_of =
                [&](const std::string& base) -> const DirEntries&
            {
                if (const auto hit = dirs_.find(base);
                    hit != dirs_.end() && hit->second.scanned)
                {
                    return hit->second;
                }
                std::unordered_map<std::string, std::string> by_fold;
                try
                {
                    namespace fs = std::filesystem;
                    std::error_code ec;
                    for (fs::directory_iterator it{fs::path{base}, ec}, end;
                         !ec && it != end;
                         it.increment(ec))
                    {
                        const std::string name =
                            it->path().filename().generic_string();
                        if (!by_fold.try_emplace(ascii_lower(name), name)
                                 .second)
                        {
                            by_fold[ascii_lower(name)] =
                                ""; // 歧义 fold：两种拼写都保持原样
                        }
                    }
                }
                catch (...)
                {
                }
                DirEntries filled;
                filled.by_fold = std::move(by_fold);
                filled.scanned = true;
                const auto [it, inserted] =
                    dirs_.insert_or_assign(base, std::move(filled));
                return it->second;
            };

            std::string out;
            std::size_t pos = 0;
            if (path.size() >= 2 && path[1] == ':')
            {
                out.assign(path.substr(0, 2)); // 盘符前缀，已小写
                pos = 2;
            }
            while (pos < path.size())
            {
                if (path[pos] == '/')
                {
                    if (out.empty() || out.back() != '/')
                    {
                        out.push_back('/');
                    }
                    ++pos;
                    continue;
                }
                std::string base = out.empty()
                                       ? std::string{"."}
                                       : (out.back() == ':' ? out + "/" : out);
                const std::size_t end = path.find('/', pos); // 段 = [pos, end)
                const std::string seg{path.substr(
                    pos,
                    end == std::string_view::npos ? std::string_view::npos
                                                  : end - pos)};
                const DirEntries& d = entries_of(base);
                std::string       actual = seg;
                if (const auto it = d.by_fold.find(ascii_lower(seg));
                    it != d.by_fold.end() && !it->second.empty()
                    && it->second != seg)
                {
                    actual = it->second;
                }
                if (!out.empty() && out.back() != '/')
                {
                    out.push_back('/');
                }
                out += actual;
                pos = end == std::string_view::npos ? path.size() : end;
            }
            if (out.empty())
            {
                out = key;
            }
            whole_.emplace(key, out);
            return out;
        }

    private:
        /**
         * @brief fold(段) → 磁盘上的拼写；"" 标记歧义 fold（两个目录入口仅
         *        大小写不同——POSIX 形态）。
         */
        struct DirEntries
        {
            std::unordered_map<std::string, std::string> by_fold;
            bool                                         scanned = false;
        };

        std::unordered_map<std::string, DirEntries>  dirs_;
        std::unordered_map<std::string, std::string> whole_;
    };

    /**
     * @brief SHA-256（FIPS 180-4），自包含实现。
     *
     * 封闭的第三方集合里没有密码学库，而 files 表需要足以支撑增量重索引
     * 决策的内容散列。
     *
     * @return 64 个小写十六进制字符。
     *
     * @internal 填充消息的布局：data + 0x80 + 零 + 64 位大端位长度。
     */
    [[nodiscard]]
    inline auto sha256_hex(std::span<const std::byte> data) -> std::string
    {
        constexpr std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
            0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
            0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
            0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
            0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        const auto rotr = [](std::uint32_t x, unsigned n)
        { return (x >> n) | (x << (32 - n)); };

        std::uint32_t h[8] = {
            0x6a09e667,
            0xbb67ae85,
            0x3c6ef372,
            0xa54ff53a,
            0x510e527f,
            0x9b05688c,
            0x1f83d9ab,
            0x5be0cd19};

        const std::size_t bit_len = data.size() * 8;
        const std::size_t padded = ((data.size() + 8) / 64 + 1) * 64;
        // 一次分配到位（容量即 padded）再就地填充：旧写法"按 size 构造拷贝
        // + push_back + resize"在同一缓冲上最多两次再分配拷贝。
        std::vector<std::uint8_t> msg(padded, 0);
        std::memcpy(msg.data(), data.data(), data.size());
        msg[data.size()] = 0x80;
        for (int i = 0; i < 8; ++i)
        {
            msg[padded - 1 - i] =
                static_cast<std::uint8_t>((bit_len >> (8 * i)) & 0xff);
        }

        for (std::size_t off = 0; off < padded; off += 64)
        {
            std::uint32_t w[64];
            for (int t = 0; t < 16; ++t)
            {
                w[t] =
                    (static_cast<std::uint32_t>(msg[off + 4 * t]) << 24)
                    | (static_cast<std::uint32_t>(msg[off + 4 * t + 1]) << 16)
                    | (static_cast<std::uint32_t>(msg[off + 4 * t + 2]) << 8)
                    | static_cast<std::uint32_t>(msg[off + 4 * t + 3]);
            }
            for (int t = 16; t < 64; ++t)
            {
                const std::uint32_t s0 =
                    rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
                const std::uint32_t s1 =
                    rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
                w[t] = w[t - 16] + s0 + w[t - 7] + s1;
            }
            std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
            std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
            for (int t = 0; t < 64; ++t)
            {
                const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                const std::uint32_t ch = (e & f) ^ (~e & g);
                const std::uint32_t t1 = hh + s1 + ch + k[t] + w[t];
                const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                const std::uint32_t t2 = s0 + maj;
                hh = g;
                g = f;
                f = e;
                e = d + t1;
                d = c;
                c = b;
                b = a;
                a = t1 + t2;
            }
            h[0] += a;
            h[1] += b;
            h[2] += c;
            h[3] += d;
            h[4] += e;
            h[5] += f;
            h[6] += g;
            h[7] += hh;
        }

        std::string out;
        out.reserve(64);
        for (const std::uint32_t word : h)
        {
            out += std::format("{:08x}", word);
        }
        return out;
    }

    /**
     * @brief sha256_hex 的文件内容便捷封装。
     * @throws std::runtime_error 文件无法读取时抛出（消息含路径）。
     */
    [[nodiscard]]
    inline auto sha256_file_hex(std::string_view path) -> std::string
    {
        std::ifstream in{
            std::filesystem::path{std::string_view{path}},
            std::ios::binary};
        if (!in)
        {
            throw std::runtime_error(
                std::format("cannot read file for hashing: {}", path));
        }
        // 分块直读进唯一驻留缓冲：旧写法 ostringstream 的内部缓冲加 str()
        // 返回拷贝让同一文件内容多份并存（哈希本身另需一份填充缓冲）。
        std::string bytes;
        char        chunk[65536];
        while (in.read(chunk, sizeof(chunk)) || in.gcount() > 0)
        {
            bytes.append(chunk, static_cast<std::size_t>(in.gcount()));
            if (!in)
            {
                break;
            }
        }
        return sha256_hex(std::as_bytes(std::span{bytes}));
    }

    /**
     * @brief 整文件读入为字符串（二进制口径，不做任何文本转换）。
     *
     * 打不开（不存在/无权限）返回 nullopt；读取中途失败同样按 nullopt
     * 处理。仓内整文件读入的单点——调用方按需在 nullopt 上抛出自带语
     * 境的异常或走静默降级路径。
     */
    [[nodiscard]]
    inline auto read_text_file(const std::string& path)
        -> std::optional<std::string>
    {
        std::ifstream in{std::filesystem::path{path}, std::ios::binary};
        if (!in)
        {
            return std::nullopt;
        }
        std::string bytes;
        char        chunk[65536];
        while (in.read(chunk, sizeof(chunk)) || in.gcount() > 0)
        {
            bytes.append(chunk, static_cast<std::size_t>(in.gcount()));
            if (!in)
            {
                break;
            }
        }
        return bytes;
    }

    /**
     * @brief 按 MSVC CRT 规则把 Windows 命令串切成 argv 记号。
     *
     * 该规则即 compile_commands.json "command" 字段在本平台上的事实语义：
     *   - 2n 个反斜杠后跟一个引号 → n 个反斜杠，该引号充当引用分隔符
     *     （切换引用模式）；
     *   - 2n+1 个反斜杠后跟一个引号 → n 个反斜杠加一个字面引号（引用
     *     模式不变）；
     *   - 后面不跟引号的反斜杠是字面量；
     *   - 双引号切换引用模式（绝不成为记号的一部分）；
     *   - 引用外的空白分隔记号；引用内的不分。
     */
    [[nodiscard]]
    inline auto tokenize_command(std::string_view command)
        -> std::vector<std::string>
    {
        std::vector<std::string> tokens;
        std::string              current;
        bool                     in_quotes = false;
        bool                     have_token = false;
        std::size_t              i = 0;
        const auto               is_space = [](char c)
        { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
        while (i < command.size())
        {
            const char c = command[i];
            if (c == '\\')
            {
                std::size_t n = 0;
                while (i + n < command.size() && command[i + n] == '\\')
                {
                    ++n;
                }
                if (i + n < command.size() && command[i + n] == '"')
                {
                    current.append(n / 2, '\\');
                    if (n % 2 == 1)
                    {
                        current.push_back('"');
                        i += n + 1; // 该引号被一并消费为字面量
                    }
                    else
                    {
                        i += n; // 该引号留待下一轮读取并切换引用模式
                    }
                }
                else
                {
                    current.append(n, '\\');
                    i += n;
                }
                have_token = true;
            }
            else if (c == '"')
            {
                in_quotes = !in_quotes;
                have_token = true; // 连续两个引号产生一个空实参
                ++i;
            }
            else if (!in_quotes && is_space(c))
            {
                if (have_token)
                {
                    tokens.push_back(std::move(current));
                    current.clear();
                    have_token = false;
                }
                ++i;
            }
            else
            {
                current.push_back(c);
                have_token = true;
                ++i;
            }
        }
        if (have_token)
        {
            tokens.push_back(std::move(current));
        }
        return tokens;
    }

    /** @brief 镜像 files.parse_status：微型整数，默认 0。
     */
    enum class ParseStatus : std::uint8_t
    {
        Pending = 0,
        Ok = 1,
        Partial = 2,
        Failed = 3,
    };

    /**
     * @brief 建立在调用方注入的持久化回调之上的双向字符串驻留器。
     *
     * 持有每个被驻留字符串的稳定拷贝；id 是 `store` 回调首次见到某字符串时
     * 返回的值（典型为 INSERT ... RETURNING 的 rowid）。`load` 让全新的驻留器
     * 收养 db 中已有的 id。本类对数据库一无所知——cg.db 负责接线回调。
     */
    class Interner
    {
    public:
        using id_type = std::int64_t; // 匹配 SQLite INTEGER / rowid id
        using LoadFn = std::function<std::optional<id_type>(std::string_view)>;
        using StoreFn = std::function<id_type(std::string_view)>;

        Interner() = default;
        Interner(LoadFn load, StoreFn store)
            : load_(std::move(load)), store_(std::move(store))
        {
        }

        /**
         * @brief 返回 s 的 id，首次见到时经 store 回调存储。
         *
         * 重复调用绝不再触 store；字符串一旦驻留内存，也绝不再触 load。
         */
        auto intern(std::string_view s) -> id_type
        {
            if (const auto it = by_str_.find(s); it != by_str_.end())
            {
                return it->second;
            }
            if (load_)
            {
                if (auto existing = load_(s))
                {
                    remember(s, *existing);
                    return *existing;
                }
            }
            if (!store_)
            {
                throw std::runtime_error(
                    "cg::util::Interner: no store callback");
            }
            const id_type id = store_(s);
            remember(s, id);
            return id;
        }

        /** @brief 仅查内存；绝不触碰回调。 */
        [[nodiscard]]
        auto lookup(std::string_view s) const -> std::optional<id_type>
        {
            if (const auto it = by_str_.find(s); it != by_str_.end())
            {
                return it->second;
            }
            return std::nullopt;
        }

        [[nodiscard]]
        auto str(id_type id) const -> std::optional<std::string_view>
        {
            if (const auto it = by_id_.find(id); it != by_id_.end())
            {
                return it->second;
            }
            return std::nullopt;
        }

        [[nodiscard]]
        auto size() const noexcept -> std::size_t
        {
            return by_str_.size();
        }

    private:
        void remember(std::string_view s, id_type id)
        {
            pool_.emplace_back(s); // 自持拷贝；deque 从不搬移元素
            const std::string_view stable = pool_.back();
            by_str_.emplace(stable, id);
            by_id_.emplace(id, stable);
        }

        std::deque<std::string>                       pool_; // 稳定存储
        std::unordered_map<std::string_view, id_type> by_str_;
        std::unordered_map<id_type, std::string_view> by_id_;
        LoadFn                                        load_;
        StoreFn                                       store_;
    };

} // namespace cg::util

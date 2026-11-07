/**
 * @module cg.extract.model
 *
 * @brief 访问器（单个翻译单元进）与图构建器（记录出）之间交换的普通
 *        记录类型，外加唯一的 unique_key 构建器：把符号身份变成驻留
 *        键字符串的那个共享函数，用于去重、upsert 与边端点身份。纯
 *        数据——无 libclang、无数据库、无
 *        I/O。
 */
export module cg.extract.model;

import std;
import cg.util;

export namespace cg::extract
{

    /**
     * @brief 镜像数据库 symbol_kinds 字典的种子 id，但不导入 cg.db
     *        （分层：cg.extract 位于 cg.graph/cg.db 之下）。
     */
    enum class SymKind : std::uint8_t
    {
        Record = 0,
        Enumeration = 1,
        EnumConstant = 2,
        Function = 3,
        Method = 4,
        Constructor = 5,
        Destructor = 6,
        Field = 7,
        Variable = 8,
        TypeAlias = 9,
    };

    [[nodiscard]]
    constexpr auto name_of(SymKind k) noexcept -> std::string_view
    {
        switch (k)
        {
        case SymKind::Record:
            return "record";
        case SymKind::Enumeration:
            return "enum";
        case SymKind::EnumConstant:
            return "enum_constant";
        case SymKind::Function:
            return "function";
        case SymKind::Method:
            return "method";
        case SymKind::Constructor:
            return "constructor";
        case SymKind::Destructor:
            return "destructor";
        case SymKind::Field:
            return "field";
        case SymKind::Variable:
            return "variable";
        case SymKind::TypeAlias:
            return "type_alias";
        }
        return "record";
    }

    /**
     * @brief 符号旗标位。与 cg::db::sym_flags 同值；
     *        在此重复是因为 cg.extract 不得导入 cg.db。cg.graph.build 原样
     *        透传。
     */
    namespace sym_flags
    {
        inline constexpr std::uint32_t is_struct = 0x01;
        inline constexpr std::uint32_t is_union = 0x02;
        inline constexpr std::uint32_t is_template = 0x04;
        inline constexpr std::uint32_t is_partial_spec = 0x08;
        inline constexpr std::uint32_t is_virtual = 0x10;
        inline constexpr std::uint32_t is_pure_virtual = 0x20;
        inline constexpr std::uint32_t is_static = 0x40;
        inline constexpr std::uint32_t is_constexpr = 0x80;
        inline constexpr std::uint32_t is_const = 0x100;
        // 记录声明带 C++ final（与 cg::db::sym_flags 同值；证据 = 记录直接子
        // 游标里的 CXXFinalAttr，libclang 探针实证 final 记录暴露该子游标）。
        inline constexpr std::uint32_t is_final = 0x400;
    } // namespace sym_flags

    /** @brief 边属性位，与 cg::db::edge_attrs 同值。 */
    namespace edge_attrs
    {
        inline constexpr std::uint32_t access_mask =
            0x03; // extends：0 pub、1 prot、2 priv
        inline constexpr std::uint32_t access_protected = 0x01;
        inline constexpr std::uint32_t access_private = 0x02;
        inline constexpr std::uint32_t is_virtual_base = 0x04; // extends
        /** @brief extends：基类是依赖形态（模板形参作实参），目标指主模板本尊。
         */
        inline constexpr std::uint32_t is_template_base = 0x08;
        inline constexpr std::uint32_t dispatch_mask = 0x07; // calls
        inline constexpr std::uint32_t dispatch_member = 0x01;
        inline constexpr std::uint32_t dispatch_virtual = 0x02;
        inline constexpr std::uint32_t template_arg_shift = 0; // specializes
        /** @internal uses：0-2 位携带产出该边的声明点风味。 */
        inline constexpr std::uint32_t uses_site_mask = 0x07;
        inline constexpr std::uint32_t uses_site_field = 0x00;
        inline constexpr std::uint32_t uses_site_param = 0x01;
        inline constexpr std::uint32_t uses_site_return = 0x02;
        inline constexpr std::uint32_t uses_site_local = 0x03;
        inline constexpr std::uint32_t uses_site_variable = 0x04;
    } // namespace edge_attrs

    /** @brief 镜像种子 edge_types id。 */
    enum class EdgeKind : std::uint8_t
    {
        Calls = 0,
        Extends = 1,
        Overrides = 2,
        TypeAlias = 3,
        FriendOf = 4,
        Specializes = 5,
        Uses = 6,
    };

    /**
     * @brief 一个定义级符号。
     *
     * @internal
     * `scope` 是不含符号自身名字的限定路径：命名空间路径加包围记录名、
     * 以 "::" 连接（"shape"、"render::Canvas"、"shape::Box"）。`ns` 是存进
     * symbols.ns_id 的纯命名空间路径（"shape"）；两者分开保存，因为一旦
     * 记录名与命名空间名交错，scope 字符串无法可靠拆回。
     */
    struct SymbolRec
    {
        SymKind     kind = SymKind::Function;
        std::string name;   // 非限定名；模板特化保留特化拼写（"Box<T*>")
        std::string ns;     // 仅命名空间路径（全局作用域为 ''）
        std::string scope;  // 命名空间路径 + 记录链，文件作用域为 ''
        std::string parent; // 包围记录名（无则 ''）；scope
                            // 文本区分不了全局记录与同名记录成员，靠走查上下文
        std::string file;   // 定义处的规范化路径
        int         line = 0; // 1 起
        int         col = 0;
        std::uint32_t            flags = 0;
        std::vector<std::string> param_types;     // 仅函数形类
        std::vector<std::string> template_params; // 仅模板
        bool                     const_method = false;
        std::string              unique_key; // 由 make_unique_key 填充
    };

    /**
     * @brief 供延迟解析的结构化 callee 元数据（翻译
     *        单元 A 的调用目标通常活在翻译单元 B）。图构建器拿它们对着
     *        符号索引跑三层级联解析。
     */
    struct CalleeMeta
    {
        std::string                 name;     // 非限定名（"area"、"~Shape"）
        std::string                 ns;       // 仅命名空间路径（无记录链）
        std::string                 parent;   // 包围记录名，自由函数为 ''
        std::optional<std::int64_t> sig_hash; // 不可计算时为 nullopt
        bool                        const_method = false;
        SymKind kind = SymKind::Function; // function 与 method 的提示
        /**
         * @brief 调用点任一实参是依赖类型（canonical 含 type-parameter）。
         *
         * @internal 重载归属裁决：同名"非模板+模板"并存时（实测大库中
         * 先入库的非模板版曾吃掉全部 4378 条调用归属），clang 在
         * 模板上下文解析到模板版但隐式实例符号不入库，边解析按名字兜底
         * 命中入库序第一个（非模板版）。该标志让解析侧在多个名字命中时
         * 选模板版重载。
         */
        bool arg_dep = false;
    };

    /**
     * @brief 边终点：或是精确 unique_key（目标定义游标在本翻译单元内可达：
     *        extends/specializes/完整类型友元/完整类型 uses），或是解析
     *        元数据（calls/overrides/type_alias/不完整类型 uses）。
     */
    struct EdgeDst
    {
        std::string key;  // 精确形态
        CalleeMeta  meta; // 延迟形态
        bool        is_key = false;
    };

    /**
     * @brief 一个边站点，解析前形态。
     *
     * @internal
     * `src_key` 恒为精确：包围函数/记录正是从本翻译单元抽取的。
     * `occurrences` 折叠同一站点行上同一 callee 的书写重复（一条语句点名
     * callee 两次）；站点键保持 (src,dst,type,file,line)。
     */
    struct EdgeRec
    {
        std::string   src_key;
        EdgeDst       dst;
        EdgeKind      kind = EdgeKind::Calls;
        std::string   site_file; // 拥有该引用站点的规范化路径
        int           site_line = 0;
        std::uint32_t attrs = 0;
        int           occurrences = 1;
    };

    /**
     * @brief 一条 `#include` 关系。目标按书写原文存储（对指令做 token 扫描
     *        获得；INCLUSION_DIRECTIVE 元数据在部分 libclang 构建上不可靠）。
     */
    struct IncludeRec
    {
        std::string file;   // 规范化的包含方路径
        std::string target; // < > 或 " " 之间的原文
        int         line = 0;
        bool        angled = false;
    };

    /**
     * @brief 一条命名模块导入关系（`import cg.util;`、`import std;`、
     *        `import cg.util.json;`）：IncludeRec 的模块对偶。
     *
     * @internal
     * module_name 是 CXCursor_ModuleImportDecl 的光标拼写，原样保留
     * （外部语料的分区导入保持其 `mod:part` 拼写）。
     */
    struct ImportRec
    {
        std::string file;        // 导入方翻译单元 main 文件的规范化路径
        std::string module_name; // 被导入模块名的原文拼写
        int         line = 0;
    };

    /** @brief 单个翻译单元的完整抽取。 */
    struct TuExtract
    {
        std::string              main_file; // 规范化的翻译单元路径
        std::vector<SymbolRec>   symbols;
        std::vector<EdgeRec>     edges;
        std::vector<IncludeRec>  includes;
        std::vector<ImportRec>   imports; // 仅 main 文件的模块导入
        std::vector<std::string> errors;  // severity>=error 的诊断拼写
        std::int64_t             external_dropped =
            0; // 卫生计数器：解析前即弃的边——索引根外的 callee/基类
        std::int64_t operator_dropped =
            0; // 卫生计数器：解析前即弃的边——运算符重载、内部名
        /**
         * @brief 本翻译单元走查遇到的全部非主文件（规范化路径；仅头缓存
         *        开启时填充）。
         *
         * @internal 头缓存的记录面输入：零产出的文件（界外头等）也在此
         * 列名，使缓存能以空分区命中、跳过其整棵子树走查。
         */
        std::vector<std::string> visited_files;
        /**
         * @brief 两个卫生计数器按归属文件的拆账（仅头缓存开启时填充）。
         *
         * @internal external_dropped/operator_dropped 是 TU 级聚合；分区
         * 缓存需要按文件份额回放/记录，两侧计数才能在缓存开关下一致。
         * 键为规范化文件路径，值为 (external, operator)。
         */
        std::unordered_map<std::string, std::pair<std::int64_t, std::int64_t>>
            dropped_by_file;
        /**
         * @brief 与 symbols 平行的「发射区」标注（仅头缓存开启时填充）。
         *
         * @internal 符号的归属文件（SymbolRec.file）与发射它的游标所在
         * 文件可以不同——基类实例注册（base_spec_of 的实例路径）从使用
         * 点所在区的游标发射、符号归属模板特化定义处文件。分区缓存按
         * 发射区归属（跳过某区=跳过其全部发射，无论归属文件是谁），
         * 本数组是记录面的归属依据；缺项时回退 s.file。
         */
        std::vector<std::string> symbol_regions;
    };

    /**
     * @brief 头级缓存的单文件产物分区：一个翻译单元的抽取按归属
     *        文件切分后，非主文件分区的可缓存形态。
     *
     * @internal 分区边界与产物归属一致：符号按定义处文件（SymbolRec.file）、
     * 边按站点文件（EdgeRec.site_file）、包含按指令所在文件（IncludeRec.file）
     * ——词法走查保证「根在某文件 F 的游标子树的产物全部归属 F」（类外定义
     * 在自己的词法位置另行成游标、宏展开游标定位在展开点），因此整分区
     * 跳过+回放与逐游标走查产出同一行集。external_dropped/operator_dropped
     * 是 TuExtract 级计数器，缓存命中回放时把记录期份额加回，保持摘要
     * 计数在缓存开关两侧一致。导入关系只产于 main 文件，永不入分区。
     */
    struct HeadPartition
    {
        std::string                file; // 归属文件（规范化路径）
        std::vector<SymbolRec>     symbols;
        /** @brief 与 symbols 平行的发射区标注（TuExtract.symbol_regions
         *         的分区切片；缓存回放后再记录时保持原发射区）。 */
        std::vector<std::string>   symbol_regions;
        std::vector<EdgeRec>       edges;
        std::vector<IncludeRec>    includes;
        std::int64_t               external_dropped = 0;
        std::int64_t               operator_dropped = 0;
    };

    /**
     * @brief 走查期头缓存查询桥（头缓存的 extract 侧接缝）。
     *
     * cg.extract 不得依赖 cg.graph（分层），桥在此定义为纯接口：返回的
     * 分区指针仅在本游标子树跳过期间使用（实现保证并发查询与记录期间的
     * 指针稳定性——条目一经发布不再原地改写）。miss 返回 nullptr，调用方
     * 照常全量走查该子树。
     */
    struct HeadCacheBridge
    {
        virtual ~HeadCacheBridge() = default;

        /**
         * @brief 按当前翻译单元的变体键查某头文件的缓存分区。
         *
         * @param file 游标子树根所在文件的规范化路径。
         * @return 命中的分区；nullptr = miss（全量走查）。
         */
        [[nodiscard]]
        virtual auto lookup(const std::string& file) const
            -> const HeadPartition* = 0;
    };

    /**
     * @brief 签名哈希：对规范参数列表（拼写以 ',' 连接）做 FNV-1a64 后的
     *        有符号 64 位重解释。
     *
     * 实现已收编至 cg::util::sig_hash_of（该值进 unique_key 与库列，全仓
     * 单一实现逐位不变）；本名是 extract 层的既有导出面，转发保持调用点
     * 不动。
     */
    [[nodiscard]]
    inline auto sig_hash_of(const std::vector<std::string>& param_types)
        -> std::int64_t
    {
        return cg::util::sig_hash_of(param_types);
    }

    /**
     * @brief 唯一的 unique_key 构建器（单一事实来源；节点侧与边侧都经过
     *        它，因此键按构造即字节一致）：
     *
     *     `<kind>`|`<scope>`|`<name>`|`<file>`[|p`<sig_hash>`][|c]
     *
     * @internal
     * - scope 是命名空间路径与包围记录名的连接；纯命名空间路径会让同一
     *   文件中定义的兄弟记录的同名成员撞键（"A::f" 与 "B::f"）。
     * - 函数形类追加 p`<sig_hash>` 后缀（重载身份）；|c 标记 const 方法。
     */
    [[nodiscard]]
    inline auto make_unique_key(const SymbolRec& s) -> std::string
    {
        const bool function_like =
            s.kind == SymKind::Function || s.kind == SymKind::Method
            || s.kind == SymKind::Constructor || s.kind == SymKind::Destructor;
        return function_like ? std::format(
                                   "{}|{}|{}|{}|p{}{}",
                                   name_of(s.kind),
                                   s.scope,
                                   s.name,
                                   s.file,
                                   sig_hash_of(s.param_types),
                                   s.const_method ? "|c" : "")
                             : std::format(
                                   "{}|{}|{}|{}",
                                   name_of(s.kind),
                                   s.scope,
                                   s.name,
                                   s.file);
    }

    /** @brief 不经 SymbolRec、直接用组件拼一条键的便捷函数。 */
    [[nodiscard]]
    inline auto make_record_key(
        std::string_view scope,
        std::string_view name,
        std::string_view file) -> std::string
    {
        return std::format("record|{}|{}|{}", scope, name, file);
    }

    /**
     * @brief 供稀疏 param/template 列使用的 JSON 数组文本。
     *
     * 实现已收编至 cg::util::json::json_string_array（字符串转义由 yyjson
     * 门面保证的仓内单点；类类型模板实参的拼写可含引号字面量，手写零转义
     * 版本击穿时写侧产出非法 JSON、读侧静默解析为空）；本名是 extract 层
     * 的既有导出面，转发保持调用点不动。
     */
    [[nodiscard]]
    inline auto json_string_array(const std::vector<std::string>& items)
        -> std::string
    {
        return cg::util::json::json_string_array(items);
    }

} // namespace cg::extract

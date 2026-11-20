/**
 * @module cg.analysis.model
 *
 * @brief 动态派发分析（cg.analysis）的纯数据类型：类型线索、调用点、
 *        dispatch 边、置信度阶梯、类型 oracle 接缝，以及与 cg.extract
 *        字节一致的 unique_key 构造器（analysis 不得导入 cg.extract，
 *        因此在此镜像公式；等价性由单测保证）。
 *
 * @internal 纯数据——无 libclang、无数据库、无 I/O。依赖方向固定为
 * analysis → db/clang，本单元甚至只依赖 cg.util。
 */
export module cg.analysis.model;

import std;
import cg.util;
import cg.util.json;

export namespace cg::analysis
{

    /**
     * @brief 证据/边置信度阶梯（数值与 db 存储形态一致：0 high、1 medium、
     *        2 low、3 fallback）。
     */
    enum class Confidence : std::uint8_t
    {
        High = 0,
        Medium = 1,
        Low = 2,
        Fallback = 3,
    };

    /** @brief 置信度的边表 REAL 取值：1.0 / 0.7 / 0.4 / 0.0。 */
    [[nodiscard]]
    constexpr auto confidence_value(Confidence c) noexcept -> double
    {
        switch (c)
        {
        case Confidence::High:
            return 1.0;
        case Confidence::Medium:
            return 0.7;
        case Confidence::Low:
            return 0.4;
        case Confidence::Fallback:
            return 0.0;
        }
        return 0.0;
    }

    [[nodiscard]]
    constexpr auto confidence_name(Confidence c) noexcept -> std::string_view
    {
        switch (c)
        {
        case Confidence::High:
            return "high";
        case Confidence::Medium:
            return "medium";
        case Confidence::Low:
            return "low";
        case Confidence::Fallback:
            return "fallback";
        }
        return "fallback";
    }

    /** @brief 取两者中更高的置信度（枚举值越小越高）。 */
    [[nodiscard]]
    constexpr auto better(Confidence a, Confidence b) noexcept -> Confidence
    {
        return static_cast<std::uint8_t>(a) <= static_cast<std::uint8_t>(b) ? a
                                                                            : b;
    }

    /** @brief 取两者中更低的置信度（边 = min(evidence, resolution)）。 */
    [[nodiscard]]
    constexpr auto worse(Confidence a, Confidence b) noexcept -> Confidence
    {
        return static_cast<std::uint8_t>(a) >= static_cast<std::uint8_t>(b) ? a
                                                                            : b;
    }

    /**
     * @brief 类型线索的来源。数值入库；字符串是稳定的 reason 词表
     *        （测试与渲染按文本断言）。
     */
    enum class HintSource : std::uint8_t
    {
        Declared = 0,    // 仅声明类型（不是动态证据）
        DirectNew,       // new T
        DirectAggregate, // T{} / T(...) / T x;
        MakeUnique,      // make_unique<T>
        MakeShared,      // make_shared<T>
        CastDown,        // static_cast/dynamic_cast<T*>
        AddressOfObject, // &obj（obj 是记录对象）
        LocalCopy,       // 同函数别名/拷贝 a = b
        ParamFlow,       // 形参（由跨函数实参传播填充）
        ReturnFlow,      // 返回值/call 结果
        FieldFlow,       // this->field 赋值（owner = 记录，按字段槽传播）
        OutParamFlow, // 出参写回：*pp = e 或出参包装壳 Set(e)（owner = 函数）
        External,     // 外部边界 RTA：接口级可达集（严禁站点级直用）
        ContainerElemFlow, // 容器元素槽：写集/拷贝流/range-for 链接行
    };

    [[nodiscard]]
    constexpr auto source_name(HintSource s) noexcept -> std::string_view
    {
        switch (s)
        {
        case HintSource::Declared:
            return "declared";
        case HintSource::DirectNew:
            return "direct_new";
        case HintSource::DirectAggregate:
            return "direct_aggregate";
        case HintSource::MakeUnique:
            return "make_unique";
        case HintSource::MakeShared:
            return "make_shared";
        case HintSource::CastDown:
            return "cast_down";
        case HintSource::AddressOfObject:
            return "address_of_object";
        case HintSource::LocalCopy:
            return "local_assignment_single";
        case HintSource::ParamFlow:
            return "param_flow_single";
        case HintSource::ReturnFlow:
            return "return_flow";
        case HintSource::FieldFlow:
            return "field_flow";
        case HintSource::OutParamFlow:
            return "out_param_flow";
        case HintSource::External:
            return "external_rta";
        case HintSource::ContainerElemFlow:
            return "container_elem_flow";
        }
        return "no_dynamic_type_evidence";
    }

    [[nodiscard]]
    constexpr auto source_of(int id) noexcept -> HintSource
    {
        return id >= 0 && id <= static_cast<int>(HintSource::ContainerElemFlow)
                   ? static_cast<HintSource>(id)
                   : HintSource::Declared;
    }

    /** @brief 返回槽的保留变量名。 */
    inline constexpr std::string_view return_slot = "#ret";

    /**
     * @brief 取址证据行的保留变量名（&Class::method / &f 落 type_hints）。
     *
     * @internal 数据契约（生产=collect 的 address_of_evidence，消费=
     * cg::db::address_taken_hints / cli 标注）：source 复用
     * HintSource::AddressOfObject(6) 且 related_key = 被取址符号的
     * unique_key 文本（与 &obj 对象行——related_key 恒空——天然区分）；
     * 本保留槽名只作槽位判别：'#' 前缀永不匹配接收者/实参变量名，propagate
     * 装载按槽名跳过（不建传播槽）。
     */
    inline constexpr std::string_view address_taken_slot = "#addr";

    /**
     * @brief 外部边界 RTA 行的保留变量名（root set 线）。
     *
     * @internal 数据契约（root set 线定案）：owner = 外部可达接口的记录
     * key（SWIG 导出 / director / 插件注册表 / dlopen 边界命中），type_set =
     * R(I) = extends 后代闭包 ∩ 全库实例化类型集。语义分级硬约束：站点
     * 级直用严禁——'#' 前缀槽名永不匹配接收者/实参变量名（find_slot 天然
     * 防线），propagate 读入显式跳过本槽（双护栏）；只做候选排序与
     * fallback_reason 标注（external_rta_narrowed），hit==1 只展示唯一候选
     * 绝不发边；hit==0 标注"无库内实例化实例（实现可能索引外）"。
     */
    inline constexpr std::string_view external_slot = "#external";

    /**
     * @brief 外部边界的 reason 词表（定案；测试与渲染按文本断言，
     * 与 source_name 同层的稳定契约）。
     *
     * @internal swig_export/plugin_registry 两词随 external 边界重写
     * 移除（swig .i 扫描与 register* 前缀猜测不再产生边界），一并删除其
     * 渲染分支；旧库残留 reason 行渲染为泛称 "external boundary"。
     */
    inline constexpr std::string_view EXTERNAL_DIRECTOR = "director";
    inline constexpr std::string_view EXTERNAL_DLOPEN = "dlopen_boundary";

    /**
     * @brief 插件加载系 API 名单（已批第三类系统名单：POSIX/Win32 加载器
     * + 符号取址 + COM 工厂入口；callee_name 精确匹配，AST 事实，不做任何
     * 前缀猜测）。
     *
     * @internal collect（界外 callee 放行判定）与 propagate（同名单短路
     * callee 按名解析，防界外 API 名撞库内符号）共用；自 analysis.external
     * 迁至公共模型层（随 dlopen 零激活修复），成员逐字未动。
     */
    inline constexpr std::string_view DLOPEN_API_NAMES[] = {
        "dlopen",
        "dlsym",
        "LoadLibraryA",
        "LoadLibraryW",
        "LoadLibraryExA",
        "LoadLibraryExW",
        "LoadPackagedLibrary",
        "GetProcAddress",
        "CoCreateInstance",
        "DllGetClassObject"};

    /** @brief callee_name 是否命中加载系 API 名单（全词精确匹配）。 */
    [[nodiscard]]
    constexpr auto is_dlopen_api(std::string_view name) -> bool
    {
        for (std::string_view api : DLOPEN_API_NAMES)
        {
            if (name == api)
            {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 平台回调 API 种子名单（callback-param 快速路径，核实 19 条）。
     *
     * @internal "第 N 参会被回调执行"是平台 API 的公开契约（参数是 void*
     * 或不透明指针，类型系统零信息——名单是唯一信息源；与 dlopen 名单同
     * 边界：平台固定名全词精确匹配，项目命名一概不收）。回调参数位 1 起；
     * 名单经双重核实（头文件签名与编译探测互证）。dispatch_async/sync
     * 剔除（Windows 无头）；std:: 系全部不进名单（界外模板实例化体 invoke
     * 采集走机制）。
     * SetWindowsHookExW 为 W 版形态，A 版/宏形态后续按需补（诚实清单）。
     */
    struct CallbackParamApi
    {
        std::string_view name;
        int              arg_index; // 1 起
    };

    inline constexpr CallbackParamApi CALLBACK_PARAM_APIS[] = {
        {"_beginthread", 1},
        {"_beginthreadex", 3},
        {"CreateThread", 3},
        {"CreateRemoteThread", 4},
        {"QueueUserWorkItem", 1},
        {"RegisterWaitForSingleObject", 3},
        {"CreateTimerQueueTimer", 3},
        {"SetWindowsHookExW", 2},
        {"EnumWindows", 1},
        {"EnumChildWindows", 2},
        {"pthread_create", 3},
        {"qsort", 4},
        {"qsort_s", 4},
        {"bsearch", 5},
        {"atexit", 1},
        {"_onexit", 1},
        {"signal", 2},
        {"ReadFileEx", 5},
        {"WriteFileEx", 5},
        {"SetConsoleCtrlHandler", 1},
    };

    /** @brief 命中名单返回回调参数位（1 起），未命中返回 0。 */
    [[nodiscard]]
    constexpr auto callback_param_slot(std::string_view name) -> int
    {
        for (const CallbackParamApi& api : CALLBACK_PARAM_APIS)
        {
            if (api.name == name)
            {
                return api.arg_index;
            }
        }
        return 0;
    }

    /**
     * @brief 字段槽描述符前缀（arg_vars/related_var 携带"字段名+域"）。
     *
     * @internal 形如 "@fld|<owner record unique_key>|<field name>"；owner 是
     * 声明该字段的记录 key（自身含 '|' 分隔符，故解码取最后一个 '|'）。
     * collect 编码、propagate 解码；裸变量名仍原样存储（旧库行兼容）。
     */
    inline constexpr std::string_view field_slot_prefix = "@fld|";

    /**
     * @brief 前缀槽编解码的公共骨架（@fld/@fnv/@gvar 三前缀同构）。
     *
     * 三前缀的形状都是 "<prefix><owner>|<name>"：owner 自身含 '|'（记录
     * key/函数 key/变量 key），name 不含，故解码取最后一个 '|'。边界语义
     * 三前缀统一：前缀不符、无 '|'、owner 段为空、name 段为空一律拒绝
     * （空段拒收曾三处手写互不一致，批 A 先行对齐、本函数定型）。
     */
    ///@{
    /** @brief 编码一个前缀槽描述符（owner + '|' + name，不查成分）。 */
    [[nodiscard]]
    inline auto encode_prefixed_slot(
        std::string_view prefix,
        std::string_view owner,
        std::string_view name) -> std::string
    {
        std::string out{prefix};
        out += owner;
        out += '|';
        out += name;
        return out;
    }

    /** @brief 解析一个前缀槽描述符；非本前缀或任一段为空时 nullopt。 */
    [[nodiscard]]
    inline auto decode_prefixed_slot(
        std::string_view prefix,
        std::string_view text)
        -> std::optional<std::pair<std::string, std::string>>
    {
        if (!text.starts_with(prefix))
        {
            return std::nullopt;
        }
        const std::string_view rest = text.substr(prefix.size());
        const std::size_t      split = rest.rfind('|');
        if (split == std::string_view::npos || split == 0
            || split + 1 >= rest.size())
        {
            return std::nullopt;
        }
        return std::pair<std::string, std::string>{
            std::string{rest.substr(0, split)},
            std::string{rest.substr(split + 1)}};
    }
    ///@}

    /** @brief 编码字段槽描述符（字段名 + 声明记录域）。 */
    [[nodiscard]]
    inline auto encode_field_slot(std::string_view owner, std::string_view name)
        -> std::string
    {
        return encode_prefixed_slot(field_slot_prefix, owner, name);
    }

    /** @brief 解析字段槽描述符；非本编码或字段不全为 nullopt。 */
    [[nodiscard]]
    inline auto decode_field_slot(std::string_view text)
        -> std::optional<std::pair<std::string, std::string>>
    {
        return decode_prefixed_slot(field_slot_prefix, text);
    }

    /**
     * @brief 容器元素槽的变量名前缀："#elem:`<容器名>`"。
     *
     * @internal 声明期元素行（Declared，static_types=[元素记录 key]）是
     * 写集/拷贝流/链接行的闸门——元素槽必须先由声明期创建，最坏 no-op。
     * 写集行与拷贝流行的 var_name 是本前缀槽名；range-for 链接行的
     * var_name 是循环变量名、related_var 是 encode_container_elem_slot 的
     * 输出（源元素槽引用），由 propagate 的函数内阶段解析拷贝。
     */
    inline constexpr std::string_view container_elem_slot_prefix = "#elem:";

    /**
     * @brief 函数域变量槽引用前缀（透传壳闸）："@fnv|<fn
     * unique_key>|`<名>`"。
     *
     * @internal 壳字段写侧穿透（ctor 初始化列表 / operator= 调用）的 RHS
     * 别名源在【写函数域】（构造/赋值运算符的形参或局部变量），而字段槽
     * 的 owner 是记录 key——裸名在 propagate 的函数内记录域解析找不到形参行。
     * related_var 编码携带写函数域（fn_key 自身含 '|'，名字不含，解码取
     * 最后一个 '|'）。与 @fld/@gvar 同族：collect 编码、propagate 解码。
     */
    inline constexpr std::string_view fn_var_slot_prefix = "@fnv|";

    /** @brief 编码函数域变量槽引用（owner = 写函数的 unique_key）。 */
    [[nodiscard]]
    inline auto encode_fn_var_slot(
        std::string_view owner,
        std::string_view name) -> std::string
    {
        return encode_prefixed_slot(fn_var_slot_prefix, owner, name);
    }

    /** @brief 解析函数域变量槽引用；非本编码或任一段为空时 nullopt。 */
    [[nodiscard]]
    inline auto decode_fn_var_slot(std::string_view text)
        -> std::optional<std::pair<std::string, std::string>>
    {
        return decode_prefixed_slot(fn_var_slot_prefix, text);
    }

    /**
     * @brief 全局变量槽引用前缀："@gvar|<variable||name|file>|`<名>`"。
     *
     * @internal 全局 MPtr/函数指针变量（gp = &C::m 形态）的目标行 owner
     * 是 variable||name|file 全局域；字段/局部拷贝流引用它时 related_var
     * 编码携带该域（裸名在 propagate 的函数内同域解析找不到全局行）。解码按
     * 最后一个 '|' 切分（owner 自身含 '|'，名字不含）。
     */
    inline constexpr std::string_view global_slot_prefix = "@gvar|";

    /** @brief 编码全局变量槽引用（owner = global_var_key_of 输出）。 */
    [[nodiscard]]
    inline auto encode_global_slot(
        std::string_view owner,
        std::string_view name) -> std::string
    {
        return encode_prefixed_slot(global_slot_prefix, owner, name);
    }

    /** @brief 解析全局变量槽引用；非本编码或任一段为空时 nullopt。 */
    [[nodiscard]]
    inline auto decode_global_slot(std::string_view text)
        -> std::optional<std::pair<std::string, std::string>>
    {
        return decode_prefixed_slot(global_slot_prefix, text);
    }

    /** @brief 编码容器元素槽引用（ref = 裸容器名或 `fld` 字段描述符）。 */
    [[nodiscard]]
    inline auto encode_container_elem_slot(std::string_view ref) -> std::string
    {
        std::string out{container_elem_slot_prefix};
        out += ref;
        return out;
    }

    /**
     * @brief 解析容器元素槽引用为 (owner_key, 槽名)；非本编码为 nullopt。
     *
     * @internal 裸容器名（owner 为空，由槽自身所在函数域解析）与
     * "@fld|`<owner>`|`<name>`" 字段描述符（owner = 声明记录）两种形态；
     * 返回的 slot_name 已带 "#elem:" 前缀，可直接供 find_slot。
     */
    [[nodiscard]]
    inline auto decode_container_elem_slot(std::string_view text)
        -> std::optional<std::pair<std::string, std::string>>
    {
        if (!text.starts_with(container_elem_slot_prefix))
        {
            return std::nullopt;
        }
        const std::string_view rest =
            text.substr(container_elem_slot_prefix.size());
        if (rest.starts_with(field_slot_prefix))
        {
            const auto field = decode_field_slot(rest);
            if (!field.has_value())
            {
                return std::nullopt;
            }
            return std::pair<std::string, std::string>{
                field->first,
                std::string{container_elem_slot_prefix}
                    + std::string{field->second}};
        }
        if (rest.empty())
        {
            return std::nullopt;
        }
        return std::pair<std::string, std::string>{
            std::string{},
            std::string{container_elem_slot_prefix} + std::string{rest}};
    }

    /**
     * @brief 签名哈希：与 cg.extract::sig_hash_of 逐字节相同（FNV-1a64，
     *        参数拼写以 ',' 连接）。
     *
     * 两层都已收编 cg::util::sig_hash_of（该值进 unique_key 与库列，全仓
     * 单一实现逐位不变）；本名是 analysis 层的既有导出面，转发保持调用点
     * 不动。
     */
    [[nodiscard]]
    inline auto sig_hash_of(const std::vector<std::string>& param_types)
        -> std::int64_t
    {
        return cg::util::sig_hash_of(param_types);
    }

    /** @brief 记录 unique_key：`record|<scope>|<name>|<file>`。 */
    [[nodiscard]]
    inline auto make_record_key(
        std::string_view scope,
        std::string_view name,
        std::string_view file) -> std::string
    {
        return std::format("record|{}|{}|{}", scope, name, file);
    }

    /**
     * @brief 函数形 unique_key：
     *        `<kind>|<scope>|<name>|<file>|p<sig_hash>[|c]`。
     */
    [[nodiscard]]
    inline auto make_function_key(
        std::string_view kind,
        std::string_view scope,
        std::string_view name,
        std::string_view file,
        std::int64_t     sig,
        bool             const_method) -> std::string
    {
        return std::format(
            "{}|{}|{}|{}|p{}{}",
            kind,
            scope,
            name,
            file,
            sig,
            const_method ? "|c" : "");
    }

    /**
     * @brief 一个可解析方法引用的元数据（跨 TU 无文件位置，故只能按
     *        名字/父/命名空间/签名级联解析）。
     */
    struct MethodRef
    {
        std::string                 name;
        std::string                 ns;
        std::string                 parent;
        std::optional<std::int64_t> sig_hash;
        bool                        const_method = false;
    };

    /**
     * @brief 延迟解析用的方法描述符（跨 TU 定义的文件位置未知时使用）。
     *
     * @internal 形如 `@fn|`<name>`|`<ns>`|`<parent>`|`<sig>`|<0|1>`；返回流线索
     * 的 related_key 用它承载“被调用函数是谁”，由 propagate 在整库符号
     * 表上解析。文件位置缺失正是跨 TU 声明/定义的常态。
     */
    [[nodiscard]]
    inline auto encode_method_ref(const MethodRef& m) -> std::string
    {
        std::string out = "@fn|";
        out += m.name;
        out += '|';
        out += m.ns;
        out += '|';
        out += m.parent;
        out += '|';
        if (m.sig_hash.has_value())
        {
            out += std::format("{}", *m.sig_hash);
        }
        out += '|';
        out += m.const_method ? '1' : '0';
        return out;
    }

    [[nodiscard]]
    inline auto decode_method_ref(std::string_view text) -> MethodRef
    {
        MethodRef m;
        if (!text.starts_with("@fn|"))
        {
            return m;
        }
        std::vector<std::string_view> parts;
        std::size_t                   start = 4;
        for (;;)
        {
            const std::size_t bar = text.find('|', start);
            if (bar == std::string_view::npos)
            {
                parts.push_back(text.substr(start));
                break;
            }
            parts.push_back(text.substr(start, bar - start));
            start = bar + 1;
        }
        if (parts.size() != 5)
        {
            return m;
        }
        m.name = std::string{parts[0]};
        m.ns = std::string{parts[1]};
        m.parent = std::string{parts[2]};
        if (!parts[3].empty())
        {
            // 宽容解析（对齐 graph.build 对 meta 值 atoll 的先例语义）：畸形
            // related_key 的 sig_hash 段不抛异常打断整轮分析，解析失败退回
            // 无 sig_hash 形态。from_chars 拒绝 '+' 前缀与 stoll 不同，而
            // encode 侧由 std::format 产出（负号或数字，无 '+'），兼容。
            std::int64_t value = 0;
            const char*  begin = parts[3].data();
            const char*  end = begin + parts[3].size();
            if (std::from_chars(begin, end, value).ec == std::errc{})
            {
                m.sig_hash = value;
            }
        }
        m.const_method = parts[4] == "1";
        return m;
    }

    /**
     * @brief 命名空间尾精确匹配——绝不子串匹配（'Base' 不得匹配
     *        'Database'）。
     *
     * @internal 0.7 置信档的解析级联判据：candidate 等于 wanted，或以
     *           "::"+wanted 收尾（"a::gfx" 尾命中 "gfx"，"MyGraphics"
     *           不命中 "Graphics"）。索引期（graph.build）与装载期
     *           （propagate）两条解析路径共用此单一实现，不得再漂移
     *           出第二份拷贝。
     */
    [[nodiscard]]
    constexpr auto ns_tail_match(
        std::string_view candidate,
        std::string_view wanted) -> bool
    {
        if (wanted.empty())
        {
            return candidate.empty();
        }
        if (candidate == wanted)
        {
            return true;
        }
        return candidate.size() > wanted.size() + 2
               && candidate.ends_with(wanted)
               && candidate.substr(candidate.size() - wanted.size() - 2, 2)
                      == "::";
    }

    /**
     * @brief 一条类型线索（采集层事实或传播结果）。
     *
     * @internal type_keys 是动态类型候选集（排序、去重）；static_types 是
     * 声明类型（解释用）。related_var/related_key/param_index 是传播边：
     * LocalCopy 指向同函数源变量，ReturnFlow 指向被调用函数 key，ParamFlow
     * 携带形参序号。
     */
    struct TypeHintRec
    {
        std::string              owner_key;
        std::string              var_name;
        std::string              file;
        int                      line = 0;
        int                      col = 0;
        std::vector<std::string> type_keys;
        std::vector<std::string> static_types;
        HintSource               source = HintSource::Declared;
        Confidence               confidence = Confidence::Low;
        std::string              fallback_reason;
        std::string              related_var;
        std::string              related_key;
        int                      param_index = -1;
    };

    /**
     * @brief 一个调用点（采集层事实）。仅项目内被调用者；虚调用携带接收者
     *        名字与静态类型，实参携带可即时推出的记录类型与同函数变量名。
     */
    struct CallSiteRec
    {
        std::string caller_key;
        MethodRef   callee;
        bool        is_virtual = false;
        /** @brief 限定调用（`p->Base::f()` / `Base::f()`）：不是动态派发。 */
        bool qualified = false;
        /**
         * @brief 间接调用（函数指针/成员指针/依赖被调用者）：无家族可解析，
         *        如实落 call_sites 并以 indirect_call 降级，绝不静默丢弃。
         */
        bool indirect = false;
        /**
         * @brief 平台回调 API 注册点（collect 界外名单放行的第 2 类站点）。
         *
         * @internal 命中 CALLBACK_PARAM_APIS：站点是"注册契约"不是调用，
         * callee（API 本身）永不解析；契约线索边（callback_reg）的发射输入。
         */
        bool        api_callback = false;
        std::string receiver_name;
        /**
         * @brief 接收者形态位（双态）：name 非空时 = 接收者经
         *        `this->field` 取出（receiver_name 是字段名）；name 为空
         *        时 = 接收者在场未解析标记（接收者表达式显式在源码里，
         *        但采集层没解出槽名——链式 getter 等非虚中间环节的对象
         *        子在本 libclang 下结构性不可达）。
         *
         * @internal 空名态是决议端隐式 this 回退的守卫输入：真隐式 this
         *           （裸 `AddRef()` 调用）无接收者表达式、位保持 false、
         *           回退照常接管；在场未解析位 true 时整档不接管（维持
         *           披露），杜绝"拿调用方所属类当静态根"的同家族错对象
         *           直连。
         */
        bool receiver_is_this = false;
        /**
         * @brief 字段接收者的所属记录 key（`obj.field->f()` 的 obj 记录）。
         *
         * @internal 字段槽以声明该字段的记录为所有者；显式对象访问没有
         * `this` 可回退，必须把记录 key 随调用点一起带到传播阶段。
         */
        std::string                           receiver_owner_key;
        std::vector<std::string>              static_receiver_types;
        std::string                           file;
        int                                   line = 0;
        int                                   col = 0;
        std::vector<std::vector<std::string>> arg_types;
        std::vector<std::string>              arg_vars;
        /**
         * @brief 实参是项目内函数调用时的延迟返回槽描述符（`@fn|...`），
         *        与 arg_types/arg_vars 逐项对齐；空串表示该实参无返回值槽。
         */
        std::vector<std::string> arg_call_keys;
        /**
         * @brief 实参的函数目标 key（取址 &f / 裸函数名 f / lambda），
         *        与 arg_types 逐项对齐；空串=该实参不是函数目标（消费
         * 直配）。
         */
        std::vector<std::string> arg_targets;
        /**
         * @brief 界外模板实例化 callee 的 USR（实例桥传入侧锚点；其余空）。
         *
         * @internal callee 本身永不解析（界外），host_usr 与实例体内采集的
         *           ExternalInvokeRec.host_usr 同源对齐——join 发 indirect 边。
         */
        std::string callee_usr;
        /** @brief 间接调用的被调用表达式名（局部变量名或字段名）。 */
        std::string callee_expr;
        /**
         * @brief 被调用表达式引用的是本函数第 N 形参（-1=不是形参）。
         *
         * @internal 形参直配消费：该站点的目标集 = 此函数所有调用点的第 N
         *           实参 arg_targets 并集。
         */
        int callee_param = -1;
    };

    /**
     * @brief 透传包装登记行（槽位基建，schema v13）：类模板 C`<T>` 存在
     *        "零参成员函数且返回类型引用模板形参"的 getter（判据见
     *        collect 的 is_passthrough_wrapper——canonical 含
     *        type-parameter-，dispatch-feasibility 零误报判据族）。
     *
     * @internal 本表是纯登记（wrapper/getter/实参下标），消费侧（类型集
     *           解包把 arg 形参 record 并入 receiver 槽）由透传闸挂接；
     *           登记本身零消费，既有边行为不变。atomic/optional/
     *           shared_ptr 及任意自定义包装同判据覆盖（零名单）。
     */
    struct PassthroughWrapperRec
    {
        std::string wrapper_key; ///< 主模板 record key
        std::string getter_key;  ///< 透传 getter 的函数形 key
        std::string getter_name; ///< getter 名（审计可读）
        std::string file;
        int         line = 0;
        int         arg_index = 0; ///< getter 返回类型引用的 own 模板形参下标
    };

    /**
     * @brief 一条平台回调注册契约线索（collect 产出，graph.build 写
     *        edges 表 type=8）。
     *
     * @internal 回调位实参是取址函数表达式（&handler）时记录：src=执行
     * 注册的函数，target=被注册的回调函数。宁漏勿猜——lambda/经变量中
     * 转的注册形态不记（槽传播接管）。call_sites 侧另有 api_callback
     * 标记的站点行（站点与线索边并存：边回答"谁被注册"，站点回答"在哪
     * 注册"）。
     */
    struct CallbackRegRec
    {
        std::string caller_key;
        std::string target_key;
        std::string site_file;
        int         site_line = 0;
    };

    /** @brief 单个翻译单元的分析产物（纯数据；不得携带 CXCursor）。 */
    /**
     * @brief 实例桥传入侧记录（schema v15 external_param_args）：
     *        界外模板实例化 callee 的第 param_index 形参收到的函数目标。
     *
     * @internal 站点在界内（调用方 TU），target_key 为弱引用函数 key；
     *           与 ExternalInvokeRec 按 (host_usr, param_index) join。
     */
    struct ExternalParamArgRec
    {
        std::string host_usr;
        int         param_index = 0;
        std::string caller_key;
        std::string target_key;
        std::string file;
        int         line = 0;
        int         col = 0;
    };

    /**
     * @brief 实例桥体内侧记录（schema v15 external_invokes）：界外
     *        实例化函数体内"对形参的调用"点（泛型算法把形参当可调用物）。
     *
     * @internal 轻量采集只记此一类事实——不收符号、不收其它结构；去重键
     *           (host_usr, param_index, file, line, col) 跨 TU 幂等。
     */
    struct ExternalInvokeRec
    {
        std::string host_usr;
        int         param_index = 0;
        std::string file;
        int         line = 0;
        int         col = 0;
    };

    /**
     * @brief lambda 表达式符号登记：LambdaExpr 的 operator() 以
     *        function 形态 key 落库（name 含定位后缀，跨 TU 唯一）。
     *
     * @internal 库内既有 331 个 operator() 符号是 SWIG 显式定义而非 lambda
     *           表达式；间接调用的目标（实参 lambda / 字段存 lambda）需要
     *           库内符号承接边，此登记即承接。
     */
    struct LambdaSymRec
    {
        std::string key;
        std::string name;
        std::string file;
        int         line = 0;
        int         col = 0;
    };

    struct TuAnalysis
    {
        std::string                        main_file;
        std::vector<TypeHintRec>           hints;
        std::vector<CallSiteRec>           call_sites;
        std::vector<PassthroughWrapperRec> passthrough_wrappers;
        std::vector<CallbackRegRec>        callback_regs;
        /** @brief 实例桥传入侧（界外模板实例形参收到的函数目标）。 */
        std::vector<ExternalParamArgRec> external_param_args;
        /** @brief 实例桥体内侧（界外实例体内对形参的调用点）。 */
        std::vector<ExternalInvokeRec> external_invokes;
        /** @brief lambda 表达式符号登记（operator() 形态函数 key）。 */
        std::vector<LambdaSymRec> lambda_symbols;
        /** @brief 界外实例化 invoke 采集触顶（超限停止，如实外露）。 */
        bool ext_invoke_capped = false;
    };

    /**
     * @brief 槽目标集封顶：实测大库三粒度分布 P99=24-27，
     *        cap = 2×P99 取整 = 56，三粒度统一。
     *
     * @internal 语义是"槽目标集"（函数/方法目标，indirect_calls 消费用），
     *           与 MAX_TYPES（类型集封顶 32，另有定案勿动）相互独立。超
     *           限不静默截断成猜测——整体标注 truncated（宁可如实报告
     *           截断，也不给出看似完整的部分答案）。
     */
    inline constexpr std::size_t SLOT_TARGET_CAP = 56;

    /**
     * @brief 按目标集封顶截断（排序去重后保留前 cap 个），返回是否截断。
     *
     * @internal 与 propagate 类型集截断同款"排序后截断"语义：键序截断保证
     *           幂等与全量/增量的逐字节一致。
     */
    [[nodiscard]]
    inline auto slot_target_cap_truncate(std::vector<std::string>& keys) -> bool
    {
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        if (keys.size() <= SLOT_TARGET_CAP)
        {
            return false;
        }
        keys.resize(SLOT_TARGET_CAP);
        return true;
    }

    /**
     * @brief dispatch 候选（一个调用点可能多目标；绝不挑一个）。
     */
    struct DispatchCandidate
    {
        std::string target_key;
        std::string target_qualified;
        Confidence  confidence = Confidence::Low;
        std::string reason;
    };

    /**
     * @brief 一条 virtual_dispatch 边（analysis.dispatch 产出，graph.build
     *        原样写入 edges 表 type=7）。
     */
    struct DispatchEdgeRec
    {
        std::string   caller_key;
        std::string   target_key;
        std::string   site_file;
        int           site_line = 0;
        std::uint32_t attrs = 0;
        double        confidence = 0.0;
        std::string   reason;
    };

    /** @brief 一个调用点的最终解析结果（回填 call_sites）。 */
    struct ResolvedCallSite
    {
        std::int64_t                site_id = 0;
        std::optional<std::int64_t> callee_id;
        std::optional<std::int64_t> method_id;
        std::string                 method_key;
        std::optional<std::int64_t> static_type_id;
        std::vector<std::string>
            receiver_types; // 静态（receiver_static
                            // 列）∪动态的排序并集（落库披露列）
        /** @brief 被调用者本身是否解析到符号（与 dispatch 是否解析无关）。 */
        bool callee_resolved = false;
        /** @brief dispatch 是否解析出至少一个候选（非虚/间接调用恒为 false）。
         */
        bool                           resolved = false;
        Confidence                     confidence = Confidence::Fallback;
        std::vector<DispatchCandidate> candidates;
        std::string                    fallback_reason;
    };

    /**
     * @brief 类型 oracle 接缝：给定一个线索与变量名，返回它认为可达的动态
     *        类型集。当前由传播状态支撑的 stub 实现；未来全程序指针/别名
     *        分析只替换本接口的实现。
     */
    class TypeOracle
    {
    public:
        virtual ~TypeOracle() = default;
        [[nodiscard]]
        virtual auto points_to(const TypeHintRec& hint) const
            -> std::vector<std::string> = 0;
    };

    /**
     * @brief JSON 字符串数组编码（类型集/实参组）；排序由调用方负责。
     *
     * 实现已收编至 cg::util::json::json_string_array（字符串转义由 yyjson
     * 门面保证的仓内单点）；本名是 analysis 层的既有导出面，转发保持调用
     * 点不动。
     */
    [[nodiscard]]
    inline auto json_string_array(std::span<const std::string> items)
        -> std::string
    {
        return cg::util::json::json_string_array(items);
    }

    /**
     * @brief JSON 二维字符串数组编码（call_sites.arg_types）。
     *
     * @internal 嵌套数组经门面的 Array::add(Array&&) 挂接，编码与转义全部
     *           由 cg.util.json 完成。
     */
    [[nodiscard]]
    inline auto json_string_array_2d(
        const std::vector<std::vector<std::string>>& rows) -> std::string
    {
        cg::util::json::Array outer;
        for (const std::vector<std::string>& row : rows)
        {
            cg::util::json::Array inner;
            for (const std::string& item : row)
            {
                inner.add(item);
            }
            outer.add(std::move(inner));
        }
        return cg::util::json::stringify(std::move(outer));
    }

    namespace detail
    {

        /**
         * @brief 经 yyjson 门面把 JSON 文本读成字符串数组的行集。
         *
         * 根须为数组；元素为字符串则成单元素行，元素为数组则取其全部字符串
         * 成员（非字符串成员跳过）。解析失败或根不是数组时返回空行集——
         * 保持装载路径对历史行格式的宽容语义，脏数据不致抛出。
         */
        [[nodiscard]]
        inline auto parse_rows(std::string_view text)
            -> std::vector<std::vector<std::string>>
        {
            std::vector<std::vector<std::string>> rows;
            const auto doc = [&]() -> std::optional<cg::util::json::Document>
            {
                try
                {
                    return cg::util::json::Document::parse(text);
                }
                catch (const std::runtime_error&)
                {
                    return std::nullopt;
                }
            }();
            if (!doc.has_value())
            {
                return rows;
            }
            const cg::util::json::ValueView root = doc->root();
            if (!root.is_array())
            {
                return rows;
            }
            const std::size_t count = root.size();
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto element = root.at(i);
                if (!element.has_value())
                {
                    continue;
                }
                if (element->is_array())
                {
                    std::vector<std::string> row;
                    const std::size_t        inner_count = element->size();
                    for (std::size_t j = 0; j < inner_count; ++j)
                    {
                        const auto item = element->at(j);
                        if (!item.has_value() || !item->is_string())
                        {
                            continue;
                        }
                        if (const auto value = item->as_string();
                            value.has_value())
                        {
                            row.emplace_back(*value);
                        }
                    }
                    rows.push_back(std::move(row));
                }
                else if (element->is_string())
                {
                    if (const auto value = element->as_string();
                        value.has_value())
                    {
                        std::vector<std::string> row;
                        row.emplace_back(*value);
                        rows.push_back(std::move(row));
                    }
                }
            }
            return rows;
        }

    } // namespace detail

    /**
     * @brief 解析一维字符串数组。
     *
     * @internal 行集拍平语义：单行直接返回，多行（二维形态）按序拼接。
     */
    [[nodiscard]]
    inline auto parse_string_array(std::string_view s)
        -> std::vector<std::string>
    {
        const auto rows = detail::parse_rows(s);
        if (rows.size() == 1)
        {
            return rows.front();
        }
        std::vector<std::string> out;
        for (const auto& row : rows)
        {
            for (const std::string& item : row)
            {
                out.push_back(item);
            }
        }
        return out;
    }

    /** @brief 解析二维字符串数组。 */
    [[nodiscard]]
    inline auto parse_string_array_2d(std::string_view s)
        -> std::vector<std::vector<std::string>>
    {
        return detail::parse_rows(s);
    }

    /**
     * @brief 候选集 JSON：`[{target,confidence,reason}]`。
     *
     * @internal 编码经 cg.util.json 完成，字符串转义由库保证。
     */
    [[nodiscard]]
    inline auto candidates_json(
        const std::vector<DispatchCandidate>& candidates) -> std::string
    {
        cg::util::json::Array arr;
        for (const DispatchCandidate& candidate : candidates)
        {
            cg::util::json::Object obj;
            obj.set("target", candidate.target_key);
            obj.set("confidence", confidence_name(candidate.confidence));
            obj.set("reason", candidate.reason);
            arr.add(std::move(obj));
        }
        return cg::util::json::stringify(std::move(arr));
    }

} // namespace cg::analysis

export module cg.test.extract;

// 针对 sample1 迷你工程夹具的提取器测试（经典 include 型
// C++ 解析输入数据）：期望的
// 符号集合含 kinds/flags、调用边形态与卫生过滤、
// overrides/extends/type_alias/specializes 边、带尖括号
// 旗标的 include 记录、仅取定义规则及纯虚例外，以及
// 按 TU 的 unique_key 去重。

import std;
import cg.util;
import cg.util.cstd;
import cg.clang;
import cg.extract;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;
    namespace ex = cg::extract;

    constexpr std::string_view fixture_dir = CCG_SAMPLE1_DIR;
    constexpr std::string_view uses_fixture_dir = CCG_USES_DIR;
    constexpr std::string_view constexpr_spec_dir = CCG_CONSTEXPR_SPEC_DIR;

    [[nodiscard]]
    auto fixture(std::string_view name) -> std::string
    {
        return std::string{fixture_dir} + "/" + std::string{name};
    }

    // 夹具的全部 TU 已解析并提取，记录已拼接。
    struct Merged
    {
        std::vector<ex::SymbolRec>  symbols;
        std::vector<ex::EdgeRec>    edges;
        std::vector<ex::IncludeRec> includes;
        std::int64_t                external_dropped = 0;
        std::int64_t                operator_dropped = 0;
    };

    // 解析 `dir` 夹具的 compile_commands.json 并逐 TU 提取拼接。
    [[nodiscard]]
    auto merged_extraction(std::string_view dir) -> Merged
    {
        namespace cc = cg::clang;
        Merged            out;
        const std::string root = cg::util::norm_path(dir);
        const auto        commands =
            cc::load(std::string{dir} + "/compile_commands.json", {});
        const cc::Index          index;
        cg::util::PathCaseCache  case_cache; // 每次测试遍历一个缓存
        const ex::ExtractOptions opts{.roots = {root}, .spellings = {}};
        for (const cc::CompileCommand& cmd : commands)
        {
            const cc::ParsedTu parsed = cc::parse(index, cmd.file, cmd.argv);
            if (!parsed.errors.empty())
            {
                continue;
            }
            ex::TuExtract one = ex::extract(parsed, opts, case_cache);
            out.external_dropped += one.external_dropped;
            out.operator_dropped += one.operator_dropped;
            for (auto& s : one.symbols)
            {
                out.symbols.push_back(std::move(s));
            }
            for (auto& e : one.edges)
            {
                out.edges.push_back(std::move(e));
            }
            for (auto& i : one.includes)
            {
                out.includes.push_back(std::move(i));
            }
        }
        return out;
    }

    [[nodiscard]]
    auto find_symbol(
        const Merged&    m,
        std::string_view name,
        std::string_view scope) -> const ex::SymbolRec*
    {
        for (const ex::SymbolRec& s : m.symbols)
        {
            if (s.name == name && s.scope == scope)
            {
                return &s;
            }
        }
        return nullptr;
    }

    /// 按（名，父类型）查找符号记录。
    [[nodiscard]]
    auto find_symbol_of_parent(
        const Merged&    m,
        std::string_view name,
        std::string_view parent) -> const ex::SymbolRec*
    {
        for (const ex::SymbolRec& s : m.symbols)
        {
            if (s.name == name && s.parent == parent)
            {
                return &s;
            }
        }
        return nullptr;
    }

    /// include 记录中是否存在目标与尖括号形态都匹配的条目。
    [[nodiscard]]
    auto has_include(const Merged& m, std::string_view target, bool angled)
        -> bool
    {
        for (const ex::IncludeRec& i : m.includes)
        {
            if (i.target == target && i.angled == angled)
            {
                return true;
            }
        }
        return false;
    }

    // 按符号名读取边端点的查找表。
    [[nodiscard]]
    auto key_names(const Merged& m)
        -> std::unordered_map<std::string, std::string>
    {
        std::unordered_map<std::string, std::string> out;
        for (const ex::SymbolRec& s : m.symbols)
        {
            out[s.unique_key] = s.name;
        }
        return out;
    }

    [[nodiscard]]
    auto call_edge(
        const Merged&    m,
        std::string_view src_name,
        std::string_view dst_name) -> const ex::EdgeRec*
    {
        const auto names = key_names(m);
        for (const ex::EdgeRec& e : m.edges)
        {
            if (e.kind != ex::EdgeKind::Calls)
            {
                continue;
            }
            const auto src = names.find(e.src_key);
            if (src == names.end() || src->second != src_name)
            {
                continue;
            }
            if (e.dst.is_key)
            {
                continue;
            }
            if (e.dst.meta.name == dst_name)
            {
                return &e;
            }
        }
        return nullptr;
    }

    [[nodiscard]]
    auto file_contains(std::string_view path, std::string_view suffix) -> bool
    {
        return path.find(suffix) != std::string_view::npos;
    }

    // ---------------------------------------------------------------------------

    void symbol_set(R& r)
    {
        const Merged      m = merged_extraction(fixture_dir);
        const std::string root = cg::util::norm_path(fixture_dir);

        // 记录。
        const ex::SymbolRec* shape = find_symbol(m, "Shape", "shape");
        cg::test::check(r, shape != nullptr, "record Shape found");
        if (shape)
        {
            cg::test::check(
                r,
                shape->kind == ex::SymKind::Record,
                "Shape kind is record");
            cg::test::check(
                r,
                shape->ns == "shape" && shape->parent.empty(),
                "Shape ns/parent");
            cg::test::check(
                r,
                file_contains(shape->file, "shape.hpp"),
                "Shape defined in shape.hpp");
        }
        const ex::SymbolRec* circle = find_symbol(m, "Circle", "shape");
        cg::test::check(r, circle != nullptr, "record Circle found");

        // 带旗标的方法。
        const ex::SymbolRec* area = find_symbol(m, "area", "shape::Shape");
        cg::test::check(r, area != nullptr, "pure virtual Shape::area found");
        if (area)
        {
            cg::test::check(
                r,
                area->kind == ex::SymKind::Method
                    && (area->flags & ex::sym_flags::is_pure_virtual),
                "Shape::area is a flagged pure-virtual method");
            cg::test::check(r, area->const_method, "Shape::area is const");
            cg::test::check(
                r,
                area->param_types.empty(),
                "Shape::area takes no parameters");
        }
        const ex::SymbolRec* circle_area =
            find_symbol(m, "area", "shape::Circle");
        cg::test::check(r, circle_area != nullptr, "Circle::area found");
        if (circle_area)
        {
            cg::test::check(
                r,
                (circle_area->flags & ex::sym_flags::is_virtual) != 0,
                "Circle::area is virtual (override)");
            cg::test::check(
                r,
                circle_area->parent == "Circle",
                "Circle::area parent is Circle");
            cg::test::check(
                r,
                file_contains(circle_area->file, "circle.cpp"),
                "Circle::area definition lives in circle.cpp");
        }
        cg::test::check(
            r,
            find_symbol(m, "describe", "shape::Circle") != nullptr,
            "Circle::describe found");
        cg::test::check(
            r,
            find_symbol(m, "~Circle", "shape::Circle") != nullptr,
            "Circle::~Circle found");
        cg::test::check(
            r,
            find_symbol(m, "~Shape", "shape::Shape") != nullptr,
            "Shape::~Shape (defaulted inline) found");

        // 重载集合：同名，形参列表与 key 各不相同。
        const ex::SymbolRec* scale_i = find_symbol(m, "scale", "shape::Circle");
        const ex::SymbolRec* scale_d = [&]
        {
            for (const ex::SymbolRec& s : m.symbols)
            {
                if (s.name == "scale" && s.scope == "shape::Circle"
                    && !s.param_types.empty() && s.param_types[0] == "double")
                {
                    return &s;
                }
            }
            return static_cast<const ex::SymbolRec*>(nullptr);
        }();
        cg::test::check(
            r,
            scale_i != nullptr && !scale_i->param_types.empty()
                && scale_i->param_types[0] == "int",
            "scale(int) found");
        cg::test::check(r, scale_d != nullptr, "scale(double) found");
        if (scale_i && scale_d)
        {
            cg::test::check(
                r,
                scale_i->unique_key != scale_d->unique_key
                    && ex::sig_hash_of(scale_i->param_types)
                           != ex::sig_hash_of(scale_d->param_types),
                "overload pair has distinct keys and sig hashes");
        }

        // 字段、变量、别名、枚举。
        cg::test::check(
            r,
            find_symbol(m, "radius_", "shape::Circle") != nullptr,
            "field radius_ found");
        const ex::SymbolRec* cap = find_symbol(m, "kMaxCircles", "render");
        cg::test::check(
            r,
            cap != nullptr && cap->kind == ex::SymKind::Variable
                && (cap->flags & ex::sym_flags::is_constexpr),
            "constexpr variable kMaxCircles found");
        const ex::SymbolRec* alias = find_symbol(m, "ShapeHandle", "shape");
        cg::test::check(
            r,
            alias != nullptr && alias->kind == ex::SymKind::TypeAlias,
            "typedef ShapeHandle found");
        const ex::SymbolRec* color = find_symbol(m, "Color", "shape");
        cg::test::check(
            r,
            color != nullptr && color->kind == ex::SymKind::Enumeration,
            "enum Color found");
        for (const std::string_view name : {"red", "green", "blue"})
        {
            cg::test::check(
                r,
                find_symbol(m, name, "shape::Color") != nullptr,
                "enum constant found");
        }

        // 模板。
        const ex::SymbolRec* box = find_symbol(m, "Box", "shape");
        cg::test::check(r, box != nullptr, "primary template Box found");
        if (box)
        {
            cg::test::check(
                r,
                (box->flags & ex::sym_flags::is_template) != 0,
                "Box flagged is_template");
            cg::test::check(
                r,
                box->template_params.size() == 1
                    && box->template_params[0] == "T",
                "Box template_params = [T]");
        }
        const ex::SymbolRec* box_ptr = find_symbol(m, "Box<T*>", "shape");
        cg::test::check(r, box_ptr != nullptr, "partial specialization found");
        if (box_ptr)
        {
            cg::test::check(
                r,
                (box_ptr->flags & ex::sym_flags::is_partial_spec) != 0,
                "Box<T*> flagged is_partial_specialization");
        }
        const ex::SymbolRec* unwrap = find_symbol(m, "unwrap", "shape");
        cg::test::check(
            r,
            unwrap != nullptr && unwrap->kind == ex::SymKind::Function
                && (unwrap->flags & ex::sym_flags::is_template) != 0,
            "function template unwrap found");

        // 自由函数与 main。
        cg::test::check(
            r,
            find_symbol(m, "pi", "shape") != nullptr,
            "pi found");
        cg::test::check(
            r,
            find_symbol(m, "area_all", "app") != nullptr,
            "app::area_all found");
        cg::test::check(
            r,
            find_symbol(m, "render_all", "app") != nullptr,
            "app::render_all found");
        cg::test::check(
            r,
            find_symbol(m, "unused_helper", "shape") != nullptr,
            "unused_helper found");
        cg::test::check(
            r,
            find_symbol(m, "main", "") != nullptr,
            "main function found at global scope");
        cg::test::check(
            r,
            find_symbol(m, "total_area", "render::Canvas") != nullptr,
            "Canvas::total_area found");

        // 任何地方都没有局部变量或形参（"value" 是模板字段而非局部
        // 变量，所以不进这个清单）。
        for (const std::string_view local :
             {"total", "canvas", "unit", "first", "second", "scaled", "c"})
        {
            cg::test::check(
                r,
                find_symbol(m, local, "") == nullptr
                    && std::ranges::find_if(
                           m.symbols,
                           [&](const ex::SymbolRec& s)
                           { return s.name == local; })
                           == m.symbols.end(),
                "no local/parameter symbol");
        }

        // 一切都在夹具根目录内（系统头文件不产出）。
        for (const ex::SymbolRec& s : m.symbols)
        {
            if (!s.file.starts_with(root))
            {
                cg::test::check(r, false, "symbol outside fixture root");
                break;
            }
        }
    }

    void edge_set(R& r)
    {
        const Merged m = merged_extraction(fixture_dir);

        // extends Circle -> Shape（key 形式的目标端）。
        bool extends_ok = false;
        for (const ex::EdgeRec& e : m.edges)
        {
            if (e.kind != ex::EdgeKind::Extends || !e.dst.is_key)
            {
                continue;
            }
            if (file_contains(e.dst.key, "|Shape|"))
            {
                extends_ok = true;
            }
        }
        cg::test::check(r, extends_ok, "extends edge Circle -> Shape recorded");

        // overrides：area/describe/~Circle 对 Shape 的方法（meta 形式）。
        for (const std::string_view name : {"area", "describe", "~Shape"})
        {
            bool found = false;
            for (const ex::EdgeRec& e : m.edges)
            {
                if (e.kind != ex::EdgeKind::Overrides || e.dst.is_key)
                {
                    continue;
                }
                if (e.dst.meta.name == name && e.dst.meta.parent == "Shape")
                {
                    found = true;
                }
            }
            cg::test::check(
                r,
                found,
                std::format("overrides edge toward Shape::{}", name));
        }

        // specializes：Box<T*> -> Box（key 形式）。
        bool specializes_ok = false;
        for (const ex::EdgeRec& e : m.edges)
        {
            if (e.kind == ex::EdgeKind::Specializes && e.dst.is_key
                && file_contains(e.dst.key, "|Box|")
                && !file_contains(e.dst.key, "|Box<"))
            {
                specializes_ok = true;
            }
        }
        cg::test::check(
            r,
            specializes_ok,
            "specializes edge Box<T*> -> Box recorded");

        // type_alias：ShapeHandle -> Shape（裸名 meta）。
        bool alias_ok = false;
        for (const ex::EdgeRec& e : m.edges)
        {
            if (e.kind == ex::EdgeKind::TypeAlias && !e.dst.is_key
                && e.dst.meta.name == "Shape")
            {
                alias_ok = true;
            }
        }
        cg::test::check(r, alias_ok, "type_alias edge ShapeHandle -> Shape");

        // 调用。
        cg::test::check(
            r,
            call_edge(m, "area", "pi") != nullptr,
            "Circle::area calls pi");
        const ex::EdgeRec* cross = call_edge(m, "total_area", "area");
        cg::test::check(
            r,
            cross != nullptr,
            "Canvas::total_area calls Circle::area (cross-file)");
        if (cross)
        {
            cg::test::check(
                r,
                cross->dst.meta.parent == "Circle"
                    && cross->dst.meta.const_method,
                "cross-file callee metadata carries parent+const");
        }
        cg::test::check(
            r,
            call_edge(m, "area_all", "total_area") != nullptr,
            "app::area_all calls Canvas::total_area");
        cg::test::check(
            r,
            call_edge(m, "render_all", "area_all") != nullptr,
            "app::render_all calls app::area_all");
        cg::test::check(
            r,
            call_edge(m, "main", "render_all") != nullptr,
            "main calls app::render_all");
        cg::test::check(
            r,
            call_edge(m, "main", "add") != nullptr,
            "main calls Canvas::add");
        cg::test::check(
            r,
            call_edge(m, "main", "scale") != nullptr,
            "main calls Circle::scale");
        cg::test::check(
            r,
            call_edge(m, "main", "get") != nullptr,
            "main calls Box::get (instantiated -> primary)");
        cg::test::check(
            r,
            call_edge(m, "get", "unwrap") != nullptr,
            "Box::get calls unwrap inside the template body");

        // 卫生检查：无外部符号、无运算符、无标准库方法。
        for (const ex::EdgeRec& e : m.edges)
        {
            if (!e.dst.is_key && !e.dst.meta.name.empty())
            {
                cg::test::check(
                    r,
                    e.dst.meta.name != "push_back" && e.dst.meta.name != "begin"
                        && e.dst.meta.name != "end"
                        && !e.dst.meta.name.starts_with("operator"),
                    "no stdlib/operator call edges");
            }
        }
        cg::test::check(
            r,
            m.external_dropped > 0,
            "external callees (push_back etc.) counted as dropped");

        // 调用点携带真实位置。
        const ex::EdgeRec* pi_call = call_edge(m, "area", "pi");
        if (pi_call)
        {
            cg::test::check(
                r,
                pi_call->site_line >= 1
                    && file_contains(pi_call->site_file, "circle.cpp"),
                "call site location points at circle.cpp");
        }
    }

    void include_set(R& r)
    {
        const Merged m = merged_extraction(fixture_dir);

        cg::test::check(
            r,
            has_include(m, "circle.hpp", false),
            "quoted include circle.hpp recorded");
        cg::test::check(
            r,
            has_include(m, "shape.hpp", false),
            "quoted include shape.hpp recorded");
        cg::test::check(
            r,
            has_include(m, "canvas.hpp", false),
            "quoted include canvas.hpp recorded");
        cg::test::check(
            r,
            has_include(m, "tmpl.hpp", false),
            "quoted include tmpl.hpp recorded");
        cg::test::check(
            r,
            has_include(m, "vector", true),
            "angled include vector recorded");
    }

    void dedup_and_determinism(R& r)
    {
        namespace cc = cg::clang;
        const std::string root = cg::util::norm_path(fixture_dir);
        const auto commands = cc::load(fixture("compile_commands.json"), {});
        const cc::Index          index;
        const ex::ExtractOptions opts{.roots = {root}, .spellings = {}};

        cg::util::PathCaseCache   case_cache; // 每次测试遍历一个缓存
        const cc::CompileCommand* main_cmd = cc::find_command(
            commands,
            cg::util::norm_path(fixture("main.cpp")));
        cg::test::check(r, main_cmd != nullptr, "main.cpp compile entry found");
        if (main_cmd == nullptr)
        {
            return;
        }

        const ex::TuExtract first = ex::extract(
            cc::parse(index, main_cmd->file, main_cmd->argv),
            opts,
            case_cache);
        const ex::TuExtract second = ex::extract(
            cc::parse(index, main_cmd->file, main_cmd->argv),
            opts,
            case_cache);

        std::vector<std::string> a, b;
        for (const ex::SymbolRec& s : first.symbols)
        {
            a.push_back(s.unique_key);
        }
        for (const ex::SymbolRec& s : second.symbols)
        {
            b.push_back(s.unique_key);
        }
        std::ranges::sort(a);
        std::ranges::sort(b);
        cg::test::check(r, a == b, "extraction is deterministic");

        // 按 TU 的 unique_key 去重：一次提取中没有 key 出现两次。
        const std::size_t unique =
            std::set<std::string>{a.begin(), a.end()}.size();
        cg::test::check(r, unique == a.size(), "per-TU symbol keys are unique");
        cg::test::check(r, !a.empty(), "main TU extraction is non-empty");

        // 仅取定义：头文件的声明没有在本 TU 内产出重复的
        // area/scale 节点（circle.cpp 里的定义不在此处，
        // 所以只出现 shape.hpp/tmpl.hpp/canvas.hpp 的定义）。
        bool saw_circle_area = false;
        for (const ex::SymbolRec& s : first.symbols)
        {
            if (s.name == "area" && s.parent == "Circle")
            {
                saw_circle_area = true;
            }
        }
        cg::test::check(
            r,
            !saw_circle_area,
            "main TU does not node Circle::area (declaration-only)");
    }

    const cg::test::Registrar reg_symbols{"extract.symbols", &symbol_set};
    const cg::test::Registrar reg_edges{"extract.edges", &edge_set};
    const cg::test::Registrar reg_includes{"extract.includes", &include_set};
    const cg::test::Registrar reg_dedup{
        "extract.dedup",
        &dedup_and_determinism};

    // sample3：src/ + inc/ 布局，其带引号 include 写成 ".." 上爬
    // 形式（"../inc/counter.hpp"）。针对静默丢头的回归：过去每个
    // 光标文件路径都以 "src/../inc/x.hpp" 到达，collapse 必须
    // 修正它；也针对同行重复调用（"return get() + get();"）。
    void dotdot_layout(R& r)
    {
        const Merged m = merged_extraction(CCG_SAMPLE3_DIR);
        // 该用例只消费 Calls 边（同行折叠断言），完整提取的
        // 其余边集无断言消费，后过滤丢弃。
        std::vector<ex::EdgeRec> call_edges;
        for (const ex::EdgeRec& e : m.edges)
        {
            if (e.kind == ex::EdgeKind::Calls)
            {
                call_edges.push_back(e);
            }
        }

        // 头文件定义的符号幸存（静默丢头回归）。
        const ex::SymbolRec* counter = find_symbol_of_parent(m, "Counter", "");
        cg::test::check(
            r,
            counter != nullptr,
            "header record Counter extracted (not dropped)");
        if (counter)
        {
            cg::test::check(
                r,
                counter->file.ends_with("/inc/counter.hpp")
                    && counter->file.find("..") == std::string::npos,
                "Counter file is the collapsed inc/ path");
        }
        const ex::SymbolRec* doubled =
            find_symbol_of_parent(m, "doubled", "Counter");
        cg::test::check(
            r,
            doubled != nullptr && doubled->const_method,
            "inline header method Counter::doubled extracted");

        // 类外定义经由 ".." 头文件保持其 record 作用域。
        const ex::SymbolRec* get = find_symbol_of_parent(m, "get", "Counter");
        cg::test::check(
            r,
            get != nullptr,
            "out-of-line Counter::get extracted");
        if (get)
        {
            cg::test::check(
                r,
                get->scope == "tally::Counter"
                    && get->file.ends_with("/src/counter.cpp"),
                "Counter::get scope and file are correct");
        }

        // 同行双重调用折叠成一条边，occurrences = 2。
        const ex::EdgeRec* doubled_get = nullptr;
        for (const ex::EdgeRec& e : call_edges)
        {
            if (e.src_key != doubled->unique_key || e.dst.is_key)
            {
                continue;
            }
            if (e.dst.meta.name == "get" && e.dst.meta.parent == "Counter")
            {
                doubled_get = &e;
            }
        }
        cg::test::check(
            r,
            doubled_get != nullptr,
            "edge Counter::doubled -> Counter::get recorded");
        if (doubled_get)
        {
            cg::test::check(
                r,
                doubled_get->occurrences == 2,
                "same-line get()+get() folds to occurrences = 2");
        }

        // 全局不变量：任何存储的路径都不保留 ".." 段。
        for (const ex::SymbolRec& s : m.symbols)
        {
            if (s.file.find("..") != std::string::npos)
            {
                cg::test::check(r, false, "symbol file keeps a .. segment");
                break;
            }
        }
        for (const ex::IncludeRec& i : m.includes)
        {
            if (i.file.find("..") != std::string::npos)
            {
                cg::test::check(r, false, "include file keeps a .. segment");
                break;
            }
        }
        // 原始指令文本按书写原样存储。
        bool saw_dotdot_include = false;
        for (const ex::IncludeRec& i : m.includes)
        {
            if (i.target == "../inc/counter.hpp" && !i.angled)
            {
                saw_dotdot_include = true;
            }
        }
        cg::test::check(
            r,
            saw_dotdot_include,
            "include target keeps the as-written .. spelling");
    }

    const cg::test::Registrar reg_dotdot{"extract.dotdot", &dotdot_layout};

    // sample3 的 FlagProbe：is_constexpr 只能来自声明的
    // 前导说明符。类内无说明符的构造函数（extent 从声明名
    // 开始）体内带 constexpr 局部变量时不能被标记——
    // 过去的整个 extent 令牌扫描会看到那个局部并
    // 错误标记符号。真正的 constexpr 方法必须被标记。
    void constexpr_flags(R& r)
    {
        const Merged m = merged_extraction(CCG_SAMPLE3_DIR);

        const ex::SymbolRec* probe =
            find_symbol_of_parent(m, "FlagProbe", "FlagProbe");
        cg::test::check(
            r,
            probe != nullptr,
            "FlagProbe no-specifier constructor extracted");
        if (probe)
        {
            cg::test::check(
                r,
                probe->kind == ex::SymKind::Constructor
                    && (probe->flags & ex::sym_flags::is_constexpr) == 0,
                "no-specifier ctor with a constexpr local body is "
                "NOT flagged is_constexpr");
        }
        const ex::SymbolRec* limit =
            find_symbol_of_parent(m, "limit", "FlagProbe");
        cg::test::check(r, limit != nullptr, "FlagProbe::limit extracted");
        if (limit)
        {
            cg::test::check(
                r,
                (limit->flags & ex::sym_flags::is_constexpr) != 0,
                "constexpr method limit IS flagged is_constexpr");
        }
    }

    const cg::test::Registrar reg_cxflags{
        "extract.constexpr_flags",
        &constexpr_flags};

    // 夹具 constexpr-spec：constexpr 检测判据（AST 重打印文本的声明头，
    // 先剥字符串字面量、跳过说明符之前的花括号组、再截到函数体起点）在
    // 宏形态谱与说明符前花括号谱上的行为锚点。方向性锚点：说明符由宏
    // 生成时源前缀只见宏别名（旧判据漏报 0），重打印文本已展开为说明符
    // 本词（判 1）；名字由宏生成且函数体内带 constexpr 局部量时，旧回退
    // 把整声明 extent 词法化吃进体内局部（误报 1），重打印截到函数体起
    // 点即拒绝（判 0）；说明符之前的花括号组（requires 复合要求的体、
    // 类类型 NTTP 花括号默认值）曾触发截断使说明符不可达（漏报 0），
    // 语境感知配对跳过后可达（判 1）；defaulted 补面前缀里的属性实参字
    // 符串恰含说明符本词（曾误报 1），补面剥字符串后拒绝（判 0）。
    void constexpr_pretty_printed_head(R& r)
    {
        const Merged m = merged_extraction(constexpr_spec_dir);
        struct Want
        {
            std::string_view name;
            std::string_view parent;
            bool             is_constexpr;
        };
        const Want wants[] = {
            // 宏形态（判据替换的两个行为翻转点）。
            {"macro_specifier_fn", "", true},
            {"macro_named_fn", "", true},
            {"body_leak_fn", "", false},
            // 基础说明符谱。
            {"constexpr_free_fn", "", true},
            {"consteval_free_fn", "", true},
            {"plain_free_fn", "", false},
            {"comment_split_fn", "", true},
            {"attr_then_constexpr_fn", "", true},
            {"tmpl_constexpr_fn", "", true},
            {"hash_attr_constexpr_fn", "", true},
            // 变量谱：字符串字面量内容不是说明符。
            {"constexpr_var", "", true},
            {"plain_var", "", false},
            {"string_content_var", "", false},
            // 类内谱：无说明符构造的体内局部量不外溢；方法命中。
            {"CtorBodyProbe", "CtorBodyProbe", false},
            {"const_method", "CtorBodyProbe", true},
            // defaulted 谱：打印器对 "= default" 丢说明符，defaulted 形态
            // 回源前缀直读（带说明符命中、不带拒绝）；"= delete" 的打印
            // 保留说明符（主路径命中）。
            {"DefaultedProbe", "DefaultedProbe", true},
            {"DefaultedPlainCopy", "DefaultedPlainCopy", false},
            // 说明符之前的花括号谱（语境感知配对跳过）：requires 复合要
            // 求的体与类类型 NTTP 花括号默认值不是函数体；requires 体之
            // 后的函数体必须仍然截断（体内局部量不得外溢）。
            {"requires_compound_brace_fn", "", true},
            {"requires_paren_fn", "", true},
            {"requires_nested_brace_fn", "", true},
            {"requires_body_local_fn", "", false},
            {"nttp_brace_default_fn", "", true},
            {"nttp_plain_default_fn", "", true},
            // defaulted 补面剥字符串：属性实参里的说明符本词不是说明符。
            {"AttrDefaulted", "AttrDefaulted", false},
            {"AttrStrEvalDefaulted", "AttrStrEvalDefaulted", false},
            {"AttrStrRealCx", "AttrStrRealCx", true},
            // 函数体超语句级跨度：判定只看声明头。
            {"huge_body_fn", "", true},
        };
        for (const Want& w : wants)
        {
            const ex::SymbolRec* s = find_symbol_of_parent(m, w.name, w.parent);
            cg::test::check(
                r,
                s != nullptr,
                std::string{w.name} + " extracted");
            if (s)
            {
                cg::test::check(
                    r,
                    ((s->flags & ex::sym_flags::is_constexpr) != 0)
                        == w.is_constexpr,
                    std::string{w.name} + " is_constexpr expected "
                        + (w.is_constexpr ? "1" : "0"));
            }
        }
        // 面外对照：函数体内局部量不作为符号保留，纯声明与 = delete
        // 函数不是定义——都不进符号表，判据无从误标。
        cg::test::check(
            r,
            find_symbol_of_parent(m, "local_in_fn", "") == nullptr,
            "function-body local is not a symbol");
        cg::test::check(
            r,
            find_symbol_of_parent(m, "declared_only_fn", "") == nullptr,
            "non-definition declaration is not a symbol");
        cg::test::check(
            r,
            find_symbol_of_parent(m, "deleted_free_fn", "") == nullptr,
            "deleted function is not a symbol");
    }

    const cg::test::Registrar reg_cxhead{
        "extract.constexpr_pretty_printed_head",
        &constexpr_pretty_printed_head};

    // ---------------------------------------------------------------------------
    // USES 提取矩阵（field/param/return/local 交叉 class/template/std）
    // 针对专用的 uses 夹具。

    /// 一条 USES 边的断言投影视图（src 侧带持有符号的 NAME）。
    struct UseEdge
    {
        std::string   src_name;
        std::string   dst; // 精确 key 或延迟名
        bool          is_key = false;
        std::uint32_t attrs = 0;
    };

    /// 是否存在 src/dst/站点属性都匹配的 USES 边（精确 key 按包含比对）。
    [[nodiscard]]
    auto has_use_edge(
        const std::vector<UseEdge>& uses,
        std::string_view            src,
        std::string_view            dst,
        std::uint32_t               site_bits) -> bool
    {
        for (const UseEdge& u : uses)
        {
            if (u.src_name == src
                && (u.is_key ? u.dst.find(dst) != std::string::npos
                             : u.dst == dst)
                && (u.attrs & ex::edge_attrs::uses_site_mask) == site_bits)
            {
                return true;
            }
        }
        return false;
    }

    /// 指向 `<name>` 的 USES 边计数（精确 key 解析名字段，延迟形态按裸名）。
    [[nodiscard]]
    auto count_uses_to(
        const std::vector<UseEdge>& uses,
        std::string_view            dst_name) -> std::int64_t
    {
        // 精确 key 形如 "record|uses|`<name>`|file"；延迟形态是
        // 裸记录名。
        std::int64_t n = 0;
        for (const UseEdge& u : uses)
        {
            if (!u.is_key)
            {
                n += u.dst == dst_name ? 1 : 0;
                continue;
            }
            const std::size_t scope_end = u.dst.find('|', 7);
            const std::size_t name_end = scope_end == std::string::npos
                                             ? std::string::npos
                                             : u.dst.find('|', scope_end + 1);
            if (name_end != std::string::npos
                && u.dst.substr(scope_end + 1, name_end - scope_end - 1)
                       == dst_name)
            {
                ++n;
            }
        }
        return n;
    }

    void uses_matrix(R& r)
    {
        const Merged m = merged_extraction(uses_fixture_dir);

        // 矩阵记录的 unique key，取自被推入的符号本身
        // （唯一事实来源：make_unique_key）。
        const ex::SymbolRec* payload = find_symbol(m, "Payload", "uses");
        const ex::SymbolRec* holder = find_symbol(m, "Holder", "uses");
        const ex::SymbolRec* carrier = find_symbol(m, "Carrier", "uses");
        const ex::SymbolRec* echo = find_symbol(m, "echo", "uses");
        cg::test::check(
            r,
            payload && holder && carrier && echo,
            "matrix records extracted");
        if (!payload || !holder || !carrier || !echo)
        {
            return;
        }

        // 全部 USES 边，src 侧带持有符号的 NAME。
        const auto           names = key_names(m);
        std::vector<UseEdge> uses;
        for (const ex::EdgeRec& e : m.edges)
        {
            if (e.kind != ex::EdgeKind::Uses)
            {
                continue;
            }
            const auto src = names.find(e.src_key);
            uses.push_back(
                {.src_name = src == names.end() ? "?" : src->second,
                 .dst = e.dst.is_key ? e.dst.key : e.dst.meta.name,
                 .is_key = e.dst.is_key,
                 .attrs = e.attrs});
        }

        // field 交叉 class / template / std 命名：Carrier 的字段。
        cg::test::check(
            r,
            has_use_edge(
                uses,
                "by_class",
                payload->unique_key,
                ex::edge_attrs::uses_site_field),
            "field x class: by_class -> Payload (exact key)");
        cg::test::check(
            r,
            has_use_edge(
                uses,
                "by_template",
                holder->unique_key,
                ex::edge_attrs::uses_site_field),
            "field x template: by_template -> Holder primary");
        cg::test::check(
            r,
            has_use_edge(
                uses,
                "by_template",
                payload->unique_key,
                ex::edge_attrs::uses_site_field),
            "field x template: template argument -> Payload");
        cg::test::check(
            r,
            has_use_edge(
                uses,
                "by_std",
                payload->unique_key,
                ex::edge_attrs::uses_site_field),
            "field x std: std::vector<Payload> -> Payload");
        cg::test::check(
            r,
            count_uses_to(uses, "vector") == 0
                && count_uses_to(uses, "allocator") == 0,
            "std template records themselves create no USES edge");

        // param/return 交叉 class（echo：同一行的两个点在存储层
        // 折叠；提取层产出两条记录）。
        cg::test::check(
            r,
            has_use_edge(
                uses,
                "echo",
                payload->unique_key,
                ex::edge_attrs::uses_site_param),
            "param x class: echo(Payload) -> Payload");
        cg::test::check(
            r,
            has_use_edge(
                uses,
                "echo",
                payload->unique_key,
                ex::edge_attrs::uses_site_return),
            "return x class: echo returns Payload");
        cg::test::check(
            r,
            has_use_edge(
                uses,
                "echo_tpl",
                holder->unique_key,
                ex::edge_attrs::uses_site_param)
                && has_use_edge(
                    uses,
                    "echo_tpl",
                    payload->unique_key,
                    ex::edge_attrs::uses_site_param),
            "param x template: echo_tpl sees Holder and Payload");
        cg::test::check(
            r,
            has_use_edge(
                uses,
                "echo_std",
                payload->unique_key,
                ex::edge_attrs::uses_site_param),
            "param x std: echo_std(std::vector<Payload>) -> Payload");

        // echo 体内 locals 交叉 class/template/std。
        cg::test::check(
            r,
            has_use_edge(
                uses,
                "echo",
                payload->unique_key,
                ex::edge_attrs::uses_site_local),
            "local x class: Payload local in echo");
        cg::test::check(
            r,
            has_use_edge(
                uses,
                "echo",
                holder->unique_key,
                ex::edge_attrs::uses_site_local),
            "local x template: Holder<Payload> local in echo");
        cg::test::check(
            r,
            has_use_edge(
                uses,
                "echo",
                payload->unique_key,
                ex::edge_attrs::uses_site_local),
            "local x std: std::vector<Payload> local in echo");

        // 跨 TU 不完整类型：按名延迟，绝不用精确 key。
        bool opaque_deferred = false;
        for (const ex::EdgeRec& e : m.edges)
        {
            if (e.kind != ex::EdgeKind::Uses || e.dst.is_key)
            {
                continue;
            }
            const auto src = names.find(e.src_key);
            if (src != names.end() && src->second == "poke"
                && e.dst.meta.name == "Opaque" && e.dst.meta.ns == "uses"
                && e.dst.meta.kind == ex::SymKind::Record)
            {
                opaque_deferred = true;
            }
        }
        cg::test::check(
            r,
            opaque_deferred,
            "poke(Opaque*) defers by name (definition in another TU)");
    }

    const cg::test::Registrar reg_uses{"extract.uses_matrix", &uses_matrix};

    // 环境变量门控的原始转储（调试辅助：CCG_DUMP=1 打印合并
    // 提取产出的每条记录，而非跑用例）。
    void dump_merged()
    {
        const Merged m = merged_extraction(fixture_dir);
        for (const ex::SymbolRec& s : m.symbols)
        {
            std::println(
                "SYM kind={} name=[{}] ns=[{}] scope=[{}] parent=[{}] "
                "flags={:x} file={}",
                static_cast<int>(s.kind),
                s.name,
                s.ns,
                s.scope,
                s.parent,
                s.flags,
                s.file);
        }
        for (const ex::EdgeRec& e : m.edges)
        {
            std::println(
                "EDGE src=[{}] dst=[{}] type={} site={}:{}",
                e.src_key,
                e.dst.is_key ? e.dst.key : e.dst.meta.name,
                static_cast<int>(e.kind),
                e.site_file,
                e.site_line);
        }
        for (const ex::IncludeRec& i : m.includes)
        {
            std::println(
                "INC file={} target=[{}] angled={} line={}",
                i.file,
                i.target,
                i.angled,
                i.line);
        }
    }

} // namespace

// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module) 用 module-unit
// main 是有意为之：项目每个源文件都是具名模块单元；普通 TU
// 的 main 会破坏这项全模块政策
export int main()
{
    if (cg::util::cstd::get_env("CCG_DUMP"))
    {
        dump_merged();
        return 0;
    }
    return cg::test::run_all();
}

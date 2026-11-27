module;
// SQLite 的打开旗标与结果码是宏，宏不跨模块边界：用到它们的测试单元在
// 全局模块片段里包含头文件。语句经 cg.db.native 的拥有型对象执行，连接
// 经类型化查询库的连接对象打开，测试里没有手动的销毁与关闭调用。
#include <sqlite3.h>

export module cg.test.schemamirror;

// 镜像对账测试：DDL/表模型/围栏两侧常量镜像既往只靠注释互锚，
// 本文件把它们全部变成可执行断言——任何一侧漂移，这里先红。
//
// 覆盖的镜像对（全部点名）：
// 1. 表结构两头：GraphStore 实建的 DDL（PRAGMA table_info 实测）对照
//    cg.db.tables 模块里表模型的列名集；
// 2. kind 枚举两头：cg::db::SymbolKind/name_of 对照 cg::extract::SymKind/
//    name_of（十种逐值），外加 db 层 kind 谓词族的字符串/枚举双载对账；
// 3. edge 类型枚举两头：EdgeType/name_of 对照 EdgeKind/name_of；
// 4. sym_flags 围栏两侧：db 侧与 extract 侧逐位值相等；
// 5. edge_attrs 围栏两侧：共享子集逐位值相等；
// 6. unique_key 公式：extract::make_unique_key 对照
//    analysis::make_record_key/make_function_key 的分段拼法；
// 7. sig_hash 已知值对照：util 单点（extract/analysis 转发）对照裸
//    fnv1a64 直算——该值进 unique_key 与库列，逐位不变是红线；
// 8. 槽判据链：@fld/@fnv/@gvar 编解码往返与边界拒绝（空段拒收），
//    加 classify_site 五桶判据链的逐桶断言（propagate/dispatch 共用
//    单点后此处是判据的钉子）。

import std;
import cg.util;
import cg.db;
import cg.db.tables;
import cg.db.native;
import sqlpp23.sqlite3;
import cg.extract;
import cg.analysis;
import cg.test.harness;

namespace
{

    using R = cg::test::Result;

    // consteval 定长数组到 vector 的搬运（长度按表列数推导）。
    template <std::size_t N>
    auto to_vector(const std::array<std::string_view, N>& names)
        -> std::vector<std::string>
    {
        std::vector<std::string> out;
        for (const std::string_view n : names)
        {
            if (!n.empty())
            {
                out.emplace_back(n);
            }
        }
        return out;
    }

    // 表模型的列数（经 _table_columns 的列元组）。
    template <typename TableSpec>
    static constexpr std::size_t column_count_v = std::tuple_size_v<
        typename TableSpec::template _table_columns<int>::_column_tuple_t>;

    // 从表模型抽列名集（顺序不敏感——sqlite 按名取列，序是排版不是契约）。
    // 哑类型 int 充当 column_t 的表参：column_t 对表类型无约束，只携带
    // 名字标签。
    template <typename TableSpec>
    consteval auto model_column_names()
        -> std::array<std::string_view, column_count_v<TableSpec>>
    {
        using Tuple =
            typename TableSpec::template _table_columns<int>::_column_tuple_t;
        std::array<std::string_view, column_count_v<TableSpec>> names{};
        [&]<std::size_t... I>(std::index_sequence<I...>)
        {
            ((names[I] = std::tuple_element_t<I, Tuple>::_spec_t::
                  _sqlpp_name_tag::name),
             ...);
        }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
        return names;
    }

    // PRAGMA table_info 的列名集（第 2 列，按返回序）。语句由拥有型对象
    // 持有：预编译失败时带回错误文本，照旧返回空集。
    auto ddl_column_names(sqlite3* db, const char* table)
        -> std::vector<std::string>
    {
        std::vector<std::string> names;
        const std::string sql = std::format("PRAGMA table_info({})", table);
        std::expected<cg::db::NativeStatement, std::string> prepared =
            cg::db::NativeStatement::prepare(db, sql.c_str());
        if (!prepared.has_value())
        {
            return names;
        }
        while (prepared->step().has_row())
        {
            const std::string_view name = prepared->column_text(1);
            if (!name.empty())
            {
                names.emplace_back(name);
            }
        }
        return names;
    }

    void ddl_matches_table_model(R& r)
    {
        const auto        path = std::filesystem::path{"ccg-schema-mirror.db"};
        const std::string p{path.string()};
        cg::test::remove_sqlite_db(path);
        {
            // 真实自举：DDL 由 GraphStore 落盘，对账对象是"实际建出来的库"。
            cg::db::GraphStore store{p};
        }

        // 探针连接由类型化查询库的连接对象持有：打开失败时连接器抛出，
        // 这里落成一条失败的断言。连接先于删库文件关闭。
        std::optional<sqlpp::sqlite3::connection> probe;
        try
        {
            sqlpp::sqlite3::connection_config probe_config;
            probe_config.path_to_database = p;
            probe_config.flags = SQLITE_OPEN_READWRITE;
            probe.emplace(probe_config);
        }
        catch (const std::exception& error)
        {
            cg::test::check(
                r,
                false,
                std::format(
                    "mirror probe opens the bootstrapped db: {}",
                    error.what()));
            cg::test::remove_sqlite_db(path);
            return;
        }
        cg::test::check(r, true, "mirror probe opens the bootstrapped db");
        sqlite3* raw = probe->native_handle();

        using cg::db::tab::AnalysisSlots_;
        using cg::db::tab::CallSites_;
        using cg::db::tab::Edges_;
        using cg::db::tab::EdgeTypes_;
        using cg::db::tab::ExternalInvokes_;
        using cg::db::tab::ExternalParamArgs_;
        using cg::db::tab::Failures_;
        using cg::db::tab::Files_;
        using cg::db::tab::Fingerprints_;
        using cg::db::tab::Imports_;
        using cg::db::tab::Includes_;
        using cg::db::tab::Meta_;
        using cg::db::tab::PassthroughWrappers_;
        using cg::db::tab::PendingUpdates_;
        using cg::db::tab::Str_;
        using cg::db::tab::SymbolKinds_;
        using cg::db::tab::Symbols_;
        using cg::db::tab::TypeHints_;
        using cg::db::tab::UpdateClaim_;

        struct Mirror
        {
            const char*              table;
            std::vector<std::string> model;
        };
        const std::vector<Mirror> mirrors = {
            {"meta", to_vector(model_column_names<Meta_>())},
            {"str", to_vector(model_column_names<Str_>())},
            {"symbol_kinds", to_vector(model_column_names<SymbolKinds_>())},
            {"edge_types", to_vector(model_column_names<EdgeTypes_>())},
            {"files", to_vector(model_column_names<Files_>())},
            {"symbols", to_vector(model_column_names<Symbols_>())},
            {"edges", to_vector(model_column_names<Edges_>())},
            {"type_hints", to_vector(model_column_names<TypeHints_>())},
            {"call_sites", to_vector(model_column_names<CallSites_>())},
            {"includes", to_vector(model_column_names<Includes_>())},
            {"fingerprints", to_vector(model_column_names<Fingerprints_>())},
            {"failures", to_vector(model_column_names<Failures_>())},
            {"imports", to_vector(model_column_names<Imports_>())},
            {"analysis_slots", to_vector(model_column_names<AnalysisSlots_>())},
            {"pending_updates",
             to_vector(model_column_names<PendingUpdates_>())},
            {"passthrough_wrappers",
             to_vector(model_column_names<PassthroughWrappers_>())},
            {"external_param_args",
             to_vector(model_column_names<ExternalParamArgs_>())},
            {"external_invokes",
             to_vector(model_column_names<ExternalInvokes_>())},
            {"update_claim", to_vector(model_column_names<UpdateClaim_>())},
        };

        for (const Mirror& m : mirrors)
        {
            std::vector<std::string> ddl = ddl_column_names(raw, m.table);
            std::vector<std::string> model = m.model;
            std::ranges::sort(ddl);
            std::ranges::sort(model);
            cg::test::check(
                r,
                ddl == model,
                std::format("table {} columns match the model", m.table));
            cg::test::check(
                r,
                !ddl.empty(),
                std::format("table {} exists in the ddl", m.table));
        }

        // coord_events 在 cg.db.tables 模块里没有表模型（读写走 db.store
        // 的原生惰性语句——表可能缺席于同版本已存库的补建窗口，进
        // make_statements 会让连接打不开）；列名集在此手写锚定，DDL 与
        // 原生语句的绑定序两侧漂移时这里先红。
        {
            const std::vector<std::string> coord_cols = {
                "id",
                "ts_ms",
                "event",
                "actor_holder",
                "other_holder",
                "actor_pid",
                "other_pid",
                "reason",
                "detail"};
            std::vector<std::string> ddl =
                ddl_column_names(raw, "coord_events");
            std::ranges::sort(ddl);
            std::vector<std::string> want = coord_cols;
            std::ranges::sort(want);
            cg::test::check(
                r,
                ddl == want,
                "coord_events columns match the audit statements");
        }
        // 连接先于删库文件关闭：库文件在连接开着的时候删不掉。
        probe.reset();
        cg::test::remove_sqlite_db(path);
    }

    void kind_enum_mirror(R& r)
    {
        using K = cg::db::SymbolKind;
        for (int i = 0; i < static_cast<int>(cg::db::kind_count); ++i)
        {
            const auto db_k = static_cast<K>(i);
            const auto ex_k = static_cast<cg::extract::SymKind>(i);
            cg::test::check(
                r,
                cg::db::name_of(db_k) == cg::extract::name_of(ex_k),
                std::format("kind {} has the same name on both fences", i));
            // kind 谓词族的字符串/枚举双载对账（既往漂移的永久钉子）。
            const std::string_view name = cg::db::name_of(db_k);
            cg::test::check(
                r,
                cg::db::is_callable(db_k) == cg::db::is_callable(name)
                    && cg::db::is_type_like(db_k) == cg::db::is_type_like(name)
                    && cg::db::is_value_like(db_k)
                           == cg::db::is_value_like(name)
                    && cg::db::is_method_like(db_k)
                           == cg::db::is_method_like(name),
                std::format("predicate overloads agree for kind {}", name));
        }
        // 谓词集合的精确断言（漂移即红）。
        cg::test::check(
            r,
            cg::db::is_callable(K::Function) && cg::db::is_callable(K::Method)
                && cg::db::is_callable(K::Constructor)
                && cg::db::is_callable(K::Destructor)
                && !cg::db::is_callable(K::Record)
                && !cg::db::is_callable(K::Enumeration)
                && !cg::db::is_callable(K::EnumConstant)
                && !cg::db::is_callable(K::Field)
                && !cg::db::is_callable(K::Variable)
                && !cg::db::is_callable(K::TypeAlias),
            "callable set is exactly fn/method/ctor/dtor");
        cg::test::check(
            r,
            cg::db::is_type_like(K::Record)
                && cg::db::is_type_like(K::Enumeration)
                && cg::db::is_type_like(K::TypeAlias)
                && !cg::db::is_type_like(K::Function)
                && !cg::db::is_type_like(K::Field),
            "type-like set is exactly record/enum/type_alias");
        cg::test::check(
            r,
            cg::db::is_value_like(K::Field)
                && cg::db::is_value_like(K::Variable)
                && !cg::db::is_value_like(K::Record),
            "value-like set is exactly field/variable");
        cg::test::check(
            r,
            cg::db::is_method_like(K::Method)
                && cg::db::is_method_like(K::Constructor)
                && cg::db::is_method_like(K::Destructor)
                && !cg::db::is_method_like(K::Function),
            "method-like set is exactly method/ctor/dtor");
    }

    void edge_type_mirror(R& r)
    {
        // extract 侧没有名字映射（EdgeKind 只按数值 id 与播种表对应，
        // graph.build 的 to_db_edge 直接 static_cast）——镜像锚是枚举值：
        // 每个具名边的两侧整数值必须相等，播种 id 才不分叉。
        struct Pair
        {
            cg::db::EdgeType      db;
            cg::extract::EdgeKind ex;
            std::string_view      name;
        };
        const Pair pairs[] = {
            {cg::db::EdgeType::Calls, cg::extract::EdgeKind::Calls, "calls"},
            {cg::db::EdgeType::Extends,
             cg::extract::EdgeKind::Extends,
             "extends"},
            {cg::db::EdgeType::Overrides,
             cg::extract::EdgeKind::Overrides,
             "overrides"},
            {cg::db::EdgeType::TypeAlias,
             cg::extract::EdgeKind::TypeAlias,
             "type_alias"},
            {cg::db::EdgeType::FriendOf,
             cg::extract::EdgeKind::FriendOf,
             "friend_of"},
            {cg::db::EdgeType::Specializes,
             cg::extract::EdgeKind::Specializes,
             "specializes"},
            {cg::db::EdgeType::Uses, cg::extract::EdgeKind::Uses, "uses"},
        };
        for (const Pair& p : pairs)
        {
            cg::test::check(
                r,
                static_cast<int>(p.db) == static_cast<int>(p.ex),
                std::format(
                    "edge type {} keeps the same id on both fences",
                    p.name));
            cg::test::check(
                r,
                cg::db::name_of(p.db) == p.name,
                std::format("edge type {} keeps its seeded name", p.name));
        }
        // db 独有边类型（派发层与回调注册管线产物，extract 不产）：id 空档
        // 7/8/9 也要钉死，播种表的 type_id 行锚在这些值上。
        cg::test::check(
            r,
            cg::db::to_id(cg::db::EdgeType::VirtualDispatch) == 7
                && cg::db::to_id(cg::db::EdgeType::CallbackReg) == 8
                && cg::db::to_id(cg::db::EdgeType::IndirectCalls) == 9,
            "db-only dispatch edges keep ids 7, 8 and 9");
    }

    void sym_flags_mirror(R& r)
    {
        namespace d = cg::db::sym_flags;
        namespace e = cg::extract::sym_flags;
        struct Pair
        {
            std::string_view name;
            std::uint32_t    db;
            std::uint32_t    extract;
        };
        const Pair pairs[] = {
            {"is_struct", d::is_struct, e::is_struct},
            {"is_union", d::is_union, e::is_union},
            {"is_template", d::is_template, e::is_template},
            {"is_partial_spec", d::is_partial_spec, e::is_partial_spec},
            {"is_virtual", d::is_virtual, e::is_virtual},
            {"is_pure_virtual", d::is_pure_virtual, e::is_pure_virtual},
            {"is_static", d::is_static, e::is_static},
            {"is_constexpr", d::is_constexpr, e::is_constexpr},
            {"is_const", d::is_const, e::is_const},
            {"is_final", d::is_final, e::is_final},
        };
        for (const Pair& p : pairs)
        {
            cg::test::check(
                r,
                p.db == p.extract,
                std::format("sym flag {} mirrors across fences", p.name));
        }
        // db 独有位：界外体锚（extract 不产、graph 层读写）——值也要钉死。
        cg::test::check(
            r,
            d::external_body == 0x200,
            "db-only external_body stays at 0x200");
    }

    void edge_attrs_mirror(R& r)
    {
        namespace d = cg::db::edge_attrs;
        namespace e = cg::extract::edge_attrs;
        struct Pair
        {
            std::string_view name;
            std::uint32_t    db;
            std::uint32_t    extract;
        };
        const Pair pairs[] = {
            {"access_mask", d::access_mask, e::access_mask},
            {"access_protected", d::access_protected, e::access_protected},
            {"access_private", d::access_private, e::access_private},
            {"is_virtual_base", d::is_virtual_base, e::is_virtual_base},
            {"is_template_base", d::is_template_base, e::is_template_base},
            {"dispatch_mask", d::dispatch_mask, e::dispatch_mask},
            {"dispatch_member", d::dispatch_member, e::dispatch_member},
            {"dispatch_virtual", d::dispatch_virtual, e::dispatch_virtual},
            {"template_arg_shift",
             d::template_arg_shift,
             e::template_arg_shift},
            {"uses_site_mask", d::uses_site_mask, e::uses_site_mask},
            {"uses_site_field", d::uses_site_field, e::uses_site_field},
            {"uses_site_param", d::uses_site_param, e::uses_site_param},
            {"uses_site_return", d::uses_site_return, e::uses_site_return},
            {"uses_site_local", d::uses_site_local, e::uses_site_local},
            {"uses_site_variable",
             d::uses_site_variable,
             e::uses_site_variable},
        };
        for (const Pair& p : pairs)
        {
            cg::test::check(
                r,
                p.db == p.extract,
                std::format("edge attr {} mirrors across fences", p.name));
        }
    }

    void unique_key_formula_mirror(R& r)
    {
        cg::extract::SymbolRec fn;
        fn.kind = cg::extract::SymKind::Function;
        fn.name = "area";
        fn.ns = "shape";
        fn.scope = "shape";
        fn.file = "e:/src/a.cpp";
        fn.param_types = {"int", "char const*"};
        fn.const_method = false;
        const std::int64_t sig = cg::util::sig_hash_of(fn.param_types);
        const std::string  want =
            std::format("function|shape|area|e:/src/a.cpp|p{}", sig);
        cg::test::check(
            r,
            cg::extract::make_unique_key(fn) == want,
            "unique_key formula: function shape without const");

        fn.const_method = true;
        cg::test::check(
            r,
            cg::extract::make_unique_key(fn) == want + "|c",
            "unique_key formula: const method carries |c");

        cg::extract::SymbolRec rec;
        rec.kind = cg::extract::SymKind::Record;
        rec.name = "Box";
        rec.scope = "shape";
        rec.file = "e:/src/box.hpp";
        const std::string record_key = cg::extract::make_unique_key(rec);
        cg::test::check(
            r,
            record_key == "record|shape|Box|e:/src/box.hpp",
            "unique_key formula: record has no p/c segment");
        // analysis 侧的记录/函数分段拼法与 extract 公式互镜。
        cg::test::check(
            r,
            cg::analysis::make_record_key("shape", "Box", "e:/src/box.hpp")
                == record_key,
            "analysis make_record_key mirrors extract record key");
        cg::test::check(
            r,
            cg::analysis::make_function_key(
                "function",
                "shape",
                "area",
                "e:/src/a.cpp",
                sig,
                false)
                == want,
            "analysis make_function_key mirrors extract function key");
    }

    void sig_hash_known_value(R& r)
    {
        // 已知值对照（该值进 unique_key 与库列，逐位不变）。
        const std::vector<std::string> params = {"int", "char const*"};
        const std::int64_t             raw =
            static_cast<std::int64_t>(cg::util::fnv1a64("int,char const*"));
        cg::test::check(
            r,
            cg::util::sig_hash_of(params) == raw,
            "util sig_hash_of is fnv1a64 of the joined spelling");
        cg::test::check(
            r,
            cg::extract::sig_hash_of(params) == raw,
            "extract sig_hash_of forwards bit-identically");
        cg::test::check(
            r,
            cg::analysis::sig_hash_of(params) == raw,
            "analysis sig_hash_of forwards bit-identically");
        const std::vector<std::string> empty;
        cg::test::check(
            r,
            cg::util::sig_hash_of(empty)
                == static_cast<std::int64_t>(cg::util::fnv1a64("")),
            "empty parameter list hashes the empty spelling");
    }

    void slot_codec_mirror(R& r)
    {
        using cg::analysis::decode_field_slot;
        using cg::analysis::decode_fn_var_slot;
        using cg::analysis::decode_global_slot;
        using cg::analysis::encode_field_slot;
        using cg::analysis::encode_fn_var_slot;
        using cg::analysis::encode_global_slot;

        // 往返：owner 自身含 '|'（记录/函数/变量 key 形态），按最后 '|' 切。
        const std::string field =
            encode_field_slot("record|shape|Box|e:/src/box.hpp", "size_");
        const auto fld = decode_field_slot(field);
        cg::test::check(
            r,
            fld.has_value()
                && fld->first
                       == "record|shape|Box|e:/"
                          "src/box.hpp"
                && fld->second == "size_",
            "field slot round-trips through the final bar");

        const std::string fnv =
            encode_fn_var_slot("function|shape|area|e:/src/a.cpp|p42", "rhs");
        const auto fv = decode_fn_var_slot(fnv);
        cg::test::check(
            r,
            fv.has_value()
                && fv->first == "function|shape|area|e:/src/a.cpp|p42"
                && fv->second == "rhs",
            "fn-var slot round-trips through the final bar");

        const std::string gvar =
            encode_global_slot("variable||g_dx|e:/src/g.cpp", "slot");
        const auto gv = decode_global_slot(gvar);
        cg::test::check(
            r,
            gv.has_value() && gv->first == "variable||g_dx|e:/src/g.cpp"
                && gv->second == "slot",
            "global slot round-trips through the final bar");

        // 边界拒绝（三前缀统一：空 owner/空 name/无分隔/前缀不符）。
        cg::test::check(
            r,
            !decode_field_slot("@fld||name").has_value(),
            "field slot rejects an empty owner segment");
        cg::test::check(
            r,
            !decode_fn_var_slot("@fnv|owner|").has_value(),
            "fn-var slot rejects an empty name segment");
        cg::test::check(
            r,
            !decode_global_slot("@gvar|owner").has_value(),
            "global slot rejects a missing separator");
        cg::test::check(
            r,
            !decode_field_slot("@fnv|owner|name").has_value(),
            "decoders stay prefix-disciplined (no cross-eating)");
        cg::test::check(
            r,
            !decode_fn_var_slot("bare_name").has_value()
                && !decode_global_slot("bare_name").has_value(),
            "bare names are not slot descriptors");
    }

    void site_bucket_chain(R& r)
    {
        using cg::analysis::classify_site;
        using cg::analysis::PropagatedSite;
        using cg::analysis::SITE_BUCKET_INDIRECT;
        using cg::analysis::SITE_BUCKET_NO_METHOD;
        using cg::analysis::SITE_BUCKET_NOT_VIRTUAL;
        using cg::analysis::SITE_BUCKET_QUALIFIED;
        using cg::analysis::SITE_BUCKET_VIRTUAL;
        PropagatedSite s;
        s.indirect = true;
        cg::test::check(
            r,
            classify_site(s) == SITE_BUCKET_INDIRECT,
            "indirect sites bucket first regardless of the rest");
        s.indirect = false;
        s.is_virtual = false;
        cg::test::check(
            r,
            classify_site(s) == SITE_BUCKET_NOT_VIRTUAL,
            "non-virtual sites bucket second");
        s.is_virtual = true;
        s.qualified = true;
        cg::test::check(
            r,
            classify_site(s) == SITE_BUCKET_QUALIFIED,
            "qualified virtual sites bucket third");
        s.qualified = false;
        cg::test::check(
            r,
            classify_site(s) == SITE_BUCKET_NO_METHOD,
            "method-less virtual sites bucket fourth");
        s.method_id = 42;
        cg::test::check(
            r,
            classify_site(s) == SITE_BUCKET_VIRTUAL,
            "resolvable virtual sites bucket last");
    }

    const cg::test::Registrar reg_ddl{
        "schema-mirror.ddl_vs_table_model",
        &ddl_matches_table_model};
    const cg::test::Registrar reg_kind{
        "schema-mirror.kind_enum",
        &kind_enum_mirror};
    const cg::test::Registrar reg_edge{
        "schema-mirror.edge_type_enum",
        &edge_type_mirror};
    const cg::test::Registrar reg_flags{
        "schema-mirror.sym_flags",
        &sym_flags_mirror};
    const cg::test::Registrar reg_attrs{
        "schema-mirror.edge_attrs",
        &edge_attrs_mirror};
    const cg::test::Registrar reg_key{
        "schema-mirror.unique_key_formula",
        &unique_key_formula_mirror};
    const cg::test::Registrar reg_sig{
        "schema-mirror.sig_hash_known_value",
        &sig_hash_known_value};
    const cg::test::Registrar reg_slot{
        "schema-mirror.slot_codec",
        &slot_codec_mirror};
    const cg::test::Registrar reg_bucket{
        "schema-mirror.site_bucket_chain",
        &site_bucket_chain};

} // namespace

// NOLINTNEXTLINE(clang-diagnostic-main-attached-to-named-module) 用 module-unit
// main 是有意为之：项目每个源文件都是具名模块单元；普通 TU
// 的 main 会破坏这项全模块政策
export int main() { return cg::test::run_all(); }

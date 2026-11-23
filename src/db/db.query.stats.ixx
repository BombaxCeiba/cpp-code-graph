module;
#include "db.tables.h"

/**
 * @module cg.db.query.stats
 *
 * @brief 统计与全表读取域：stats
 * 汇总、表计数、文件清单与路径映射、符号索引投影、全表行读取与重复键清点、includes/imports/fingerprints/failures。
 */
export module cg.db.query.stats;

import std;
import cg.util;
import sqlpp23.core;
import sqlpp23.sqlite3;
import cg.db.schema;
import cg.db.store;
import cg.db.query.lookup;

namespace cg::db
{

    /**
     * @brief files 表是否有任一行（EXISTS 语义的存在性探针）。
     *
     * @internal 供"库是否为空"的快速判定（引擎身份比对前的库内容探测）：
     * stats() 要对四张表分组计数，每次编辑都会走到的路径只需这一比特。
     * LIMIT 1 的单列探针在首行命中即返回，代价与 files 行数无关。
     */
    export auto has_files(GraphStore& store) -> bool
    {
        const tab::Files f{};
        // sqlpp 结果是惰性流（begin/end 非常量），不能以 const 接收。
        auto rows = store.conn()(sqlpp::select(f.fileId).from(f).limit(1u));
        return rows.begin() != rows.end();
    }

    export auto stats(GraphStore& store) -> Stats
    {
        Stats               out;
        const tab::Files    f{};
        const tab::Symbols  s{};
        const tab::Edges    e{};
        const tab::Includes inc{};
        const tab::Str      str{};

        {
            auto rows = store.conn()(sqlpp::select(
                                         f.parseStatus,
                                         sqlpp::count(f.fileId).as(tab::cnt))
                                         .from(f)
                                         .group_by(f.parseStatus));
            for (const auto& row : rows)
            {
                const std::int64_t n = row.cnt;
                out.files += n;
                switch (static_cast<cg::util::ParseStatus>(
                    static_cast<int>(row.parseStatus)))
                {
                case cg::util::ParseStatus::Ok:
                    out.files_ok += n;
                    break;
                case cg::util::ParseStatus::Partial:
                    out.files_partial += n;
                    break;
                case cg::util::ParseStatus::Failed:
                    out.files_failed += n;
                    break;
                case cg::util::ParseStatus::Pending:
                    out.files_pending += n;
                    break;
                }
            }
        }
        {
            auto rows = store.conn()(
                sqlpp::select(s.kindId, sqlpp::count(s.symId).as(tab::cnt))
                    .from(s)
                    .group_by(s.kindId));
            for (const auto& row : rows)
            {
                const std::int64_t n = row.cnt;
                out.symbols += n;
                const auto kind = static_cast<std::int64_t>(row.kindId);
                if (kind >= 0 && kind < kind_count)
                {
                    out.symbols_by_kind[kind] += n;
                }
            }
        }
        {
            auto rows = store.conn()(
                sqlpp::select(e.typeId, sqlpp::count(e.edgeId).as(tab::cnt))
                    .from(e)
                    .group_by(e.typeId));
            for (const auto& row : rows)
            {
                const std::int64_t n = row.cnt;
                out.edges += n;
                const auto type = static_cast<std::int64_t>(row.typeId);
                if (type >= 0 && type < edge_type_count)
                {
                    out.edges_by_type[type] += n;
                }
            }
        }
        {
            auto rows = store.conn()(
                sqlpp::select(sqlpp::count(inc.fileId).as(tab::cnt)).from(inc));
            for (const auto& row : rows)
            {
                out.includes = row.cnt;
            }
            auto angled = store.conn()(
                sqlpp::select(sqlpp::count(inc.fileId).as(tab::cnt))
                    .from(inc)
                    .where(inc.angled == 1));
            for (const auto& row : angled)
            {
                out.includes_angled = row.cnt;
            }
        }
        {
            const tab::Imports imp{};
            auto               rows = store.conn()(
                sqlpp::select(sqlpp::count(imp.fileId).as(tab::cnt)).from(imp));
            for (const auto& row : rows)
            {
                out.imports = row.cnt;
            }
        }
        {
            auto rows = store.conn()(
                sqlpp::select(sqlpp::count(str.id).as(tab::cnt)).from(str));
            for (const auto& row : rows)
            {
                out.strings = row.cnt;
            }
        }
        {
            const tab::TypeHints th{};
            for (const auto& row : store.conn()(
                     sqlpp::select(sqlpp::count(th.hintId).as(tab::cnt))
                         .from(th)))
            {
                out.type_hints = row.cnt;
            }
            const tab::CallSites cs{};
            for (const auto& row : store.conn()(
                     sqlpp::select(sqlpp::count(cs.siteId).as(tab::cnt))
                         .from(cs)))
            {
                out.call_sites = row.cnt;
            }
        }
        // 下面两个 meta 是机器来源（写入侧恒为 std::to_string 纯数字），
        // 只可能被外部 sqlite 篡改或版本残留弄脏——解析用邻居同款宽容
        // 语义（atoll，畸形归 0）：stats 是全库摘要命令，绝不因单个 meta
        // 畸形而整单失败（此前 stoll 会让整条 ccg stats 以
        // "invalid stoll argument" 退出）。
        if (const auto v = store.meta("analysis_rounds"))
        {
            out.analysis_rounds = std::atoll(v->c_str());
        }
        if (const auto v = store.meta("analysis_work_items"))
        {
            out.analysis_work_items = std::atoll(v->c_str());
        }
        if (const auto v = store.meta("analysis_budget_exhausted"))
        {
            out.analysis_budget_exhausted = *v == "1";
        }
        if (const auto v = store.meta("app_version"))
        {
            out.app_version = *v;
        }
        if (const auto t = store.meta("last_index_at"))
        {
            out.last_index_at = t;
        }
        return out;
    }

    export auto symbol_count(GraphStore& store) -> std::int64_t
    {
        const tab::Symbols s{};
        auto               rows = store.conn()(
            sqlpp::select(sqlpp::count(s.symId).as(tab::cnt)).from(s));
        std::int64_t cnt = 0;
        for (const auto& row : rows)
        {
            cnt = row.cnt; // 跑到 DONE：中途 return 会钉住隐式读快照
        }
        return cnt;
    }

    export auto edge_count(GraphStore& store) -> std::int64_t
    {
        const tab::Edges e{};
        auto             rows = store.conn()(
            sqlpp::select(sqlpp::count(e.edgeId).as(tab::cnt)).from(e));
        std::int64_t cnt = 0;
        for (const auto& row : rows)
        {
            cnt = row.cnt; // 跑到 DONE：中途 return 会钉住隐式读快照
        }
        return cnt;
    }

    /**
     * @brief 每条 files 行，路径文本已解析（cg.graph.build 的删除源码差分
     *        拿它与已加载的编译数据库对照）。
     */
    export auto all_files(GraphStore& store) -> std::vector<FileRow>
    {
        const tab::Files     f{};
        std::vector<FileRow> out;
        auto rows = store.conn()(sqlpp::select(sqlpp::all_of(f)).from(f));
        for (const auto& row : rows)
        {
            FileRow fr;
            fr.id = static_cast<FileId>(row.fileId);
            fr.path = store.str_of(static_cast<StrId>(row.pathId));
            if (row.contentHash.has_value())
            {
                fr.content_hash = std::string{*row.contentHash};
            }
            fr.status = static_cast<cg::util::ParseStatus>(
                static_cast<int>(row.parseStatus));
            fr.error_count = static_cast<int>(row.errorCount);
            if (row.lastError.has_value())
            {
                fr.last_error = std::string{*row.lastError};
            }
            if (row.indexedAt.has_value())
            {
                fr.indexed_at = std::string{*row.indexedAt};
            }
            out.push_back(std::move(fr));
        }
        return out;
    }

    /**
     * @brief 在场 files 行的 id 全集（单条查询，仅主键列）。
     *
     * @internal 提交段开头的陈旧快照核对用：索引/更新的解析期在事务外拿过
     *           一份 file_id 快照（row_by_path 等），并发写者可能在期间删
     *           除过行；提交事务内以此集合作差，死 id 的引用改为复活/丢弃，
     *           绝不让旧 id 直接流进外键列（受控实验已证实该穿插会以
     *           FOREIGN KEY 整批回滚收场）。只选 id 列，不做路径文本解析。
     */
    export auto all_file_ids(GraphStore& store) -> std::unordered_set<FileId>
    {
        const tab::Files           f{};
        std::unordered_set<FileId> out;
        auto rows = store.conn()(sqlpp::select(f.fileId).from(f));
        for (const auto& row : rows)
        {
            out.insert(static_cast<FileId>(row.fileId));
        }
        return out;
    }

    /**
     * @brief 全部文件的路径映射（file_id -> path），单条查询两列物化。
     *
     * @internal 站点装载路径的文件名解析用：调用点按行内 file_id 直取路径
     *           （整表一次），替代逐行单键查询（行内无 file_id 反查场景时
     *           无单行查询必要；files 表在分析期只读，映射与逐行查询结果
     *           一致）。
     */
    export auto file_path_map(GraphStore& store)
        -> std::unordered_map<FileId, std::string>
    {
        const tab::Files                        f{};
        std::unordered_map<FileId, std::string> out;
        auto rows = store.conn()(sqlpp::select(f.fileId, f.pathId).from(f));
        for (const auto& row : rows)
        {
            out.emplace(
                static_cast<FileId>(row.fileId),
                store.str_of(static_cast<StrId>(row.pathId)));
        }
        return out;
    }

    /**
     * @brief 供内存解析索引使用的纤薄符号投影（cg.graph.build）：不做限定名
     *        遍历，整表一条查询。
     */
    export struct IndexRow
    {
        SymId                       id = 0;
        SymbolKind                  kind = SymbolKind::Record;
        std::string                 name;
        std::string                 ns;
        std::optional<SymId>        parent_id;
        std::string                 parent; // 父符号的非限定名，无父时为 ''
        std::optional<std::int64_t> sig_hash;
        std::uint32_t               flags = 0;
        FileId                      file_id = 0;
        std::string                 unique_key;
    };

    /**
     * @brief 整表读出 IndexRow。
     *
     * @internal 父名从刚加载的行解析（单次额外遍历）。
     */
    export auto all_symbol_index_rows(GraphStore& store)
        -> std::vector<IndexRow>
    {
        const tab::Symbols    s{};
        std::vector<IndexRow> out;
        auto                  rows = store.conn()(sqlpp::select(
                                                      s.symId,
                                                      s.kindId,
                                                      s.nameId,
                                                      s.nsId,
                                                      s.parentId,
                                                      s.sigHash,
                                                      s.flags,
                                                      s.fileId,
                                                      s.uniqueKeyId)
                                                      .from(s));
        for (const auto& row : rows)
        {
            IndexRow ir;
            ir.id = static_cast<SymId>(row.symId);
            ir.kind = symbol_kind_of(static_cast<std::int64_t>(row.kindId));
            ir.name = store.str_of(static_cast<StrId>(row.nameId));
            ir.ns = store.str_of(static_cast<StrId>(row.nsId));
            ir.unique_key = store.str_of(static_cast<StrId>(row.uniqueKeyId));
            if (row.parentId.has_value())
            {
                ir.parent_id = static_cast<SymId>(*row.parentId);
            }
            if (row.sigHash.has_value())
            {
                ir.sig_hash = static_cast<std::int64_t>(*row.sigHash);
            }
            ir.flags = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(row.flags));
            ir.file_id = static_cast<FileId>(row.fileId);
            out.push_back(std::move(ir));
        }
        std::unordered_map<SymId, std::string*> names;
        names.reserve(out.size() * 2);
        for (IndexRow& ir : out)
        {
            names.emplace(ir.id, &ir.name);
        }
        for (IndexRow& ir : out)
        {
            if (ir.parent_id.has_value())
            {
                if (const auto it = names.find(*ir.parent_id);
                    it != names.end())
                {
                    ir.parent = *it->second;
                }
            }
        }
        return out;
    }

    /**
     * @brief 字符串已解析的完整符号行，供增量等价性门整表读取（一条
     *        select；调用方自行构建投影——下方 id 稳定的 dump 嵌入的是
     *        存储顺序的 id）。
     */
    export struct FullSymbolRow
    {
        SymId                       id = 0;
        SymbolKind                  kind = SymbolKind::Record;
        std::string                 name;
        std::string                 ns;
        std::string                 unique_key;
        std::optional<std::int64_t> sig_hash;
        FileId                      file_id = 0;
        int                         line = 0;
        int                         col = 0;
        std::optional<SymId>        parent_id;
        std::uint32_t               flags = 0;
        std::optional<std::string>  param_types;
        std::optional<std::string>  template_params;
    };

    export auto all_symbol_rows(GraphStore& store) -> std::vector<FullSymbolRow>
    {
        const tab::Symbols         s{};
        std::vector<FullSymbolRow> out;
        auto rows = store.conn()(sqlpp::select(sqlpp::all_of(s)).from(s));
        for (const auto& row : rows)
        {
            FullSymbolRow fr;
            fr.id = static_cast<SymId>(row.symId);
            fr.kind = symbol_kind_of(static_cast<std::int64_t>(row.kindId));
            fr.name = store.str_of(static_cast<StrId>(row.nameId));
            fr.ns = store.str_of(static_cast<StrId>(row.nsId));
            fr.unique_key = store.str_of(static_cast<StrId>(row.uniqueKeyId));
            if (row.sigHash.has_value())
            {
                fr.sig_hash = static_cast<std::int64_t>(*row.sigHash);
            }
            fr.file_id = static_cast<FileId>(row.fileId);
            fr.line = static_cast<int>(row.line);
            fr.col = static_cast<int>(row.col);
            if (row.parentId.has_value())
            {
                fr.parent_id = static_cast<SymId>(*row.parentId);
            }
            fr.flags = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(row.flags));
            if (row.paramTypes.has_value())
            {
                fr.param_types = std::string{*row.paramTypes};
            }
            if (row.templateParams.has_value())
            {
                fr.template_params = std::string{*row.templateParams};
            }
            out.push_back(std::move(fr));
        }
        return out;
    }

    /**
     * @brief 一条 select 读出每条边行（遍历查询保持逐符号；这是门禁与
     *        审计的批量读取）。
     */
    export auto all_edges(GraphStore& store) -> std::vector<EdgeRow>
    {
        const tab::Edges     e{};
        std::vector<EdgeRow> out;
        auto rows = store.conn()(sqlpp::select(sqlpp::all_of(e)).from(e));
        for (const auto& row : rows)
        {
            out.push_back(
                {.id = static_cast<EdgeId>(row.edgeId),
                 .src = static_cast<SymId>(row.srcId),
                 .dst = static_cast<SymId>(row.dstId),
                 .type = edge_type_of(static_cast<std::int64_t>(row.typeId)),
                 .site_file = static_cast<FileId>(row.siteFileId),
                 .site_line = static_cast<int>(row.siteLine),
                 .attrs = static_cast<std::uint32_t>(
                     static_cast<std::int64_t>(row.attrs)),
                 .confidence = row.confidence,
                 .occurrences = static_cast<std::int64_t>(row.occurrences)});
        }
        return out;
    }

    /**
     * @brief 重复行探测组（完整性断言；测试与未来的 doctor）：每个函数返回
     *        对 schema 声明为 UNIQUE 的键持有超过一行的分组数。
     *
     * @internal upsert 保证这一点；保持在 SQL 里的直接 SQL 检查让证明保持
     * 诚实。
     */
    export auto duplicate_symbol_keys(GraphStore& store) -> std::int64_t
    {
        const tab::Symbols s{};
        auto rows = store.conn()(sqlpp::select(s.uniqueKeyId)
                                     .from(s)
                                     .group_by(s.uniqueKeyId)
                                     .having(sqlpp::count(s.symId) > 1));
        return std::ranges::distance(rows);
    }

    export auto duplicate_edge_sites(GraphStore& store) -> std::int64_t
    {
        const tab::Edges e{};
        auto             rows = store.conn()(
            sqlpp::select(e.srcId, e.dstId, e.typeId, e.siteFileId, e.siteLine)
                .from(e)
                .group_by(e.srcId, e.dstId, e.typeId, e.siteFileId, e.siteLine)
                .having(sqlpp::count(e.edgeId) > 1));
        return std::ranges::distance(rows);
    }

    export auto duplicate_includes(GraphStore& store) -> std::int64_t
    {
        const tab::Includes inc{};
        auto                rows =
            store.conn()(sqlpp::select(inc.fileId, inc.targetId, inc.line)
                             .from(inc)
                             .group_by(inc.fileId, inc.targetId, inc.line)
                             .having(sqlpp::count(inc.fileId) > 1));
        return std::ranges::distance(rows);
    }

    /**
     * @brief 一条 include 图边：包含方文件 -> 指令中书写的目标文本。
     *
     * @internal 目标解析成文件 id 是调用方的事（规则取决于包含方目录与
     * include 搜索路径；`angled` 告诉严格解析器是否搜索当前目录）。
     */
    export struct IncludeLink
    {
        FileId      includer = 0;
        std::string target;
        bool        angled = false;
    };

    export auto all_includes(GraphStore& store) -> std::vector<IncludeLink>
    {
        const tab::Includes      inc{};
        std::vector<IncludeLink> out;
        auto                     rows = store.conn()(
            sqlpp::select(inc.fileId, inc.targetId, inc.angled).from(inc));
        for (const auto& row : rows)
        {
            out.push_back(
                {.includer = static_cast<FileId>(row.fileId),
                 .target = store.str_of(static_cast<StrId>(row.targetId)),
                 .angled = row.angled != 0});
        }
        return out;
    }

    /**
     * @brief 每条 imports 行，模块文本已解析（模块图差分与增量等价面）。
     */
    export auto all_imports(GraphStore& store) -> std::vector<ImportLink>
    {
        const tab::Imports      imp{};
        std::vector<ImportLink> out;
        auto                    rows = store.conn()(
            sqlpp::select(imp.fileId, imp.moduleId, imp.line).from(imp));
        for (const auto& row : rows)
        {
            out.push_back(
                {.file = static_cast<FileId>(row.fileId),
                 .module_name = store.str_of(static_cast<StrId>(row.moduleId)),
                 .line = static_cast<int>(row.line)});
        }
        return out;
    }

    /**
     * @brief 每条新鲜度指纹，一条查询读全（增量过期检查拿它们与整张
     *        files 表的 stat() 结果对照）。
     */
    export auto all_fingerprints(GraphStore& store)
        -> std::vector<FingerprintRow>
    {
        const tab::Fingerprints     fp{};
        std::vector<FingerprintRow> out;
        auto rows = store.conn()(sqlpp::select(sqlpp::all_of(fp)).from(fp));
        for (const auto& row : rows)
        {
            out.push_back(
                {.file_id = static_cast<FileId>(row.fileId),
                 .mtime_ns = static_cast<std::int64_t>(row.mtimeNs),
                 .size = static_cast<std::int64_t>(row.sizeBytes),
                 .cmd_digest = std::string{row.cmdDigest}});
        }
        return out;
    }

    /**
     * @brief 全部失败条目（schema v7），供一次索引运行在 pass 1 之前快照。
     *
     * @internal 行按 (file_id, cmd_digest) 键控：同一文件的多条编译条目
     * 各自独立判定，一条失败不污染另一条成功的条目。
     */
    export auto all_failures(GraphStore& store) -> std::vector<FailureRow>
    {
        const tab::Failures     fl{};
        std::vector<FailureRow> out;
        auto rows = store.conn()(sqlpp::select(sqlpp::all_of(fl)).from(fl));
        for (const auto& row : rows)
        {
            FailureRow f;
            f.file_id = static_cast<FileId>(row.fileId);
            f.cmd_digest = std::string{row.cmdDigest};
            f.content_hash = std::string{row.contentHash};
            f.mtime_ns = static_cast<std::int64_t>(row.mtimeNs);
            f.size = static_cast<std::int64_t>(row.sizeBytes);
            f.engine_token = std::string{row.engineToken};
            f.feed_digest = std::string{row.feedDigest};
            f.roots_digest = std::string{row.rootsDigest};
            f.failure_class = std::string{row.failureClass};
            f.failure_text = std::string{row.failureText};
            f.deps_json = std::string{row.depsJson};
            f.attempts = static_cast<std::int64_t>(row.attempts);
            f.first_failed_at = std::string{row.firstFailedAt};
            f.last_failed_at = std::string{row.lastFailedAt};
            f.retry_after = static_cast<std::int64_t>(row.retryAfter);
            out.push_back(std::move(f));
        }
        return out;
    }
} // namespace cg::db

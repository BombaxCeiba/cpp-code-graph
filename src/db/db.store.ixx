module;
#include <sqlite3.h>

#include "db.tables.h"

/**
 * @module cg.db.store
 *
 * @brief GraphStore：数据库连接的唯一拥有者。打开/PRAGMA/DDL 引导与
 *        版本守卫、预编译语句缓存、字符串驻留、files/symbols/edges/
 *        includes 的幂等 upsert、按文件清除，以及事务 RAII 守卫。
 *
 * @internal
 * 全局模块片段承载 db.tables.h（为何不能是 import 见该头）与连接配置
 * 所需的 SQLITE_OPEN_* 宏旗标：宏不跨模块边界，include 只放在使用它
 * 的这一个单元；sqlite3 声明也就留在此处（全局模块片段单一属主
 * 规则）。
 */
export module cg.db.store;

import std;
import cg.util;
import cg.util.cstd;
import sqlpp23.core;
import sqlpp23.sqlite3;
import cg.db.schema;

namespace cg::db
{

    /**
     * @brief schema 版本不匹配：库由另一代 ccg 写就，拒绝打开。
     *
     * 类型化（而非裸 runtime_error）使调用方（cg.cli.runners 的
     * open_store_create）能按类型识别这一类失败并决定是否重建文件，
     * 不依赖错误文案匹配。
     */
    export struct SchemaMismatchError final : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    namespace
    {

        /** @brief 预编译语句元组的下标（见 make_statements）。 */
        enum St : std::uint8_t
        {
            INTERN,            // insert str，冲突 do nothing，返回 id
            STR_BY_S,          // select id from str where s = ?
            STR_BY_ID,         // select s from str where id = ?
            FILE_INSERT,       // insert files(path_id)，冲突 do nothing
            FILE_BY_PATH,      // select file_id from files where path_id = ?
            FILE_HASH_BY_PATH, // select content_hash from files where path_id =
                               // ?
            FILE_STATUS, // update files set parse_status, ... where file_id = ?
            FILE_HASH,   // update files set content_hash where file_id = ?
            FILE_PATH,   // update files set path_id where file_id = ?
            SYM_INSERT,  // insert symbols(...)，冲突 do nothing
            SYM_BY_KEY,  // select sym_id from symbols where unique_key_id = ?
            SYM_REFRESH, // update symbols set ... where sym_id = ?
            EDGE_INSERT, // insert edges(...)，冲突 do nothing
            EDGE_BY_KEY, // select edge_id from edges where site key = ?
            EDGE_BUMP,   // update edges set occurrences = occurrences + ? where
                         // edge_id =
                         // ?
            INCLUDE_INSERT, // insert includes(...)，冲突 do nothing
            IMPORT_INSERT,  // insert imports(...)，冲突 do nothing
            DEL_EDGES_SITE, // delete from edges where site_file_id = ?
            DEL_INCLUDES,   // delete from includes where file_id = ?
            DEL_IMPORTS,    // delete from imports where file_id = ?
            DEL_FILE,       // delete from files where file_id = ?（FK 级联）
            KEYS_BY_FILE, // select unique_key_id from symbols where file_id = ?
            SYM_BY_ID,    // select * from symbols where sym_id = ?
            FILE_BY_ID,   // select * from files where file_id = ?
            FP_INSERT,    // insert fingerprints(...)，冲突 do nothing
            FP_UPDATE,    // update fingerprints set ... where file_id = ?
            FP_DELETE, // delete from fingerprints where (file_id, cmd_digest) =
                       // ?
            FAIL_INSERT, // insert failures(...)，冲突 do nothing
            FAIL_UPDATE, // update failures set ... where (file_id, cmd_digest)
                         // = ?
            FAIL_DELETE, // delete from failures where (file_id, cmd_digest) = ?
            PENDING_INSERT, // insert pending_updates(path)，冲突 do nothing
            PENDING_SELECT, // select path from pending_updates order by path
            PENDING_DELETE, // delete from pending_updates where path =
                            // ?（批后按覆盖集清除）
            CLAIM_BY_ID, // select holder, lease_until from update_claim where
                         // id
            CLAIM_HEARTBEAT, // update update_claim set lease_until where
                             // id+holder
            CLAIM_RELEASE, // delete from update_claim where id = 1 and holder =
                           // ?
            HINT_INSERT,   // insert type_hints，冲突 do nothing
            HINT_DELETE_FILE,    // delete from type_hints where file_id = ?
            SITE_INSERT,         // insert call_sites，冲突 do nothing
            SITE_BY_KEY,         // select site_id from call_sites where 唯一键
            SITE_DELETE_FILE,    // delete from call_sites where file_id = ?
            SITE_REFRESH,        // update call_sites 解析结果列 where site_id
            EDGE_DELETE_TYPE,    // delete from edges where type_id = ?
            EDGE_DELETE_VD_FILE, // delete from edges where type_id=7 and
                                 // site_file_id=?
            EDGE_DELETE_VD_SITE, // delete from edges where type_id=7 and
                                 // (src,site_file,site_line)=?
            SLOT_INSERT,         // insert analysis_slots，冲突 do nothing
            SLOT_UPDATE,         // update analysis_slots 全列 where (owner_sym,
                                 // var_name)
            SLOT_SELECT_ALL,     // select * from analysis_slots，确定序
            SLOT_DELETE_ALL,     // delete from analysis_slots
            SLOT_DELETE_KEY,     // delete from analysis_slots where (owner_sym,
                                 // var_name) =
                                 // ?
            PTW_INSERT,          // insert passthrough_wrappers，冲突 do nothing
            PTW_SELECT_ALL,      // select * from passthrough_wrappers，确定序
            EPA_INSERT,          // insert external_param_args，冲突 do nothing
            EPA_SELECT_ALL,      // select * from external_param_args，确定序
            EVI_INSERT,          // insert external_invokes，冲突 do nothing
            EVI_SELECT_ALL,      // select * from external_invokes，确定序
            EDGES_FROM_AGG, // select dst,type,sum,min,max,max where src group
                            // by
            EDGES_TO_AGG,  // select src,type,sum,min,max,max where dst group by
            EDGES_OF_OUT,  // select * from edges where src = ? and type = ?
            EDGES_OF_IN,   // select * from edges where dst = ? and type = ?
            DEL_SYM_BY_ID, // delete from symbols where sym_id = ?
            CONTRIB_INSERT,    // insert symbol_contributions，冲突 do nothing
            CONTRIB_OF_ENTRY,  // select sym_id from symbol_contributions where
                               // (file_id, cmd_digest) = ? order by sym_id
            CONTRIB_OF_FILE,   // select sym_id from symbol_contributions where
                               // file_id = ? order by sym_id
            CONTRIB_DEL_ONE,   // delete from symbol_contributions where
                               // (sym_id, file_id, cmd_digest) = ?
            CONTRIB_DEL_ENTRY, // delete from symbol_contributions where
                               // (file_id, cmd_digest) = ?
            CONTRIB_DEL_FILE,  // delete from symbol_contributions where file_id
                               // = ?
            CONTRIB_EXISTS, // select 1 from symbol_contributions where sym_id
                            // = ? limit 1
            COUNT,          // 语句计数哨兵
        };

        /**
         * @brief 从声明持有者记号里取出 pid；不是本格式时返回 0。
         *
         * @internal
         * 记号由 cg.db.schema 的 update_claim_token 生成，完整形状
         * `ccg-update:pid:<digits>:t<ticks>`（前缀常量 claim_token_prefix 与
         * 生成函数都在 schema，格式单点）；这里只认这个形状（逐位解析，
         * 溢出即放弃），任何别的字符串——测试用的 "H1"、手写占位符——都返回 0，
         * 表示 "无 pid 信息"，调用方退回纯租约判定。故意不导出：它是本单元
         * 与持有者记号之间唯一的格式耦合点，且只在 claim_update 里使用。
         */
        /// coord_events 写侧的文本绑定：空 string_view 的 data() 可为
        /// nullptr（会绑成 NULL，撞上 NOT NULL 列），一律回退空串。
        void bind_coord_text(sqlite3_stmt* stmt, int idx, std::string_view text)
        {
            const char* data = text.data() == nullptr ? "" : text.data();
            sqlite3_bind_text(
                stmt,
                idx,
                data,
                static_cast<int>(text.size()),
                SQLITE_TRANSIENT);
        }

        /// coord_events 读侧的列文本（NULL 列回空串）。
        [[nodiscard]]
        auto coord_column_text(sqlite3_stmt* stmt, int col) -> std::string
        {
            const unsigned char* text = sqlite3_column_text(stmt, col);
            return text == nullptr
                       ? std::string{}
                       : std::string{reinterpret_cast<const char*>(text)};
        }

        [[nodiscard]]
        auto holder_pid(std::string_view holder) -> std::int64_t
        {
            constexpr std::string_view prefix = cg::db::claim_token_prefix;
            if (!holder.starts_with(prefix))
            {
                return 0;
            }
            std::int64_t      pid = 0;
            std::size_t       i = prefix.size();
            const std::size_t digits = i;
            while (i < holder.size() && holder[i] >= '0' && holder[i] <= '9')
            {
                pid = pid * 10 + (holder[i] - '0');
                if (pid > 4'000'000'000LL)
                {
                    return 0; // 非 pid 形状：宁可当无信息，也绝不误判
                }
                ++i;
            }
            if (i == digits || i >= holder.size() || holder[i] != ':')
            {
                return 0;
            }
            return pid;
        }

        /**
         * @brief 一次性构建全部预编译语句。
         *
         * 写路径的每个热 SQL 形状一条语句。
         *
         * @internal
         * 本 sqlpp23 pin 的一个上游限制塑造了缓存形态：不存在 `excluded.`
         * 限定符，且参数的名字标签每条语句只能出现一次，因此冲突路径需要
         * 重绑同值的 upsert 被拆开：insert-on-conflict-do-nothing（冲突表现为
         * affected_rows == 0）加一个按键或 id 的 select/refresh。各段形状：
         * - 驻留：新鲜插入报告新 rowid；冲突（DO NOTHING）报告零受影响行
         *   → 回退 select。
         * - 边站点 upsert 与符号 upsert 同样拆分：新鲜行报告新 rowid；冲突
         *   （DO NOTHING）落到按键 select 与 upsert_edge 中的出现次数递增。
         * - 指纹 upsert 与其它 upsert 同拆分形态：新鲜行报告 rowid；冲突落到
         *   按 (file, digest) 对键控的 update。cmdDigest 只在 WHERE 子句绑定
         *   ——名字标签每语句一次，而摘要是键、从不是被改列。
         * - 实时协调（schema v6，多进程队列租约协议）。入队是 INSERT .. DO
         *   NOTHING：同一 path 的并发入队是同一份工作，主键静默去重。
         * - sqlite3 连接器自家的 insert_into 构建的语句带 on_conflict 子句；
         *   core 的不带。
         * 更早的形态曾把边 upsert 作为一条未预编译的
         * INSERT..ON CONFLICT..DO UPDATE..RETURNING 语句运行——上游预编译
         * RETURNING 编不过——它每条边都重新解析 SQL 文本、在语料尺度上主导了
         * store 阶段；现在是与符号相同的预编译拆分形态。
         */
        auto str_and_file_statements(sqlpp::sqlite3::connection& db)
        {
            using sqlpp::delete_from;
            using sqlpp::parameter;
            using sqlpp::select;
            using sqlpp::update;

            const tab::Str   str{};
            const tab::Files f{};

            return std::make_tuple(
                db.prepare(
                    sqlpp::sqlite3::insert_into(str)
                        .set(str.s = parameter(str.s))
                        .on_conflict(str.s)
                        .do_nothing()),
                db.prepare(
                    select(str.id).from(str).where(str.s == parameter(str.s))),
                db.prepare(
                    select(str.s).from(str).where(str.id == parameter(str.id))),
                db.prepare(
                    sqlpp::sqlite3::insert_into(f)
                        .set(f.pathId = parameter(f.pathId))
                        .on_conflict(f.pathId)
                        .do_nothing()),
                db.prepare(select(f.fileId).from(f).where(
                    f.pathId == parameter(f.pathId))),
                db.prepare(select(f.contentHash)
                               .from(f)
                               .where(f.pathId == parameter(f.pathId))),
                db.prepare(update(f)
                               .set(
                                   f.parseStatus = parameter(f.parseStatus),
                                   f.errorCount = parameter(f.errorCount),
                                   f.lastError = parameter(f.lastError),
                                   f.indexedAt = parameter(f.indexedAt))
                               .where(f.fileId == parameter(f.fileId))),
                db.prepare(update(f)
                               .set(f.contentHash = parameter(f.contentHash))
                               .where(f.fileId == parameter(f.fileId))),
                db.prepare(update(f)
                               .set(f.pathId = parameter(f.pathId))
                               .where(f.fileId == parameter(f.fileId))));
        }

        auto symbol_edge_import_statements(sqlpp::sqlite3::connection& db)
        {
            using sqlpp::delete_from;
            using sqlpp::parameter;
            using sqlpp::select;
            using sqlpp::update;

            const tab::Symbols  s{};
            const tab::Edges    e{};
            const tab::Includes inc{};
            const tab::Imports  imp{};

            return std::make_tuple(
                db.prepare(
                    sqlpp::sqlite3::insert_into(s)
                        .set(
                            s.uniqueKeyId = parameter(s.uniqueKeyId),
                            s.kindId = parameter(s.kindId),
                            s.nameId = parameter(s.nameId),
                            s.nsId = parameter(s.nsId),
                            s.sigHash = parameter(s.sigHash),
                            s.fileId = parameter(s.fileId),
                            s.line = parameter(s.line),
                            s.col = parameter(s.col),
                            s.parentId = parameter(s.parentId),
                            s.flags = parameter(s.flags),
                            s.paramTypes = parameter(s.paramTypes),
                            s.templateParams = parameter(s.templateParams))
                        .on_conflict(s.uniqueKeyId)
                        .do_nothing()),
                db.prepare(select(s.symId).from(s).where(
                    s.uniqueKeyId == parameter(s.uniqueKeyId))),
                db.prepare(
                    update(s)
                        .set(
                            s.kindId = parameter(s.kindId),
                            s.nameId = parameter(s.nameId),
                            s.nsId = parameter(s.nsId),
                            s.sigHash = parameter(s.sigHash),
                            s.fileId = parameter(s.fileId),
                            s.line = parameter(s.line),
                            s.col = parameter(s.col),
                            s.parentId = parameter(s.parentId),
                            s.flags = parameter(s.flags),
                            s.paramTypes = parameter(s.paramTypes),
                            s.templateParams = parameter(s.templateParams))
                        .where(s.symId == parameter(s.symId))),
                db.prepare(
                    sqlpp::sqlite3::insert_into(e)
                        .set(
                            e.srcId = parameter(e.srcId),
                            e.dstId = parameter(e.dstId),
                            e.typeId = parameter(e.typeId),
                            e.siteFileId = parameter(e.siteFileId),
                            e.siteLine = parameter(e.siteLine),
                            e.attrs = parameter(e.attrs),
                            e.confidence = parameter(e.confidence),
                            e.occurrences = parameter(e.occurrences))
                        .on_conflict(
                            e.srcId,
                            e.dstId,
                            e.typeId,
                            e.siteFileId,
                            e.siteLine)
                        .do_nothing()),
                db.prepare(select(e.edgeId).from(e).where(
                    e.srcId == parameter(e.srcId)
                    and e.dstId == parameter(e.dstId)
                    and e.typeId == parameter(e.typeId)
                    and e.siteFileId == parameter(e.siteFileId)
                    and e.siteLine == parameter(e.siteLine))),
                db.prepare(update(e)
                               .set(
                                   e.occurrences =
                                       e.occurrences + parameter(e.occurrences))
                               .where(e.edgeId == parameter(e.edgeId))),
                db.prepare(
                    sqlpp::sqlite3::insert_into(inc)
                        .set(
                            inc.fileId = parameter(inc.fileId),
                            inc.targetId = parameter(inc.targetId),
                            inc.line = parameter(inc.line),
                            inc.angled = parameter(inc.angled))
                        .on_conflict(inc.fileId, inc.targetId, inc.line)
                        .do_nothing()),
                db.prepare(
                    sqlpp::sqlite3::insert_into(imp)
                        .set(
                            imp.fileId = parameter(imp.fileId),
                            imp.moduleId = parameter(imp.moduleId),
                            imp.line = parameter(imp.line))
                        .on_conflict(imp.fileId, imp.moduleId, imp.line)
                        .do_nothing()));
        }

        auto purge_fingerprint_failure_statements(
            sqlpp::sqlite3::connection& db)
        {
            using sqlpp::delete_from;
            using sqlpp::parameter;
            using sqlpp::select;
            using sqlpp::update;

            const tab::Edges        e{};
            const tab::Includes     inc{};
            const tab::Imports      imp{};
            const tab::Files        f{};
            const tab::Symbols      s{};
            const tab::Fingerprints fp{};
            const tab::Failures     fl{};

            return std::make_tuple(
                db.prepare(delete_from(e).where(
                    e.siteFileId == parameter(e.siteFileId))),
                db.prepare(delete_from(inc).where(
                    inc.fileId == parameter(inc.fileId))),
                db.prepare(delete_from(imp).where(
                    imp.fileId == parameter(imp.fileId))),
                db.prepare(
                    delete_from(f).where(f.fileId == parameter(f.fileId))),
                db.prepare(select(s.uniqueKeyId)
                               .from(s)
                               .where(s.fileId == parameter(s.fileId))),
                db.prepare(
                    sqlpp::select(sqlpp::all_of(s))
                        .from(s)
                        .where(s.symId == parameter(s.symId))),
                db.prepare(
                    sqlpp::select(sqlpp::all_of(f))
                        .from(f)
                        .where(f.fileId == parameter(f.fileId))),
                db.prepare(
                    sqlpp::sqlite3::insert_into(fp)
                        .set(
                            fp.fileId = parameter(fp.fileId),
                            fp.mtimeNs = parameter(fp.mtimeNs),
                            fp.sizeBytes = parameter(fp.sizeBytes),
                            fp.cmdDigest = parameter(fp.cmdDigest))
                        .on_conflict(fp.fileId, fp.cmdDigest)
                        .do_nothing()),
                db.prepare(
                    update(fp)
                        .set(
                            fp.mtimeNs = parameter(fp.mtimeNs),
                            fp.sizeBytes = parameter(fp.sizeBytes))
                        .where(
                            fp.fileId == parameter(fp.fileId)
                            and fp.cmdDigest == parameter(fp.cmdDigest))),
                db.prepare(delete_from(fp).where(
                    fp.fileId == parameter(fp.fileId)
                    and fp.cmdDigest == parameter(fp.cmdDigest))),
                db.prepare(
                    sqlpp::sqlite3::insert_into(fl)
                        .set(
                            fl.fileId = parameter(fl.fileId),
                            fl.cmdDigest = parameter(fl.cmdDigest),
                            fl.contentHash = parameter(fl.contentHash),
                            fl.mtimeNs = parameter(fl.mtimeNs),
                            fl.sizeBytes = parameter(fl.sizeBytes),
                            fl.engineToken = parameter(fl.engineToken),
                            fl.feedDigest = parameter(fl.feedDigest),
                            fl.rootsDigest = parameter(fl.rootsDigest),
                            fl.failureClass = parameter(fl.failureClass),
                            fl.failureText = parameter(fl.failureText),
                            fl.depsJson = parameter(fl.depsJson),
                            fl.attempts = parameter(fl.attempts),
                            fl.firstFailedAt = parameter(fl.firstFailedAt),
                            fl.lastFailedAt = parameter(fl.lastFailedAt),
                            fl.retryAfter = parameter(fl.retryAfter))
                        .on_conflict(fl.fileId, fl.cmdDigest)
                        .do_nothing()),
                db.prepare(
                    update(fl)
                        .set(
                            fl.contentHash = parameter(fl.contentHash),
                            fl.mtimeNs = parameter(fl.mtimeNs),
                            fl.sizeBytes = parameter(fl.sizeBytes),
                            fl.engineToken = parameter(fl.engineToken),
                            fl.feedDigest = parameter(fl.feedDigest),
                            fl.rootsDigest = parameter(fl.rootsDigest),
                            fl.failureClass = parameter(fl.failureClass),
                            fl.failureText = parameter(fl.failureText),
                            fl.depsJson = parameter(fl.depsJson),
                            fl.attempts = parameter(fl.attempts),
                            fl.firstFailedAt = parameter(fl.firstFailedAt),
                            fl.lastFailedAt = parameter(fl.lastFailedAt),
                            fl.retryAfter = parameter(fl.retryAfter))
                        .where(
                            fl.fileId == parameter(fl.fileId)
                            and fl.cmdDigest == parameter(fl.cmdDigest))),
                db.prepare(delete_from(fl).where(
                    fl.fileId == parameter(fl.fileId)
                    and fl.cmdDigest == parameter(fl.cmdDigest))));
        }

        auto lease_site_statements(sqlpp::sqlite3::connection& db)
        {
            using sqlpp::delete_from;
            using sqlpp::parameter;
            using sqlpp::select;
            using sqlpp::update;

            const tab::PendingUpdates pu{};
            const tab::UpdateClaim    c{};
            const tab::TypeHints      th{};
            const tab::CallSites      cs{};
            const tab::Edges          e{};

            return std::make_tuple(
                db.prepare(
                    sqlpp::sqlite3::insert_into(pu)
                        .set(pu.path = parameter(pu.path))
                        .on_conflict(pu.path)
                        .do_nothing()),
                db.prepare(
                    sqlpp::select(pu.path).from(pu).order_by(pu.path.asc())),
                db.prepare(
                    delete_from(pu).where(pu.path == parameter(pu.path))),
                db.prepare(select(c.holder, c.leaseUntil)
                               .from(c)
                               .where(c.id == parameter(c.id))),
                db.prepare(update(c)
                               .set(c.leaseUntil = parameter(c.leaseUntil))
                               .where(
                                   c.id == parameter(c.id)
                                   and c.holder == parameter(c.holder))),
                db.prepare(delete_from(c).where(
                    c.id == parameter(c.id)
                    and c.holder == parameter(c.holder))),
                db.prepare(
                    sqlpp::sqlite3::insert_into(th)
                        .set(
                            th.ownerId = parameter(th.ownerId),
                            th.varName = parameter(th.varName),
                            th.fileId = parameter(th.fileId),
                            th.line = parameter(th.line),
                            th.col = parameter(th.col),
                            th.typeSet = parameter(th.typeSet),
                            th.source = parameter(th.source),
                            th.confidence = parameter(th.confidence),
                            th.fallbackReason = parameter(th.fallbackReason),
                            th.paramIndex = parameter(th.paramIndex),
                            th.relatedVar = parameter(th.relatedVar),
                            th.relatedKey = parameter(th.relatedKey))
                        .on_conflict(
                            th.ownerId,
                            th.fileId,
                            th.line,
                            th.col,
                            th.varName,
                            th.source)
                        .do_nothing()),
                db.prepare(
                    delete_from(th).where(th.fileId == parameter(th.fileId))),
                db.prepare(
                    sqlpp::sqlite3::insert_into(cs)
                        .set(
                            cs.callerId = parameter(cs.callerId),
                            cs.calleeId = parameter(cs.calleeId),
                            cs.calleeKey = parameter(cs.calleeKey),
                            cs.calleeName = parameter(cs.calleeName),
                            cs.calleeNs = parameter(cs.calleeNs),
                            cs.calleeParent = parameter(cs.calleeParent),
                            cs.calleeSig = parameter(cs.calleeSig),
                            cs.calleeConst = parameter(cs.calleeConst),
                            cs.methodId = parameter(cs.methodId),
                            cs.methodKey = parameter(cs.methodKey),
                            cs.isVirtual = parameter(cs.isVirtual),
                            cs.qualified = parameter(cs.qualified),
                            cs.indirect = parameter(cs.indirect),
                            cs.receiverName = parameter(cs.receiverName),
                            cs.receiverIsThis = parameter(cs.receiverIsThis),
                            cs.receiverOwnerKey =
                                parameter(cs.receiverOwnerKey),
                            cs.staticTypeId = parameter(cs.staticTypeId),
                            cs.fileId = parameter(cs.fileId),
                            cs.line = parameter(cs.line),
                            cs.col = parameter(cs.col),
                            cs.argTypes = parameter(cs.argTypes),
                            cs.argVars = parameter(cs.argVars),
                            cs.argCallKeys = parameter(cs.argCallKeys),
                            cs.argTargets = parameter(cs.argTargets),
                            cs.receiverTypes = parameter(cs.receiverTypes),
                            cs.receiverStatic = parameter(cs.receiverStatic),
                            cs.calleeResolved = parameter(cs.calleeResolved),
                            cs.resolved = parameter(cs.resolved),
                            cs.confidence = parameter(cs.confidence),
                            cs.candidates = parameter(cs.candidates),
                            cs.fallbackReason = parameter(cs.fallbackReason),
                            cs.calleeUsr = parameter(cs.calleeUsr),
                            cs.calleeExpr = parameter(cs.calleeExpr),
                            cs.calleeParam = parameter(cs.calleeParam))
                        .on_conflict(
                            cs.callerId,
                            cs.fileId,
                            cs.line,
                            cs.col,
                            cs.calleeKey,
                            cs.isVirtual,
                            cs.indirect)
                        .do_nothing()),
                db.prepare(select(cs.siteId).from(cs).where(
                    cs.callerId == parameter(cs.callerId)
                    and cs.fileId == parameter(cs.fileId)
                    and cs.line == parameter(cs.line)
                    and cs.col == parameter(cs.col)
                    and cs.calleeKey == parameter(cs.calleeKey)
                    and cs.isVirtual == parameter(cs.isVirtual)
                    and cs.indirect == parameter(cs.indirect))),
                db.prepare(
                    delete_from(cs).where(cs.fileId == parameter(cs.fileId))),
                db.prepare(
                    update(cs)
                        .set(
                            cs.calleeId = parameter(cs.calleeId),
                            cs.methodId = parameter(cs.methodId),
                            cs.methodKey = parameter(cs.methodKey),
                            cs.staticTypeId = parameter(cs.staticTypeId),
                            cs.receiverTypes = parameter(cs.receiverTypes),
                            cs.calleeResolved = parameter(cs.calleeResolved),
                            cs.resolved = parameter(cs.resolved),
                            cs.confidence = parameter(cs.confidence),
                            cs.candidates = parameter(cs.candidates),
                            cs.fallbackReason = parameter(cs.fallbackReason))
                        .where(cs.siteId == parameter(cs.siteId))),
                db.prepare(
                    delete_from(e).where(e.typeId == parameter(e.typeId))),
                db.prepare(delete_from(e).where(
                    e.typeId == parameter(e.typeId)
                    and e.siteFileId == parameter(e.siteFileId))),
                db.prepare(delete_from(e).where(
                    e.typeId == parameter(e.typeId)
                    and e.srcId == parameter(e.srcId)
                    and e.siteFileId == parameter(e.siteFileId)
                    and e.siteLine == parameter(e.siteLine))));
        }

        auto payload_slot_statements(sqlpp::sqlite3::connection& db)
        {
            using sqlpp::delete_from;
            using sqlpp::parameter;
            using sqlpp::select;
            using sqlpp::update;

            const tab::AnalysisSlots       asl{};
            const tab::PassthroughWrappers ptw{};
            const tab::ExternalParamArgs   epa{};
            const tab::ExternalInvokes     evi{};

            return std::make_tuple(
                db.prepare(
                    sqlpp::sqlite3::insert_into(asl)
                        .set(
                            asl.ownerSym = parameter(asl.ownerSym),
                            asl.varName = parameter(asl.varName),
                            asl.typeSet = parameter(asl.typeSet),
                            asl.confidence = parameter(asl.confidence),
                            asl.source = parameter(asl.source),
                            asl.reason = parameter(asl.reason),
                            asl.truncated = parameter(asl.truncated),
                            asl.ver = parameter(asl.ver),
                            asl.aliasSrc = parameter(asl.aliasSrc),
                            asl.retSrc = parameter(asl.retSrc))
                        .on_conflict(asl.ownerSym, asl.varName)
                        .do_nothing()),
                db.prepare(update(asl)
                               .set(
                                   asl.typeSet = parameter(asl.typeSet),
                                   asl.confidence = parameter(asl.confidence),
                                   asl.source = parameter(asl.source),
                                   asl.reason = parameter(asl.reason),
                                   asl.truncated = parameter(asl.truncated),
                                   asl.ver = parameter(asl.ver),
                                   asl.aliasSrc = parameter(asl.aliasSrc),
                                   asl.retSrc = parameter(asl.retSrc))
                               .where(
                                   asl.ownerSym == parameter(asl.ownerSym)
                                   and asl.varName == parameter(asl.varName))),
                db.prepare(
                    sqlpp::select(sqlpp::all_of(asl))
                        .from(asl)
                        .order_by(asl.ownerSym.asc(), asl.varName.asc())),
                db.prepare(delete_from(asl)),
                db.prepare(delete_from(asl).where(
                    asl.ownerSym == parameter(asl.ownerSym)
                    and asl.varName == parameter(asl.varName))),
                db.prepare(
                    sqlpp::sqlite3::insert_into(ptw)
                        .set(
                            ptw.wrapperKey = parameter(ptw.wrapperKey),
                            ptw.getterKey = parameter(ptw.getterKey),
                            ptw.getterName = parameter(ptw.getterName),
                            ptw.fileId = parameter(ptw.fileId),
                            ptw.line = parameter(ptw.line),
                            ptw.argIndex = parameter(ptw.argIndex))
                        .on_conflict(ptw.wrapperKey, ptw.getterKey)
                        .do_nothing()),
                db.prepare(
                    sqlpp::select(sqlpp::all_of(ptw))
                        .from(ptw)
                        .order_by(ptw.wrapperKey.asc(), ptw.getterKey.asc())),
                db.prepare(
                    sqlpp::sqlite3::insert_into(epa)
                        .set(
                            epa.hostUsr = parameter(epa.hostUsr),
                            epa.paramIndex = parameter(epa.paramIndex),
                            epa.callerKey = parameter(epa.callerKey),
                            epa.targetKey = parameter(epa.targetKey),
                            epa.fileId = parameter(epa.fileId),
                            epa.line = parameter(epa.line),
                            epa.col = parameter(epa.col))
                        .on_conflict(
                            epa.hostUsr,
                            epa.paramIndex,
                            epa.fileId,
                            epa.line,
                            epa.col)
                        .do_nothing()),
                db.prepare(
                    sqlpp::select(sqlpp::all_of(epa))
                        .from(epa)
                        .order_by(
                            epa.hostUsr.asc(),
                            epa.paramIndex.asc(),
                            epa.fileId.asc(),
                            epa.line.asc())),
                db.prepare(
                    sqlpp::sqlite3::insert_into(evi)
                        .set(
                            evi.hostUsr = parameter(evi.hostUsr),
                            evi.paramIndex = parameter(evi.paramIndex),
                            evi.fileId = parameter(evi.fileId),
                            evi.line = parameter(evi.line),
                            evi.col = parameter(evi.col))
                        .on_conflict(
                            evi.hostUsr,
                            evi.paramIndex,
                            evi.fileId,
                            evi.line,
                            evi.col)
                        .do_nothing()),
                db.prepare(
                    sqlpp::select(sqlpp::all_of(evi))
                        .from(evi)
                        .order_by(
                            evi.hostUsr.asc(),
                            evi.paramIndex.asc(),
                            evi.fileId.asc(),
                            evi.line.asc())));
        }

        auto aggregate_contrib_statements(sqlpp::sqlite3::connection& db)
        {
            using sqlpp::delete_from;
            using sqlpp::parameter;
            using sqlpp::select;
            using sqlpp::update;

            const tab::Edges               e{};
            const tab::Symbols             s{};
            const tab::SymbolContributions sc{};

            // 读侧聚合：类型过滤不上 SQL——(dst,type) 分组各自独立
            // 聚合，调用方按类型集丢组与 WHERE 过滤逐位等价，语句形状保持
            // 单参数（预编译表只收固定形状）。
            return std::make_tuple(
                db.prepare(
                    sqlpp::select(
                        e.dstId,
                        e.typeId,
                        sqlpp::sum(e.occurrences).as(tab::occ_sum),
                        sqlpp::min(e.siteLine).as(tab::site_min),
                        sqlpp::max(e.confidence).as(tab::max_conf),
                        sqlpp::max(e.attrs).as(tab::max_attrs))
                        .from(e)
                        .where(e.srcId == parameter(e.srcId))
                        .group_by(e.dstId, e.typeId)),
                db.prepare(
                    sqlpp::select(
                        e.srcId,
                        e.typeId,
                        sqlpp::sum(e.occurrences).as(tab::occ_sum),
                        sqlpp::min(e.siteLine).as(tab::site_min),
                        sqlpp::max(e.confidence).as(tab::max_conf),
                        sqlpp::max(e.attrs).as(tab::max_attrs))
                        .from(e)
                        .where(e.dstId == parameter(e.dstId))
                        .group_by(e.srcId, e.typeId)),
                db.prepare(
                    sqlpp::select(sqlpp::all_of(e))
                        .from(e)
                        .where(
                            e.srcId == parameter(e.srcId)
                            and e.typeId == parameter(e.typeId))),
                db.prepare(
                    sqlpp::select(sqlpp::all_of(e))
                        .from(e)
                        .where(
                            e.dstId == parameter(e.dstId)
                            and e.typeId == parameter(e.typeId))),
                db.prepare(delete_from(s).where(s.symId == parameter(s.symId))),

                // 贡献登记（schema v18 行生老病死记账制）：投票幂等、
                // 撤票按全键、条目/文件整撤配套旧票读取（差集撤票的
                // 输入面）、归零仲裁的 EXISTS 探测走 PK 前缀。
                db.prepare(
                    sqlpp::sqlite3::insert_into(sc)
                        .set(
                            sc.symId = parameter(sc.symId),
                            sc.fileId = parameter(sc.fileId),
                            sc.cmdDigest = parameter(sc.cmdDigest))
                        .on_conflict(sc.symId, sc.fileId, sc.cmdDigest)
                        .do_nothing()),
                db.prepare(
                    sqlpp::select(sc.symId)
                        .from(sc)
                        .where(
                            sc.fileId == parameter(sc.fileId)
                            and sc.cmdDigest == parameter(sc.cmdDigest))
                        .order_by(sc.symId.asc())),
                db.prepare(
                    sqlpp::select(sc.symId)
                        .from(sc)
                        .where(sc.fileId == parameter(sc.fileId))
                        .order_by(sc.symId.asc())),
                db.prepare(
                    sqlpp::delete_from(sc).where(
                        sc.symId == parameter(sc.symId)
                        and sc.fileId == parameter(sc.fileId)
                        and sc.cmdDigest == parameter(sc.cmdDigest))),
                db.prepare(
                    sqlpp::delete_from(sc).where(
                        sc.fileId == parameter(sc.fileId)
                        and sc.cmdDigest == parameter(sc.cmdDigest))),
                db.prepare(
                    sqlpp::delete_from(sc).where(
                        sc.fileId == parameter(sc.fileId))),
                db.prepare(
                    sqlpp::select(sc.symId)
                        .from(sc)
                        .where(sc.symId == parameter(sc.symId))
                        .limit(1)));
        }

        auto make_statements(sqlpp::sqlite3::connection& db)
        {
            return std::tuple_cat(
                str_and_file_statements(db),
                symbol_edge_import_statements(db),
                purge_fingerprint_failure_statements(db),
                lease_site_statements(db),
                payload_slot_statements(db),
                aggregate_contrib_statements(db));
        }

        using Statements = decltype(make_statements(
            std::declval<sqlpp::sqlite3::connection&>()));

        /** @brief 语句枚举与语句元组同长（增删语句两侧必须同步的护栏）。 */
        static_assert(
            std::tuple_size_v<Statements> == static_cast<std::size_t>(COUNT));

        auto make_config(std::string_view path)
            -> std::shared_ptr<sqlpp::sqlite3::connection_config>
        {
            auto cfg = std::make_shared<sqlpp::sqlite3::connection_config>();
            cfg->path_to_database = std::string{path};
            cfg->flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
            // 性能排查钩子：CCG_SQL_TRACE 置位时把语句活动记到 stderr——动态
            // 语句记 Preparing（每次执行一条）、预编译语句记 resetting（每次
            // 执行一条），两者之和即 SQLite 往返数；默认关闭，零行为影响。
            if (cg::util::cstd::get_env("CCG_SQL_TRACE").has_value())
            {
                cfg->debug = sqlpp::debug_logger(
                    {sqlpp::log_category::statement},
                    [](sqlpp::log_category, const std::string& msg)
                    { std::println(std::cerr, "SQL {}", msg); });
            }
            return cfg;
        }

    } // namespace

    /**
     * @brief 图存储门面：进程内数据库连接的唯一拥有者。
     *
     * @internal
     * 全店不变式：sqlpp 的结果包装器从不重置其缓存的语句，因此每个结果
     * 循环都必须跑到完成（步进到 DONE）——DONE 释放隐式读事务；中途弃置
     * 的游标会把它挂着，而一旦任何其它连接提交，本连接后续的每个
     * autocommit 写都会立刻以 BUSY_SNAPSHOT 失败（busy_timeout 熬不过
     * 过期快照）。绝不在结果循环内部 return。
     */
    export class GraphStore
    {
    public:
        /**
         * @brief 打开（必要时创建）`path` 处的数据库（":memory:" 可用），
         *        武装忙超时与 PRAGMA，在全新文件上建 schema，并拒绝不兼容
         *        的 schema_version。
         *
         * @internal
         * 忙超时先于任何触碰文件的操作：WAL pragma 与引导 DDL 也是
         * 写，它们必须排在并发写者（实时协调、引擎批处理）后面，而不是
         * 以未等待的 "database is locked" 失败。connection_config 没有超时
         * 字段，该 PRAGMA 是每条连接唯一的武装点。
         */
        explicit GraphStore(std::string_view path);
        /**
         * @brief 同上，但把忙超时（毫秒）作为连接上的**第一条**语句武装。
         *
         * @internal
         * 钩子的快速路径必须在一个仍持有写锁的批次前进场：默认 30 秒的忙
         * 超时会让一次探测/入队把整个工具事件拖住。专用重载让调用方把等待
         * 压到百毫秒级，失败即走旁路（journal），绝不阻塞会话。
         */
        explicit GraphStore(
            std::string_view path,
            std::int64_t     busy_timeout_ms);
        /**
         * @internal
         * 用户提供的空函数体，刻意不是 `= default`：MSVC 的模块重发射路径
         * 会从 BMI 把类外 defaulted 成员重新综合成 COMDAT，而本模块 TU
         * 仍持有强定义 → LNK2005（已探针验证，MSVC 移植注记）。此处语义
         * 等价：该类既非平凡可析构、也不经析构函数多态。
         */
        ~GraphStore();
        GraphStore(const GraphStore&) = delete;
        GraphStore& operator=(const GraphStore&) = delete;
        GraphStore(GraphStore&&) = delete;
        GraphStore& operator=(GraphStore&&) = delete;

        /**
         * @brief 返回 `s` 的 str id，首次见到时存入。重复调用由内存缓存
         *        供答。
         */
        auto intern(std::string_view s) -> StrId;
        [[nodiscard]]
        auto find_str(std::string_view s) -> std::optional<StrId>;
        auto str_of(StrId id) -> std::string;

        /** @brief 对规范化路径文本幂等；新行以 pending 状态开始。 */
        auto upsert_file(std::string_view normalized_path) -> FileId;
        [[nodiscard]]
        auto file(FileId id) -> std::optional<FileRow>;
        /**
         * @brief FileId -> 规范化路径文本的缓存版查询（轻量投影配套）。
         *
         * @internal
         * 与 file() 的差别只在正向路径缓存：symbol_row 与批量投影每次物化
         * 都要路径文本，逐符号回查 files/str 是纯放大。只缓存命中（路径
         * 文本）；行不存在时不缓存、返回空。失效时机（细节写明）：
         * - erase_file：整缓存清空（行已删，任何残留都是悬空映射）；
         * - set_file_path：整缓存清空（该 id 的路径文本已改写）；
         * - upsert_file 不失效：它只按路径文本幂等新增行，新 id 不可能在
         *   缓存里，既有 id 的路径文本也不变；
         * - hash/status 等其余写不触碰路径列，无需失效。
         * 缓存挂在连接生命周期上（每 GraphStore 一份）。
         */
        [[nodiscard]]
        auto file_path(FileId id) -> std::optional<std::string>;
        /** @brief 按规范化路径文本查找而不创建行（文件从未被索引过时为
         * nullopt）。 */
        [[nodiscard]]
        auto find_file(std::string_view normalized_path)
            -> std::optional<FileRow>;
        void set_file_hash(FileId id, std::string_view sha256_hex);
        /**
         * @brief 把 files.content_hash 重置为 NULL。
         *
         * @internal
         * 之前已索引、后从磁盘消失的 main 保留其行（状态 Failed），但不得
         * 保留已不存在字节的哈希：同一棵树的全量重建从不写这种哈希（缺失
         * 的 main 从不被解析），且下一次运行的 stat 确认不得与已消失的
         * 内容比较。
         */
        void clear_file_hash(FileId id);
        /**
         * @brief 重写一条 files 行的路径文本（大小写不敏感文件系统上的
         *        大小写漂移）。
         *
         * @internal 行保留其 id，因此每个依赖方（符号、边站点、includes、
         * 指纹）都继续指向它。旧路径文本可以像任何退役键一样滞留在 str
         * 里（非语义）。
         */
        void set_file_path(FileId id, std::string_view normalized_path);
        void set_file_status(
            FileId                          id,
            cg::util::ParseStatus           status,
            int                             error_count,
            std::optional<std::string_view> last_error,
            std::optional<std::string_view> indexed_at);

        /**
         * @brief 以 unique_key 键控的幂等 upsert：同一
         *        unique_key 插入两次只保留一行并返回同一 sym_id；位置/旗标
         *        列从新抽取刷新。
         */
        auto upsert_symbol(const SymbolWrite& w) -> SymId;

        /**
         * @brief 符号行是否仍在库中（主键点查，不做行物化）。
         *
         * 提交段派生数据发射（类型线索/调用点）的 owner 符号在解析期
         * 解析、在逐文件 purge 之后才落库——两时点之间符号行可能被
         * purge 判死删除（源不再发射该符号，或全败清除误伤）；行缺席
         * 时按"unresolved dropped"同语义丢弃该派生行，与边发射的
         * known_symbol_ids 预检同一防线语义（实测事故先例的外键防线补全）。
         */
        [[nodiscard]]
        auto symbol_exists(SymId id) -> bool;

        /**
         * @brief 诊断：本连接上仍处步进中（已 step、既未步到完成也未
         *        reset）的语句 SQL 清单，分号分隔。
         *
         * @internal 步进中的语句把隐式读事务（其 WAL 读快照）挂在连接
         * 上；此后任何其它连接提交，本连接的写升级（BEGIN IMMEDIATE /
         * autocommit 写）都会以忙处理器不介入的 BUSY_SNAPSHOT 瞬时失
         * 败。提交段拿写锁遇到这类瞬时 busy 时，把这份清单放进错误信息
         * 可以直接点名是哪条查询的游标被中途弃置——定位残留读快照的第
         * 一现场。空串 = 连接上无步进中语句。
         */
        [[nodiscard]]
        auto busy_statements_diag() -> std::string;

        /**
         * @brief 每个 (src, dst, type, site_file, site_line) 一行；重见同一
         *        站点递增 occurrences 并返回同一 edge_id。
         */
        auto upsert_edge(const EdgeWrite& w) -> EdgeId;

        /**
         * @brief 对 (file, target, line) 幂等；无出现计数器。
         */
        void upsert_include(const IncludeWrite& w);

        /**
         * @brief 对 (file, module, line) 幂等：模块翻译单元中每条书写出的
         *        import 指令一行。
         */
        void upsert_import(const ImportWrite& w);

        /**
         * @brief 对每个 (file, compile command) 对幂等：写下一次索引运行为
         *        该条目对照的 stat 身份。非 main 文件以 '' 作摘要。
         */
        void set_fingerprint(const FingerprintRow& fp);

        /**
         * @brief 丢弃一条指纹行（退役的命令摘要）。
         *
         * @internal
         * 同一组编译命令的全量重建数据库只持有各条目当前的摘要，因此增量
         * 运行必须抹掉摘要不再出现的行——命令变了、条目删了、或文件不再
         * 是翻译单元 main。
         */
        void erase_fingerprint(FileId file, std::string_view cmd_digest);

        /**
         * @brief 写入/刷新一条 (file, cmd_digest) 失败记录（schema v7）。
         *
         * @internal
         * 失败指纹是"该条目上次失败时的世界状态"：主文件内容、argv 摘要、
         * 引擎版本、模块 feed 代际、抽取根集与依赖闭包。下一次运行只在该
         * 状态仍成立时跳过重试。冲突路径拆成 insert-do-nothing + 按键
         * update（本 sqlpp23 pin 不支持 excluded.，且参数名字标签每语句
         * 只能出现一次）。级联：files 行删除时由外键带走。
         */
        void set_failure(const FailureRow& failure);
        /** @brief 删除一条失败记录（成功解析、或命令摘要退役）。 */
        void erase_failure(FileId file, std::string_view cmd_digest);

        /**
         * @brief 移除该文件的站点属主边、includes/imports 与派生数据行
         *        （线索/调用点）——它们随本批 upsert 重写。
         *
         * @internal schema v18 起不再删除该文件上的符号行：键失配只是
         *           “本批新提取不再发射它”的事实，行生死由贡献票数仲裁
         *           （symbol_contributions 归零时在 dispatch 装载前删除）。
         *           删除顺序纪律（并发安全论证的一环）不变：本方法仍是
         *           “先删引用方”的单点，erase_file 删 files 行永远排在
         *           本方法之后。
         */
        void purge_file(FileId file);

        /**
         * @brief 按行 id 删除符号（终态确证真死的键行；引用方随外键
         *        级联消失——edges/call_sites/type_hints CASCADE、
         *        callee/method/static_type SET NULL、子成员 CASCADE）。
         *
         * @internal 行清除面（重写集逐文件 purge）够不到挂在未编辑
         *           文件上的死键（模板实例位：注册点在被删的使用处、
         *           实体挂在模板定义文件）——本入口是其显式删除半边；
         *           行不存在时是幂等空操作。
         */
        void erase_symbols(std::span<const SymId> ids);

        /**
         * @name 贡献登记（schema v18，行生老病死记账制）。
         *
         * @internal 一票 = (sym_id, file_id, cmd_digest) 一行，条目身份与
         *           fingerprints/failures 同款。调用时序纪律：全部投票与
         *           撤票都发生在提交段同一事务内（崩溃一致性由事务回滚
         *           保证）；归零仲裁（zero_contribution_symbols）必须最
         *           后跑，其结果在 dispatch 装载前经 erase_symbols 删除。
         */
        ///@{
        /**
         * @brief 投票（幂等）：本条目对 `ids` 里每个符号各投一票。
         *
         * @param digest 条目命令摘要（调用方持有 freshness 扫描算得的那
         *               份，与 fingerprints/failures 的键逐字节一致）。
         */
        void contribute_symbols(
            FileId                 file,
            std::string_view       digest,
            std::span<const SymId> ids);

        /**
         * @brief 条目旧票读取（差集撤票的输入）：该 (file, digest) 当前
         *        投过票的全部符号，sym_id 升序。
         */
        [[nodiscard]]
        auto contributions_of_entry(FileId file, std::string_view digest)
            -> std::vector<SymId>;

        /** @brief 撤一票（全键；行不存在时是幂等空操作）。 */
        void retract_contribution(
            SymId            sym,
            FileId           file,
            std::string_view digest);

        /**
         * @brief 整条目撤票（条目退役），返回受影响的符号集（归零仲裁
         *        候选）。
         */
        auto retract_entry_contributions(FileId file, std::string_view digest)
            -> std::vector<SymId>;

        /**
         * @brief 整文件撤票（全败清除/盘缺/抹除），返回受影响的符号集。
         */
        auto retract_file_contributions(FileId file) -> std::vector<SymId>;

        /**
         * @brief 条目旧票读取的整文件形态（只读不删；heal 折叠等"删除
         *        交给外键、受影响集交给调用方"的场景）。
         */
        [[nodiscard]]
        auto contributions_of_file(FileId file) -> std::vector<SymId>;

        /**
         * @brief 归零仲裁：candidates 里已无任何剩余票的符号（PK 前缀
         *        EXISTS 探测），保持输入序。
         *
         * @internal 调用方保证 candidates 恰是本批全部撤票动作触及的
         *           符号——行从创建起至少有一票（发射即投票），“从有票
         *           变零票”必经一次撤票，因此归零集是撤票集的子集。
         */
        [[nodiscard]]
        auto zero_contribution_symbols(std::span<const SymId> candidates)
            -> std::vector<SymId>;
        ///@}

        /**
         * @brief 写入一条类型线索（schema v10 派生数据；冲突静默去重）。
         */
        void upsert_type_hint(const TypeHintWrite& w);
        /** @brief 整表读取类型线索（全局传播固定点输入），确定性顺序。 */
        [[nodiscard]]
        auto all_type_hints() -> std::vector<TypeHintRow>;
        /**
         * @brief 传播装载的列裁剪读取（all_type_hints 的投影形）。
         *
         * @internal 固定点装载不消费行 id（槽以 owner+var 寻址、行身份由
         *           装载序隐含），裁掉 hintId 列减一行一列的反序列化。
         *           未选字段保持默认值；顺序与全表读逐字节一致。
         */
        [[nodiscard]]
        auto load_type_hints_analysis() -> std::vector<TypeHintRow>;
        /**
         * @brief load_type_hints_analysis 的按文件投影（快照恢复的脏文件
         *        delta 专用）。
         *
         * @internal 只读 file_id 命中给定集合的行，列裁剪与排序和全表读
         *           逐字节一致（同一 order_by），空集返回空——调用方据此
         *           走全量或纯快照路径。
         */
        [[nodiscard]]
        auto load_type_hints_analysis_files(const std::vector<FileId>& files)
            -> std::vector<TypeHintRow>;
        /**
         * @brief load_type_hints_analysis 的按来源投影（source == 给定值
         *        的全行集）。
         *
         * @internal 快照增量路径给保留槽回灌转型声称缓冲用：声称行只占
         *           hint 总量的极小子集（WHERE 走表扫但免整表物化），列裁
         *           剪与排序和全表读逐字节一致（同一 order_by）；文件级
         *           过滤（重读集排除）由调用方做——集是内存小集。
         */
        [[nodiscard]]
        auto load_type_hints_of_source(std::int64_t source)
            -> std::vector<TypeHintRow>;

        /**
         * @brief 单属主的类型线索读取（查询路径的属主点查）。
         *
         * @internal WHERE ownerId 走 type_hints_owner_var 复合索引前缀，
         *           不做列裁剪（行含 id），排序与 all_type_hints 的确定性
         *           order_by 一致。全表需求（分析装载、跨属主摘要）仍走
         *           all_type_hints 系。
         */
        [[nodiscard]]
        auto type_hints_of_owner(SymId owner) -> std::vector<TypeHintRow>;
        /**
         * @brief 指定来源+槽名的类型线索读取（WHERE 投影，全表读的过滤形）。
         *
         * @internal 供 dispatch 的 external_of 装载直取 R(I) 边界行：与
         *           all_type_hints 后内存过滤 source/var_name 的旧行集逐行
         *           相等（同列、同一 order_by，WHERE 只删行不改序不换列）。
         *           (source, varName) 无专用索引时引擎仍走表扫，但免整表
         *           物化与逐行反序列化。
         */
        [[nodiscard]]
        auto type_hints_of_source(
            std::int64_t     source,
            std::string_view var_name) -> std::vector<TypeHintRow>;

        /**
         * @brief 写入一条调用点并返回 site_id；同一唯一键重复写返回既有 id
         *        （本批与历史行的身份一致）。
         */
        auto upsert_call_site(const CallSiteWrite& w) -> std::int64_t;
        /** @brief 整表读取调用点（传播与 dispatch 输入），确定性顺序。 */
        [[nodiscard]]
        auto all_call_sites() -> std::vector<CallSiteRow>;
        /**
         * @brief 传播装载的列裁剪读取（all_call_sites 的投影形）。
         *
         * @internal 固定点装载与写回比对不消费 callee_usr/callee_expr/
         *           callee_param/arg_targets（间接消费走专用投影查询
         *           indirect_site_rows/arg_target_rows，不经本行）——裁掉
         *           四列。未选字段保持默认值；顺序与全表读逐字节一致。
         */
        [[nodiscard]]
        auto load_call_sites_analysis() -> std::vector<CallSiteRow>;
        /** @brief 回填一个调用点的解析结果（传播/dispatch 之后）。 */
        void refresh_call_site(const CallSiteRow& row);
        /** @brief 删除某一类型的全部边（dispatch、indirect_calls
         * 边全局重算前调用）。 */
        void erase_edges_of_type(EdgeType type);
        /**
         * @brief 删除某文件站点上的全部 virtual_dispatch 边（编辑路径：
         *      延迟收敛批只按文件粒度撤旧派发边，绝不整类清空）。
         *
         * @internal edges_own(site_file_id) 服务本形状的文件过滤；与
         * purge_file 的全类型站点边删除互补——这里只动
         * type_id=7，供派发数据自身的失效纪律单独调用。
         */
        void erase_vd_edges_of_file(FileId file);
        /**
         * @brief 删除一个站点三元组 (caller, site_file, site_line) 上的全部
         *        virtual_dispatch 边（站点粒度边 diff 的删除半边）。
         *
         * @internal 同一行多个调用点（同 caller 同文件同行不同列）共享三元
         *           组：调用方必须把受影响集合按三元组扩张后一起重解析。
         */
        void erase_vd_edges_of_site(
            SymId  caller,
            FileId site_file,
            int    site_line);

        /**
         * @brief 写入/刷新一行传播槽终态（schema v12）。
         *
         * @internal 冲突路径拆分形态（本 sqlpp23 pin 无 excluded.）：insert
         *           do nothing 命中冲突时落到按键 update 全列。
         */
        void upsert_analysis_slot(const AnalysisSlotWrite& w);
        /** @brief 整表读取槽终态（确定序：owner_sym, var_name）。 */
        [[nodiscard]]
        auto all_analysis_slots() -> std::vector<AnalysisSlotRow>;

        /**
         * @brief 透传包装登记 upsert（schema v13）：per-file 重写语义靠
         *        file_id 外键的 purge 级联（与 type_hints 同款）。
         */
        void upsert_passthrough_wrapper(const PassthroughWrapperWrite& w);
        /** @brief 整表读取透传包装登记（确定序：wrapper_key, getter_key）。 */
        [[nodiscard]]
        auto all_passthrough_wrappers() -> std::vector<PassthroughWrapperRow>;
        /** @brief 实例桥传入侧 upsert（schema v15，冲突 do nothing 幂等）。 */
        void upsert_external_param_arg(const ExternalParamArgWrite& w);
        /** @brief 整表读取实例桥传入侧（确定序：host_usr, param_index, file,
         * line）。
         */
        [[nodiscard]]
        auto all_external_param_args() -> std::vector<ExternalParamArgRow>;
        /** @brief 实例桥体内侧 upsert。 */
        void upsert_external_invoke(const ExternalInvokeWrite& w);
        /** @brief 整表读取实例桥体内侧（确定序）。 */
        [[nodiscard]]
        auto all_external_invokes() -> std::vector<ExternalInvokeRow>;
        /** @brief 清空槽终态表（降级全量重算后的整表重写前调用）。 */
        void erase_analysis_slots_all();
        /** @brief 删除一行槽终态（增量写回的消失槽半边）。 */
        void erase_analysis_slot(
            std::string_view owner_key,
            std::string_view var_name);
        /**
         * @brief purge/erase 前捕获该文件的派生数据旧边见证（设计评审修正）。
         *
         * @internal 只读不删：调用方先捕获再走 purge_file/erase_file。行仍
         *           在是前提——符号行此时尚未被级联（purge 的符号删除在
         *           站点/线索行删除之后，此处 join 仍能解析 owner）。
         */
        [[nodiscard]]
        auto capture_analysis_purge(
            FileId                 file,
            std::span<const StrId> fresh_unique_key_ids) -> PurgeCapture;

        /**
         * @brief 整行移除一条 files 行（删除源码处理）：FK ON DELETE
         *        CASCADE 带走它的符号、站点属主边
         *        与 includes。
         */
        void erase_file(FileId file);
        // （删除顺序纪律见 purge_file 块注释：先 purge_file 清引用方，再
        //  erase_file 删被引用的 files 行；全库调用点均照此顺序，外键
        //  CASCADE 仅作残余兜底。）

        /**
         * @internal
         * 罕见写：每次调用序列化字面值（无参数，因此冲突路径的赋值可以
         * 复用同一个值而不撞参数名字标签）。
         */
        void set_meta(std::string_view key, std::string_view value);
        [[nodiscard]]
        auto meta(std::string_view key) -> std::optional<std::string>;
        /**
         * @brief 移除一条 meta 行（键不存在 = 无操作）。
         *
         * @internal
         * meta 键值契约的 NULL 侧：llvm_path 缺席意味着"默认引擎解析"
         * （ccg config unset）。
         */
        void erase_meta(std::string_view key);

        /**
         * @brief 为图构建器的批量重建（单个事务内数以百万计的键控
         *        upsert）调高连接的页缓存，事后回落到 SQLite 默认。
         *
         * @internal
         * cache_size 是 per-connection 旋钮：不向文件写任何东西，
         * journal/synchronous 保持打开时的原样——批处理无论如何都是单个
         * 事务。负值 cache_size = 每连接 KiB；64 MiB 覆盖批 upsert 的
         * 工作集（符号 + 边 + str 索引），默认 2 MiB 不够。
         */
        void set_bulk_cache(bool on);

        /**
         * @brief 武装本连接的 SQLite 忙超时（毫秒）。
         *
         * 并发的引擎批处理可能持有写锁数分钟，下面的入队/声明事务把它们
         * 熬过去而不是失败（busy_timeout 是 per-connection 的；不向文件
         * 写任何东西）。
         *
         * @internal
         * 实时协调（schema v6，租约裁决协议）一组的原则：SQLite 本身
         * 就是协调者——无锁文件；活性由租约时间戳裁决，持有者记号里的 pid
         * 只用于识别"租约未到期但持有进程已被杀死"的孤儿声明。组内每个
         * 方法跑一个短事务即返回；它们周围的引擎工作发生在任何协调事务
         * 之外。
         */
        void set_busy_timeout_ms(std::int64_t ms);

        /**
         * @brief ENQUEUE：一个短事务插入每条规范化路径（INSERT .. DO
         *        NOTHING——表的主键对同一文件的并发入队去重）。尊重忙
         *        超时。
         */
        void enqueue_pending(std::span<const std::string> paths);

        /**
         * @brief CLAIM：一个短 IMMEDIATE 事务。
         *
         * @return 本连接插入/刷新了单例声明行（lease_until = now +
         *         lease_ms）时为 true——要么此前无声明、要么前一个租约
         *         已过期或持有进程已死而被窃取（崩溃自愈）；存在存活声明
         *         时为 false：工作由持有者或更晚的事件覆盖，调用方立即
         *         以 0 退出。
         *
         * @internal
         * `now` 在 BEGIN IMMEDIATE 等待之后采样：事务可能已在
         * busy_timeout 里排在长写者后面，等待前的时间戳会把等待【期间】
         * 过期的租约判成仍存活——保守（把窃取推迟到下一事件）但可避免；
         * 行一旦可原子读取，由新鲜时钟裁决。存活判定在时间戳之上加一层
         * 持有者 pid 探活：宿主超时杀死更新进程时，析构守卫来不及删行，
         * 租约仍是未来时间戳；只按 lease_until 判活会使后续所有事件都
         * 只入队然后退出。探活只作否定判断（见 process_alive 的纪律），
         * 无 pid 的记号（测试/手写）退回纯租约语义。探测在提交任何东西
         * 之前完全消费：中途弃置的结果会挂住其语句，带活跃语句的 COMMIT
         * 以 busy 失败（symbol_row 的第一阶段规则，同一病灶）。至多一行
         * 存在（id = 1 主键）。存活声明分支：读在 IMMEDIATE 事务内进行，
         * 没有写者能在此决定之前塞进新声明。声明行的插入是罕见的字面值
         * 写（set_meta 形态）：每库一行，从不热。
         */
        [[nodiscard]]
        auto claim_update(std::int64_t lease_ms, std::string_view holder)
            -> bool;

        /**
         * @brief STEAL（写命令专用）：一个短 IMMEDIATE 事务，**无条件**把
         *        单例声明行改写为本持有者（holder + lease_until = now +
         *        lease_ms）。
         *
         * 与 claim_update 的关系：写侧语义完全相同（改 holder 与
         * lease_until 两列），唯一差别是触发方——claim_update 只在
         * 「无声明 / 租约已过期 / 持有进程已死」时才落笔（被动偷取），
         * 本方法连「活声明」也照偷（主动偷取）。写命令（`ccg index` /
         * `ccg msbuild` 的索引尾部）用它在一开始就接管协调权：常驻的
         * 增量持有者随后在下一个心跳点（批间 peek / 批内事务心跳 / 批后
         * 出队）看到自己的行已被改写而优雅让位，多写者并存的窗口被压到
         * 「对方在飞批收尾」一段（配合提交段删除纪律与陈旧 id 复活兜底）。
         *
         * @return 被改写前的旧行（holder, lease_until)；此前无声明行时为
         *         nullopt。只作诊断报告（"从谁手里接管"），活性裁决从不
         *         依赖它。
         *
         * @param audit_detail 顶替活声明时随 steal 审计行落库的 detail
         *        JSON（调用方掌握 takeover 租约的记账上下文）；空则记
         *        一个最小摘要。顶替过期/空行的接管不写 steal 审计。
         *
         * @internal 与 claim_update 同走 INSERT .. ON CONFLICT DO UPDATE 的
         * 单点写形态；`now` 同样在 BEGIN IMMEDIATE 等待之后采样。BEGIN
         * IMMEDIATE 撞上并发写者（在飞批的写锁）按调用方武装的 busy
         * 超时抛出，由调用方的退避重试循环承接——本方法自身不重试。
         * steal 审计与偷取写同一事务提交（偷取成功=审计在；偷取回滚=
         * 什么都没发生，无需审计）。
         */
        [[nodiscard]]
        auto steal_update(
            std::int64_t     lease_ms,
            std::string_view holder,
            std::string_view audit_detail = {})
            -> std::optional<std::pair<std::string, std::int64_t>>;

        /**
         * @brief PEEK + HEARTBEAT：一个短 IMMEDIATE 事务，刷新持有者的
         *        租约并读取 pending_updates，**不删除任何行**。
         *
         * @return files 为空 = 队列已收敛（释放并退出）；claim_lost =
         *         声明在运行中被窃（自己的租约中途过期）——循环停止，
         *         窃取者拥有队列。
         *
         * @internal
         * 心跳先行：运行中被窃的声明（自己的租约中途过期）不得继续处理
         * 窃取者的队列。peek 与清除分离是"不丢更新"的核心：批运行期间
         * 的新编辑仍留在队列里（同路径的再次入队被主键折叠），批次完成
         * 后由 clear_covered 只删除"覆盖集内且内容哈希与批前快照相同"
         * 的路径——内容再变的条目保留给下一批。
         */
        [[nodiscard]]
        auto peek_pending(std::int64_t lease_ms, std::string_view holder)
            -> PendingPeek;

        /**
         * @brief 批后出队：按 PendingClearInput 的判定规则删除队列行
         *        （一个短 IMMEDIATE 事务）。
         *
         * @param entries 批后出队的待判定输入（覆盖集与批前快照哈希）。
         * @param lease_ms 心跳租约时长；为 0 表示不心跳。
         * @param current_hash 在事务内按需重算某路径的当前内容哈希；与
         *        批前快照比较，调用方不再在事务外预计算。
         * @param holder 非空时在同一事务内先心跳租约；声明已被窃则放弃
         *        本次出队（返回 0），由新持有者拥有队列。
         * @return 实际删除的行数（并发入队折叠成同一行，删除数不超过
         *         entries 数；同一事务内复核后重新入队的行不计入）。
         *
         * @internal
         * 只由 update 持有者在批次完成后调用。删除条件见
         * PendingClearInput 的块注释：未跟踪一律删除；覆盖、快照哈希
         * 与当前哈希一致、且 files.content_hash 无记录或等于该哈希
         * （封住批内 edit→revert 的残余窗口；无记录不构成反证，避免
         * 持有者循环永不收敛）才删除；其余保留给下一批。空输入是
         * 合法无操作。
         * 判定用的事务内重算 + 删除后复核 + 立即重新入队封住"哈希读取
         * 与 DELETE 之间发生编辑"的窗口（见实现注释）。哈希回调在
         * IMMEDIATE 事务持有期内调用，故整个判定-删除序列是一个原子
         * 决策，不会跨租约/跨批次。
         */
        auto clear_covered(
            std::span<const PendingClearInput>                    entries,
            const std::function<std::string(const std::string&)>& current_hash,
            std::string_view                                      holder = {},
            std::int64_t lease_ms = 0) -> std::size_t;

        /**
         * @brief 释放声明——但只删我们自己那行（WHERE holder = ?），因此
         *        运行中过期并被窃的租约无法删除窃取者的声明。
         *
         * @return 行原本属于我们且已删除时为 true。
         */
        auto release_update(std::string_view holder) -> bool;

        /**
         * @brief 在**调用方已打开的事务内**续自己的租约（HEARTBEAT）。
         *
         * 供全量索引的提交段使用：那是一个单一大写事务（BEGIN IMMEDIATE
         * 到 COMMIT），SQLite 无嵌套事务、跨连接的 IMMEDIATE 心跳又被
         * 自己的写锁挡死，peek_pending 的"短事务心跳"模式在里面不可用
         * ——但同连接同事务再执行一条已预编译的 UPDATE 是合法的，这就是
         * 唯一能在提交段内在途续期的接缝。
         *
         * @return false（匹配零行）= 声明已被窃（自己的租约中途过期），
         *         调用方必须放弃本次提交（ROLLBACK）——窃取者拥有队列。
         *
         * @internal 与 peek_pending 的心跳完全同款判定（CLAIM_HEARTBEAT
         * 语句，UPDATE .. WHERE id+holder）：被窃后匹配零行，绝不改写
         * 窃取者的行。刻意不开自己的事务、不做节流——节流是调用方按
         * wall clock 的职责（提交段每 ~60s 一次即可）。
         */
        auto heartbeat_in_tx(std::string_view holder, std::int64_t lease_ms)
            -> bool;

        /**
         * @brief 解析期租约心跳（KEEPALIVE）：自足的短 IMMEDIATE 事务内
         *        续自己的租约，不读不写任何业务表。
         *
         * 供全量索引/引擎批的**解析段**使用：那一段只读、不持锁、耗时
         * 以分钟计（大库实测数十分钟），远超任何租约时长；持有者的声明
         * 若在整个解析段无人续期，钩子的"有主才不派生"判定会看到租约
         * 已过期而派生新写者反偷（真库实测的判决性失败）。本方法与
         * heartbeat_in_tx 走同一条 CLAIM_HEARTBEAT 语句、同一"零行=被偷"
         * 判定，差别只在事务归属：这里自带 BEGIN IMMEDIATE..COMMIT，由
         * 持有者进程的伴随心跳线程在**独立连接**上周期调用，绝不与主
         * 线程的连接共享任何状态。
         *
         * @return false（匹配零行）= 声明已被窃（自己的租约中途过期），
         *         调用方按 ClaimStolenError 语义优雅让位——窃取者拥有
         *         队列。
         *
         * @internal BUSY（撞上并发短写事务）按异常上抛，由调用方吞掉并
         * 在下一周期重试：BUSY 只说明锁被别人短暂持有，绝不等于被偷。
         */
        auto renew_update_lease(std::string_view holder, std::int64_t lease_ms)
            -> bool;

        /**
         * @brief 当前声明行，供诊断/测试：（holder, lease_until），对照
         *        调用方自己的时钟。无声明时为 nullopt。
         *
         * @internal
         * 返回前完全消费：中途弃置的结果挂住语句，本连接下一次写以
         * busy 失败（claim_update 的探测遵守同一规则）。至多一行。
         * 全店的结果循环不变式见 GraphStore 类块。
         */
        [[nodiscard]]
        auto update_claim_holder()
            -> std::optional<std::pair<std::string, std::int64_t>>;

        /**
         * @brief 更新协调状态的只读快照：声明存活/租约/持有者 pid 与
         *        pending_updates 全部路径。
         *
         * @internal
         * 一个 DEFERRED 事务内读两件东西后立即提交，pid 探活在事务之外做：
         * 绝不写、不心跳、不抢声明。`live` 与 `claim_update` 同规则（租约
         * 未到期；持有者记号带 pid 且进程已死则判死）。这是钩子快速标脏
         * 能"先问一句、再决定是否派生 update"的唯一依据。
         */
        [[nodiscard]]
        auto claim_snapshot() -> cg::db::ClaimSnapshot;

        /**
         * @brief 写一条协调事件审计行（coord_events，独立短事务）。
         *
         * 供埋点在事务之外补记（回滚后的 rollback、钩子的 enqueue-reject
         * 等）。失败（表缺席于补建窗口、busy 耗尽）静默返回 false——
         * 审计是旁路尽力而为通道，绝不把调用方的协调动作拖失败。
         */
        bool log_coord_event(const cg::db::CoordEventInput& input);

        /**
         * @brief 自 since_ms（unix 纪元毫秒）以来的协调事件，按 ts 升序。
         *
         * 失败/表缺席返回空（读侧与写侧同一容错哲学）。
         */
        [[nodiscard]]
        auto coord_events_since(std::int64_t since_ms)
            -> std::vector<cg::db::CoordEventRow>;

        /**
         * @brief 自 since_ms 以来各事件类型的条数（event 升序）。
         *
         * 供 `ccg hook status --json` 的 24 小时计数——一眼看出"昨晚发生
         * 过几次 steal/rollback"。
         */
        [[nodiscard]]
        auto coord_event_counts_since(std::int64_t since_ms)
            -> std::vector<std::pair<std::string, std::int64_t>>;

        /**
         * @brief 老化：删除早于 keep 窗口的行，并把总量压到 cap_rows 内。
         *
         * @param keep_ms 保留窗口（毫秒；截止 = now - keep_ms）。
         * @param cap_rows 行数封顶（按 ts 新者保留）。
         * @return 删除的行数；失败静默返回 0。
         */
        auto coord_events_age(std::int64_t keep_ms, std::int64_t cap_rows)
            -> std::size_t;

        /**
         * @name 头缓存旁路表读写（head_cache/file_instr_digest）。
         *
         * @internal 原生惰性语句（coord_events 同款形态：表可能缺席于
         * busy 窗口，绝不反噬主流程——失败即空集/静默 false，调用方一律
         * 把失败当缓存 miss 处理）。payload 对本层不透明，编解码归
         * cg.graph.headcache。
         */
        ///@{
        /** @brief 载入指定行版本戳的全部缓存行（他版本行被忽略）。 */
        [[nodiscard]]
        auto head_cache_load(std::int64_t row_ver)
            -> std::vector<cg::db::HeadCacheRow>;

        /**
         * @brief 在调用方事务内存放缓存行：先删 invalidate 文件的全部
         * 变体行，再 INSERT OR REPLACE 新行。
         *
         * @return 落盘行数；失败静默返回 0（缓存可丢弃再生）。
         */
        auto head_cache_store(
            const std::vector<cg::db::HeadCacheRow>& entries,
            const std::vector<cg::db::FileId>&       invalidate) -> std::size_t;

        /** @brief 载入全部指令摘要行（键 file_id）。 */
        [[nodiscard]]
        auto file_instr_digest_all()
            -> std::unordered_map<cg::db::FileId, cg::db::FileInstrDigestRow>;

        /** @brief 在调用方事务内存放指令摘要行（INSERT OR REPLACE）。 */
        void file_instr_digest_store(
            const std::vector<cg::db::FileInstrDigestRow>& rows);
        ///@}

        /**
         * @brief 事务 RAII 守卫。
         *
         * 经连接自身的执行路径跑 BEGIN/COMMIT/ROLLBACK（与 sqlpp23 的
         * transaction_t 内部发出的相同语句；那个类类型未被 sqlpp23 模块
         * 导出，因此守卫直接驱动语句）。销毁未完成的 Tx 即回滚。
         *
         * @internal
         * `immediate` 在 BEGIN（BEGIN IMMEDIATE）时取写锁而非首次写时：
         * 实时 claim/take 事务必须相对其它写者原子裁决，引擎批处理事务
         * 也不得竞速升级其 WAL 读快照——窗口期内已有写者提交时，升级以
         * 不可等待的 BUSY（SQLITE_BUSY_SNAPSHOT）失败，而 BEGIN
         * IMMEDIATE 像任何取锁一样停在 busy_timeout 里。
         *
         * 刻意只持有 store 引用与 std 状态：这里出现 sqlpp 类型的成员会
         * 把该类型放进导出（嵌套）类的 IFC（见下方 Impl）。
         */
        class Tx
        {
        public:
            explicit Tx(GraphStore& store, bool immediate = false)
                : store_(store)
            {
                exec_(immediate ? "BEGIN IMMEDIATE" : "BEGIN");
            }
            Tx(const Tx&) = delete;
            Tx&  operator=(const Tx&) = delete;
            void commit()
            {
                exec_("COMMIT");
                done_ = true;
            }
            void rollback()
            {
                exec_("ROLLBACK");
                done_ = true;
            }
            ~Tx()
            {
                if (!done_)
                {
                    try
                    {
                        exec_("ROLLBACK");
                    }
                    catch (...)
                    {
                    }
                }
            }

        private:
            void exec_(std::string_view sql);

            GraphStore& store_;
            bool        done_ = false;
        };
        auto begin_tx(bool immediate = false) -> Tx
        {
            return Tx{*this, immediate};
        }

        /**
         * @brief 组装完整的 SymbolRow（字符串已解析、限定名从父链构建、
         *        命名空间取自最外层祖先）。
         *
         * @internal
         * 第一阶段：在任何其它语句运行之前完全消费原始行。此前的形态在
         * 结果循环内部解析 str/file/parent 连接；父查找会重入这条预编译
         * 语句，在结果仍打开时重绑定它会损坏尚未读取的文本列（在带父的
         * 符号上观察到 param/template 乱码）。第二阶段：语句闲置后再做
         * 连接解析。
         */
        [[nodiscard]]
        auto symbol_row(SymId id) -> std::optional<SymbolRow>;
        /** @brief
         * 沿父链上行（带防损坏环路的守卫），再冠以最外层祖先的命名空间路径。
         */
        [[nodiscard]]
        auto qualified_name(SymId id) -> std::string;

        /**
         * @brief 单符号的出边聚合（每个 (对端, 类型) 一组：SUM(occurrences)、
         *        MIN(site_line)、MAX(confidence)、MAX(attrs)）。
         *
         * @internal 预编译读语句：类型过滤留给调用方——分组各自独立
         * 聚合，按类型丢组与 SQL 层 WHERE type IN (...) 逐位等价。结果行的
         * 组序与索引序一致（(dst_id, type_id) 升序），与旧的当场构建语句版
         * 相同。
         */
        [[nodiscard]]
        auto edges_from_agg(SymId src) -> std::vector<EdgeAggRow>;

        /**
         * @brief 单符号的入边聚合；语义与行序约定同 edges_from_agg 的镜像
         *        ((src_id, type_id) 升序)。
         */
        [[nodiscard]]
        auto edges_to_agg(SymId dst) -> std::vector<EdgeAggRow>;

        /**
         * @brief 单符号单一类型的原始边行（出边 src==id 或入边 dst==id），
         *        用于需要站点/置信度/属性的场合。
         *
         * @internal 预编译读语句；行序沿用索引自然序（出边按
         * (dst, site_file, site_line)，入边按 edge_id），与旧的当场构建
         * 语句版相同。
         */
        [[nodiscard]]
        auto edges_of_raw(SymId id, bool outgoing, EdgeType type)
            -> std::vector<EdgeRow>;

        /**
         * @brief 运行一条裸 SQL 语句（测试模拟 schema 漂移：删表、戳字典）。
         *
         * @internal 与 conn() 相同的接缝哲学，但不向导入方暴露连接类型。
         */
        void exec(std::string_view sql);

        /**
         * @internal 仅供 cg.db.query 的内部接缝。sqlpp23 类型绝不出 cg.db。
         */
        auto conn() -> sqlpp::sqlite3::connection&;

    private:
        auto store_str_(std::string_view s) -> StrId;
        auto load_str_(std::string_view s) -> std::optional<StrId>;

        /**
         * @internal 库内记录的内容哈希（files.content_hash，按路径查）：
         * 未驻留或无 files 行时无值。出队判定用它对快照哈希——不一致
         * 说明索引不是当前字节的结论。
         */
        auto indexed_hash_of_(std::string_view path)
            -> std::optional<std::string>;

        /**
         * @internal
         * 全新文件探测保持在 catalog 表上走类型化 sqlpp23（cg.db 内无任何
         * 裸 SQL）。项目未发布：不维护跨版本迁移——已存库的版本戳与当前
         * schema_version 不符（旧版本或缺戳）时抛 SchemaMismatchError 拒绝
         * 打开，由调用方重建文件（`ccg index --force`；`ccg msbuild` 对
         * schema 失配默认重建）。
         */
        void bootstrap_();

        /**
         * @internal 在**调用方已打开的事务内**插一条 coord_events
         * （steal/orphan-recover 与偷取写同事务提交：偷取成功=审计在，
         * 偷取回滚=什么都没发生）。任何失败吞掉返回 false——审计绝不
         * 反噬偷取/接管本体。
         */
        bool log_coord_event_tx_(const cg::db::CoordEventInput& input);

        /**
         * @internal 给一个已死 pid 的全部悬挂 run-start（之后没有同 pid
         * run-finish 的 run-start）补记 run-finish（reason=abandoned），
         * 与接管同一事务。被杀/断电的运行者自己写不了收尾——发现者
         * （接管者）代为闭合，运行黑洞从此有库内证据。补记的 finish
         * 让悬挂判定自愈为假，同一悬挂不会被下一个接管者重复补记。
         */
        void abandon_dangling_runs_tx_(
            std::int64_t     dead_pid,
            std::string_view discoverer);

        /**
         * @internal 全新库的字典种子写入：symbol_kinds/edge_types 各一条
         * 多行类型化 insert（DDL 建表后调用，仅全新库一次）。名单唯一来源
         * 是 schema 模块的 name_of(SymbolKind)/name_of(EdgeType)，枚举值即
         * 种子 id，杜绝名单双份。
         */
        void seed_dictionaries_();

        struct Impl;

        std::unique_ptr<Impl> impl_;
        cg::util::Interner    interner_;
        /** @internal FileId -> 路径文本的小缓存（失效规则见 file_path）。 */
        std::unordered_map<FileId, std::string> file_path_cache_;
    };

    /**
     * @internal
     * GraphStore 携带 sqlpp23 的状态，藏在 unique_ptr 后面：导出类的 IFC
     * 完全不得描述 sqlpp23/ tab:: 类型，否则每个导入单元都要重新合并那些
     * 模板实例化、并使 MSVC IFC 读取器崩溃（见 db.tables.h）。
     */
    struct GraphStore::Impl
    {
        explicit Impl(std::shared_ptr<sqlpp::sqlite3::connection_config> c)
            : cfg_{std::move(c)}, db_{cfg_}
        {
            install_sql_diag_();
        }

        std::shared_ptr<sqlpp::sqlite3::connection_config> cfg_;
        sqlpp::sqlite3::connection                         db_;
        std::optional<Statements>                          st_;

        /**
         * @internal 语句级失败诊断（默认关闭，置环境变量 CCG_SQL_DIAG 开启）。
         *
         * 开启时经 sqlite3_trace_v2(SQLITE_TRACE_STMT) 把本连接逐条语句的
         * 展开文本（绑定参数已内联成字面量）连同序号/墙钟记入定长环缓冲；
         * 任何一条语句抛错时（run 包装层捕获）把 sqlite 主/扩展错误码、
         * errmsg 与环缓冲倾倒到 stderr。供并发写库类故障定位"哪条语句、
         * 哪张表、什么顺序"——外键失败时失败语句即缓冲末条（或紧邻末条，
         * SQLITE_TRACE_STMT 在语句开跑的首次 step 前即回调）。单连接单
         * 线程使用，缓冲不加锁。
         */
        static constexpr std::size_t diag_ring_cap = 64;
        static constexpr std::size_t diag_sql_max = 4000;
        bool                         diag_on_ = false;
        std::uint64_t                diag_seq_ = 0;
        std::deque<std::string>      diag_ring_;

        // coord_events 的原生惰性语句：这张旁路审计表可能缺席于同版本
        // 已存库首次打开的极端 busy 窗口（bootstrap 的幂等补建拿不到
        // 写锁），把它的语句放进 make_statements 会让整个连接打不开——
        // 审计是尽力而为通道，绝不反噬协调主流程，故走原生 sqlite3 C
        // API、首次使用时才 prepare（表缺失时当场补建一次再试）。
        struct StmtDeleter
        {
            void operator()(sqlite3_stmt* s) const
            {
                if (s != nullptr)
                {
                    sqlite3_finalize(s);
                }
            }
        };
        using NativeStmt = std::unique_ptr<sqlite3_stmt, StmtDeleter>;
        NativeStmt coord_ins_;
        NativeStmt coord_sel_;
        NativeStmt coord_counts_;
        NativeStmt coord_age_;
        NativeStmt coord_trim_;
        NativeStmt coord_cnt_;
        NativeStmt coord_dangling_;

        // 头缓存旁路表的原生惰性语句（coord_stmt_ 同款容错哲学：表缺席
        // 先补建，仍失败返回空——缓存读写绝不反噬主流程）。
        NativeStmt hc_ins_;
        NativeStmt hc_del_file_;
        NativeStmt hc_sel_;
        NativeStmt fid_ins_;
        NativeStmt fid_sel_;

        /** @internal 首次使用时 prepare 并确保旁路表在位；失败返回空。 */
        auto side_stmt_(NativeStmt& slot, const char* sql) -> sqlite3_stmt*
        {
            if (slot == nullptr)
            {
                try
                {
                    for (const std::string_view ddl : head_cache_side_ddl)
                    {
                        run(ddl);
                    }
                }
                catch (const std::exception&)
                {
                }
                sqlite3_stmt* raw = nullptr;
                if (sqlite3_prepare_v2(
                        db_.native_handle(),
                        sql,
                        -1,
                        &raw,
                        nullptr)
                    != SQLITE_OK)
                {
                    sqlite3_finalize(raw);
                    return nullptr;
                }
                slot.reset(raw);
            }
            sqlite3_reset(slot.get());
            sqlite3_clear_bindings(slot.get());
            return slot.get();
        }

        /** @internal 首次使用时 prepare；表缺失先补建，仍失败返回空。 */
        auto coord_stmt_(NativeStmt& slot, const char* sql) -> sqlite3_stmt*
        {
            if (slot == nullptr)
            {
                try
                {
                    for (const std::string_view ddl : coord_events_ddl)
                    {
                        run(ddl);
                    }
                }
                catch (const std::exception&)
                {
                }
                sqlite3_stmt* raw = nullptr;
                if (sqlite3_prepare_v2(
                        db_.native_handle(),
                        sql,
                        -1,
                        &raw,
                        nullptr)
                    != SQLITE_OK)
                {
                    // 旁路失败留痕（一行 stderr，绝不抛）：调试与运维唯一
                    // 的现场线索——表缺席之外的原因（连接状态等）在此现形。
                    std::println(
                        std::cerr,
                        "ccg: note: coord audit statement unavailable - {}",
                        sqlite3_errmsg(db_.native_handle()));
                    sqlite3_finalize(raw);
                    return nullptr;
                }
                slot.reset(raw);
            }
            sqlite3_reset(slot.get());
            sqlite3_clear_bindings(slot.get());
            return slot.get();
        }

        void install_sql_diag_()
        {
            if (!cg::util::cstd::get_env("CCG_SQL_DIAG").has_value())
            {
                return;
            }
            diag_on_ = true;
            sqlite3_trace_v2(
                db_.native_handle(),
                SQLITE_TRACE_STMT,
                &Impl::diag_trace_cb_,
                this);
        }

        static int diag_trace_cb_(unsigned, void* ctx, void*, void* x)
        {
            auto*       self = static_cast<Impl*>(ctx);
            const char* sql = static_cast<const char*>(x);
            if (sql == nullptr)
            {
                return 0;
            }
            const std::int64_t ms = cg::util::cstd::epoch_ms();
            std::string        line =
                std::format("[{:>6} {:>16}] ", ++self->diag_seq_, ms);
            line.append(sql, std::strlen(sql));
            if (line.size() > diag_sql_max)
            {
                line.resize(diag_sql_max);
                line.append(" ...<truncated>");
            }
            self->diag_ring_.push_back(std::move(line));
            if (self->diag_ring_.size() > diag_ring_cap)
            {
                self->diag_ring_.pop_front();
            }
            return 0;
        }

        /** @internal 倾倒诊断（语句失败时自动触发）；绝不抛出。 */
        void diag_dump_(std::string_view banner)
        {
            if (!diag_on_)
            {
                return;
            }
            try
            {
                const int ext = sqlite3_extended_errcode(db_.native_handle());
                std::println(std::cerr, "ccg-sql-diag: {}", banner);
                if (!banner.empty() && ext != 0)
                {
                    std::println(
                        std::cerr,
                        "ccg-sql-diag: sqlite primary={} extended={} msg={}",
                        ext & 0xff,
                        ext,
                        sqlite3_errmsg(db_.native_handle()));
                }
                std::println(
                    std::cerr,
                    "ccg-sql-diag: recent statements on this "
                    "connection (oldest first, seq + epoch-ms):");
                for (const std::string& line : diag_ring_)
                {
                    std::println(std::cerr, "ccg-sql-diag:   {}", line);
                }
                std::fflush(stderr);
            }
            catch (...)
            {
            }
        }

        /**
         * @internal 本连接所有语句的统一执行口：失败时带上下文倾倒再重抛。
         *
         * 转发引用保持 sqlpp connection::operator() 的 const/右值重载语义，
         * 预编译语句与动态语句（含 BEGIN/COMMIT 字面 SQL）一概覆盖。失败
         * 语句若为预编译语句（暴露 native_handle），先经 sqlite3_expanded_sql
         * 把绑定参数内联成字面量打出来——trace_v2 回调只给未展开文本，
         * 参数值是定位外键缺哪行父表的钥匙。
         */
        template <typename S>
        auto run(S&& s) -> decltype(db_(std::forward<S>(s)))
        {
            try
            {
                return db_(std::forward<S>(s));
            }
            catch (const std::exception& e)
            {
                if constexpr (requires { s.native_handle(); })
                {
                    if (diag_on_)
                    {
                        if (char* exp = sqlite3_expanded_sql(s.native_handle()))
                        {
                            std::println(
                                std::cerr,
                                "ccg-sql-diag: failed statement (expanded): {}",
                                std::string_view{exp});
                            sqlite3_free(exp);
                        }
                    }
                }
                diag_dump_(std::format("statement failed: {}", e.what()));
                throw;
            }
        }
    };

    void GraphStore::exec(std::string_view sql) { impl_->run(sql); }

    auto GraphStore::conn() -> sqlpp::sqlite3::connection&
    {
        return impl_->db_;
    }

    GraphStore::GraphStore(std::string_view path)
        : GraphStore(path, default_busy_timeout_ms)
    {
    }

    GraphStore::GraphStore(std::string_view path, std::int64_t busy_timeout_ms)
        : impl_(std::make_unique<Impl>(make_config(path))), interner_()
    {
        impl_->run(std::format("PRAGMA busy_timeout = {}", busy_timeout_ms));
        for (const std::string_view pragma : open_pragmas)
        {
            impl_->run(pragma);
        }
        bootstrap_();
        impl_->st_.emplace(make_statements(impl_->db_));
        interner_ = cg::util::Interner(
            [this](std::string_view s) { return load_str_(s); },
            [this](std::string_view s) { return store_str_(s); });
    }

    GraphStore::~GraphStore() {}

    void GraphStore::Tx::exec_(std::string_view sql) { store_.impl_->run(sql); }

    void GraphStore::bootstrap_()
    {
        const tab::SqliteMaster sm{};
        bool                    have_meta = false;
        {
            auto rows = impl_->run(
                sqlpp::select(sm.name).from(sm).where(
                    sm.type == "table" and sm.name == "meta"));
            have_meta = std::ranges::distance(rows) > 0;
        }
        if (!have_meta)
        {
            for (const std::string_view stmt : ddl_statements)
            {
                impl_->run(stmt);
            }
            seed_dictionaries_();
            set_meta("schema_version", std::to_string(schema_version));
            set_meta("app_version", cg::util::version);
        }
        else
        {
            const auto version = meta("schema_version");
            if (!(version && std::atoi(version->c_str()) == schema_version))
            {
                throw SchemaMismatchError(
                    std::format(
                        "database schema version mismatch (db has '{}', "
                        "expected {}); recreate it with 'ccg index --force'",
                        version.value_or("<missing>"),
                        schema_version));
            }
        }
        // coord_events 的幂等补建（新库已由上方 DDL 建成，此处为 no-op；
        // 已存的同版本库在这里拿到这张后加的旁路审计表——见
        // coord_events_ddl 的块注释）。失败（探针连接的短 busy 撞上
        // 写者等）只意味着审计暂时缺席：语句层的惰性补建会兜底，
        // 绝不让打开失败。
        try
        {
            for (const std::string_view stmt : coord_events_ddl)
            {
                impl_->run(stmt);
            }
        }
        catch (const std::exception&)
        {
        }
        // 头缓存旁路表的幂等补建 + 侧版本复核（head_cache_side_ddl 块注
        // 释）：版本不匹配的缓存行整表丢弃——缓存是加速器不是事实源，
        // 旧版本行留着只会造成错读。只读连接的写失败被吞（查询路径不消
        // 费缓存表）。meta 戳缺席（老库首次补建）时同样清一次再写戳。
        try
        {
            for (const std::string_view stmt : head_cache_side_ddl)
            {
                impl_->run(stmt);
            }
            const auto side_ver = meta("head_cache_side_ver");
            if (!side_ver.has_value()
                || std::atoi(side_ver->c_str()) != head_cache_side_ver)
            {
                impl_->run("delete from head_cache");
                impl_->run("delete from file_instr_digest");
                set_meta(
                    "head_cache_side_ver",
                    std::to_string(head_cache_side_ver));
            }
        }
        catch (const std::exception&)
        {
        }
    }

    void GraphStore::seed_dictionaries_()
    {
        const tab::SymbolKinds sk{};
        const tab::EdgeTypes   et{};
        auto                   kinds =
            sqlpp::sqlite3::insert_into(sk).columns(sk.kindId, sk.name);
        for (std::int64_t i = 0; i < static_cast<std::int64_t>(kind_count); ++i)
        {
            kinds.add_values(
                sk.kindId = i,
                sk.name = std::string{name_of(symbol_kind_of(i))});
        }
        impl_->run(kinds);
        auto edges =
            sqlpp::sqlite3::insert_into(et).columns(et.typeId, et.name);
        for (std::int64_t i = 0; i < static_cast<std::int64_t>(edge_type_count);
             ++i)
        {
            edges.add_values(
                et.typeId = i,
                et.name = std::string{name_of(edge_type_of(i))});
        }
        impl_->run(edges);
    }

    namespace
    {
        /**
         * @internal
         * 文本参数绑定 std::string（parameter_value_t`<text>`）；string_view 只
         * 显式转换。
         */
        auto to_string_param(std::string_view s) -> std::string
        {
            return std::string{s};
        }
        auto to_opt_string_param(std::optional<std::string_view> s)
            -> std::optional<std::string>
        {
            if (!s.has_value())
            {
                return std::nullopt;
            }
            return std::string{*s};
        }
    } // namespace

    auto GraphStore::store_str_(std::string_view s) -> StrId
    {
        auto& ins = std::get<INTERN>(*impl_->st_);
        ins.parameters.s = to_string_param(s);
        const auto result = impl_->run(ins);
        if (result.affected_rows != 0)
        {
            return static_cast<StrId>(result.last_insert_id); // 新 rowid
        }
        auto& sel = std::get<STR_BY_S>(*impl_->st_);
        sel.parameters.s = to_string_param(s);
        auto  rows = impl_->run(sel);
        StrId found = 0;
        for (const auto& row : rows)
        {
            found = static_cast<StrId>(row.id);
        }
        if (found == 0)
        {
            throw std::runtime_error(
                "cg.db: str row disappeared between upsert and select");
        }
        return found;
    }

    auto GraphStore::indexed_hash_of_(std::string_view path)
        -> std::optional<std::string>
    {
        const StrId path_id = find_str(path).value_or(0);
        if (path_id == 0)
        {
            return std::nullopt; // 未驻留 / 无 files 行：无法核对
        }
        auto& sel = std::get<FILE_HASH_BY_PATH>(*impl_->st_);
        sel.parameters.pathId = path_id;
        auto                       rows = impl_->run(sel);
        std::optional<std::string> found;
        for (const auto& row : rows)
        {
            if (row.contentHash.has_value())
            {
                found = std::string{*row.contentHash};
            }
        }
        return found;
    }

    auto GraphStore::load_str_(std::string_view s) -> std::optional<StrId>
    {
        auto& sel = std::get<STR_BY_S>(*impl_->st_);
        sel.parameters.s = to_string_param(s);
        auto                 rows = impl_->run(sel);
        std::optional<StrId> found;
        for (const auto& row : rows)
        {
            found = static_cast<StrId>(row.id);
        }
        return found;
    }

    auto GraphStore::intern(std::string_view s) -> StrId
    {
        return interner_.intern(s);
    }

    auto GraphStore::find_str(std::string_view s) -> std::optional<StrId>
    {
        if (const auto cached = interner_.lookup(s))
        {
            return *cached;
        }
        return load_str_(s);
    }

    auto GraphStore::str_of(StrId id) -> std::string
    {
        if (const auto cached = interner_.str(id))
        {
            return std::string{*cached};
        }
        auto& sel = std::get<STR_BY_ID>(*impl_->st_);
        sel.parameters.id = id;
        auto                       rows = impl_->run(sel);
        std::optional<std::string> found;
        for (const auto& row : rows)
        {
            found = std::string{row.s};
        }
        if (!found.has_value())
        {
            throw std::runtime_error(
                std::format("cg.db: no str row with id {}", id));
        }
        return *found;
    }

    auto GraphStore::upsert_file(std::string_view normalized_path) -> FileId
    {
        const StrId path_id = intern(normalized_path);
        auto&       ins = std::get<FILE_INSERT>(*impl_->st_);
        ins.parameters.pathId = path_id;
        const auto result = impl_->run(ins);
        if (result.affected_rows != 0)
        {
            return static_cast<FileId>(result.last_insert_id);
        }
        auto& sel = std::get<FILE_BY_PATH>(*impl_->st_);
        sel.parameters.pathId = path_id;
        auto   rows = impl_->run(sel);
        FileId found = 0;
        for (const auto& row : rows)
        {
            found = static_cast<FileId>(row.fileId);
        }
        if (found == 0)
        {
            throw std::runtime_error(
                "cg.db: files row disappeared between upsert and select");
        }
        return found;
    }

    auto GraphStore::file(FileId id) -> std::optional<FileRow>
    {
        auto& sel = std::get<FILE_BY_ID>(*impl_->st_);
        sel.parameters.fileId = id;
        auto                   rows = impl_->run(sel);
        std::optional<FileRow> out;
        for (const auto& row : rows)
        {
            FileRow hit;
            hit.id = static_cast<FileId>(row.fileId);
            hit.path = str_of(static_cast<StrId>(row.pathId));
            if (row.contentHash.has_value())
            {
                hit.content_hash = std::string{*row.contentHash};
            }
            hit.status = static_cast<cg::util::ParseStatus>(
                static_cast<int>(row.parseStatus));
            hit.error_count = static_cast<int>(row.errorCount);
            if (row.lastError.has_value())
            {
                hit.last_error = std::string{*row.lastError};
            }
            if (row.indexedAt.has_value())
            {
                hit.indexed_at = std::string{*row.indexedAt};
            }
            out = hit;
        }
        return out;
    }

    auto GraphStore::file_path(FileId id) -> std::optional<std::string>
    {
        if (const auto hit = file_path_cache_.find(id);
            hit != file_path_cache_.end())
        {
            return hit->second;
        }
        auto& sel = std::get<FILE_BY_ID>(*impl_->st_);
        sel.parameters.fileId = id;
        auto                 rows = impl_->run(sel);
        std::optional<StrId> path_id;
        for (const auto& row : rows)
        {
            path_id = static_cast<StrId>(row.pathId);
        }
        if (!path_id.has_value())
        {
            return std::nullopt;
        }
        // 命中才入缓存：悬空 id 的下次查询仍然直达库（不把负结果铸死）。
        std::string path = str_of(*path_id);
        file_path_cache_.emplace(id, path);
        return path;
    }

    auto GraphStore::find_file(std::string_view normalized_path)
        -> std::optional<FileRow>
    {
        const StrId path_id = find_str(normalized_path).value_or(0);
        if (path_id == 0)
        {
            return std::nullopt;
        }
        auto& sel = std::get<FILE_BY_PATH>(*impl_->st_);
        sel.parameters.pathId = path_id;
        auto   rows = impl_->run(sel);
        FileId found = 0;
        for (const auto& row : rows)
        {
            found = static_cast<FileId>(row.fileId);
        }
        if (found == 0)
        {
            return std::nullopt;
        }
        return file(found);
    }

    void GraphStore::set_file_hash(FileId id, std::string_view sha256_hex)
    {
        auto& upd = std::get<FILE_HASH>(*impl_->st_);
        upd.parameters.fileId = id;
        upd.parameters.contentHash = to_string_param(sha256_hex);
        impl_->run(upd);
    }

    void GraphStore::clear_file_hash(FileId id)
    {
        auto& upd = std::get<FILE_HASH>(*impl_->st_);
        upd.parameters.fileId = id;
        upd.parameters.contentHash = std::nullopt;
        impl_->run(upd);
    }

    void GraphStore::set_file_path(FileId id, std::string_view normalized_path)
    {
        const StrId path_id = intern(normalized_path);
        auto&       upd = std::get<FILE_PATH>(*impl_->st_);
        upd.parameters.fileId = id;
        upd.parameters.pathId = path_id;
        impl_->run(upd);
        // 该 id 的路径文本已改写：路径缓存整体作废（失效规则见 file_path）。
        file_path_cache_.clear();
    }

    void GraphStore::set_file_status(
        FileId                          id,
        cg::util::ParseStatus           status,
        int                             error_count,
        std::optional<std::string_view> last_error,
        std::optional<std::string_view> indexed_at)
    {
        auto& upd = std::get<FILE_STATUS>(*impl_->st_);
        upd.parameters.fileId = id;
        upd.parameters.parseStatus =
            static_cast<std::int64_t>(static_cast<int>(status));
        upd.parameters.errorCount = error_count;
        upd.parameters.lastError = to_opt_string_param(last_error);
        upd.parameters.indexedAt = to_opt_string_param(indexed_at);
        impl_->run(upd);
    }

    auto GraphStore::upsert_symbol(const SymbolWrite& w) -> SymId
    {
        const StrId key_id = intern(w.unique_key);
        const StrId name_id = intern(w.name);
        const StrId ns_id = intern(w.ns);

        auto& ins = std::get<SYM_INSERT>(*impl_->st_);
        ins.parameters.uniqueKeyId = key_id;
        ins.parameters.kindId = to_id(w.kind);
        ins.parameters.nameId = name_id;
        ins.parameters.nsId = ns_id;
        ins.parameters.sigHash = w.sig_hash;
        ins.parameters.fileId = w.file_id;
        ins.parameters.line = w.line;
        ins.parameters.col = w.col;
        ins.parameters.parentId = w.parent_id;
        ins.parameters.flags = static_cast<std::int64_t>(w.flags);
        ins.parameters.paramTypes = to_opt_string_param(w.param_types);
        ins.parameters.templateParams = to_opt_string_param(w.template_params);
        const auto result = impl_->run(ins);
        if (result.affected_rows != 0)
        {
            return static_cast<SymId>(result.last_insert_id);
        }

        auto& sel = std::get<SYM_BY_KEY>(*impl_->st_);
        sel.parameters.uniqueKeyId = key_id;
        SymId existing = 0;
        {
            auto rows = impl_->run(sel);
            for (const auto& row : rows)
            {
                existing = static_cast<SymId>(row.symId);
            }
        }
        auto& upd = std::get<SYM_REFRESH>(*impl_->st_);
        upd.parameters.symId = existing;
        upd.parameters.kindId = to_id(w.kind);
        upd.parameters.nameId = name_id;
        upd.parameters.nsId = ns_id;
        upd.parameters.sigHash = w.sig_hash;
        upd.parameters.fileId = w.file_id;
        upd.parameters.line = w.line;
        upd.parameters.col = w.col;
        upd.parameters.parentId = w.parent_id;
        upd.parameters.flags = static_cast<std::int64_t>(w.flags);
        upd.parameters.paramTypes = to_opt_string_param(w.param_types);
        upd.parameters.templateParams = to_opt_string_param(w.template_params);
        impl_->run(upd);
        return existing;
    }

    auto GraphStore::symbol_exists(SymId id) -> bool
    {
        auto& sel = std::get<SYM_BY_ID>(*impl_->st_);
        sel.parameters.symId = id;
        auto rows = impl_->run(sel);
        // 循环必须跑到完成（DONE）再返回：本查询在解析段/解析期租约心跳
        // 在场的窗口里高频执行，中途 return 会把语句留在步进中——其隐
        // 式读事务钉住连接的 WAL 读快照，此后任何其它连接（心跳线程、
        // 钩子进程）提交一次，本连接提交段的 BEGIN IMMEDIATE 就永久以
        // BUSY_SNAPSHOT 瞬时失败（忙处理器不介入、重试无解）。
        bool found = false;
        for (const auto& row : rows)
        {
            static_cast<void>(row);
            found = true;
        }
        return found;
    }

    auto GraphStore::busy_statements_diag() -> std::string
    {
        std::string   out;
        sqlite3_stmt* st = nullptr;
        while ((st = sqlite3_next_stmt(impl_->db_.native_handle(), st))
               != nullptr)
        {
            if (sqlite3_stmt_busy(st) != 0)
            {
                if (!out.empty())
                {
                    out += " ; ";
                }
                const char* sql = sqlite3_sql(st);
                out += sql != nullptr ? sql : "<unnamed>";
            }
        }
        return out;
    }

    auto GraphStore::upsert_edge(const EdgeWrite& w) -> EdgeId
    {
        auto& ins = std::get<EDGE_INSERT>(*impl_->st_);
        ins.parameters.srcId = w.src;
        ins.parameters.dstId = w.dst;
        ins.parameters.typeId = to_id(w.type);
        ins.parameters.siteFileId = w.site_file;
        ins.parameters.siteLine = w.site_line;
        ins.parameters.attrs = static_cast<std::int64_t>(w.attrs);
        ins.parameters.confidence = w.confidence;
        ins.parameters.occurrences = w.occurrences;
        const auto result = impl_->run(ins);
        if (result.affected_rows != 0)
        {
            return static_cast<EdgeId>(result.last_insert_id);
        }

        auto& sel = std::get<EDGE_BY_KEY>(*impl_->st_);
        sel.parameters.srcId = w.src;
        sel.parameters.dstId = w.dst;
        sel.parameters.typeId = to_id(w.type);
        sel.parameters.siteFileId = w.site_file;
        sel.parameters.siteLine = w.site_line;
        std::optional<EdgeId> existing;
        {
            auto rows = impl_->run(sel);
            for (const auto& row : rows)
            {
                existing = static_cast<EdgeId>(row.edgeId);
            }
        }
        if (!existing.has_value())
        {
            throw std::runtime_error(
                "cg.db: edge row disappeared between upsert and select");
        }
        auto& bump = std::get<EDGE_BUMP>(*impl_->st_);
        bump.parameters.occurrences = w.occurrences;
        bump.parameters.edgeId = *existing;
        impl_->run(bump);
        return *existing;
    }

    void GraphStore::upsert_include(const IncludeWrite& w)
    {
        const StrId target_id = intern(w.target);
        auto&       ps = std::get<INCLUDE_INSERT>(*impl_->st_);
        ps.parameters.fileId = w.file;
        ps.parameters.targetId = target_id;
        ps.parameters.line = w.line;
        ps.parameters.angled = w.angled ? 1 : 0;
        impl_->run(ps);
    }

    void GraphStore::upsert_import(const ImportWrite& w)
    {
        const StrId module_id = intern(w.module_name);
        auto&       ps = std::get<IMPORT_INSERT>(*impl_->st_);
        ps.parameters.fileId = w.file;
        ps.parameters.moduleId = module_id;
        ps.parameters.line = w.line;
        impl_->run(ps);
    }

    void GraphStore::upsert_type_hint(const TypeHintWrite& w)
    {
        auto& ins = std::get<HINT_INSERT>(*impl_->st_);
        ins.parameters.ownerId = w.owner_id;
        ins.parameters.varName = to_string_param(w.var_name);
        ins.parameters.fileId = w.file_id;
        ins.parameters.line = w.line;
        ins.parameters.col = w.col;
        ins.parameters.typeSet = to_string_param(w.type_set);
        ins.parameters.source = w.source;
        ins.parameters.confidence = w.confidence;
        ins.parameters.fallbackReason = to_string_param(w.fallback_reason);
        ins.parameters.paramIndex = w.param_index;
        ins.parameters.relatedVar = to_string_param(w.related_var);
        ins.parameters.relatedKey = to_string_param(w.related_key);
        impl_->run(ins);
    }

    auto GraphStore::all_type_hints() -> std::vector<TypeHintRow>
    {
        const tab::TypeHints     th{};
        std::vector<TypeHintRow> out;
        auto                     rows = impl_->run(
            sqlpp::select(sqlpp::all_of(th))
                .from(th)
                .order_by(
                    th.ownerId.asc(),
                    th.varName.asc(),
                    th.fileId.asc(),
                    th.line.asc(),
                    th.col.asc(),
                    th.source.asc()));
        for (const auto& row : rows)
        {
            TypeHintRow r = to_type_hint_row(row);
            r.id = static_cast<std::int64_t>(row.hintId);
            out.push_back(std::move(r));
        }
        return out;
    }

    auto GraphStore::load_type_hints_analysis() -> std::vector<TypeHintRow>
    {
        const tab::TypeHints     th{};
        std::vector<TypeHintRow> out;
        auto                     rows = impl_->run(
            sqlpp::select(
                th.ownerId,
                th.varName,
                th.fileId,
                th.line,
                th.col,
                th.typeSet,
                th.source,
                th.confidence,
                th.fallbackReason,
                th.paramIndex,
                th.relatedVar,
                th.relatedKey)
                .from(th)
                .order_by(
                    th.ownerId.asc(),
                    th.varName.asc(),
                    th.fileId.asc(),
                    th.line.asc(),
                    th.col.asc(),
                    th.source.asc()));
        for (const auto& row : rows)
        {
            out.push_back(to_type_hint_row(row));
        }
        return out;
    }

    auto GraphStore::load_type_hints_analysis_files(
        const std::vector<FileId>& files) -> std::vector<TypeHintRow>
    {
        if (files.empty())
        {
            return {};
        }
        const std::vector<std::int64_t> id_list{files.begin(), files.end()};
        const tab::TypeHints            th{};
        std::vector<TypeHintRow>        out;
        auto                            rows = impl_->run(
            sqlpp::select(
                th.ownerId,
                th.varName,
                th.fileId,
                th.line,
                th.col,
                th.typeSet,
                th.source,
                th.confidence,
                th.fallbackReason,
                th.paramIndex,
                th.relatedVar,
                th.relatedKey)
                .from(th)
                .where(th.fileId.in(id_list))
                .order_by(
                    th.ownerId.asc(),
                    th.varName.asc(),
                    th.fileId.asc(),
                    th.line.asc(),
                    th.col.asc(),
                    th.source.asc()));
        for (const auto& row : rows)
        {
            out.push_back(to_type_hint_row(row));
        }
        return out;
    }

    auto GraphStore::load_type_hints_of_source(std::int64_t source)
        -> std::vector<TypeHintRow>
    {
        const tab::TypeHints     th{};
        std::vector<TypeHintRow> out;
        auto                     rows = impl_->run(
            sqlpp::select(
                th.ownerId,
                th.varName,
                th.fileId,
                th.line,
                th.col,
                th.typeSet,
                th.source,
                th.confidence,
                th.fallbackReason,
                th.paramIndex,
                th.relatedVar,
                th.relatedKey)
                .from(th)
                .where(th.source == source)
                .order_by(
                    th.ownerId.asc(),
                    th.varName.asc(),
                    th.fileId.asc(),
                    th.line.asc(),
                    th.col.asc(),
                    th.source.asc()));
        for (const auto& row : rows)
        {
            out.push_back(to_type_hint_row(row));
        }
        return out;
    }

    auto GraphStore::type_hints_of_owner(SymId owner)
        -> std::vector<TypeHintRow>
    {
        const tab::TypeHints     th{};
        std::vector<TypeHintRow> out;
        auto                     rows = impl_->run(
            sqlpp::select(sqlpp::all_of(th))
                .from(th)
                .where(th.ownerId == owner)
                .order_by(
                    th.ownerId.asc(),
                    th.varName.asc(),
                    th.fileId.asc(),
                    th.line.asc(),
                    th.col.asc(),
                    th.source.asc()));
        for (const auto& row : rows)
        {
            TypeHintRow r = to_type_hint_row(row);
            r.id = static_cast<std::int64_t>(row.hintId);
            out.push_back(std::move(r));
        }
        return out;
    }

    auto GraphStore::type_hints_of_source(
        std::int64_t     source,
        std::string_view var_name) -> std::vector<TypeHintRow>
    {
        const tab::TypeHints     th{};
        std::vector<TypeHintRow> out;
        auto                     rows = impl_->run(
            sqlpp::select(sqlpp::all_of(th))
                .from(th)
                .where(
                    th.source == source && th.varName == std::string{var_name})
                .order_by(
                    th.ownerId.asc(),
                    th.varName.asc(),
                    th.fileId.asc(),
                    th.line.asc(),
                    th.col.asc(),
                    th.source.asc()));
        for (const auto& row : rows)
        {
            TypeHintRow r = to_type_hint_row(row);
            r.id = static_cast<std::int64_t>(row.hintId);
            out.push_back(std::move(r));
        }
        return out;
    }

    auto GraphStore::upsert_call_site(const CallSiteWrite& w) -> std::int64_t
    {
        auto& ins = std::get<SITE_INSERT>(*impl_->st_);
        ins.parameters.callerId = w.caller_id;
        ins.parameters.calleeId = w.callee_id;
        ins.parameters.calleeKey = to_string_param(w.callee_key);
        ins.parameters.calleeName = to_string_param(w.callee_name);
        ins.parameters.calleeNs = to_string_param(w.callee_ns);
        ins.parameters.calleeParent = to_string_param(w.callee_parent);
        ins.parameters.calleeSig = w.callee_sig;
        ins.parameters.calleeConst = w.callee_const ? 1 : 0;
        ins.parameters.methodId = w.method_id;
        ins.parameters.methodKey = to_string_param(w.method_key);
        ins.parameters.isVirtual = w.is_virtual ? 1 : 0;
        ins.parameters.qualified = w.qualified ? 1 : 0;
        ins.parameters.indirect = w.indirect ? 1 : 0;
        ins.parameters.receiverName = to_string_param(w.receiver_name);
        ins.parameters.receiverIsThis = w.receiver_is_this ? 1 : 0;
        ins.parameters.receiverOwnerKey = to_string_param(w.receiver_owner_key);
        ins.parameters.staticTypeId = w.static_type_id;
        ins.parameters.fileId = w.file_id;
        ins.parameters.line = w.line;
        ins.parameters.col = w.col;
        ins.parameters.argTypes = to_string_param(w.arg_types);
        ins.parameters.argVars = to_string_param(w.arg_vars);
        ins.parameters.argCallKeys = to_string_param(w.arg_call_keys);
        ins.parameters.argTargets = to_string_param(w.arg_targets);
        ins.parameters.receiverTypes = to_string_param(w.receiver_types);
        ins.parameters.receiverStatic = to_string_param(w.receiver_static);
        ins.parameters.calleeResolved = w.callee_resolved ? 1 : 0;
        ins.parameters.resolved = w.resolved ? 1 : 0;
        ins.parameters.confidence = w.confidence;
        ins.parameters.candidates = to_string_param(w.candidates);
        ins.parameters.fallbackReason = to_string_param(w.fallback_reason);
        ins.parameters.calleeUsr = to_string_param(w.callee_usr);
        ins.parameters.calleeExpr = to_string_param(w.callee_expr);
        ins.parameters.calleeParam = w.callee_param;
        impl_->run(ins);

        auto& sel = std::get<SITE_BY_KEY>(*impl_->st_);
        sel.parameters.callerId = w.caller_id;
        sel.parameters.fileId = w.file_id;
        sel.parameters.line = w.line;
        sel.parameters.col = w.col;
        sel.parameters.calleeKey = to_string_param(w.callee_key);
        sel.parameters.isVirtual = w.is_virtual ? 1 : 0;
        // 回查 WHERE 含 indirect 第七条件（与唯一键七列一致）：漏绑时参数
        // 零初始化为 0，indirect=1 的行回查失配返回 0——"重复 upsert 返回
        // 既有 id"的承诺对这类行被打破。
        sel.parameters.indirect = w.indirect ? 1 : 0;
        std::int64_t found = 0;
        {
            auto rows = impl_->run(sel);
            for (const auto& row : rows)
            {
                found = static_cast<std::int64_t>(row.siteId);
            }
        }
        return found;
    }

    auto GraphStore::all_call_sites() -> std::vector<CallSiteRow>
    {
        const tab::CallSites     cs{};
        std::vector<CallSiteRow> out;
        auto                     rows = impl_->run(
            sqlpp::select(sqlpp::all_of(cs))
                .from(cs)
                .order_by(
                    cs.callerId.asc(),
                    cs.fileId.asc(),
                    cs.line.asc(),
                    cs.col.asc(),
                    cs.calleeKey.asc(),
                    cs.isVirtual.asc(),
                    cs.indirect.asc()));
        for (const auto& row : rows)
        {
            CallSiteRow r = to_call_site_row(row);
            r.arg_targets = std::string{row.argTargets};
            r.callee_usr = std::string{row.calleeUsr};
            r.callee_expr = std::string{row.calleeExpr};
            r.callee_param = static_cast<int>(row.calleeParam);
            out.push_back(std::move(r));
        }
        return out;
    }

    auto GraphStore::load_call_sites_analysis() -> std::vector<CallSiteRow>
    {
        const tab::CallSites     cs{};
        std::vector<CallSiteRow> out;
        auto                     rows = impl_->run(
            sqlpp::select(
                cs.siteId,
                cs.callerId,
                cs.calleeId,
                cs.calleeKey,
                cs.calleeName,
                cs.calleeNs,
                cs.calleeParent,
                cs.calleeSig,
                cs.calleeConst,
                cs.methodId,
                cs.methodKey,
                cs.isVirtual,
                cs.qualified,
                cs.indirect,
                cs.receiverName,
                cs.receiverIsThis,
                cs.receiverOwnerKey,
                cs.staticTypeId,
                cs.fileId,
                cs.line,
                cs.col,
                cs.argTypes,
                cs.argVars,
                cs.argCallKeys,
                cs.receiverTypes,
                cs.receiverStatic,
                cs.calleeResolved,
                cs.resolved,
                cs.confidence,
                cs.candidates,
                cs.fallbackReason)
                .from(cs)
                .order_by(
                    cs.callerId.asc(),
                    cs.fileId.asc(),
                    cs.line.asc(),
                    cs.col.asc(),
                    cs.calleeKey.asc(),
                    cs.isVirtual.asc(),
                    cs.indirect.asc()));
        for (const auto& row : rows)
        {
            out.push_back(to_call_site_row(row));
        }
        return out;
    }

    void GraphStore::refresh_call_site(const CallSiteRow& row)
    {
        auto& upd = std::get<SITE_REFRESH>(*impl_->st_);
        upd.parameters.siteId = row.id;
        upd.parameters.calleeId = row.callee_id;
        upd.parameters.methodId = row.method_id;
        upd.parameters.methodKey = to_string_param(row.method_key);
        upd.parameters.staticTypeId = row.static_type_id;
        upd.parameters.receiverTypes = to_string_param(row.receiver_types);
        upd.parameters.calleeResolved = row.callee_resolved ? 1 : 0;
        upd.parameters.resolved = row.resolved ? 1 : 0;
        upd.parameters.confidence = row.confidence;
        upd.parameters.candidates = to_string_param(row.candidates);
        upd.parameters.fallbackReason = to_string_param(row.fallback_reason);
        impl_->run(upd);
    }

    void GraphStore::erase_edges_of_type(EdgeType type)
    {
        auto& del = std::get<EDGE_DELETE_TYPE>(*impl_->st_);
        del.parameters.typeId = to_id(type);
        impl_->run(del);
    }

    void GraphStore::erase_vd_edges_of_file(FileId file)
    {
        auto& del = std::get<EDGE_DELETE_VD_FILE>(*impl_->st_);
        del.parameters.typeId = to_id(EdgeType::VirtualDispatch);
        del.parameters.siteFileId = file;
        impl_->run(del);
    }

    void GraphStore::erase_vd_edges_of_site(
        SymId  caller,
        FileId site_file,
        int    site_line)
    {
        auto& del = std::get<EDGE_DELETE_VD_SITE>(*impl_->st_);
        del.parameters.typeId = to_id(EdgeType::VirtualDispatch);
        del.parameters.srcId = caller;
        del.parameters.siteFileId = site_file;
        del.parameters.siteLine = site_line;
        impl_->run(del);
    }

    void GraphStore::upsert_analysis_slot(const AnalysisSlotWrite& w)
    {
        auto& ins = std::get<SLOT_INSERT>(*impl_->st_);
        ins.parameters.ownerSym = to_string_param(w.owner_key);
        ins.parameters.varName = to_string_param(w.var_name);
        ins.parameters.typeSet = to_string_param(w.type_set);
        ins.parameters.confidence = w.confidence;
        ins.parameters.source = w.source;
        ins.parameters.reason = to_string_param(w.reason);
        ins.parameters.truncated = w.truncated ? 1 : 0;
        ins.parameters.ver = w.ver;
        ins.parameters.aliasSrc = to_string_param(w.alias_src);
        ins.parameters.retSrc = to_string_param(w.ret_src);
        if (impl_->run(ins).affected_rows != 0)
        {
            return;
        }
        auto& upd = std::get<SLOT_UPDATE>(*impl_->st_);
        upd.parameters.ownerSym = to_string_param(w.owner_key);
        upd.parameters.varName = to_string_param(w.var_name);
        upd.parameters.typeSet = to_string_param(w.type_set);
        upd.parameters.confidence = w.confidence;
        upd.parameters.source = w.source;
        upd.parameters.reason = to_string_param(w.reason);
        upd.parameters.truncated = w.truncated ? 1 : 0;
        upd.parameters.ver = w.ver;
        upd.parameters.aliasSrc = to_string_param(w.alias_src);
        upd.parameters.retSrc = to_string_param(w.ret_src);
        impl_->run(upd);
    }

    auto GraphStore::all_analysis_slots() -> std::vector<AnalysisSlotRow>
    {
        auto& sel = std::get<SLOT_SELECT_ALL>(*impl_->st_);
        std::vector<AnalysisSlotRow> out;
        auto                         rows = impl_->run(sel);
        for (const auto& row : rows)
        {
            AnalysisSlotRow r;
            r.owner_key = std::string{row.ownerSym};
            r.var_name = std::string{row.varName};
            r.type_set = std::string{row.typeSet};
            r.confidence = static_cast<int>(row.confidence);
            r.source = static_cast<int>(row.source);
            r.reason = std::string{row.reason};
            r.truncated = static_cast<std::int64_t>(row.truncated) != 0;
            r.ver = static_cast<int>(row.ver);
            r.alias_src = std::string{row.aliasSrc};
            r.ret_src = std::string{row.retSrc};
            out.push_back(std::move(r));
        }
        return out;
    }

    void GraphStore::erase_analysis_slots_all()
    {
        impl_->run(std::get<SLOT_DELETE_ALL>(*impl_->st_));
    }

    void GraphStore::erase_analysis_slot(
        std::string_view owner_key,
        std::string_view var_name)
    {
        auto& del = std::get<SLOT_DELETE_KEY>(*impl_->st_);
        del.parameters.ownerSym = to_string_param(owner_key);
        del.parameters.varName = to_string_param(var_name);
        impl_->run(del);
    }

    void GraphStore::upsert_passthrough_wrapper(
        const PassthroughWrapperWrite& w)
    {
        auto& ins = std::get<PTW_INSERT>(*impl_->st_);
        ins.parameters.wrapperKey = to_string_param(w.wrapper_key);
        ins.parameters.getterKey = to_string_param(w.getter_key);
        ins.parameters.getterName = to_string_param(w.getter_name);
        ins.parameters.fileId = static_cast<std::int64_t>(w.file_id);
        ins.parameters.line = w.line;
        ins.parameters.argIndex = w.arg_index;
        // on_conflict do nothing：同 (wrapper, getter) 重复登记幂等，登记
        // 字段在定义处一次定型（结构事实），无 update 分支需求。
        impl_->run(ins);
    }

    auto GraphStore::all_passthrough_wrappers()
        -> std::vector<PassthroughWrapperRow>
    {
        auto& sel = std::get<PTW_SELECT_ALL>(*impl_->st_);
        std::vector<PassthroughWrapperRow> out;
        auto                               rows = impl_->run(sel);
        for (const auto& row : rows)
        {
            PassthroughWrapperRow r;
            r.wrapper_key = std::string{row.wrapperKey};
            r.getter_key = std::string{row.getterKey};
            r.getter_name = std::string{row.getterName};
            r.file_id = static_cast<std::int64_t>(row.fileId);
            r.line = static_cast<int>(row.line);
            r.arg_index = static_cast<int>(row.argIndex);
            out.push_back(std::move(r));
        }
        return out;
    }

    void GraphStore::upsert_external_param_arg(const ExternalParamArgWrite& w)
    {
        auto& ins = std::get<EPA_INSERT>(*impl_->st_);
        ins.parameters.hostUsr = to_string_param(w.host_usr);
        ins.parameters.paramIndex = w.param_index;
        ins.parameters.callerKey = to_string_param(w.caller_key);
        ins.parameters.targetKey = to_string_param(w.target_key);
        ins.parameters.fileId = static_cast<std::int64_t>(w.file_id);
        ins.parameters.line = w.line;
        ins.parameters.col = w.col;
        impl_->run(ins);
    }

    auto GraphStore::all_external_param_args()
        -> std::vector<ExternalParamArgRow>
    {
        auto& sel = std::get<EPA_SELECT_ALL>(*impl_->st_);
        std::vector<ExternalParamArgRow> out;
        auto                             rows = impl_->run(sel);
        for (const auto& row : rows)
        {
            ExternalParamArgRow r;
            r.host_usr = std::string{row.hostUsr};
            r.param_index = static_cast<int>(row.paramIndex);
            r.caller_key = std::string{row.callerKey};
            r.target_key = std::string{row.targetKey};
            r.file_id = static_cast<std::int64_t>(row.fileId);
            r.line = static_cast<int>(row.line);
            r.col = static_cast<int>(row.col);
            out.push_back(std::move(r));
        }
        return out;
    }

    void GraphStore::upsert_external_invoke(const ExternalInvokeWrite& w)
    {
        auto& ins = std::get<EVI_INSERT>(*impl_->st_);
        ins.parameters.hostUsr = to_string_param(w.host_usr);
        ins.parameters.paramIndex = w.param_index;
        ins.parameters.fileId = static_cast<std::int64_t>(w.file_id);
        ins.parameters.line = w.line;
        ins.parameters.col = w.col;
        impl_->run(ins);
    }

    auto GraphStore::all_external_invokes() -> std::vector<ExternalInvokeRow>
    {
        auto& sel = std::get<EVI_SELECT_ALL>(*impl_->st_);
        std::vector<ExternalInvokeRow> out;
        auto                           rows = impl_->run(sel);
        for (const auto& row : rows)
        {
            ExternalInvokeRow r;
            r.host_usr = std::string{row.hostUsr};
            r.param_index = static_cast<int>(row.paramIndex);
            r.file_id = static_cast<std::int64_t>(row.fileId);
            r.line = static_cast<int>(row.line);
            r.col = static_cast<int>(row.col);
            out.push_back(std::move(r));
        }
        return out;
    }

    auto GraphStore::capture_analysis_purge(
        FileId                 file,
        std::span<const StrId> fresh_unique_key_ids) -> PurgeCapture
    {
        PurgeCapture out;
        {
            // 该文件线索行涉及的槽键（C++ 侧去重）：行删除即槽输入变化——
            // 槽的 owner 可以属于别的文件（字段槽按使用点落行）。
            const tab::TypeHints th{};
            const tab::Symbols   s{};
            auto                 rows = impl_->run(
                sqlpp::select(s.uniqueKeyId, th.varName)
                    .from(th.join(s).on(th.ownerId == s.symId))
                    .where(th.fileId == file));
            std::set<std::pair<std::string, std::string>> seen;
            for (const auto& row : rows)
            {
                seen.emplace(
                    str_of(static_cast<StrId>(row.uniqueKeyId)),
                    std::string{row.varName});
            }
            out.slots.assign(seen.begin(), seen.end());
        }
        {
            const tab::CallSites cs{};
            auto                 rows = impl_->run(
                sqlpp::select(cs.calleeKey)
                    .from(cs)
                    .where(cs.fileId == file and cs.calleeKey != ""));
            std::set<std::string> seen;
            for (const auto& row : rows)
            {
                seen.insert(std::string{row.calleeKey});
            }
            out.callee_keys.assign(seen.begin(), seen.end());
        }
        {
            std::vector<StrId> keep{
                fresh_unique_key_ids.begin(),
                fresh_unique_key_ids.end()};
            std::ranges::sort(keep);
            const tab::Symbols s{};
            auto               rows = impl_->run(
                sqlpp::select(s.uniqueKeyId, s.nameId)
                    .from(s)
                    .where(s.fileId == file));
            for (const auto& row : rows)
            {
                const StrId key = static_cast<StrId>(row.uniqueKeyId);
                if (!std::ranges::binary_search(keep, key))
                {
                    out.doomed_keys.push_back(str_of(key));
                    out.doomed_names.push_back(
                        str_of(static_cast<StrId>(row.nameId)));
                }
            }
        }
        return out;
    }

    void GraphStore::set_fingerprint(const FingerprintRow& fp)
    {
        auto& ins = std::get<FP_INSERT>(*impl_->st_);
        ins.parameters.fileId = fp.file_id;
        ins.parameters.mtimeNs = fp.mtime_ns;
        ins.parameters.sizeBytes = fp.size;
        ins.parameters.cmdDigest = to_string_param(fp.cmd_digest);
        if (impl_->run(ins).affected_rows != 0)
        {
            return;
        }
        auto& upd = std::get<FP_UPDATE>(*impl_->st_);
        upd.parameters.fileId = fp.file_id;
        upd.parameters.mtimeNs = fp.mtime_ns;
        upd.parameters.sizeBytes = fp.size;
        upd.parameters.cmdDigest = to_string_param(fp.cmd_digest);
        impl_->run(upd);
    }

    void GraphStore::erase_fingerprint(FileId file, std::string_view cmd_digest)
    {
        auto& del = std::get<FP_DELETE>(*impl_->st_);
        del.parameters.fileId = file;
        del.parameters.cmdDigest = to_string_param(cmd_digest);
        impl_->run(del);
    }

    void GraphStore::set_failure(const FailureRow& failure)
    {
        auto& ins = std::get<FAIL_INSERT>(*impl_->st_);
        ins.parameters.fileId = failure.file_id;
        ins.parameters.cmdDigest = to_string_param(failure.cmd_digest);
        ins.parameters.contentHash = to_string_param(failure.content_hash);
        ins.parameters.mtimeNs = failure.mtime_ns;
        ins.parameters.sizeBytes = failure.size;
        ins.parameters.engineToken = to_string_param(failure.engine_token);
        ins.parameters.feedDigest = to_string_param(failure.feed_digest);
        ins.parameters.rootsDigest = to_string_param(failure.roots_digest);
        ins.parameters.failureClass = to_string_param(failure.failure_class);
        ins.parameters.failureText = to_string_param(failure.failure_text);
        ins.parameters.depsJson = to_string_param(failure.deps_json);
        ins.parameters.attempts = failure.attempts;
        ins.parameters.firstFailedAt = to_string_param(failure.first_failed_at);
        ins.parameters.lastFailedAt = to_string_param(failure.last_failed_at);
        ins.parameters.retryAfter = failure.retry_after;
        if (impl_->run(ins).affected_rows != 0)
        {
            return;
        }
        auto& upd = std::get<FAIL_UPDATE>(*impl_->st_);
        upd.parameters.fileId = failure.file_id;
        upd.parameters.cmdDigest = to_string_param(failure.cmd_digest);
        upd.parameters.contentHash = to_string_param(failure.content_hash);
        upd.parameters.mtimeNs = failure.mtime_ns;
        upd.parameters.sizeBytes = failure.size;
        upd.parameters.engineToken = to_string_param(failure.engine_token);
        upd.parameters.feedDigest = to_string_param(failure.feed_digest);
        upd.parameters.rootsDigest = to_string_param(failure.roots_digest);
        upd.parameters.failureClass = to_string_param(failure.failure_class);
        upd.parameters.failureText = to_string_param(failure.failure_text);
        upd.parameters.depsJson = to_string_param(failure.deps_json);
        upd.parameters.attempts = failure.attempts;
        upd.parameters.firstFailedAt = to_string_param(failure.first_failed_at);
        upd.parameters.lastFailedAt = to_string_param(failure.last_failed_at);
        upd.parameters.retryAfter = failure.retry_after;
        impl_->run(upd);
    }

    void GraphStore::erase_failure(FileId file, std::string_view cmd_digest)
    {
        auto& del = std::get<FAIL_DELETE>(*impl_->st_);
        del.parameters.fileId = file;
        del.parameters.cmdDigest = to_string_param(cmd_digest);
        impl_->run(del);
    }

    void GraphStore::set_bulk_cache(bool on)
    {
        impl_->run(
            on ? "PRAGMA cache_size = -65536" : "PRAGMA cache_size = -2000");
    }

    void GraphStore::set_busy_timeout_ms(std::int64_t ms)
    {
        impl_->run(std::format("PRAGMA busy_timeout = {}", ms));
    }

    void GraphStore::enqueue_pending(std::span<const std::string> paths)
    {
        if (paths.empty())
        {
            return;
        }
        auto& ins = std::get<PENDING_INSERT>(*impl_->st_);
        Tx    t{*this, true};
        for (const std::string& path : paths)
        {
            ins.parameters.path = path;
            impl_->run(ins);
        }
        t.commit();
    }

    auto GraphStore::claim_update(
        std::int64_t     lease_ms,
        std::string_view holder) -> bool
    {
        Tx t{*this, true};
        // 声明行存绝对 unix 纪元毫秒（租约协议的存活判据）：读它的另一个进程
        // 无需机器自身之外的共享时钟即可判定存活。
        const std::int64_t now = cg::util::cstd::epoch_ms();
        bool               live = false;
        // 接管审计的上下文（live=false 且确有旧行时随接管同事务落表）。
        const char*  recover_reason = nullptr;
        std::string  prev_holder;
        std::int64_t prev_pid = 0;
        std::int64_t prev_lease = 0;
        {
            auto& sel = std::get<CLAIM_BY_ID>(*impl_->st_);
            sel.parameters.id = 1;
            auto rows = impl_->run(sel);
            for (const auto& row : rows)
            {
                live = row.leaseUntil > now; // 过期 => 可窃取
                prev_holder = std::string{row.holder};
                prev_lease = static_cast<std::int64_t>(row.leaseUntil);
                if (!live)
                {
                    recover_reason = "lease-expired";
                }
                if (live)
                {
                    // 孤儿租约：持有进程已被宿主超时杀死（PostToolUse hook
                    // 超时即典型现场），lease_until 却仍是未来时间戳。只按
                    // 时间戳判定会让后续每个事件都入队后退出，队列停摆到
                    // 租约自然到期。持有者记号内嵌 pid 时立即探活；无 pid
                    // 的记号（测试/手写）退回纯租约语义。
                    const std::string  holder_text{row.holder};
                    const std::int64_t pid = holder_pid(holder_text);
                    if (pid > 0 && !cg::util::platform::process_alive(pid))
                    {
                        live = false;
                        prev_pid = pid;
                        recover_reason = "pid-dead";
                    }
                }
            }
        }
        if (live)
        {
            t.commit();
            return false;
        }
        const tab::UpdateClaim uc{};
        const std::int64_t     until = now + lease_ms;
        impl_->run(
            sqlpp::sqlite3::insert_into(uc)
                .set(
                    uc.id = std::int64_t{1},
                    uc.holder = holder,
                    uc.leaseUntil = until)
                .on_conflict(uc.id)
                .do_update(uc.holder = holder, uc.leaseUntil = until));
        if (recover_reason != nullptr && !prev_holder.empty())
        {
            // 与接管同一事务提交（见 log_coord_event_tx_ 的纪律注释）：
            // 被杀检测与孤儿接管是同一事件（orphan-recover），reason
            // 区分 pid-dead 与 lease-expired。
            (void)log_coord_event_tx_(
                CoordEventInput{
                    .event = CoordEvent::OrphanRecover,
                    .actor_holder = holder,
                    .other_holder = prev_holder,
                    .actor_pid = holder_pid(holder),
                    .other_pid = prev_pid,
                    .reason = recover_reason,
                    .detail = std::format(
                        "{{\"prevLeaseUntil\":{},\"prevHolderPid\":{}}}",
                        prev_lease,
                        prev_pid),
                });
            if (prev_pid > 0)
            {
                // 只有确证进程已死才补记（租约自然过期的持有者可能
                // 还活着，它的收尾仍会自己来）。
                abandon_dangling_runs_tx_(prev_pid, holder);
            }
        }
        t.commit();
        return true;
    }

    auto GraphStore::steal_update(
        std::int64_t     lease_ms,
        std::string_view holder,
        std::string_view audit_detail)
        -> std::optional<std::pair<std::string, std::int64_t>>
    {
        Tx                                                  t{*this, true};
        std::optional<std::pair<std::string, std::int64_t>> displaced;
        {
            auto& sel = std::get<CLAIM_BY_ID>(*impl_->st_);
            sel.parameters.id = 1;
            auto rows = impl_->run(sel);
            for (const auto& row : rows)
            {
                displaced = std::pair{
                    std::string{row.holder},
                    static_cast<std::int64_t>(row.leaseUntil)};
            }
        }
        const tab::UpdateClaim uc{};
        const std::int64_t     until = cg::util::cstd::epoch_ms() + lease_ms;
        impl_->run(
            sqlpp::sqlite3::insert_into(uc)
                .set(
                    uc.id = std::int64_t{1},
                    uc.holder = holder,
                    uc.leaseUntil = until)
                .on_conflict(uc.id)
                .do_update(uc.holder = holder, uc.leaseUntil = until));
        // 只在顶替了仍活的声明时记 steal（过期/空行的接管是普通自愈，
        // 由 claim_update 的 orphan-recover/expire 审计覆盖，与调用方
        // "只在顶替活声明时出注记"的口径一致）；与偷取同一事务提交。
        if (displaced.has_value() && !displaced->first.empty()
            && displaced->second > cg::util::cstd::epoch_ms())
        {
            (void)log_coord_event_tx_(
                CoordEventInput{
                    .event = CoordEvent::Steal,
                    .actor_holder = holder,
                    .other_holder = displaced->first,
                    .actor_pid = holder_pid(holder),
                    .other_pid = holder_pid(displaced->first),
                    .reason = "index-takeover",
                    .detail = audit_detail.empty()
                                  ? std::format(
                                        R"({{"takeoverLeaseMs":{}}})",
                                        lease_ms)
                                  : std::string{audit_detail},
                });
        }
        t.commit();
        return displaced;
    }

    auto GraphStore::peek_pending(
        std::int64_t     lease_ms,
        std::string_view holder) -> PendingPeek
    {
        PendingPeek out;
        Tx          t{*this, true};
        {
            auto& beat = std::get<CLAIM_HEARTBEAT>(*impl_->st_);
            beat.parameters.id = 1;
            beat.parameters.holder = to_string_param(holder);
            beat.parameters.leaseUntil = cg::util::cstd::epoch_ms() + lease_ms;
            if (impl_->run(beat).affected_rows == 0)
            {
                out.claim_lost = true;
                t.commit();
                return out;
            }
        }
        {
            auto& sel = std::get<PENDING_SELECT>(*impl_->st_);
            auto  rows = impl_->run(sel);
            for (const auto& row : rows)
            {
                out.files.emplace_back(row.path);
            }
        }
        // 刻意不删：批次结束后的 clear_covered 才是唯一的出队点。
        t.commit();
        return out;
    }

    auto GraphStore::clear_covered(
        std::span<const PendingClearInput>                    entries,
        const std::function<std::string(const std::string&)>& current_hash,
        std::string_view                                      holder,
        std::int64_t lease_ms) -> std::size_t
    {
        if (entries.empty())
        {
            return 0;
        }
        auto& del = std::get<PENDING_DELETE>(*impl_->st_);
        auto& ins = std::get<PENDING_INSERT>(*impl_->st_);
        Tx    t{*this, true};
        if (!holder.empty())
        {
            // 同一租约的守护：入队判定与删除必须发生在持有者仍拥有声明
            // 的期间。心跳匹配零行 = 运行中租约过期已被窃，新持有者拥有
            // 队列，本次出队整体放弃（行保留，下一批重新处理，安全）。
            auto& beat = std::get<CLAIM_HEARTBEAT>(*impl_->st_);
            beat.parameters.id = 1;
            beat.parameters.holder = to_string_param(holder);
            beat.parameters.leaseUntil = cg::util::cstd::epoch_ms() + lease_ms;
            if (impl_->run(beat).affected_rows == 0)
            {
                t.commit();
                return 0;
            }
        }
        // 库内记录的内容哈希：编辑 → 回退到原字节时，快照哈希与当前
        // 磁盘哈希会重新相等（决定哈希看不出编辑发生过），但引擎可能
        // 已经把中间状态写进了 files.content_hash。两者不一致说明索引
        // 不是当前字节的结论——保留队列行，让下一批重析。
        std::size_t removed = 0;
        for (const PendingClearInput& entry : entries)
        {
            if (!entry.tracked)
            {
                del.parameters.path = entry.path;
                removed +=
                    static_cast<std::size_t>(impl_->run(del).affected_rows);
                continue;
            }
            if (!entry.covered)
            {
                continue; // 本批没处理它：保留脏标记
            }
            // 判定哈希在 IMMEDIATE 事务内重算：调用方事务外算好的哈希与
            // DELETE 之间若发生编辑，旧实现会把编辑者的并发入队一并删掉。
            const std::string decided = current_hash(entry.path);
            if (decided != entry.snapshot_hash)
            {
                continue; // 批运行期间又变了：保留给下一批
            }
            // 批内 edit→revert 的残余窗口：字节已回到原样，但库里的
            // content_hash 记录的是中间状态。此时删除队列行就会永久留下
            // 一个与磁盘不符的索引，故索引哈希必须等于决定哈希。
            // 缺失（文件在批内被删除并 purge、或从未成功哈希）不构成
            // 反证：保守到永远保留会让持有者循环永不收敛（peek 非空→
            // 重跑→再保留），因此只在"有记录且不一致"时保留。
            const std::optional<std::string> indexed =
                indexed_hash_of_(entry.path);
            if (indexed.has_value() && *indexed != decided)
            {
                continue;
            }
            del.parameters.path = entry.path;
            const auto affected = impl_->run(del).affected_rows;
            if (affected == 0)
            {
                continue; // 行已不在队列（并发出队）：无事可做
            }
            removed += static_cast<std::size_t>(affected);
            // 删除后复核，封住最后一个窗口：哈希读取完成后、DELETE 执行
            // 前落地的编辑可能已经入队，却被同一条 DELETE 吃掉。此刻重读
            // 字节；与决策值不同就立即插回（DELETE 之后 INSERT 是全新行，
            // 不会被本事务已删的行挡住）。若编辑发生在本复核之后，它的
            // 入队必然晚于本次 DELETE，行自然存活。
            const std::string after = current_hash(entry.path);
            if (after != decided)
            {
                ins.parameters.path = entry.path;
                impl_->run(ins); // INSERT .. DO NOTHING
            }
        }
        t.commit();
        return removed;
    }

    auto GraphStore::release_update(std::string_view holder) -> bool
    {
        auto& del = std::get<CLAIM_RELEASE>(*impl_->st_);
        del.parameters.id = 1;
        del.parameters.holder = to_string_param(holder);
        return impl_->run(del).affected_rows != 0;
    }

    auto GraphStore::heartbeat_in_tx(
        std::string_view holder,
        std::int64_t     lease_ms) -> bool
    {
        auto& beat = std::get<CLAIM_HEARTBEAT>(*impl_->st_);
        beat.parameters.id = 1;
        beat.parameters.holder = to_string_param(holder);
        beat.parameters.leaseUntil = cg::util::cstd::epoch_ms() + lease_ms;
        return impl_->run(beat).affected_rows != 0;
    }

    auto GraphStore::renew_update_lease(
        std::string_view holder,
        std::int64_t     lease_ms) -> bool
    {
        // 与 peek_pending 同款自足短事务形态：BEGIN IMMEDIATE 抢到锁的
        // 瞬间完成判定与续期，BUSY 按异常上抛交调用方处置（见块注释）。
        Tx         t{*this, true};
        const bool renewed = heartbeat_in_tx(holder, lease_ms);
        t.commit();
        return renewed;
    }

    auto GraphStore::claim_snapshot() -> cg::db::ClaimSnapshot
    {
        ClaimSnapshot out;
        {
            // DEFERRED 读事务：批次可能正持有写锁，读在 WAL 下不受影响；
            // 结果循环全部跑完再提交，绝不中途弃置游标。
            Tx t{*this, false};
            {
                auto& sel = std::get<CLAIM_BY_ID>(*impl_->st_);
                sel.parameters.id = 1;
                auto rows = impl_->run(sel);
                for (const auto& row : rows)
                {
                    out.has_claim = true;
                    out.holder = std::string{row.holder};
                    out.lease_until = static_cast<std::int64_t>(row.leaseUntil);
                }
            }
            {
                auto& sel = std::get<PENDING_SELECT>(*impl_->st_);
                auto  rows = impl_->run(sel);
                for (const auto& row : rows)
                {
                    out.pending.emplace_back(row.path);
                }
            }
            t.commit();
        }
        // 探活在事务之外：OpenProcess 等系统调用不得跨事务持有读快照。
        if (out.has_claim && out.lease_until > cg::util::cstd::epoch_ms())
        {
            out.live = true;
            out.pid = holder_pid(out.holder);
            if (out.pid > 0 && !cg::util::platform::process_alive(out.pid))
            {
                out.live = false;
            }
        }
        return out;
    }

    bool GraphStore::log_coord_event_tx_(const cg::db::CoordEventInput& input)
    {
        try
        {
            sqlite3_stmt* stmt = impl_->coord_stmt_(
                impl_->coord_ins_,
                "insert into coord_events"
                "(ts_ms,event,actor_holder,other_holder,actor_pid,other_pid,"
                "reason,detail) values(?,?,?,?,?,?,?,?)");
            if (stmt == nullptr)
            {
                return false;
            }
            // 空 string_view 的 data() 可为 nullptr——bind_coord_text 会把
            // 它绑成 NULL，撞上 NOT NULL 列（DEFAULT 只对缺省生效）；一律
            // 回退空串。
            const std::string event{name_of(input.event)};
            sqlite3_bind_int64(stmt, 1, cg::util::cstd::epoch_ms());
            bind_coord_text(stmt, 2, event);
            bind_coord_text(stmt, 3, input.actor_holder);
            bind_coord_text(stmt, 4, input.other_holder);
            sqlite3_bind_int64(stmt, 5, input.actor_pid);
            sqlite3_bind_int64(stmt, 6, input.other_pid);
            bind_coord_text(stmt, 7, input.reason);
            bind_coord_text(stmt, 8, input.detail);
            const int rc = sqlite3_step(stmt);
            return rc == SQLITE_DONE;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    void GraphStore::abandon_dangling_runs_tx_(
        std::int64_t     dead_pid,
        std::string_view discoverer)
    {
        try
        {
            sqlite3_stmt* stmt = impl_->coord_stmt_(
                impl_->coord_dangling_,
                "select actor_holder, ts_ms from coord_events s "
                "where s.event = 'run-start' and s.actor_pid = ? "
                "and not exists (select 1 from coord_events f "
                "where f.event = 'run-finish' and f.actor_pid = ? "
                "and f.ts_ms >= s.ts_ms)");
            if (stmt == nullptr)
            {
                return;
            }
            sqlite3_bind_int64(stmt, 1, dead_pid);
            sqlite3_bind_int64(stmt, 2, dead_pid);
            const std::int64_t now = cg::util::cstd::epoch_ms();
            int                rc = SQLITE_ROW;
            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
            {
                const unsigned char* holder_text = sqlite3_column_text(stmt, 0);
                const std::int64_t   start_ts = sqlite3_column_int64(stmt, 1);
                const std::string    run_holder =
                    holder_text == nullptr
                        ? std::string{}
                        : std::string{
                              reinterpret_cast<const char*>(holder_text)};
                (void)log_coord_event_tx_(
                    CoordEventInput{
                        .event = CoordEvent::RunFinish,
                        .actor_holder = run_holder,
                        .other_holder = discoverer,
                        .actor_pid = dead_pid,
                        .other_pid = holder_pid(discoverer),
                        .reason = "abandoned",
                        .detail = std::format(
                            "{{\"abandonedAtMs\":{},\"startTsMs\":{}}}",
                            now,
                            start_ts),
                    });
            }
        }
        catch (const std::exception&)
        {
        }
    }

    bool GraphStore::log_coord_event(const cg::db::CoordEventInput& input)
    {
        try
        {
            Tx         t{*this, true};
            const bool ok = log_coord_event_tx_(input);
            t.commit();
            return ok;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    auto GraphStore::coord_events_since(std::int64_t since_ms)
        -> std::vector<cg::db::CoordEventRow>
    {
        std::vector<CoordEventRow> out;
        try
        {
            sqlite3_stmt* stmt = impl_->coord_stmt_(
                impl_->coord_sel_,
                "select id,ts_ms,event,actor_holder,other_holder,actor_pid,"
                "other_pid,reason,detail from coord_events where ts_ms >= ? "
                "order by ts_ms,id");
            if (stmt == nullptr)
            {
                return out;
            }
            sqlite3_bind_int64(stmt, 1, since_ms);
            int rc = SQLITE_ROW;
            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
            {
                CoordEventRow row;
                row.id = sqlite3_column_int64(stmt, 0);
                row.ts_ms = sqlite3_column_int64(stmt, 1);
                row.event = coord_column_text(stmt, 2);
                row.actor_holder = coord_column_text(stmt, 3);
                row.other_holder = coord_column_text(stmt, 4);
                row.actor_pid = sqlite3_column_int64(stmt, 5);
                row.other_pid = sqlite3_column_int64(stmt, 6);
                row.reason = coord_column_text(stmt, 7);
                row.detail = coord_column_text(stmt, 8);
                out.push_back(std::move(row));
            }
        }
        catch (const std::exception&)
        {
        }
        return out;
    }

    auto GraphStore::coord_event_counts_since(std::int64_t since_ms)
        -> std::vector<std::pair<std::string, std::int64_t>>
    {
        std::vector<std::pair<std::string, std::int64_t>> out;
        try
        {
            sqlite3_stmt* stmt = impl_->coord_stmt_(
                impl_->coord_counts_,
                "select event,count(*) from coord_events where ts_ms >= ? "
                "group by event order by event");
            if (stmt == nullptr)
            {
                return out;
            }
            sqlite3_bind_int64(stmt, 1, since_ms);
            int rc = SQLITE_ROW;
            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
            {
                const unsigned char* text = sqlite3_column_text(stmt, 0);
                out.emplace_back(
                    text == nullptr
                        ? std::string{}
                        : std::string{reinterpret_cast<const char*>(text)},
                    sqlite3_column_int64(stmt, 1));
            }
        }
        catch (const std::exception&)
        {
        }
        return out;
    }

    auto GraphStore::coord_events_age(
        std::int64_t keep_ms,
        std::int64_t cap_rows) -> std::size_t
    {
        std::size_t removed = 0;
        try
        {
            Tx            t{*this, true};
            sqlite3_stmt* age = impl_->coord_stmt_(
                impl_->coord_age_,
                "delete from coord_events where ts_ms < ?");
            if (age != nullptr)
            {
                sqlite3_bind_int64(
                    age,
                    1,
                    cg::util::cstd::epoch_ms() - keep_ms);
                if (sqlite3_step(age) == SQLITE_DONE)
                {
                    removed += static_cast<std::size_t>(
                        sqlite3_changes(impl_->db_.native_handle()));
                }
            }
            // 行数封顶：总量超 cap 时按 ts 新者保留。COUNT 走主键索引，
            // 空表/小表上是微秒级——老化入口可以挂在每次 flush 上。
            sqlite3_stmt* trim = impl_->coord_stmt_(
                impl_->coord_trim_,
                "delete from coord_events where id not in "
                "(select id from coord_events order by ts_ms desc, id desc "
                "limit ?)");
            std::int64_t total = 0;
            {
                sqlite3_stmt* cnt = impl_->coord_stmt_(
                    impl_->coord_cnt_,
                    "select count(*) from coord_events");
                if (cnt != nullptr && sqlite3_step(cnt) == SQLITE_ROW)
                {
                    total = sqlite3_column_int64(cnt, 0);
                }
            }
            if (trim != nullptr && total > cap_rows)
            {
                sqlite3_bind_int64(trim, 1, cap_rows);
                if (sqlite3_step(trim) == SQLITE_DONE)
                {
                    removed += static_cast<std::size_t>(
                        sqlite3_changes(impl_->db_.native_handle()));
                }
            }
            t.commit();
        }
        catch (const std::exception&)
        {
        }
        return removed;
    }

    auto GraphStore::head_cache_load(std::int64_t row_ver)
        -> std::vector<cg::db::HeadCacheRow>
    {
        std::vector<HeadCacheRow> out;
        try
        {
            sqlite3_stmt* sel = impl_->side_stmt_(
                impl_->hc_sel_,
                "select file_id,main_id,variant_digest,closure_digest,"
                "payload,ver from head_cache where ver = ?");
            if (sel == nullptr)
            {
                return out;
            }
            sqlite3_bind_int64(sel, 1, row_ver);
            int rc = SQLITE_ROW;
            while ((rc = sqlite3_step(sel)) == SQLITE_ROW)
            {
                const auto* payload = static_cast<const unsigned char*>(
                    sqlite3_column_blob(sel, 4));
                const int    payload_len = sqlite3_column_bytes(sel, 4);
                HeadCacheRow row;
                row.file_id = sqlite3_column_int64(sel, 0);
                row.main_id = sqlite3_column_int64(sel, 1);
                row.variant_digest.assign(
                    reinterpret_cast<const char*>(sqlite3_column_text(sel, 2)),
                    static_cast<std::size_t>(sqlite3_column_bytes(sel, 2)));
                row.closure_digest.assign(
                    reinterpret_cast<const char*>(sqlite3_column_text(sel, 3)),
                    static_cast<std::size_t>(sqlite3_column_bytes(sel, 3)));
                if (payload != nullptr && payload_len > 0)
                {
                    row.payload.assign(
                        reinterpret_cast<const char*>(payload),
                        static_cast<std::size_t>(payload_len));
                }
                row.ver = sqlite3_column_int64(sel, 5);
                out.push_back(std::move(row));
            }
        }
        catch (const std::exception&)
        {
        }
        return out;
    }

    auto GraphStore::head_cache_store(
        const std::vector<cg::db::HeadCacheRow>& entries,
        const std::vector<cg::db::FileId>&       invalidate) -> std::size_t
    {
        std::size_t stored = 0;
        try
        {
            sqlite3_stmt* del = impl_->side_stmt_(
                impl_->hc_del_file_,
                "delete from head_cache where file_id = ?");
            if (del != nullptr)
            {
                for (const FileId fid : invalidate)
                {
                    sqlite3_bind_int64(del, 1, fid);
                    sqlite3_step(del);
                    sqlite3_reset(del);
                    sqlite3_clear_bindings(del);
                }
            }
            sqlite3_stmt* ins = impl_->side_stmt_(
                impl_->hc_ins_,
                "insert or replace into head_cache"
                "(file_id,main_id,variant_digest,closure_digest,payload,ver) "
                "values(?,?,?,?,?,?)");
            if (ins == nullptr)
            {
                return stored;
            }
            for (const HeadCacheRow& row : entries)
            {
                sqlite3_bind_int64(ins, 1, row.file_id);
                sqlite3_bind_int64(ins, 2, row.main_id);
                sqlite3_bind_text(
                    ins,
                    3,
                    row.variant_digest.c_str(),
                    static_cast<int>(row.variant_digest.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_text(
                    ins,
                    4,
                    row.closure_digest.c_str(),
                    static_cast<int>(row.closure_digest.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_blob(
                    ins,
                    5,
                    row.payload.data(),
                    static_cast<int>(row.payload.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_int64(ins, 6, row.ver);
                if (sqlite3_step(ins) == SQLITE_DONE)
                {
                    ++stored;
                }
                sqlite3_reset(ins);
                sqlite3_clear_bindings(ins);
            }
        }
        catch (const std::exception&)
        {
        }
        return stored;
    }

    auto GraphStore::file_instr_digest_all()
        -> std::unordered_map<cg::db::FileId, cg::db::FileInstrDigestRow>
    {
        std::unordered_map<FileId, FileInstrDigestRow> out;
        try
        {
            sqlite3_stmt* sel = impl_->side_stmt_(
                impl_->fid_sel_,
                "select file_id,d_narrow,d_all,d_include,n_narrow,n_all,"
                "n_include,ver from file_instr_digest where ver = ?");
            if (sel == nullptr)
            {
                return out;
            }
            sqlite3_bind_int64(sel, 1, head_cache_side_ver);
            int rc = SQLITE_ROW;
            while ((rc = sqlite3_step(sel)) == SQLITE_ROW)
            {
                FileInstrDigestRow row;
                row.file_id = sqlite3_column_int64(sel, 0);
                row.d_narrow.assign(
                    reinterpret_cast<const char*>(sqlite3_column_text(sel, 1)),
                    static_cast<std::size_t>(sqlite3_column_bytes(sel, 1)));
                row.d_all.assign(
                    reinterpret_cast<const char*>(sqlite3_column_text(sel, 2)),
                    static_cast<std::size_t>(sqlite3_column_bytes(sel, 2)));
                row.d_include.assign(
                    reinterpret_cast<const char*>(sqlite3_column_text(sel, 3)),
                    static_cast<std::size_t>(sqlite3_column_bytes(sel, 3)));
                row.n_narrow = sqlite3_column_int64(sel, 4);
                row.n_all = sqlite3_column_int64(sel, 5);
                row.n_include = sqlite3_column_int64(sel, 6);
                row.ver = sqlite3_column_int64(sel, 7);
                out.emplace(row.file_id, std::move(row));
            }
        }
        catch (const std::exception&)
        {
        }
        return out;
    }

    void GraphStore::file_instr_digest_store(
        const std::vector<cg::db::FileInstrDigestRow>& rows)
    {
        try
        {
            sqlite3_stmt* ins = impl_->side_stmt_(
                impl_->fid_ins_,
                "insert or replace into file_instr_digest"
                "(file_id,d_narrow,d_all,d_include,n_narrow,n_all,n_include,"
                "ver) values(?,?,?,?,?,?,?,?)");
            if (ins == nullptr)
            {
                return;
            }
            for (const FileInstrDigestRow& row : rows)
            {
                sqlite3_bind_int64(ins, 1, row.file_id);
                sqlite3_bind_text(
                    ins,
                    2,
                    row.d_narrow.c_str(),
                    static_cast<int>(row.d_narrow.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_text(
                    ins,
                    3,
                    row.d_all.c_str(),
                    static_cast<int>(row.d_all.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_text(
                    ins,
                    4,
                    row.d_include.c_str(),
                    static_cast<int>(row.d_include.size()),
                    SQLITE_TRANSIENT);
                sqlite3_bind_int64(ins, 5, row.n_narrow);
                sqlite3_bind_int64(ins, 6, row.n_all);
                sqlite3_bind_int64(ins, 7, row.n_include);
                sqlite3_bind_int64(ins, 8, head_cache_side_ver);
                sqlite3_step(ins);
                sqlite3_reset(ins);
                sqlite3_clear_bindings(ins);
            }
        }
        catch (const std::exception&)
        {
        }
    }

    auto GraphStore::update_claim_holder()
        -> std::optional<std::pair<std::string, std::int64_t>>
    {
        std::optional<std::pair<std::string, std::int64_t>> out;
        auto& sel = std::get<CLAIM_BY_ID>(*impl_->st_);
        sel.parameters.id = 1;
        auto rows = impl_->run(sel);
        for (const auto& row : rows)
        {
            out = std::pair{
                std::string{row.holder},
                static_cast<std::int64_t>(row.leaseUntil)};
        }
        return out;
    }

    void GraphStore::purge_file(FileId file)
    {
        // 行生死仲裁收敛：该文件上的符号行生死由贡献票数仲裁——键失配
        // 只说明“本批新提取不再发射它”，行的删除发生在票数归零时
        // （erase_symbols，dispatch 装载前）。此处只清“按文件替换”的
        // 派生行（站点边/包含/导入/线索/调用点），它们随本批 upsert
        // 重写。
        {
            auto& del = std::get<DEL_EDGES_SITE>(*impl_->st_);
            del.parameters.siteFileId = file;
            impl_->run(del);
        }
        {
            auto& del = std::get<DEL_INCLUDES>(*impl_->st_);
            del.parameters.fileId = file;
            impl_->run(del);
        }
        {
            auto& del = std::get<DEL_IMPORTS>(*impl_->st_);
            del.parameters.fileId = file;
            impl_->run(del);
        }
        // schema v10 派生数据：线索与调用点严格按拥有文件替换。
        {
            auto& del = std::get<HINT_DELETE_FILE>(*impl_->st_);
            del.parameters.fileId = file;
            impl_->run(del);
        }
        {
            auto& del = std::get<SITE_DELETE_FILE>(*impl_->st_);
            del.parameters.fileId = file;
            impl_->run(del);
        }
    }

    void GraphStore::erase_symbols(std::span<const SymId> ids)
    {
        if (ids.empty())
        {
            return;
        }
        auto& del = std::get<DEL_SYM_BY_ID>(*impl_->st_);
        for (const SymId id : ids)
        {
            del.parameters.symId = id;
            impl_->run(del);
        }
    }

    void GraphStore::contribute_symbols(
        FileId                 file,
        std::string_view       digest,
        std::span<const SymId> ids)
    {
        if (ids.empty())
        {
            return;
        }
        auto& ins = std::get<CONTRIB_INSERT>(*impl_->st_);
        for (const SymId id : ids)
        {
            ins.parameters.symId = id;
            ins.parameters.fileId = file;
            ins.parameters.cmdDigest = digest;
            impl_->run(ins);
        }
    }

    auto GraphStore::contributions_of_entry(
        FileId           file,
        std::string_view digest) -> std::vector<SymId>
    {
        std::vector<SymId> out;
        auto&              sel = std::get<CONTRIB_OF_ENTRY>(*impl_->st_);
        sel.parameters.fileId = file;
        sel.parameters.cmdDigest = digest;
        auto rows = impl_->run(sel);
        for (const auto& row : rows)
        {
            out.push_back(static_cast<SymId>(row.symId));
        }
        return out;
    }

    void GraphStore::retract_contribution(
        SymId            sym,
        FileId           file,
        std::string_view digest)
    {
        auto& del = std::get<CONTRIB_DEL_ONE>(*impl_->st_);
        del.parameters.symId = sym;
        del.parameters.fileId = file;
        del.parameters.cmdDigest = digest;
        impl_->run(del);
    }

    auto GraphStore::retract_entry_contributions(
        FileId           file,
        std::string_view digest) -> std::vector<SymId>
    {
        // 先读后删：受影响符号集是归零仲裁的输入，必须在删除前物化。
        std::vector<SymId> affected = contributions_of_entry(file, digest);
        auto&              del = std::get<CONTRIB_DEL_ENTRY>(*impl_->st_);
        del.parameters.fileId = file;
        del.parameters.cmdDigest = digest;
        impl_->run(del);
        return affected;
    }

    auto GraphStore::contributions_of_file(FileId file) -> std::vector<SymId>
    {
        std::vector<SymId> out;
        auto&              sel = std::get<CONTRIB_OF_FILE>(*impl_->st_);
        sel.parameters.fileId = file;
        auto rows = impl_->run(sel);
        for (const auto& row : rows)
        {
            out.push_back(static_cast<SymId>(row.symId));
        }
        return out;
    }

    auto GraphStore::retract_file_contributions(FileId file)
        -> std::vector<SymId>
    {
        // 先读后删（条目整撤同款纪律）：受影响符号集是归零仲裁的输入。
        std::vector<SymId> affected = contributions_of_file(file);
        auto&              del = std::get<CONTRIB_DEL_FILE>(*impl_->st_);
        del.parameters.fileId = file;
        impl_->run(del);
        return affected;
    }

    auto GraphStore::zero_contribution_symbols(
        std::span<const SymId> candidates) -> std::vector<SymId>
    {
        std::vector<SymId> out;
        auto&              sel = std::get<CONTRIB_EXISTS>(*impl_->st_);
        for (const SymId id : candidates)
        {
            sel.parameters.symId = id;
            auto rows = impl_->run(sel);
            bool has_vote = false;
            // 循环跑到 DONE（LIMIT 1 最多一行；本店游标纪律）。
            for (const auto& row : rows)
            {
                static_cast<void>(row);
                has_vote = true;
            }
            if (!has_vote)
            {
                out.push_back(id);
            }
        }
        return out;
    }

    void GraphStore::erase_file(FileId file)
    {
        auto& del = std::get<DEL_FILE>(*impl_->st_);
        del.parameters.fileId = file;
        impl_->run(del);
        // 行已删除：路径缓存整体作废（失效规则见 file_path 的文档块）。
        file_path_cache_.clear();
    }

    void GraphStore::set_meta(std::string_view key, std::string_view value)
    {
        const tab::Meta m{};
        impl_->run(
            sqlpp::sqlite3::insert_into(m)
                .set(m.key = key, m.value = value)
                .on_conflict(m.key)
                .do_update(m.value = value));
    }

    auto GraphStore::meta(std::string_view key) -> std::optional<std::string>
    {
        const tab::Meta m{};
        auto            rows =
            impl_->run(sqlpp::select(m.value).from(m).where(m.key == key));
        std::optional<std::string> out;
        for (const auto& row : rows)
        {
            out = std::string{row.value};
        }
        return out;
    }

    void GraphStore::erase_meta(std::string_view key)
    {
        const tab::Meta m{};
        impl_->run(sqlpp::delete_from(m).where(m.key == key));
    }

    auto GraphStore::symbol_row(SymId id) -> std::optional<SymbolRow>
    {
        SymbolRow            out;
        StrId                name_id = 0;
        StrId                ns_id = 0;
        StrId                key_id = 0;
        std::optional<SymId> parent_id;
        bool                 found = false;
        {
            auto& sel = std::get<SYM_BY_ID>(*impl_->st_);
            sel.parameters.symId = id;
            auto rows = impl_->run(sel);
            for (const auto& row : rows)
            {
                found = true;
                out.id = static_cast<SymId>(row.symId);
                out.kind =
                    symbol_kind_of(static_cast<std::int64_t>(row.kindId));
                name_id = static_cast<StrId>(row.nameId);
                ns_id = static_cast<StrId>(row.nsId);
                key_id = static_cast<StrId>(row.uniqueKeyId);
                out.sig_hash =
                    row.sigHash.has_value()
                        ? std::optional{static_cast<std::int64_t>(*row.sigHash)}
                        : std::nullopt;
                out.file_id = static_cast<FileId>(row.fileId);
                out.line = static_cast<int>(row.line);
                out.col = static_cast<int>(row.col);
                if (row.parentId.has_value())
                {
                    parent_id = static_cast<SymId>(*row.parentId);
                }
                out.flags = static_cast<std::uint32_t>(
                    static_cast<std::int64_t>(row.flags));
                if (row.paramTypes.has_value())
                {
                    out.param_types = std::string{*row.paramTypes};
                }
                if (row.templateParams.has_value())
                {
                    out.template_params = std::string{*row.templateParams};
                }
            }
        }
        if (!found)
        {
            return std::nullopt;
        }

        out.name = str_of(name_id);
        out.ns = str_of(ns_id);
        out.unique_key = str_of(key_id);
        if (const auto path = file_path(out.file_id))
        {
            out.file = *path;
        }
        if (parent_id.has_value())
        {
            out.parent_id = parent_id;
            StrId parent_name_id = 0;
            auto& sel = std::get<SYM_BY_ID>(*impl_->st_);
            sel.parameters.symId = *parent_id;
            auto rows = impl_->run(sel);
            for (const auto& row : rows)
            {
                parent_name_id = static_cast<StrId>(row.nameId);
            }
            out.parent = str_of(parent_name_id);
        }
        out.qualified_name = qualified_name(out.id);
        return out;
    }

    auto GraphStore::edges_from_agg(SymId src) -> std::vector<EdgeAggRow>
    {
        std::vector<EdgeAggRow> out;
        auto&                   sel = std::get<EDGES_FROM_AGG>(*impl_->st_);
        sel.parameters.srcId = src;
        auto rows = impl_->run(sel);
        for (const auto& row : rows)
        {
            out.push_back(
                {.other = static_cast<SymId>(row.dstId),
                 .type = edge_type_of(static_cast<std::int64_t>(row.typeId)),
                 .occurrences_sum = row.occ_sum.value_or(0),
                 .min_site_line = row.site_min.value_or(0),
                 .confidence = row.max_conf.value_or(1.0),
                 .attrs = static_cast<std::uint32_t>(
                     static_cast<std::int64_t>(row.max_attrs.value_or(0)))});
        }
        return out;
    }

    auto GraphStore::edges_to_agg(SymId dst) -> std::vector<EdgeAggRow>
    {
        std::vector<EdgeAggRow> out;
        auto&                   sel = std::get<EDGES_TO_AGG>(*impl_->st_);
        sel.parameters.dstId = dst;
        auto rows = impl_->run(sel);
        for (const auto& row : rows)
        {
            out.push_back(
                {.other = static_cast<SymId>(row.srcId),
                 .type = edge_type_of(static_cast<std::int64_t>(row.typeId)),
                 .occurrences_sum = row.occ_sum.value_or(0),
                 .min_site_line = row.site_min.value_or(0),
                 .confidence = row.max_conf.value_or(1.0),
                 .attrs = static_cast<std::uint32_t>(
                     static_cast<std::int64_t>(row.max_attrs.value_or(0)))});
        }
        return out;
    }

    auto GraphStore::edges_of_raw(SymId id, bool outgoing, EdgeType type)
        -> std::vector<EdgeRow>
    {
        std::vector<EdgeRow> out;
        if (outgoing)
        {
            auto& sel = std::get<EDGES_OF_OUT>(*impl_->st_);
            sel.parameters.srcId = id;
            sel.parameters.typeId = to_id(type);
            auto rows = impl_->run(sel);
            for (const auto& row : rows)
            {
                out.push_back(to_edge_row(row, type));
            }
        }
        else
        {
            auto& sel = std::get<EDGES_OF_IN>(*impl_->st_);
            sel.parameters.dstId = id;
            sel.parameters.typeId = to_id(type);
            auto rows = impl_->run(sel);
            for (const auto& row : rows)
            {
                out.push_back(to_edge_row(row, type));
            }
        }
        return out;
    }

    auto GraphStore::qualified_name(SymId id) -> std::string
    {
        std::vector<std::string> parts;
        std::string              ns;
        std::optional<SymId>     current = id;
        int                      depth = 0;
        while (current.has_value())
        {
            if (++depth > 64)
            { // 损坏的父环；在此截断
                break;
            }
            auto& sel = std::get<SYM_BY_ID>(*impl_->st_);
            sel.parameters.symId = *current;
            bool advanced = false;
            auto rows = impl_->run(sel);
            for (const auto& row : rows)
            {
                parts.push_back(str_of(static_cast<StrId>(row.nameId)));
                ns = str_of(static_cast<StrId>(row.nsId));
                if (row.parentId.has_value())
                {
                    current = static_cast<SymId>(*row.parentId);
                }
                else
                {
                    current.reset();
                }
                advanced = true;
            }
            if (!advanced)
            {
                current.reset();
            }
        }
        std::ranges::reverse(parts);
        std::string qualified = ns;
        for (const std::string& part : parts)
        {
            if (!qualified.empty())
            {
                qualified += "::";
            }
            qualified += part;
        }
        return qualified;
    }

    /**
     * @brief RAII 声明释放（实时协调协议）。
     *
     * @internal 原定义在 cg.cli.update；全量索引的提交段租约（分段认领
     * + 事务内心跳 + 排队）复用同一协议后下沉 db 层，cli.update 改为
     * 引用（行为零变化）。析构函数删除声明行——但只删本持有者拥有的行
     * （release_update 按持有者匹配），故运行中途租约过期被偷走的声明
     * 绝不会删掉偷取者的声明。不可拷贝不可移动：每个声明恰一个拥有者。
     * 析构的守卫方式与 ~Tx 的 ROLLBACK（本文件）和 BulkCacheRestore
     * （graph.build.ixx）完全一致：析构也会在引擎批次异常退栈时运行，若
     * 该失败把连接留在持久错误状态（盘满、I/O 错误），无守卫的释放会从
     * 析构二次抛出并终止进程。垂死持有者留下的声明按普通方式回收：租约
     * 到期、下一个更新者偷走。
     */
    export class UpdateClaimGuard
    {
    public:
        UpdateClaimGuard(GraphStore& store, std::string holder)
            : store_(store), holder_(std::move(holder))
        {
        }
        ~UpdateClaimGuard()
        {
            try
            {
                (void)store_.release_update(holder_);
            }
            catch (...)
            {
            }
        }
        UpdateClaimGuard(const UpdateClaimGuard&) = delete;
        UpdateClaimGuard& operator=(const UpdateClaimGuard&) = delete;
        UpdateClaimGuard(UpdateClaimGuard&&) = delete;
        UpdateClaimGuard& operator=(UpdateClaimGuard&&) = delete;

    private:
        GraphStore& store_;
        std::string holder_;
    };

} // namespace cg::db

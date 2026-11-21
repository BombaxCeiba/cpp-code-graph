module;
#include "db.tables.h"

/**
 * @module cg.db.schema
 *
 * @brief 图数据库的 schema 描述（SCHEMA 规格文档）：全新数据库上逐字经
 *        sqlpp23 连接调用运算符执行的 DDL 文本、每次打开都执行的
 *        PRAGMA 清单、kind/边类型字典（枚举形态）、flag 与 attr 位段、
 *        每个 cg.db 单元共享的 sqlpp23 表模型，以及 store 与 query
 *        模块交换的 id/行/值类型。
 *
 * @internal
 * sqlpp23 表模型放在 db.tables.h（经典头，经本文件上方的全局模块片段
 * include）：MSVC 要求表结构体在每个实例化单元都以全局模块形态可见
 * ——定义在一个模块单元里、再从兄弟单元导入会让 IFC 读取器崩溃
 * （探针矩阵结论：定义进模块再跨模块导入的两种形态（双模块/本模块）
 * 失败，各消费者经全局模块片段包含经典头的形态可行；完整机理见
 * db.tables.h）。
 */
export module cg.db.schema;

import std;
import cg.util;
import cg.util.platform;

export namespace cg::db
{

    /**
     * @brief 数据库 schema 版本号。
     *
     * @internal
     * 不兼容的 DDL 变更、或对未变源码也改变已存行的抽取变更时递增。
     * 项目未发布：不维护跨版本迁移——版本戳与
     * 当前值不符的已存库在打开时直接拒绝，由 `ccg index --force` 重建
     * 文件；全新库由 ddl_statements 一步建成当前版本形态。
     */
    inline constexpr int schema_version = 19;

    /** @brief 每次打开数据库时执行的 PRAGMA 清单。 */
    inline constexpr std::string_view open_pragmas[] = {
        "PRAGMA journal_mode = WAL",
        "PRAGMA synchronous = NORMAL",
        "PRAGMA foreign_keys = ON",
        "PRAGMA temp_store = MEMORY",
    };

    /**
     * @brief 每个 GraphStore 连接上最先武装的 SQLite 忙超时（毫秒）。
     *
     * 必须是连接上的第一条语句（先于上述 PRAGMA、先于任何引导写）。
     *
     * @internal
     * SQLite 出厂默认为 0——每个争用写都立刻失败；sqlpp23 的 sqlite3
     * 连接器自身没有超时旋钮。没有这条，任何 ccg 进程遇到另一个写者
     * （实时更新的协调事务、它分钟级的引擎批处理、外部持有者）都会以
     * "database is locked" 死掉而不是排队等待。30 秒吸收协调事务与短锁；
     * 更新路径另行调高（default_update_busy_timeout_ms）以熬过引擎批
     * 处理。
     */
    inline constexpr std::int64_t default_busy_timeout_ms = 30'000;

    /**
     * @brief 协调排队循环里 claim 探测事务的短 busy（ms）。
     *
     * @internal 探测的 BEGIN IMMEDIATE 会与持有者的写锁相撞：短 busy 让
     * 落败快速回到可观测的等待循环（打印/退避/心跳），而非把进程钉在
     * SQLite 锁队列上静默睡到持有者退场（排队租约协议明令避免的形态）。
     */
    inline constexpr std::int64_t default_coord_probe_busy_ms = 1'000;

    /**
     * @brief 解析期租约心跳连接的短 busy（ms）。
     *
     * @internal 心跳线程自带独立连接，其 BEGIN IMMEDIATE 只会与钩子入队
     * 等毫秒级短写事务相撞：短 busy 让落败快速回到周期等待、下一周期
     * 再试（心跳周期按租约四分之一取值，连续数次落败仍在租约余量内）；
     * 绝不把心跳线程钉在锁队列上——stop() 的 join 因此恒快，提交段拿
     * 写锁前不会被自己的心跳拖住。
     */
    inline constexpr std::int64_t default_lease_keepalive_busy_ms = 5'000;

    /**
     * @brief 全新数据库上执行一次的 DDL（一步建成当前版本的完整形态），
     *        每条目一个语句。纯 DDL：symbol_kinds/edge_types 的字典种子
     *        不在本数组里，由 store 侧引导在建表后经类型化 insert 写入
     *        （名单唯一来源是本模块的 name_of(SymbolKind)/name_of(EdgeType)，
     *        避免名单双份）。
     *
     * @internal sqlpp23 连接调用运算符拒绝多语句字符串。
     */
    inline constexpr std::string_view ddl_statements[] = {
        R"(CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
) WITHOUT ROWID)",

        R"(CREATE TABLE str (
    id  INTEGER PRIMARY KEY,
    s   TEXT NOT NULL
))",
        "CREATE UNIQUE INDEX str_s ON str(s)",

        R"(CREATE TABLE symbol_kinds (
    kind_id INTEGER PRIMARY KEY,
    name    TEXT NOT NULL UNIQUE
))",

        R"(CREATE TABLE edge_types (
    type_id INTEGER PRIMARY KEY,
    name    TEXT NOT NULL UNIQUE
))",

        R"(CREATE TABLE files (
    file_id      INTEGER PRIMARY KEY,
    path_id      INTEGER NOT NULL UNIQUE REFERENCES str(id),
    content_hash TEXT,
    parse_status INTEGER NOT NULL DEFAULT 0,
    error_count  INTEGER NOT NULL DEFAULT 0,
    last_error   TEXT,
    indexed_at   TEXT
))",
        "CREATE INDEX files_pending ON files(parse_status) WHERE parse_status <> 1",

        R"(CREATE TABLE symbols (
    sym_id        INTEGER PRIMARY KEY,
    unique_key_id INTEGER NOT NULL UNIQUE REFERENCES str(id),
    kind_id       INTEGER NOT NULL REFERENCES symbol_kinds(kind_id),
    name_id       INTEGER NOT NULL REFERENCES str(id),
    ns_id         INTEGER NOT NULL REFERENCES str(id),
    sig_hash      INTEGER,
    file_id       INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    line          INTEGER NOT NULL,
    col           INTEGER NOT NULL,
    parent_id     INTEGER REFERENCES symbols(sym_id) ON DELETE CASCADE,
    flags         INTEGER NOT NULL DEFAULT 0,
    param_types   TEXT,
    template_params TEXT
))",
        "CREATE INDEX symbols_name   ON symbols(name_id)",
        "CREATE INDEX symbols_file   ON symbols(file_id)",
        "CREATE INDEX symbols_parent ON symbols(parent_id)",

        R"(CREATE TABLE edges (
    edge_id      INTEGER PRIMARY KEY,
    src_id       INTEGER NOT NULL REFERENCES symbols(sym_id) ON DELETE CASCADE,
    dst_id       INTEGER NOT NULL REFERENCES symbols(sym_id) ON DELETE CASCADE,
    type_id      INTEGER NOT NULL REFERENCES edge_types(type_id),
    site_file_id INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    site_line    INTEGER NOT NULL,
    attrs        INTEGER NOT NULL DEFAULT 0,
    confidence   REAL NOT NULL DEFAULT 1.0,
    occurrences  INTEGER NOT NULL DEFAULT 1
))",
        "CREATE UNIQUE INDEX edges_site ON edges(src_id, dst_id, type_id, "
        "site_file_id, site_line)",
        "CREATE INDEX edges_dst ON edges(dst_id, type_id)",
        "CREATE INDEX edges_own ON edges(site_file_id)",
        // type_id 普通索引：按边类型取行的装载查询共用——Extends/
        // Specializes 继承图装载、dispatch 证据整读、整类计数。代价：
        // 全部边类型的每次插入与删除都多维护一棵 B 树、约每十万行边
        // 多占 1MB 库体积，换取任意边类型的索引直达装载。此前仅覆盖
        // virtual_dispatch 的部分索引被本索引完全包含而退役：部分索引
        // 对其余边类型零维护，双份并存时付双份维护账的只有
        // virtual_dispatch 一种边。
        "CREATE INDEX edges_type ON edges(type_id)",

        R"(CREATE TABLE type_hints (
    hint_id         INTEGER PRIMARY KEY,
    owner_id        INTEGER NOT NULL REFERENCES symbols(sym_id) ON DELETE CASCADE,
    var_name        TEXT    NOT NULL DEFAULT '',
    file_id         INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    line            INTEGER NOT NULL,
    col             INTEGER NOT NULL,
    type_set        TEXT    NOT NULL DEFAULT '[]',
    source          INTEGER NOT NULL,
    confidence      INTEGER NOT NULL,
    fallback_reason TEXT    NOT NULL DEFAULT '',
    param_index     INTEGER NOT NULL DEFAULT -1,
    related_var     TEXT    NOT NULL DEFAULT '',
    related_key     TEXT    NOT NULL DEFAULT '',
    UNIQUE(owner_id, file_id, line, col, var_name, source)
))",
        "CREATE INDEX type_hints_owner ON type_hints(owner_id)",
        "CREATE INDEX type_hints_file ON type_hints(file_id)",
        "CREATE INDEX type_hints_owner_var ON type_hints(owner_id, var_name)",

        R"(CREATE TABLE call_sites (
    site_id         INTEGER PRIMARY KEY,
    caller_id       INTEGER NOT NULL REFERENCES symbols(sym_id) ON DELETE CASCADE,
    callee_id       INTEGER REFERENCES symbols(sym_id) ON DELETE SET NULL,
    callee_key      TEXT    NOT NULL DEFAULT '',
    callee_name     TEXT    NOT NULL DEFAULT '',
    callee_ns       TEXT    NOT NULL DEFAULT '',
    callee_parent   TEXT    NOT NULL DEFAULT '',
    callee_sig      INTEGER,
    callee_const    INTEGER NOT NULL DEFAULT 0,
    method_id       INTEGER REFERENCES symbols(sym_id) ON DELETE SET NULL,
    method_key      TEXT    NOT NULL DEFAULT '',
    is_virtual      INTEGER NOT NULL DEFAULT 0,
    qualified       INTEGER NOT NULL DEFAULT 0,
    indirect        INTEGER NOT NULL DEFAULT 0,  -- 函数指针/成员指针/依赖调用
    receiver_name   TEXT    NOT NULL DEFAULT '',
    receiver_is_this INTEGER NOT NULL DEFAULT 0, -- receiver_name 是 this->field
    receiver_owner_key TEXT NOT NULL DEFAULT '',  -- 字段接收者的声明记录 key
    static_type_id  INTEGER REFERENCES symbols(sym_id) ON DELETE SET NULL,
    file_id         INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    line            INTEGER NOT NULL,
    col             INTEGER NOT NULL,
    arg_types       TEXT    NOT NULL DEFAULT '[]',
    arg_vars        TEXT    NOT NULL DEFAULT '[]', -- 与 arg_types 对齐的同函数变量名
    arg_call_keys   TEXT    NOT NULL DEFAULT '[]', -- 与 arg_types 对齐的延迟返回槽描述符
    arg_targets     TEXT    NOT NULL DEFAULT '[]', -- 与 arg_types 对齐的函数目标 key（取址/裸名实参）
    receiver_types  TEXT    NOT NULL DEFAULT '[]',  -- 静态∪动态排序并集（dispatch 写回的披露面）
    receiver_static TEXT    NOT NULL DEFAULT '[]',  -- parse 专写：接收者声明静态类型（装载判证面）
    callee_resolved INTEGER NOT NULL DEFAULT 0,   -- 被调用者解析（与 dispatch 无关）
    resolved        INTEGER NOT NULL DEFAULT 0,   -- dispatch 解析出候选
    confidence      INTEGER NOT NULL DEFAULT 3,
    candidates      TEXT    NOT NULL DEFAULT '[]',
    fallback_reason TEXT    NOT NULL DEFAULT '',
    callee_usr      TEXT    NOT NULL DEFAULT '',  -- 界外模板实例化 callee 的 USR（实例桥）
    callee_expr     TEXT    NOT NULL DEFAULT '',  -- 间接调用的被调用表达式名（变量/字段）
    callee_param    INTEGER NOT NULL DEFAULT -1,  -- 被调用表达式是本函数形参时的序号
    UNIQUE(caller_id, file_id, line, col, callee_key, is_virtual, indirect)
))",
        "CREATE INDEX call_sites_caller ON call_sites(caller_id)",
        "CREATE INDEX call_sites_callee ON call_sites(callee_id)",
        "CREATE INDEX call_sites_method ON call_sites(method_id)",
        "CREATE INDEX call_sites_file ON call_sites(file_id)",
        "CREATE INDEX call_sites_open ON call_sites(resolved) WHERE resolved = 0",

        R"(CREATE TABLE includes (
    file_id   INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    target_id INTEGER NOT NULL REFERENCES str(id),
    line      INTEGER NOT NULL,
    angled    INTEGER NOT NULL,
    UNIQUE (file_id, target_id, line)
))",
        "CREATE INDEX includes_target ON includes(target_id)",

        R"(CREATE TABLE fingerprints (
    file_id    INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    mtime_ns   INTEGER NOT NULL,
    size       INTEGER NOT NULL,
    cmd_digest TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (file_id, cmd_digest)
))",

        R"(CREATE TABLE failures (
    file_id         INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    cmd_digest      TEXT    NOT NULL,
    content_hash    TEXT    NOT NULL DEFAULT '',
    mtime_ns        INTEGER NOT NULL DEFAULT 0,
    size            INTEGER NOT NULL DEFAULT 0,
    engine_token    TEXT    NOT NULL DEFAULT '',
    feed_digest     TEXT    NOT NULL DEFAULT '',
    roots_digest    TEXT    NOT NULL DEFAULT '',
    failure_class   TEXT    NOT NULL DEFAULT '',
    failure_text    TEXT    NOT NULL DEFAULT '',
    deps_json       TEXT    NOT NULL DEFAULT '',
    attempts        INTEGER NOT NULL DEFAULT 1,
    first_failed_at TEXT    NOT NULL DEFAULT '',
    last_failed_at  TEXT    NOT NULL DEFAULT '',
    retry_after     INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (file_id, cmd_digest)
) WITHOUT ROWID)",
        "CREATE INDEX failures_retry ON failures(retry_after) WHERE retry_after > "
        "0",

        R"(CREATE TABLE imports (
    file_id   INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    module_id INTEGER NOT NULL REFERENCES str(id),
    line      INTEGER NOT NULL,
    UNIQUE (file_id, module_id, line)
))",
        "CREATE INDEX imports_module ON imports(module_id)",

        R"(CREATE TABLE analysis_slots (
    owner_sym  TEXT    NOT NULL,
    var_name   TEXT    NOT NULL,
    type_set   TEXT    NOT NULL DEFAULT '[]',
    confidence INTEGER NOT NULL,
    source     INTEGER NOT NULL,
    reason     TEXT    NOT NULL DEFAULT '',
    truncated  INTEGER NOT NULL DEFAULT 0,
    ver        INTEGER NOT NULL,
    alias_src  TEXT    NOT NULL DEFAULT '',
    ret_src    TEXT    NOT NULL DEFAULT '',
    PRIMARY KEY (owner_sym, var_name)
) WITHOUT ROWID)",

        R"(CREATE TABLE pending_updates (
    path TEXT PRIMARY KEY
) WITHOUT ROWID)",

        R"(CREATE TABLE passthrough_wrappers (  -- slot inventory
    wrapper_key TEXT    NOT NULL,  -- primary template record unique_key
    getter_key  TEXT    NOT NULL,  -- passthrough getter function-form key
    getter_name TEXT    NOT NULL,
    file_id     INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    line        INTEGER NOT NULL,
    arg_index   INTEGER NOT NULL,  -- own template parameter index returned
    PRIMARY KEY (wrapper_key, getter_key)
) WITHOUT ROWID)",

        R"(CREATE TABLE external_param_args (  -- instance bridge (no FK
    -- 级联转发条目的 site 在界外头（host 体内），file_id 同
    -- external_invokes 的外键违规实证——去 FK，file_id=0 表未收录。
    host_usr    TEXT    NOT NULL,  -- out-of-roots template instance USR
    param_index INTEGER NOT NULL,
    caller_key  TEXT    NOT NULL,  -- '' when this row is a cascade forward
    target_key  TEXT    NOT NULL,  -- &fn / lambda key / 'ext#<host>#<i>'
    file_id     INTEGER NOT NULL,  -- 0 = site in an untracked out-of-roots file
    line        INTEGER NOT NULL,
    col         INTEGER NOT NULL,
    PRIMARY KEY (host_usr, param_index, file_id, line, col)
) WITHOUT ROWID)",

        R"(CREATE TABLE external_invokes (  -- instance bridge (v16: no FK
    -- invoke 站点在界外头文件——files 表不收录界外路径，外键必违规
    -- （实证：带外键时 122 条 insert 全部静默失败）；file_id=0 表示站点文件
    -- 未收录，删除级联语义不适用（界外文件永不 purge）。
    host_usr    TEXT    NOT NULL,  -- out-of-roots template instance USR
    param_index INTEGER NOT NULL,  -- invoked parameter ordinal
    file_id     INTEGER NOT NULL,  -- 0 = 站点在未收录的界外文件
    line        INTEGER NOT NULL,
    col         INTEGER NOT NULL,
    PRIMARY KEY (host_usr, param_index, file_id, line, col)
) WITHOUT ROWID)",

        R"(CREATE TABLE update_claim (
    id          INTEGER PRIMARY KEY CHECK (id = 1),
    holder      TEXT NOT NULL,
    lease_until INTEGER NOT NULL
))",

        R"(CREATE TABLE symbol_contributions (
    sym_id     INTEGER NOT NULL REFERENCES symbols(sym_id) ON DELETE CASCADE,
    file_id    INTEGER NOT NULL REFERENCES files(file_id) ON DELETE CASCADE,
    cmd_digest TEXT    NOT NULL,
    PRIMARY KEY (sym_id, file_id, cmd_digest)
) WITHOUT ROWID)",
        "CREATE INDEX symbol_contributions_entry ON symbol_contributions("
        "file_id, cmd_digest)",
    };

    /**
     * @brief 协调事件审计表（coord_events）的幂等 DDL。
     *
     * 协调状态变更（偷取/接管/回滚/拒收）既往只有 stderr 一个出口，而
     * stderr 的寿命等于进程寿命（hook 派生的 detach 路径甚至随
     * delete-on-close 临时文件蒸发）——本表把"谁在何时偷了谁/谁回滚了
     * 什么"落进库内，事后一次只读查询即可重建。
     *
     * @internal 独立成数组而不并入 ddl_statements：这是纯新增的旁路
     * 审计表（不动任何既有表、不改已存行语义），按版本政策属于兼容
     * 变更，不递增 schema_version——已存库（版本戳与当前值一致）在
     * bootstrap 时对这几条幂等补建，新库与旧库最终形态一致。语句全部
     * IF NOT EXISTS 形态，重放安全；读写在 store 层走惰性 prepare 的
     * 原生语句（表在极端 busy 窗口可能尚未建成，审计作为旁路尽力而为
     * 通道绝不反噬协调主流程）。
     */
    inline constexpr std::string_view coord_events_ddl[] = {
        R"(CREATE TABLE IF NOT EXISTS coord_events (
    id           INTEGER PRIMARY KEY,
    ts_ms        INTEGER NOT NULL,
    event        TEXT NOT NULL,
    actor_holder TEXT NOT NULL DEFAULT '',
    other_holder TEXT NOT NULL DEFAULT '',
    actor_pid    INTEGER NOT NULL DEFAULT 0,
    other_pid    INTEGER NOT NULL DEFAULT 0,
    reason       TEXT NOT NULL DEFAULT '',
    detail       TEXT NOT NULL DEFAULT ''
))",
        "CREATE INDEX IF NOT EXISTS coord_events_ts ON coord_events(ts_ms)",
    };

    /**
     * @brief 头级缓存与指令摘要的旁路表 DDL（幂等补建）。
     *
     * @internal 与 coord_events 同族的纯新增旁路表（不动既有表、不改已存
     * 行语义），不递增 schema_version——同版本旧库在 bootstrap 时对这几条
     * 幂等补建。两张表都带自己的版本列（ver），行级过滤旧版本（analysis_
     * slots 先例）；此外 meta 键 head_cache_side_ver 不匹配时整表丢弃重建
     * （缓存可丢弃再生——语义是加速器不是事实源，引擎版本/根集变化即全
     * 部失效）。file_id 无外键：旁路表保持零耦合，文件行删除时由写侧
     * 顺手清理（缓存行残留只造成 miss，不造成错数据）。
     */
    inline constexpr std::string_view head_cache_side_ddl[] = {
        R"(CREATE TABLE IF NOT EXISTS head_cache (
    file_id        INTEGER NOT NULL,
    main_id        INTEGER NOT NULL,
    variant_digest TEXT    NOT NULL,
    closure_digest TEXT    NOT NULL,
    payload        BLOB    NOT NULL,
    ver            INTEGER NOT NULL,
    PRIMARY KEY (file_id, main_id, variant_digest, closure_digest)
) WITHOUT ROWID)",
        R"(CREATE TABLE IF NOT EXISTS file_instr_digest (
    file_id     INTEGER NOT NULL PRIMARY KEY,
    d_narrow    TEXT    NOT NULL,
    d_all       TEXT    NOT NULL,
    d_include   TEXT    NOT NULL,
    n_narrow    INTEGER NOT NULL DEFAULT 0,
    n_all       INTEGER NOT NULL DEFAULT 0,
    n_include   INTEGER NOT NULL DEFAULT 0,
    ver         INTEGER NOT NULL
))",
    };

    /**
     * @brief 旁路缓存表的版本号（payload 编码与语义版本）。
     *
     * @internal 与 schema_version 独立：只影响可丢弃再生的缓存行，不匹配
     * 时旧行整表清除，旧库无感升级。
     */
    inline constexpr int head_cache_side_ver = 1;

    /** @brief head_cache 载荷行（payload 为调用方编解码的不透明字节串，
     *        内含主文件锚的路径与哈希；ver = 侧版本#纪元的联合哈希，
     *        行级过滤换引擎/根集后的旧缓存。id 在此处用裸整数：FileId
     *        别名声明在本文件更下方，旁路行结构不挪动既有声明序）。 */
    struct HeadCacheRow
    {
        std::int64_t file_id = 0;
        std::int64_t main_id = 0;
        std::string  variant_digest;
        std::string  closure_digest;
        std::string  payload;
        std::int64_t ver = 0;
    };

    /** @brief file_instr_digest 行（指令摘要三变体；ver 行级过滤，
     *        与 head_cache 同纪律——摘要只与文件内容相关，ver 触换版即弃）。 */
    struct FileInstrDigestRow
    {
        std::int64_t file_id = 0;
        std::string  d_narrow;
        std::string  d_all;
        std::string  d_include;
        std::int64_t n_narrow = 0;
        std::int64_t n_all = 0;
        std::int64_t n_include = 0;
        std::int64_t ver = 0;
    };

    /** @brief 协调事件类型（coord_events.event 列的取值）。 */
    enum class CoordEvent : std::uint8_t
    {
        Steal,         ///< 写命令启动接管，顶替了一条仍活的声明
        Rollback,      ///< 持有者批内发现被偷，整批回滚让位
        OrphanRecover, ///< 死 pid / 过期租约的接管（自愈）
        EnqueueReject, ///< 入队被拒（文件不在语料面等）
        RunStart,      ///< 一次 update/index 运行启动（每次运行一条）
        RunFinish,     ///< 同一运行的收尾（正常/异常/被接管者补记）
    };

    /** @brief name_of(CoordEvent)：审计行的 event 列值。 */
    [[nodiscard]]
    constexpr auto name_of(CoordEvent e) noexcept -> std::string_view
    {
        switch (e)
        {
        case CoordEvent::Steal:
            return "steal";
        case CoordEvent::Rollback:
            return "rollback";
        case CoordEvent::OrphanRecover:
            return "orphan-recover";
        case CoordEvent::EnqueueReject:
            return "enqueue-reject";
        case CoordEvent::RunStart:
            return "run-start";
        case CoordEvent::RunFinish:
            return "run-finish";
        }
        return "";
    }

    /** @brief 一条协调事件的写入输入（detail 为调用方拼好的 JSON 文本）。 */
    struct CoordEventInput
    {
        CoordEvent       event = CoordEvent::Steal;
        std::string_view actor_holder;
        std::string_view other_holder;
        std::int64_t     actor_pid = 0;
        std::int64_t     other_pid = 0;
        std::string_view reason;
        std::string_view detail;
    };

    /** @brief coord_events 的一行（读侧物化）。 */
    struct CoordEventRow
    {
        std::int64_t id = 0;
        std::int64_t ts_ms = 0;
        std::string  event;
        std::string  actor_holder;
        std::string  other_holder;
        std::int64_t actor_pid = 0;
        std::int64_t other_pid = 0;
        std::string  reason;
        std::string  detail;
    };

    /**
     * @brief 声明租约默认值：持有者的声明在无心跳时保持存活多久。
     *
     * @internal
     * 数据库协调更新协议（排队租约 + 事务内心跳）的默认旋钮之一；两个旋钮都是
     * per-database 配置（meta 键 update_lease_ms / update_busy_timeout_ms，
     * 经 `ccg config set`），本常量是未配置时的回退值。必须超过最坏情况下
     * 单个批处理的时长（take 循环在批之间心跳），而非最坏会话；全语料
     * 强制重解析在目标机器上约 4 分钟，5 分钟覆盖其尾部并留出余量。孤儿
     * 声明（持有进程被杀、租约未到期）由持有者 pid 探活立即接管，故这里
     * 不必再为"被杀进程"保留 15 分钟窗口。
     */
    inline constexpr std::int64_t default_update_lease_ms = 300'000;

    /**
     * @brief 更新进程连接上武装的 SQLite 忙超时默认值（毫秒）。
     *
     * @internal
     * 同为 per-database 配置回退（meta 键 update_busy_timeout_ms，经
     * `ccg config set`）。并发的引擎批处理可能持有写锁数分钟，入队/声明
     * 事务把它们熬过去而不是失败（钩子是分离的，用户可见侧永远不会阻塞）。
     */
    inline constexpr std::int64_t default_update_busy_timeout_ms = 600'000;

    /**
     * @brief 项目文件事件触发重采集前的静默窗默认值（毫秒）。
     *
     * @internal
     * 联动旋钮之一（meta 键 msbuild_refresh_debounce_ms，经
     * `ccg config set`），本常量是未配置时的回退值。IDE 对
     * .vcxproj/.props 的保存常是连发（保存→再保存→属性页批量落盘）；
     * update 持有者 peek 到项目文件行后先等队列静默再派生采集，窗口内
     * 连发的编辑被合并进同一次重采集。等待期间声明租约照常心跳；窗口
     * 之外到达的编辑由采集结束后的 peek 循环收敛。
     */
    inline constexpr std::int64_t default_msbuild_refresh_debounce_ms = 3'000;

    /**
     * @brief 索引并发上限（统一工人池规模）的默认值：0 = 自动按核。
     *
     * @internal
     * per-database 配置（meta 键 index_jobs，经 `ccg config set index-jobs`）
     * 的回退值。该上限统管一次索引运行的两段工人池——BMI 自建段
     * （SelfBuildOptions.jobs）与 TU 解析段（IndexOptions.jobs），两段时间
     * 上先后发生不叠加，峰值并发即该值。取值语义：0 = 显式回自动、1-64
     * 生效（与 --jobs 旗标同一区间，旗标优先于库配置）；"自动"解析为
     * cg::util::pool::auto_worker_count（clamp(cores-2, 1, 12)，该函数的
     * 调参依据见其注释）。读侧（resolve_jobs）对 meta 值做同一严格校验，
     * 手改库写出的畸形值按未配置处理。
     */
    inline constexpr int default_index_jobs = 0;

    /**
     * @brief 更新声明持有者记号的前缀（claim token 协议的格式单点）。
     *
     * @internal
     * 完整形状 `ccg-update:pid:<digits>:t<ticks>`。消费端是 db.store 的
     * holder_pid（逐位解析，只认这个形状）；生成端是 update_claim_token
     * 与全量索引提交段的排队声明——三处必须同格式，拼写散落曾迫使解析端
     * 用注释锚定生成端（批 D 下沉为常量+函数后注释锚定退役）。
     */
    inline constexpr std::string_view claim_token_prefix = "ccg-update:pid:";

    /**
     * @brief 生成一份更新声明的持有者记号。
     *
     * 记号含 pid（给人读、供持有者探活）与一个 steady_clock 滴答（使单
     * 进程的两次更新——测试、钩子风暴——持有不同声明）；仅作诊断——活性
     * 由租约时间戳裁定，从不由该字符串裁定。claim 的持有者语义与进程角色
     * 无关：跨二进制（msvc hook 与 clang index 写同一库）的 pid 探活靠它
     * 互通，故生成点收拢在 db 层而非某个 CLI 单元。
     */
    [[nodiscard]]
    inline auto update_claim_token() -> std::string
    {
        return std::format(
            "ccg-update:pid:{}:t{}",
            cg::util::platform::current_pid(),
            std::chrono::steady_clock::now().time_since_epoch().count());
    }

    /** @brief 镜像种子 symbol_kinds 表（枚举值即种子 kind_id）。 */
    enum class SymbolKind : std::uint8_t
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

    inline constexpr auto kind_count = 10;

    [[nodiscard]]
    constexpr auto to_id(SymbolKind k) noexcept -> std::int64_t
    {
        return static_cast<std::int64_t>(k);
    }

    [[nodiscard]]
    constexpr auto symbol_kind_of(std::int64_t id) -> SymbolKind
    {
        if (id < 0 || id >= kind_count)
        {
            throw std::invalid_argument("unknown symbol kind id");
        }
        return static_cast<SymbolKind>(id);
    }

    [[nodiscard]]
    constexpr auto name_of(SymbolKind k) noexcept -> std::string_view
    {
        switch (k)
        {
        case SymbolKind::Record:
            return "record";
        case SymbolKind::Enumeration:
            return "enum";
        case SymbolKind::EnumConstant:
            return "enum_constant";
        case SymbolKind::Function:
            return "function";
        case SymbolKind::Method:
            return "method";
        case SymbolKind::Constructor:
            return "constructor";
        case SymbolKind::Destructor:
            return "destructor";
        case SymbolKind::Field:
            return "field";
        case SymbolKind::Variable:
            return "variable";
        case SymbolKind::TypeAlias:
            return "type_alias";
        }
        return "record";
    }

    /**
     * @brief kind 身份谓词族：全仓 kind 集合判定的单一事实源。
     *
     * 历史上五套表示（枚举、name_of 字符串、钩子的字符串比较、抽取层的
     * SymKind、渲染层的集合写法）各自手写集合比较，漂移即口径分裂（批 A
     * 的 is_callable_kind 双版本漂移即此类）。本族是它们共同的积木：调用
     * 侧用谓词组合出自己的集合（如"可调用 ∪ 类型样"），谓词本身不承载
     * 调用侧的集合差异。字符串重载只认 name_of 值域的拼写——与枚举重载
     * 逐 kind 等值（tests/schema-mirror 的镜像对账钉死）。
     *
     * @internal 成员函数样（is_method_like）是可调用集合去掉自由函数的
     * 子集：钩子的族合并/成员访问语义只关心成员函数形态，单独成谓词避免
     * 各处手写三元比较再次漂移。
     */
    ///@{
    [[nodiscard]]
    constexpr auto is_callable(SymbolKind k) noexcept -> bool
    {
        return k == SymbolKind::Function || k == SymbolKind::Method
               || k == SymbolKind::Constructor || k == SymbolKind::Destructor;
    }

    [[nodiscard]]
    constexpr auto is_type_like(SymbolKind k) noexcept -> bool
    {
        return k == SymbolKind::Record || k == SymbolKind::Enumeration
               || k == SymbolKind::TypeAlias;
    }

    [[nodiscard]]
    constexpr auto is_value_like(SymbolKind k) noexcept -> bool
    {
        return k == SymbolKind::Field || k == SymbolKind::Variable;
    }

    [[nodiscard]]
    constexpr auto is_method_like(SymbolKind k) noexcept -> bool
    {
        return k == SymbolKind::Method || k == SymbolKind::Constructor
               || k == SymbolKind::Destructor;
    }

    [[nodiscard]]
    constexpr auto kind_name_is(std::string_view name, SymbolKind k) noexcept
        -> bool
    {
        return name == name_of(k);
    }

    [[nodiscard]]
    constexpr auto is_callable(std::string_view kind) noexcept -> bool
    {
        using K = SymbolKind;
        return kind_name_is(kind, K::Function) || kind_name_is(kind, K::Method)
               || kind_name_is(kind, K::Constructor)
               || kind_name_is(kind, K::Destructor);
    }

    [[nodiscard]]
    constexpr auto is_type_like(std::string_view kind) noexcept -> bool
    {
        using K = SymbolKind;
        return kind_name_is(kind, K::Record)
               || kind_name_is(kind, K::Enumeration)
               || kind_name_is(kind, K::TypeAlias);
    }

    [[nodiscard]]
    constexpr auto is_value_like(std::string_view kind) noexcept -> bool
    {
        using K = SymbolKind;
        return kind_name_is(kind, K::Field) || kind_name_is(kind, K::Variable);
    }

    [[nodiscard]]
    constexpr auto is_method_like(std::string_view kind) noexcept -> bool
    {
        using K = SymbolKind;
        return kind_name_is(kind, K::Method)
               || kind_name_is(kind, K::Constructor)
               || kind_name_is(kind, K::Destructor);
    }
    ///@}

    /** @brief 镜像种子 edge_types 表。 */
    enum class EdgeType : std::uint8_t
    {
        Calls = 0,
        Extends = 1,
        Overrides = 2,
        TypeAlias = 3,
        FriendOf = 4,
        Specializes = 5,
        Uses = 6,
        VirtualDispatch = 7,
        /** @brief 平台回调注册契约线索边（analysis 管线 CallbackRegRec 产出）。
         */
        CallbackReg = 8,
        /** @brief 间接调用边（消费分流：实例桥/形参直配/槽目标集）。 */
        IndirectCalls = 9,
    };

    inline constexpr auto edge_type_count = 10;

    [[nodiscard]]
    constexpr auto to_id(EdgeType t) noexcept -> std::int64_t
    {
        return static_cast<std::int64_t>(t);
    }

    [[nodiscard]]
    constexpr auto edge_type_of(std::int64_t id) -> EdgeType
    {
        if (id < 0 || id >= edge_type_count)
        {
            throw std::invalid_argument("unknown edge type id");
        }
        return static_cast<EdgeType>(id);
    }

    [[nodiscard]]
    constexpr auto name_of(EdgeType t) noexcept -> std::string_view
    {
        switch (t)
        {
        case EdgeType::Calls:
            return "calls";
        case EdgeType::Extends:
            return "extends";
        case EdgeType::Overrides:
            return "overrides";
        case EdgeType::TypeAlias:
            return "type_alias";
        case EdgeType::FriendOf:
            return "friend_of";
        case EdgeType::Specializes:
            return "specializes";
        case EdgeType::Uses:
            return "uses";
        case EdgeType::VirtualDispatch:
            return "virtual_dispatch";
        case EdgeType::CallbackReg:
            return "callback_reg";
        case EdgeType::IndirectCalls:
            return "indirect_calls";
        }
        return "calls";
    }

    /** @brief symbols.flags 位域字典。 */
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
        // 符号实体在界外体内（界外 lambda 等）——file_id 挂靠引入它的
        // TU 主文件作锚，真实坐标在 name（operator()@L行:文件）里。
        inline constexpr std::uint32_t external_body = 0x200;
        // final 语法位（双宿主）：record 符号 = 类级 final，method 符号 =
        // 方法级 final。两侧采集判据同构——宿主游标的直接子游标含
        // CXXFinalAttr 即源码硬证据。record 侧由 dispatch 静态决议的
        // "语法 final ∪ extends 图内无后代"并集口径消费；method 侧由
        // 最近覆盖唯一时的方法级 final 接力判定消费（类级证据与方法级
        // 证据的直连目标恒同，二者是封印证据的并集而非竞争）。
        inline constexpr std::uint32_t is_final = 0x400;
    } // namespace sym_flags

    /** @brief edges.attrs 位域字典。 */
    namespace edge_attrs
    {
        inline constexpr std::uint32_t access_mask =
            0x03; // extends：0 public，1 protected，2 private
        inline constexpr std::uint32_t access_protected = 0x01;
        inline constexpr std::uint32_t access_private = 0x02;
        inline constexpr std::uint32_t is_virtual_base = 0x04; // extends
        /** @brief extends：基类是依赖形态（模板形参作实参），目标指主模板本尊。
         */
        inline constexpr std::uint32_t is_template_base = 0x08; // extends
        inline constexpr std::uint32_t dispatch_mask =
            0x07; // calls：0 直接，1 成员，2 虚派发
        inline constexpr std::uint32_t dispatch_member = 0x01;
        inline constexpr std::uint32_t dispatch_virtual = 0x02;
        inline constexpr std::uint32_t template_arg_shift =
            0; // specializes：实参数量在 0-3 位
        /** @internal uses：0-2
         * 位携带产出该边的声明点风味（field/param/return/local/variable）。 */
        inline constexpr std::uint32_t uses_site_mask = 0x07;
        inline constexpr std::uint32_t uses_site_field = 0x00;
        inline constexpr std::uint32_t uses_site_param = 0x01;
        inline constexpr std::uint32_t uses_site_return = 0x02;
        inline constexpr std::uint32_t uses_site_local = 0x03;
        inline constexpr std::uint32_t uses_site_variable = 0x04;
        /** @internal virtual_dispatch：推断自类型线索（区别于家族级
         * fallback）。 */
        inline constexpr std::uint32_t dispatch_inferred = 0x08;
        /** @internal
         * virtual_dispatch：无动态类型证据时的家族级降级边（confidence 0）。
         */
        inline constexpr std::uint32_t dispatch_fallback = 0x10;
        /** @brief indirect_calls（type
         * 9）：分辨率档位（蓝图置信层四档的前两档， heuristic/unknown 属
         * 逃逸层）。 */
        inline constexpr std::uint32_t indirect_resolution_mask = 0x30;
        /** @internal indirect_calls：目标集唯一（槽/实参直配解析出单一目标）。
         */
        inline constexpr std::uint32_t indirect_exact = 0x10;
        /** @internal indirect_calls：有界多目标之一（GCC speculative
         * 子边同构）。 */
        inline constexpr std::uint32_t indirect_candidate = 0x20;
    } // namespace edge_attrs

    using StrId = std::int64_t;
    using FileId = std::int64_t;
    using SymId = std::int64_t;
    using EdgeId = std::int64_t;

    /**
     * @brief 为调用方完全物化的符号（字符串已从 str 解析）。
     */
    struct SymbolRow
    {
        SymId                       id = 0;
        SymbolKind                  kind = SymbolKind::Record;
        std::string                 name;
        std::string                 ns; // 完整命名空间路径（全局作用域为 ''）
        std::string                 qualified_name; // ns + 父链 + 名字
        std::optional<std::int64_t> sig_hash;
        FileId                      file_id = 0;
        std::string                 file; // 规范化路径文本
        int                         line = 0;
        int                         col = 0;
        std::optional<SymId>        parent_id;
        std::string                 parent; // 父符号的非限定名，无父时为 ''
        std::uint32_t               flags = 0;
        std::optional<std::string>  param_types;
        std::optional<std::string>  template_params;
        std::string                 unique_key;
    };

    /** @brief 一条原始边行。 */
    struct EdgeRow
    {
        EdgeId        id = 0;
        SymId         src = 0;
        SymId         dst = 0;
        EdgeType      type = EdgeType::Calls;
        FileId        site_file = 0;
        int           site_line = 0;
        std::uint32_t attrs = 0;
        double        confidence = 1.0;
        std::int64_t  occurrences = 1;
    };

    /**
     * @brief Edges 查询行 -> EdgeRow 的统一解包。
     *
     * @internal type 由调用方给定：单类型查询（edges_of_type 等）拿的是
     * 查询参数本身，混合查询才从 typeId 换算。模板只做 sqlpp 结果行类型
     * 推导，不做列集探测（不做深模板化——db.tables.h 的 MSVC IFC 雷区）。
     */
    template <typename Row>
    [[nodiscard]]
    auto to_edge_row(const Row& row, EdgeType type) -> EdgeRow
    {
        return {
            .id = static_cast<EdgeId>(row.edgeId),
            .src = static_cast<SymId>(row.srcId),
            .dst = static_cast<SymId>(row.dstId),
            .type = type,
            .site_file = static_cast<FileId>(row.siteFileId),
            .site_line = static_cast<int>(row.siteLine),
            .attrs = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(row.attrs)),
            .confidence = row.confidence,
            .occurrences = static_cast<std::int64_t>(row.occurrences)};
    }

    /**
     * @brief 按对侧符号聚合的边（callers/callees 形态：每个 (src,dst) 对的
     *        MIN(site_line) 与 SUM(occurrences)）。
     */
    struct EdgeAggRow
    {
        SymId        other = 0;
        EdgeType     type = EdgeType::Calls;
        std::int64_t occurrences_sum = 0;
        std::int64_t min_site_line = 0;
        /** @brief 该 (对端, 类型) 分组内的最高置信度与属性并集。 */
        double        confidence = 1.0;
        std::uint32_t attrs = 0;
    };

    /**
     * @brief 一条类型线索行（schema v10）：某符号内某个变量/形参/返回槽的
     *        动态类型候选集（项目 record unique_key，排序）。
     *
     * @internal source 与 confidence 是枚举整数；type_set 是 JSON 字符串
     * 数组。owner_id 级联：符号被清除时线索随之消失。
     */
    struct TypeHintRow
    {
        std::int64_t id = 0;
        SymId        owner_id = 0;
        std::string  var_name; // "" = 匿名；"#ret" 返回槽；形参存其名字
        FileId       file_id = 0;
        int          line = 0;
        int          col = 0;
        std::string  type_set; // JSON 数组文本
        int          source = 0;
        int          confidence = 3;
        std::string  fallback_reason;
        int          param_index = -1;
        std::string  related_var;
        std::string  related_key;
    };

    /**
     * @brief TypeHints 查询行 -> TypeHintRow 的统一解包。
     *
     * @internal hintId 只在含该列的查询里补填（装载路径不取 id，id 保持
     * 默认 0）；列名相同而 select 集不同，故模板只做行类型推导。
     */
    template <typename Row>
    [[nodiscard]]
    auto to_type_hint_row(const Row& row) -> TypeHintRow
    {
        TypeHintRow r;
        r.owner_id = static_cast<SymId>(row.ownerId);
        r.var_name = std::string{row.varName};
        r.file_id = static_cast<FileId>(row.fileId);
        r.line = static_cast<int>(row.line);
        r.col = static_cast<int>(row.col);
        r.type_set = std::string{row.typeSet};
        r.source = static_cast<int>(row.source);
        r.confidence = static_cast<int>(row.confidence);
        r.fallback_reason = std::string{row.fallbackReason};
        r.param_index = static_cast<int>(row.paramIndex);
        r.related_var = std::string{row.relatedVar};
        r.related_key = std::string{row.relatedKey};
        return r;
    }

    /** @brief 一条调用点行（schema v10）。 */
    struct CallSiteRow
    {
        std::int64_t                id = 0;
        SymId                       caller_id = 0;
        std::optional<SymId>        callee_id;
        std::string                 callee_key;
        std::string                 callee_name;
        std::string                 callee_ns;
        std::string                 callee_parent;
        std::optional<std::int64_t> callee_sig;
        bool                        callee_const = false;
        std::optional<SymId>        method_id;
        std::string                 method_key;
        bool                        is_virtual = false;
        bool                        qualified = false;
        bool                        indirect = false;
        std::string                 receiver_name;
        bool                        receiver_is_this = false;
        std::string                 receiver_owner_key;
        std::optional<SymId>        static_type_id;
        FileId                      file_id = 0;
        int                         line = 0;
        int                         col = 0;
        std::string                 arg_types; // JSON 数组的数组
        std::string                 arg_vars;  // JSON 数组（与 arg_types 对齐）
        std::string arg_call_keys;             // JSON 数组（与 arg_types 对齐）
        std::string arg_targets;               // JSON 数组（与 arg_types 对齐）
        std::string receiver_types; // JSON 数组（静态∪动态并集，披露面）
        std::string
             receiver_static; // JSON 数组（parse 首写的纯声明静态，判证面）
        bool callee_resolved = false;
        bool resolved = false;
        int  confidence = 3;
        std::string candidates; // JSON: [{target,confidence,reason}]
        std::string fallback_reason;
        std::string callee_usr;        // 界外模板实例 callee USR（实例桥）
        std::string callee_expr;       // 间接调用的被调用表达式名（消费）
        int         callee_param = -1; // 被调用表达式是本函数第 N 形参
    };

    /**
     * @brief CallSites 查询行 -> CallSiteRow 的统一解包（装载/全量两侧共
     *        有的三十列）。
     *
     * @internal 两侧 select 集不同：装载路径不取 argTargets/calleeUsr/
     * calleeExpr/calleeParam 四列（行类型无该成员），含列的调用点在解包后
     * 自行补填；模板只做行类型推导，不做列集探测。
     */
    template <typename Row>
    [[nodiscard]]
    auto to_call_site_row(const Row& row) -> CallSiteRow
    {
        CallSiteRow r;
        r.id = static_cast<std::int64_t>(row.siteId);
        r.caller_id = static_cast<SymId>(row.callerId);
        if (row.calleeId.has_value())
        {
            r.callee_id = static_cast<SymId>(*row.calleeId);
        }
        r.callee_key = std::string{row.calleeKey};
        r.callee_name = std::string{row.calleeName};
        r.callee_ns = std::string{row.calleeNs};
        r.callee_parent = std::string{row.calleeParent};
        if (row.calleeSig.has_value())
        {
            r.callee_sig = static_cast<std::int64_t>(*row.calleeSig);
        }
        r.callee_const = static_cast<std::int64_t>(row.calleeConst) != 0;
        if (row.methodId.has_value())
        {
            r.method_id = static_cast<SymId>(*row.methodId);
        }
        r.method_key = std::string{row.methodKey};
        r.is_virtual = static_cast<std::int64_t>(row.isVirtual) != 0;
        r.qualified = static_cast<std::int64_t>(row.qualified) != 0;
        r.indirect = static_cast<std::int64_t>(row.indirect) != 0;
        r.receiver_name = std::string{row.receiverName};
        r.receiver_is_this = static_cast<std::int64_t>(row.receiverIsThis) != 0;
        r.receiver_owner_key = std::string{row.receiverOwnerKey};
        if (row.staticTypeId.has_value())
        {
            r.static_type_id = static_cast<SymId>(*row.staticTypeId);
        }
        r.file_id = static_cast<FileId>(row.fileId);
        r.line = static_cast<int>(row.line);
        r.col = static_cast<int>(row.col);
        r.arg_types = std::string{row.argTypes};
        r.arg_vars = std::string{row.argVars};
        r.arg_call_keys = std::string{row.argCallKeys};
        r.receiver_types = std::string{row.receiverTypes};
        r.receiver_static = std::string{row.receiverStatic};
        r.callee_resolved = static_cast<std::int64_t>(row.calleeResolved) != 0;
        r.resolved = static_cast<std::int64_t>(row.resolved) != 0;
        r.confidence = static_cast<int>(row.confidence);
        r.candidates = std::string{row.candidates};
        r.fallback_reason = std::string{row.fallbackReason};
        return r;
    }

    /**
     * @brief analysis_slots 一行（schema v12）：全局传播固定点在槽
     *        (owner_key, var_name) 上的终态。type_set 是排序 JSON 数组文本，
     *        与 PropagatedHint.types 的物化形态逐字节一致。
     */
    struct AnalysisSlotRow
    {
        std::string owner_key;
        std::string var_name;
        std::string type_set;
        int         confidence = 0;
        int         source = 0;
        std::string reason;
        bool        truncated = false;
        int         ver = 0;
        std::string alias_src; // 上次运行解析的函数内别名源槽键（"" = 无）
        std::string ret_src;   // 上次运行解析的返回值传播源槽键（"" = 无）
        /**
         * @brief 快照恢复形态的类型集区段（int32 arena 的 (off,len)）。
         *
         * @internal 仅快照恢复路径填充：type_set 保持空串、type_seg 有效，
         *           语义与 JSON 文本形态恒等（同一类型域、id 序 == 字符串
         *           序，区段 == 排序去重 id 集 == JSON 物化的串集）。SQL
         *           读路径与写回载荷永不填充；需要 JSON 文本的消费点先经
         *           type_seg 物化（闭包内重算槽的比对，量级极小）。
         */
        std::optional<std::pair<std::uint32_t, std::uint32_t>> type_seg;
    };

    /** @brief analysis_slots 写载荷（string_view 在匹配 upsert
     * 返回前保持有效）。
     */
    struct AnalysisSlotWrite
    {
        std::string_view owner_key;
        std::string_view var_name;
        std::string_view type_set;
        int              confidence = 0;
        int              source = 0;
        std::string_view reason;
        bool             truncated = false;
        int              ver = 0;
        std::string_view alias_src;
        std::string_view ret_src;
    };

    /**
     * @brief passthrough_wrappers 一行（schema v13，槽位基础设施）：透传包装
     *        登记表——类模板 + 零参成员函数返回 own 模板形参（atomic 的
     *        load / optional 的 value / shared_ptr 的 get 同判据覆盖）。
     *
     * @internal 纯登记（零消费）：解包动作（getter 调用点把 arg 形参
     *           record 并入 receiver 槽）由透传闸挂接。key 是弱引用文本
     *           （与 call_sites.callee_key 同形态），file_id 外键承载
     *           per-file 重写语义（purge 级联删除，upsert 即当前事实）。
     */
    struct PassthroughWrapperRow
    {
        std::string  wrapper_key;
        std::string  getter_key;
        std::string  getter_name;
        std::int64_t file_id = 0;
        int          line = 0;
        int          arg_index = 0;
    };

    /** @brief passthrough_wrappers 写载荷（string_view 生命周期同上）。 */
    struct PassthroughWrapperWrite
    {
        std::string_view wrapper_key;
        std::string_view getter_key;
        std::string_view getter_name;
        std::int64_t     file_id = 0;
        int              line = 0;
        int              arg_index = 0;
    };

    /**
     * @brief external_param_args 行（schema v15，实例桥）：界外模板实例
     *        的形参 N 收到的函数目标证据（传入侧）。
     *
     * @internal host_usr 是界外实例 decl 的 USR（调用点与实例体内采集两侧
     *           同源对齐）；caller_key/target_key 是弱引用文本（同
     *           call_sites.callee_key 形态）；file_id 外键承载 per-file
     *           重写（purge 级联），upsert 冲突 do nothing（同站点幂等）。
     */
    struct ExternalParamArgRow
    {
        std::string  host_usr;
        int          param_index = 0;
        std::string  caller_key;
        std::string  target_key;
        std::int64_t file_id = 0;
        int          line = 0;
        int          col = 0;
    };

    /** @brief external_param_args 写载荷（string_view 生命周期同上）。 */
    struct ExternalParamArgWrite
    {
        std::string_view host_usr;
        int              param_index = 0;
        std::string_view caller_key;
        std::string_view target_key;
        std::int64_t     file_id = 0;
        int              line = 0;
        int              col = 0;
    };

    /** @brief external_invokes 行（实例桥体内侧）：实例体内对形参 N
     * 的调用点。
     */
    struct ExternalInvokeRow
    {
        std::string  host_usr;
        int          param_index = 0;
        std::int64_t file_id = 0;
        int          line = 0;
        int          col = 0;
    };

    /** @brief external_invokes 写载荷。 */
    struct ExternalInvokeWrite
    {
        std::string_view host_usr;
        int              param_index = 0;
        std::int64_t     file_id = 0;
        int              line = 0;
        int              col = 0;
    };

    /**
     * @brief 逐文件清除前的派生数据捕获（设计评审修正："被删行下游"的旧
     *        边见证）：该文件即将被 purge/erase 的行里，增量种子构造还需要
     *        的旧信息。
     *
     * @internal slots = 该文件 type_hints 行涉及的全部 (owner_key, var_name)
     *           槽键（行删除即槽输入变化，槽可能属于干净文件——字段槽的
     *           行按使用点落在使用方文件里）；callee_keys = 该文件
     *           call_sites 行上次解析出的被调用者键（站点删除使"实参→
     *           形参"边失效，形参槽可能属于干净符号）；doomed_keys = 该
     *           文件即将删除的符号 unique_key（外部干净站点按 callee_key
     *           文本回指它们的扇入判定）。
     */
    struct PurgeCapture
    {
        std::vector<std::pair<std::string, std::string>> slots;
        std::vector<std::string>                         callee_keys;
        std::vector<std::string>                         doomed_keys;
        /** @brief 即将删除符号的非限定名（`fn` 消费者的保守匹配面）。 */
        std::vector<std::string> doomed_names;
    };

    /** @brief type_hints 写载荷。 */
    struct TypeHintWrite
    {
        SymId            owner_id = 0;
        std::string_view var_name;
        FileId           file_id = 0;
        int              line = 0;
        int              col = 0;
        std::string_view type_set;
        int              source = 0;
        int              confidence = 3;
        std::string_view fallback_reason;
        int              param_index = -1;
        std::string_view related_var;
        std::string_view related_key;
    };

    /** @brief call_sites 写载荷（插入时未解析；resolution 由分析回填）。 */
    struct CallSiteWrite
    {
        SymId                       caller_id = 0;
        std::optional<SymId>        callee_id = std::nullopt;
        std::string_view            callee_key;
        std::string_view            callee_name;
        std::string_view            callee_ns;
        std::string_view            callee_parent;
        std::optional<std::int64_t> callee_sig = std::nullopt;
        bool                        callee_const = false;
        std::optional<SymId>        method_id = std::nullopt;
        std::string_view            method_key;
        bool                        is_virtual = false;
        bool                        qualified = false;
        bool                        indirect = false;
        std::string_view            receiver_name;
        bool                        receiver_is_this = false;
        std::string_view            receiver_owner_key;
        std::optional<SymId>        static_type_id = std::nullopt;
        FileId                      file_id = 0;
        int                         line = 0;
        int                         col = 0;
        std::string_view            arg_types;
        std::string_view            arg_vars;
        std::string_view            arg_call_keys;
        std::string_view            arg_targets;
        std::string_view            receiver_types;
        std::string_view            receiver_static;
        bool                        callee_resolved = false;
        bool                        resolved = false;
        int                         confidence = 3;
        std::string_view            candidates;
        std::string_view            fallback_reason;
        std::string_view            callee_usr;
        std::string_view            callee_expr;
        int                         callee_param = -1;
    };

    /** @brief 一条 files 行，路径文本已解析。 */
    struct FileRow
    {
        FileId                     id = 0;
        std::string                path;
        std::optional<std::string> content_hash;
        cg::util::ParseStatus      status = cg::util::ParseStatus::Pending;
        int                        error_count = 0;
        std::optional<std::string> last_error;
        std::optional<std::string> indexed_at;
    };

    /**
     * @brief 一个（文件, 编译命令）对的构建系统新鲜度指纹。
     *
     * stat 身份（mtime + size），sha256 内容哈希作决胜（files.content_hash），
     * 外加规范化的命令摘要。每个编译过该文件的命令一行——有多个编译数据库
     * 条目的文件带多行，各行对照自己的条目检查，因此同一文件的多个副本
     * 各自保持新鲜、而不是争夺同一个槽位。非 main 文件用 ''。无行 =
     * 不可验证 = 过期。
     */
    struct FingerprintRow
    {
        FileId       file_id = 0;
        std::int64_t mtime_ns = 0;
        std::int64_t size = 0;
        std::string  cmd_digest; // 非 main 文件为 ''
    };

    /**
     * @brief 一条失败条目：某个（文件, 编译命令）上一次解析失败的身份与
     *        世界状态。
     *
     * @internal
     * 键是 (file_id, cmd_digest) 而不是 file_id：同一文件可有多条编译
     * 条目，一条成功一条失败是真实语料（参考语料的 11 个失败文件里有 3 个
     * 正是这种形态）。只有该条目的世界状态（主文件内容、argv、引擎、
     * 模块 feed、抽取根集、依赖闭包）全部与记录一致时才跳过重试——
     * 否则重试。deps_json 是换行分隔的 "path\tmtime\t`<size>`"（size=-1
     * 表示失败时该依赖不存在），用于闭包变化触发重试。
     */
    struct FailureRow
    {
        FileId       file_id = 0;
        std::string  cmd_digest;
        std::string  content_hash; // 主文件失败时字节的 sha256；'' = 不可验证
        std::int64_t mtime_ns = 0;
        std::int64_t size = 0;
        std::string  engine_token;
        std::string  feed_digest;
        std::string  roots_digest;
        std::string  failure_class;
        std::string  failure_text;
        std::string  deps_json;
        std::int64_t attempts = 1;
        std::string  first_failed_at;
        std::string  last_failed_at;
        std::int64_t retry_after = 0; // unix ms；仅瞬时类 > 0
    };

    /** @brief 数据库摘要（ccg stats）。kind/type 槽位按枚举 id 索引。 */
    struct Stats
    {
        std::int64_t files = 0;
        std::int64_t files_ok = 0;
        std::int64_t files_partial = 0;
        std::int64_t files_failed = 0;
        std::int64_t files_pending = 0;
        std::int64_t symbols = 0;
        std::int64_t symbols_by_kind[kind_count] = {};
        std::int64_t edges = 0;
        std::int64_t edges_by_type[edge_type_count] = {};
        std::int64_t includes = 0;
        std::int64_t includes_angled = 0;
        std::int64_t imports = 0; // 模块导入关系（v5）
        std::int64_t strings = 0;
        /** @brief 派生分析事实的整库计数与最近一次运行的固定点可观测值。 */
        std::int64_t               type_hints = 0;
        std::int64_t               call_sites = 0;
        std::int64_t               analysis_rounds = 0;
        std::int64_t               analysis_work_items = 0;
        bool                       analysis_budget_exhausted = false;
        std::string                app_version;
        std::optional<std::string> last_index_at;
    };

    /**
     * @brief 符号写载荷：各 string_view 必须保持有效，直到与之匹配的 upsert
     *        调用返回为止（入栈途中即被驻留/拷贝）。
     */
    struct SymbolWrite
    {
        std::string_view                unique_key;
        SymbolKind                      kind = SymbolKind::Function;
        std::string_view                name;
        std::string_view                ns; // 全局作用域为 ''
        std::optional<std::int64_t>     sig_hash = std::nullopt;
        FileId                          file_id = 0;
        int                             line = 0;
        int                             col = 0;
        std::optional<SymId>            parent_id = std::nullopt;
        std::uint32_t                   flags = 0;
        std::optional<std::string_view> param_types = std::nullopt;
        std::optional<std::string_view> template_params = std::nullopt;
    };

    struct EdgeWrite
    {
        SymId         src = 0;
        SymId         dst = 0;
        EdgeType      type = EdgeType::Calls;
        FileId        site_file = 0;
        int           site_line = 0;
        std::uint32_t attrs = 0;
        double        confidence = 1.0;
        std::int64_t  occurrences = 1; // 该站点折叠的书写重复
    };

    struct IncludeWrite
    {
        FileId           file = 0; // 包含方
        std::string_view target;   // 指令中书写的原文
        int              line = 0;
        bool             angled = false;
    };

    /**
     * @brief 模块翻译单元的一条模块导入关系（`import cg.util;`）：
     *        IncludeWrite 的命名模块对偶。
     *
     * @internal
     * module_name 是被导入名字的光标原文拼写（"std"、"cg.util"、
     * "cg.util.json"；外部语料可能仍拼出分区导入，按原文存储）。
     */
    struct ImportWrite
    {
        FileId           file = 0; // 导入方翻译单元的 main 文件
        std::string_view module_name;
        int              line = 0;
    };

    /** @brief 一条 imports 行，模块文本已解析（all_imports 形态）。 */
    struct ImportLink
    {
        FileId      file = 0;
        std::string module_name;
        int         line = 0;
    };

    /**
     * @brief pending_updates 队列一次 peek 的结果（持有者循环的短事务）。
     *
     * @internal
     * 队列中当前的全部路径（空 = 队列已排干），以及 peek 是否发现声明被
     * 窃（自己的租约在运行中过期、另一个更新者接管——持有者就此停止；
     * 窃取者拥有队列）。peek 不删除任何行：批次完成后由 clear_covered
     * 只清除"本批覆盖且内容未再变化"的条目。
     */
    struct PendingPeek
    {
        std::vector<std::string> files;
        bool                     claim_lost = false;
    };

    /**
     * @brief 更新协调状态的一次只读快照（钩子的快速标脏/积压汇报用）。
     *
     * @internal
     * 与 `peek_pending` 的区别：不取声明、不心跳、不写、不删——纯读者。
     * `live` 的判定规则与 `claim_update` 完全一致（租约未到期，且持有者
     * 记号内嵌的 pid 仍存活）；否则一次编辑事件就能安全地短路而不是派生
     * 一次可能长达数秒的 update。`pending` 按路径排序，是 pending_updates
     * 当前内容的全部（空 = 队列已收敛）。
     */
    struct ClaimSnapshot
    {
        bool                     has_claim = false;
        bool                     live = false;
        std::string              holder;
        std::int64_t             lease_until = 0;
        std::int64_t             pid = 0; // holder 内嵌 pid；无则 0
        std::vector<std::string> pending; // pending_updates，ORDER BY path
    };

    /**
     * @brief 批后出队判定的一条输入（`GraphStore::clear_covered`）。
     *
     * @internal
     * 判定规则（"宁可多跑一次，绝不丢更新"）：
     * - 未跟踪（不在 db/compile db）→ 删除：永远无事可做，留着只会
     *   让 peek 永远返回它。
     * - 被跟踪但不在本批覆盖集 → 保留：本批没有处理它。
     * - 覆盖且当前哈希 == snapshot_hash → 删除：批次所用的字节就是当前
     *   字节。当前哈希由 clear_covered 在其 IMMEDIATE 事务内重算，判定
     *   与 DELETE 之间不存在调用方先算好、事务外再用的 TOCTOU 窗口。
     * - 覆盖但哈希不同 → 保留：批运行期间又被编辑，下一批必须重跑。
     */
    struct PendingClearInput
    {
        std::string path;            // 队列中的原始路径（删除键）
        std::string snapshot_hash;   // 批前内容哈希（空 = 批前读不到）
        bool        tracked = true;  // 在 db/compile db 中
        bool        covered = false; // 在本批 IndexSummary::covered_files 中
    };

    /** @brief 分层查找接受的 --kind / --file 消歧器。 */
    struct SymbolFilter
    {
        std::optional<SymbolKind>       kind = std::nullopt;
        std::optional<std::string_view> file_substring = std::nullopt;
    };

    struct FindResult
    {
        int tier = 0; // 1 精确，2 限定名后缀，3 子串（0 = 无命中）
        std::vector<SymbolRow> matches;
    };

} // namespace cg::db

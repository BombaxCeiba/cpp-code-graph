/**
 * @module cg.analysis.snapshot
 *
 * @brief 槽侧内存态的多段单文件快照：dispatch 提交后原子落盘，下一
 *        进程装载时 mmap 恢复 + 按文件指纹 delta，替代 hints 全表读与
 *        analysis_slots 全表读（warm）。
 *
 * @internal 文件形态：定长头部（magic/版本戳/snapshot_seq/计数与
 *           CRC）+ 段目录（kind/offset/bytes/crc32，10 段）+ 数据段
 *           （16 字节对齐，逐段 CRC32）+ 尾部（数据区总长/数据区
 *           CRC/magic 复写）。段：字符串池（写侧去重）、槽记录（定长
 *           128 字节，初始与终态两套区段+标量）、var 域、类型域（字
 *           典序）、初始 arena、终态 arena、行级别名边、槽源文件索引、
 *           静态类型集、指纹基线。全部整数小端固定，头部端序标记不符
 *           即弃；段内 int64 访问依赖 16 字节段对齐（映射地址页对齐 +
 *           段偏移 16 的倍数），未对齐访问不出现在任何路径上。
 *
 *           身份与失效：快照路径 = `<db_path>`.snap（与库同目录同基名，
 *           多配置库天然隔离）。头部 snapshot_seq 与库 meta 的同名键
 *           相等 + 全部版本戳相符才可恢复；任一失配或任何段 CRC 坏都
 *           弃快照，调用方走既有全表读路径（零分叉降级）。写侧在提
 *           交事务内递增 meta snapshot_seq 并序列化，事务提交成功后由
 *           调用方经 tmp+atomic_replace 落盘——先库后快照，崩溃窗口
 *           全安全（库提交快照未写则 seq 失配自弃，不存在快照比库新
 *           的窗口）。并发读者只读映射与单写者原子替换的共存安全性由
 *           原子替换协议背书（旧映射者继续读旧字节）。
 *
 *           槽身份存键字符串不存符号 id：符号行 id 在文件重写后不稳
 *           定（删旧插新），owner 规范 id 随符号投影首见序漂移；恢复
 *           时以键串经当前符号域重新规范化，符号已删的槽丢弃——其源
 *           文件必在指纹脏集，SQL 重建路径同样建不出该槽，两条装载路
 *           径行为一致。var 域 id 在恢复批保持快照值（域原样载入，脏
 *           文件新变量追加新号），域 id 连续由恢复侧自检验证。
 *
 *           类型域保序：恢复域 = sort(快照域 ∪ 脏文件新类型串)，新串
 *           插入产生保序重映射（旧 id → 新 id 严格递增），两个 arena
 *           段逐元素重映射后 "整数序 == 字符串序" 的全局不变量仍然成
 *           立（域是字典序超集，序性质保持；MAX_TYPES 截断与集合并
 *           集归并都依赖该性质）。域内含已从库中消失的串是常态（脏文
 *           件删除形态），只是内部编号有洞，对一切消费面不可观测。
 *
 *           双 arena 必要性：闭包内槽复位到初始值重算、闭包外槽沿用
 *           warm 终态——初始态与终态都要持久化，且传播期的追加与分代
 *           压缩不回写快照，两份 buf 在各自时刻独立拍下。
 *
 *           x64-only：32 位进程不做映射+堆双份（超 2GB 地址空间），
 *           恢复与写盘资格由调用方按库路径有效性判定。
 */

module;
#include <cstddef> // offsetof 是宏，import std 不透传宏，须在全局模块片段引入

export module cg.analysis.snapshot;

import std;
import cg.util;
import cg.util.platform;
import cg.db;

export namespace cg::analysis::snap
{

    /** @brief 快照格式版本（头部 fmt_ver；结构变更时递增，失配即弃）。 */
    inline constexpr std::uint32_t format_version = 1;

    /** @brief 头部与尾部的魔数。 */
    inline constexpr std::string_view magic = "CCGSNAP1";

    /** @brief 槽记录的定长字节数（快照内形态）。 */
    inline constexpr std::size_t slot_record_bytes = 128;

    /**
     * @brief 槽的快照记录：初始态与终态各一套区段+标量。
     *
     * @internal 初始标量取装载段收尾时刻（oracle 之后）的槽形态；终态标
     *           量取 finalize 循环的 PropagatedHint 形态（写回八列同源）。
     *           字符串在文件内经串池 (off,len) 引用，恢复后物化为串；
     *           static_types 与 files 是与槽对齐的变长附属集。
     */
    struct SlotRec
    {
        std::string owner_key;
        std::string var_name;
        // ---- 初始态（装载段收尾）----
        std::uint32_t init_off = 0;
        std::uint32_t init_len = 0;
        int           confidence = 0;
        int           source = 0;
        std::string   reason;
        std::string   related_var;
        std::string   related_key;
        int           param_index = -1;
        bool          has_outparam_flow = false;
        /** @brief 槽带 out_param_this 派生行（层级变化批的种子面）。 */
        bool derived_seed = false;
        // ---- 终态（finalize；warm 行同源）----
        std::uint32_t warm_off = 0;
        std::uint32_t warm_len = 0;
        int           warm_confidence = 0;
        int           warm_source = 0;
        std::string   warm_reason;
        std::string   alias_src;
        std::string   ret_src;
        bool          truncated = false;
        // ---- 平面附属 ----
        std::vector<std::string>    static_types;
        std::vector<cg::db::FileId> files;
    };

    /**
     * @brief 一份快照的完整平面数据（写侧装配、读侧恢复同构）。
     *
     * @internal arena 的 int32 是类型域下标；var_domain 条目 id 连续
     *           [0, next_var_id)；row_alias 的槽下标域与 slots 同序；
     *           fingerprints 是写侧提交事务内的全表终态（恢复侧的比对
     *           基线）。
     */
    struct SnapshotData
    {
        /** @brief 写侧事务内递增后的 meta 值（头部身份戳）。 */
        std::uint64_t                                     snapshot_seq = 0;
        std::uint32_t                                     slots_ver = 0;
        std::uint32_t                                     schema_version = 0;
        std::vector<SlotRec>                              slots;
        std::vector<std::int32_t>                         arena_init;
        std::vector<std::int32_t>                         arena_warm;
        std::vector<std::pair<std::string, std::int32_t>> var_domain;
        std::int32_t                                      next_var_id = 0;
        /** @brief 类型域（字典序；恢复侧 merge 脏文件新串后整体保序重映射）。
         */
        std::vector<std::string> type_of_id;
        /** @brief 行级别名边（槽下标, related_var）。 */
        std::vector<std::pair<std::uint32_t, std::string>> row_alias;
        /** @brief 指纹基线。 */
        std::vector<cg::db::FingerprintRow> fingerprints;
    };

    /**
     * @brief 恢复请求：路径 + 库内 snapshot_seq 当前值（调用方在装载前读）。
     */
    struct RestoreRequest
    {
        std::filesystem::path path;
        std::uint64_t         snapshot_seq = 0;
    };

    /**
     * @brief 恢复结果：平面数据 + 指纹比对出的脏文件集（快照写后库指纹
     *        变化过的文件：条目新增/删除/字段变化，按 (file_id, cmd_digest)
     *        主键对称差）。
     */
    struct RestoreResult
    {
        SnapshotData                       data;
        std::unordered_set<cg::db::FileId> dirty_files;
    };

    namespace detail
    {

        [[nodiscard]]
        inline auto crc32(std::span<const std::byte> bytes) -> std::uint32_t
        {
            static const std::array<std::uint32_t, 256> table = []
            {
                std::array<std::uint32_t, 256> t{};
                for (std::uint32_t i = 0; i < 256; ++i)
                {
                    std::uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                    {
                        c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
                    }
                    t[i] = c;
                }
                return t;
            }();
            std::uint32_t c = 0xFFFFFFFFU;
            for (const std::byte b : bytes)
            {
                c = table[(c ^ static_cast<std::uint8_t>(b)) & 0xFFU]
                    ^ (c >> 8);
            }
            return c ^ 0xFFFFFFFFU;
        }

        /** @brief 串池：写侧去重追加，(off,len) 引用。 */
        class StringPool
        {
        public:
            /** @brief 登记或复用，返回串池内偏移（长度由调用方自持）。 */
            [[nodiscard]]
            auto add(std::string_view s) -> std::uint32_t
            {
                if (const auto it = index_.find(s); it != index_.end())
                {
                    return it->second;
                }
                const auto off = static_cast<std::uint32_t>(buf_.size());
                buf_.append(s);
                index_.emplace(std::string{s}, off);
                return off;
            }

            [[nodiscard]]
            auto take() -> std::string
            {
                return std::move(buf_);
            }

        private:
            std::string buf_;
            // 透明哈希：intern(string_view) 的查重路径免临时键拷贝。
            std::unordered_map<
                std::string,
                std::uint32_t,
                cg::util::string_hash,
                std::equal_to<>>
                index_;
        };

        /** @brief 段构造器：字节追加 + 16 字节尾部对齐。 */
        class SegmentBuilder
        {
        public:
            /** @brief 当前段内偏移（供变长记录互指）。 */
            [[nodiscard]]
            auto size() const -> std::size_t
            {
                return buf_.size();
            }

            void append(std::span<const std::byte> bytes)
            {
                buf_.append(
                    reinterpret_cast<const char*>(bytes.data()),
                    bytes.size());
            }

            template <typename Pod>
            void append_pod(const Pod& value)
            {
                static_assert(std::is_trivially_copyable_v<Pod>);
                append(
                    {reinterpret_cast<const std::byte*>(&value), sizeof(Pod)});
            }

            /** @brief 尾部对齐到 16 字节（填充零字节）。 */
            void align()
            {
                while (buf_.size() % 16 != 0)
                {
                    buf_.push_back('\0');
                }
            }

            [[nodiscard]]
            auto take() -> std::string
            {
                return std::move(buf_);
            }

        private:
            std::string buf_;
        };

        /** @brief 快照头部（96 字节；小端）。 */
        struct HeaderPod
        {
            char          magic[8];
            std::uint32_t fmt_ver;
            std::uint32_t slots_ver;
            std::uint32_t schema_version;
            std::uint32_t endian_marker;
            std::uint64_t snapshot_seq;
            std::uint64_t slot_count;
            std::uint32_t var_count;
            std::uint32_t next_var_id;
            std::uint64_t type_count;
            std::uint64_t arena_init_count;
            std::uint64_t arena_warm_count;
            std::uint64_t row_alias_count;
            std::uint64_t fingerprint_count;
            std::uint32_t header_crc32; // 覆盖本结构此前全部字节
            std::uint32_t reserved;
        };
        static_assert(sizeof(HeaderPod) == 96);

        /** @brief 段目录条目（32 字节）。 */
        struct SegEntryPod
        {
            std::uint32_t kind;
            std::uint32_t reserved;
            std::uint64_t offset;
            std::uint64_t bytes;
            std::uint32_t crc32;
            std::uint32_t reserved2;
        };
        static_assert(sizeof(SegEntryPod) == 32);

        /** @brief 槽记录（128 字节；字段全 4 字节宽，无对齐洞）。 */
        struct SlotPod
        {
            std::uint32_t owner_off, owner_len;
            std::uint32_t var_off, var_len;
            std::uint32_t init_off, init_len;
            std::int32_t  confidence;
            std::int32_t  source;
            std::uint32_t reason_off, reason_len;
            std::uint32_t relvar_off, relvar_len;
            std::uint32_t relkey_off, relkey_len;
            std::int32_t  param_index;
            std::uint32_t pad0;
            std::uint32_t warm_off, warm_len;
            std::int32_t  warm_confidence;
            std::int32_t  warm_source;
            std::uint32_t wreason_off, wreason_len;
            std::uint32_t alias_off, alias_len;
            std::uint32_t ret_off, ret_len;
            std::uint32_t statics_off;
            std::uint32_t files_off;
            std::uint32_t
                flags;          // bit0 = has_outparam_flow，bit1 = derived_seed
            std::uint32_t tail; // byte0 = truncated
            std::uint32_t reserved[2];
        };
        static_assert(sizeof(SlotPod) == slot_record_bytes);

    } // namespace detail

    /**
     * @brief 序列化一份快照（头部、段目录、10 个数据段、尾部）。
     *
     * @return 完整文件字节；空串表示数据自身不健全（var 域 id 不连续或
     *         arena 的 id 越出类型域），调用方按"快照不可写"处理。
     */
    [[nodiscard]]
    inline auto serialize(const SnapshotData& data) -> std::string
    {
        using detail::SegmentBuilder;
        using detail::SlotPod;
        using detail::StringPool;

        // ---- 平面自检：var 域 id 连续、arena id 落在类型域内。----
        std::vector<char> var_seen(
            data.next_var_id > 0 ? static_cast<std::size_t>(data.next_var_id)
                                 : 0,
            0);
        for (const auto& [name, id] : data.var_domain)
        {
            (void)name;
            if (id < 0 || static_cast<std::size_t>(id) >= var_seen.size()
                || var_seen[static_cast<std::size_t>(id)] != 0)
            {
                return {};
            }
            var_seen[static_cast<std::size_t>(id)] = 1;
        }
        for (const char seen : var_seen)
        {
            if (seen == 0)
            {
                return {};
            }
        }
        const std::uint64_t type_count = data.type_of_id.size();
        const auto          ids_ok = [&](const std::vector<std::int32_t>& buf)
        {
            for (const std::int32_t id : buf)
            {
                if (id < 0 || static_cast<std::uint64_t>(id) >= type_count)
                {
                    return false;
                }
            }
            return true;
        };
        if (!ids_ok(data.arena_init) || !ids_ok(data.arena_warm))
        {
            return {};
        }

        // ---- 段构造（顺序即段目录 kind 1..10）。----
        StringPool pool;
        const auto sref = [&](std::string_view s)
        {
            const std::uint32_t off = pool.add(s);
            return std::pair<std::uint32_t, std::uint32_t>{
                off,
                static_cast<std::uint32_t>(s.size())};
        };

        SegmentBuilder seg_slots;
        SegmentBuilder seg_var;
        SegmentBuilder seg_types;
        SegmentBuilder seg_init;
        SegmentBuilder seg_warm;
        SegmentBuilder seg_alias;
        SegmentBuilder seg_slotfiles;
        SegmentBuilder seg_statics;
        SegmentBuilder seg_fps;

        for (const SlotRec& s : data.slots)
        {
            SlotPod pod{};
            const auto [owner_off, owner_len] = sref(s.owner_key);
            const auto [var_off, var_len] = sref(s.var_name);
            const auto [reason_off, reason_len] = sref(s.reason);
            const auto [relvar_off, relvar_len] = sref(s.related_var);
            const auto [relkey_off, relkey_len] = sref(s.related_key);
            const auto [wreason_off, wreason_len] = sref(s.warm_reason);
            const auto [alias_off, alias_len] = sref(s.alias_src);
            const auto [ret_off, ret_len] = sref(s.ret_src);
            pod.owner_off = owner_off;
            pod.owner_len = owner_len;
            pod.var_off = var_off;
            pod.var_len = var_len;
            pod.init_off = s.init_off;
            pod.init_len = s.init_len;
            pod.confidence = s.confidence;
            pod.source = s.source;
            pod.reason_off = reason_off;
            pod.reason_len = reason_len;
            pod.relvar_off = relvar_off;
            pod.relvar_len = relvar_len;
            pod.relkey_off = relkey_off;
            pod.relkey_len = relkey_len;
            pod.param_index = s.param_index;
            pod.warm_off = s.warm_off;
            pod.warm_len = s.warm_len;
            pod.warm_confidence = s.warm_confidence;
            pod.warm_source = s.warm_source;
            pod.wreason_off = wreason_off;
            pod.wreason_len = wreason_len;
            pod.alias_off = alias_off;
            pod.alias_len = alias_len;
            pod.ret_off = ret_off;
            pod.ret_len = ret_len;
            pod.flags =
                (s.has_outparam_flow ? 1U : 0U) | (s.derived_seed ? 2U : 0U);
            pod.tail = s.truncated ? 1U : 0U;
            pod.statics_off = static_cast<std::uint32_t>(seg_statics.size());
            const auto n_statics =
                static_cast<std::uint32_t>(s.static_types.size());
            seg_statics.append_pod(n_statics);
            for (const std::string& t : s.static_types)
            {
                const auto [off, len] = sref(t);
                seg_statics.append_pod(off);
                seg_statics.append_pod(len);
            }
            pod.files_off = static_cast<std::uint32_t>(seg_slotfiles.size());
            const auto n_files = static_cast<std::uint32_t>(s.files.size());
            seg_slotfiles.append_pod(n_files);
            for (const cg::db::FileId f : s.files)
            {
                seg_slotfiles.append_pod(f);
            }
            seg_slots.append_pod(pod);
        }
        for (const auto& [name, id] : data.var_domain)
        {
            const auto [off, len] = sref(name);
            seg_var.append_pod(off);
            seg_var.append_pod(len);
            seg_var.append_pod(id);
        }
        for (const std::string& t : data.type_of_id)
        {
            const auto [off, len] = sref(t);
            seg_types.append_pod(off);
            seg_types.append_pod(len);
        }
        for (const std::int32_t v : data.arena_init)
        {
            seg_init.append_pod(v);
        }
        for (const std::int32_t v : data.arena_warm)
        {
            seg_warm.append_pod(v);
        }
        for (const auto& [slot, rel] : data.row_alias)
        {
            const auto [off, len] = sref(rel);
            seg_alias.append_pod(slot);
            seg_alias.append_pod(off);
            seg_alias.append_pod(len);
        }
        for (const cg::db::FingerprintRow& fp : data.fingerprints)
        {
            const auto [off, len] = sref(fp.cmd_digest);
            seg_fps.append_pod(fp.file_id);
            seg_fps.append_pod(fp.mtime_ns);
            seg_fps.append_pod(fp.size);
            seg_fps.append_pod(off);
            seg_fps.append_pod(len);
        }

        SegmentBuilder    seg_strings;
        const std::string strings = pool.take();
        seg_strings.append(
            {reinterpret_cast<const std::byte*>(strings.data()),
             strings.size()});

        SegmentBuilder* builders[] = {
            &seg_strings,
            &seg_slots,
            &seg_var,
            &seg_types,
            &seg_init,
            &seg_warm,
            &seg_alias,
            &seg_slotfiles,
            &seg_statics,
            &seg_fps};
        struct Segment
        {
            std::string bytes;
        };
        std::vector<Segment> segments;
        segments.reserve(10);
        for (SegmentBuilder* b : builders)
        {
            b->align();
            segments.push_back({.bytes = b->take()});
        }

        // ---- 头部 + 目录 + 数据 + 尾部组装。----
        detail::HeaderPod header{};
        std::memcpy(header.magic, magic.data(), magic.size());
        header.fmt_ver = format_version;
        header.slots_ver = data.slots_ver;
        header.schema_version = data.schema_version;
        header.endian_marker = 0x01020304U;
        header.snapshot_seq = data.snapshot_seq;
        header.slot_count = data.slots.size();
        header.var_count = data.var_domain.size();
        header.next_var_id = static_cast<std::uint32_t>(data.next_var_id);
        header.type_count = type_count;
        header.arena_init_count = data.arena_init.size();
        header.arena_warm_count = data.arena_warm.size();
        header.row_alias_count = data.row_alias.size();
        header.fingerprint_count = data.fingerprints.size();

        constexpr std::size_t n_seg = 10;
        const std::size_t     data_offset =
            sizeof(detail::HeaderPod) + n_seg * sizeof(detail::SegEntryPod);
        std::size_t                      cursor = data_offset;
        std::vector<detail::SegEntryPod> entries;
        entries.reserve(n_seg);
        for (std::size_t i = 0; i < n_seg; ++i)
        {
            detail::SegEntryPod entry{};
            entry.kind = static_cast<std::uint32_t>(i + 1);
            entry.offset = cursor;
            entry.bytes = segments[i].bytes.size();
            entry.crc32 = detail::crc32(
                {reinterpret_cast<const std::byte*>(segments[i].bytes.data()),
                 segments[i].bytes.size()});
            entries.push_back(entry);
            cursor += segments[i].bytes.size();
        }

        std::string out;
        out.reserve(cursor + 24);
        const std::span<const std::byte> head_prefix{
            reinterpret_cast<const std::byte*>(&header),
            offsetof(detail::HeaderPod, header_crc32)};
        header.header_crc32 = detail::crc32(head_prefix);
        out.append(reinterpret_cast<const char*>(&header), sizeof(header));
        out.append(
            reinterpret_cast<const char*>(entries.data()),
            entries.size() * sizeof(detail::SegEntryPod));
        for (const Segment& seg : segments)
        {
            out += seg.bytes;
        }
        const std::uint64_t total = cursor;
        const std::uint32_t data_crc = detail::crc32(
            {reinterpret_cast<const std::byte*>(out.data()) + data_offset,
             total - data_offset});
        char tail[24];
        std::memcpy(tail, &total, sizeof(total));
        std::memcpy(tail + 8, &data_crc, sizeof(data_crc));
        const std::uint32_t tail_pad = 0;
        std::memcpy(tail + 12, &tail_pad, sizeof(tail_pad));
        std::memcpy(tail + 16, magic.data(), magic.size());
        out.append(tail, sizeof(tail));
        return out;
    }

    /**
     * @brief 尝试恢复：mmap 快照，校验身份与完整性，比对指纹得脏文件集。
     *
     * @param req 恢复请求（路径 + 库 meta snapshot_seq 当前值）。
     * @param slots_ver 期望的槽行版本戳（analysis_slots_ver）。
     * @param schema_version 期望的库 schema 版本。
     * @param fingerprints_now 库内当前全量指纹（调用方读一次传入）。
     * @param miss_reason 失败时写入人话原因（可观测面）；成功时清空。
     * @return 恢复结果；任何校验失败返回 nullopt（调用方走全表读路径）。
     */
    [[nodiscard]]
    inline auto restore(
        const RestoreRequest&                   req,
        std::uint32_t                           slots_ver,
        std::uint32_t                           schema_version,
        std::span<const cg::db::FingerprintRow> fingerprints_now,
        std::string& miss_reason) -> std::optional<RestoreResult>
    {
        using detail::HeaderPod;
        using detail::SegEntryPod;

        miss_reason.clear();
        const auto fail = [&miss_reason](std::string_view why)
        {
            miss_reason.assign(why);
            return std::optional<RestoreResult>{};
        };
        const cg::util::platform::ReadOnlyMap map{req.path};
        if (!map.valid())
        {
            return fail("open");
        }
        const std::span<const std::byte> bytes = map.bytes();
        constexpr std::size_t            n_seg = 10;
        constexpr std::size_t            min_size =
            sizeof(HeaderPod) + n_seg * sizeof(SegEntryPod) + 24;
        if (bytes.size() < min_size)
        {
            return fail("size");
        }

        // ---- 头部：身份与 CRC。----
        const auto* head = reinterpret_cast<const HeaderPod*>(bytes.data());
        if (std::memcmp(head->magic, magic.data(), magic.size()) != 0)
        {
            return fail("magic");
        }
        if (head->endian_marker != 0x01020304U)
        {
            return fail("endian");
        }
        if (detail::crc32({bytes.data(), offsetof(HeaderPod, header_crc32)})
            != head->header_crc32)
        {
            return fail("header-crc");
        }
        if (head->fmt_ver != format_version)
        {
            return fail("fmt-ver");
        }
        if (head->slots_ver != slots_ver
            || head->schema_version != schema_version)
        {
            return fail("ver");
        }
        if (head->snapshot_seq != req.snapshot_seq)
        {
            return fail("seq");
        }

        // ---- 尾部与段目录（目录顺序即 kind 1..10；逐段 CRC 全量校验）。----
        const std::size_t data_offset =
            sizeof(HeaderPod) + n_seg * sizeof(SegEntryPod);
        const std::size_t tail_offset = bytes.size() - 24;
        if (tail_offset < data_offset)
        {
            return fail("size");
        }
        std::uint64_t total = 0;
        std::uint32_t data_crc = 0;
        std::memcpy(&total, bytes.data() + tail_offset, sizeof(total));
        std::memcpy(
            &data_crc,
            bytes.data() + tail_offset + 8,
            sizeof(data_crc));
        if (total != tail_offset
            || std::memcmp(
                   bytes.data() + tail_offset + 16,
                   magic.data(),
                   magic.size())
                   != 0)
        {
            return fail("tail");
        }
        if (detail::crc32({bytes.data() + data_offset, total - data_offset})
            != data_crc)
        {
            return fail("data-crc");
        }
        const auto* entries = reinterpret_cast<const SegEntryPod*>(
            bytes.data() + sizeof(HeaderPod));
        const auto seg_span = [&](std::size_t idx) -> std::span<const std::byte>
        {
            const SegEntryPod& e = entries[idx];
            return {bytes.data() + e.offset, static_cast<std::size_t>(e.bytes)};
        };
        for (std::size_t i = 0; i < n_seg; ++i)
        {
            const SegEntryPod& e = entries[i];
            // 段目录不受任何 CRC 覆盖，本行是唯一防线；offset/bytes 均 64
            // 位无符号，"和上界"写法在 offset 或 bytes 接近 2^64 时回绕成小
            // 值骗过检查（放行后越界 span 野读）。先证上界再作减法：两支合
            // 取与"数学意义上 e.offset + e.bytes <= tail_offset"完全等价。
            if (e.kind != i + 1 || e.offset < data_offset
                || e.offset > tail_offset || e.bytes > tail_offset - e.offset
                || (e.offset % 16) != 0)
            {
                return fail("seg-dir");
            }
            if (detail::crc32(seg_span(i)) != e.crc32)
            {
                return fail("seg-crc");
            }
        }

        // ---- 定长读取辅助（全部 memcpy，无对齐假设之外的访问）。----
        const auto u32_at = [](std::span<const std::byte> seg,
                               std::size_t                off) -> std::uint32_t
        {
            std::uint32_t v = 0;
            std::memcpy(&v, seg.data() + off, sizeof(v));
            return v;
        };
        const auto i32_at = [](std::span<const std::byte> seg,
                               std::size_t                off) -> std::int32_t
        {
            std::int32_t v = 0;
            std::memcpy(&v, seg.data() + off, sizeof(v));
            return v;
        };
        const auto i64_at = [](std::span<const std::byte> seg,
                               std::size_t                off) -> std::int64_t
        {
            std::int64_t v = 0;
            std::memcpy(&v, seg.data() + off, sizeof(v));
            return v;
        };
        const auto fid_at = [&i64_at](
                                std::span<const std::byte> seg,
                                std::size_t off) -> cg::db::FileId
        { return static_cast<cg::db::FileId>(i64_at(seg, off)); };
        const std::span<const std::byte> seg_strings = seg_span(0);
        const std::span<const std::byte> seg_slots = seg_span(1);
        const std::span<const std::byte> seg_var = seg_span(2);
        const std::span<const std::byte> seg_types = seg_span(3);
        const std::span<const std::byte> seg_init = seg_span(4);
        const std::span<const std::byte> seg_warm = seg_span(5);
        const std::span<const std::byte> seg_alias = seg_span(6);
        const std::span<const std::byte> seg_slotfiles = seg_span(7);
        const std::span<const std::byte> seg_statics = seg_span(8);
        const std::span<const std::byte> seg_fps = seg_span(9);
        const auto str_at = [&](std::uint32_t off,
                                std::uint32_t len) -> std::string_view
        {
            if (static_cast<std::uint64_t>(off) + len > seg_strings.size())
            {
                return {};
            }
            return {
                reinterpret_cast<const char*>(seg_strings.data()) + off,
                len};
        };

        // ---- 计数一致性（头 vs 段字节量）：段尾有 16 字节对齐填充，等值
        //      检查不成立——校验"段足以容纳头声明的条目量"即可，条目读取
        //      逐条走头计数（指纹条目 32 字节/条）。----
        const auto fits = [](std::uint64_t seg_bytes, std::uint64_t need)
        { return seg_bytes >= need; };
        if (!fits(seg_slots.size(), head->slot_count * slot_record_bytes)
            || !fits(
                seg_var.size(),
                static_cast<std::uint64_t>(head->var_count) * 12)
            || !fits(seg_types.size(), head->type_count * 8)
            || !fits(seg_init.size(), head->arena_init_count * 4)
            || !fits(seg_warm.size(), head->arena_warm_count * 4)
            || !fits(seg_alias.size(), head->row_alias_count * 12)
            || !fits(seg_fps.size(), head->fingerprint_count * 32))
        {
            return fail("counts");
        }

        // ---- 指纹比对：键 = (file_id, cmd_digest) 的精确对（有序 map 免
        //      哈希碰撞误判），对称差即脏文件集。----
        struct FpVal
        {
            std::int64_t mtime_ns;
            std::int64_t size;
        };
        using FpKey = std::pair<cg::db::FileId, std::string>;
        std::map<FpKey, FpVal> fp_now;
        for (const cg::db::FingerprintRow& fp : fingerprints_now)
        {
            fp_now.emplace(
                FpKey{fp.file_id, fp.cmd_digest},
                FpVal{fp.mtime_ns, fp.size});
        }
        RestoreResult          result;
        std::map<FpKey, FpVal> fp_base;
        for (std::size_t i = 0; i < head->fingerprint_count; ++i)
        {
            const std::size_t off = i * 32;
            const FpKey       key{
                fid_at(seg_fps, off),
                std::string{str_at(
                    u32_at(seg_fps, off + 24),
                    u32_at(seg_fps, off + 28))}};
            const FpVal val{
                i64_at(seg_fps, off + 8),
                i64_at(seg_fps, off + 16)};
            if (!fp_base.emplace(key, val).second)
            {
                return fail("fp-dup");
            }
        }
        for (const auto& [key, val] : fp_base)
        {
            const auto it = fp_now.find(key);
            if (it == fp_now.end() || it->second.mtime_ns != val.mtime_ns
                || it->second.size != val.size)
            {
                result.dirty_files.insert(key.first);
            }
        }
        for (const auto& [key, val] : fp_now)
        {
            (void)val;
            if (!fp_base.contains(key))
            {
                result.dirty_files.insert(key.first);
            }
        }

        // ---- 平面数据物化。----
        SnapshotData& data = result.data;
        data.snapshot_seq = head->snapshot_seq;
        data.slots_ver = head->slots_ver;
        data.schema_version = head->schema_version;
        data.next_var_id = static_cast<std::int32_t>(head->next_var_id);

        data.arena_init.resize(
            static_cast<std::size_t>(head->arena_init_count));
        for (std::size_t i = 0; i < data.arena_init.size(); ++i)
        {
            data.arena_init[i] = i32_at(seg_init, i * 4);
        }
        data.arena_warm.resize(
            static_cast<std::size_t>(head->arena_warm_count));
        for (std::size_t i = 0; i < data.arena_warm.size(); ++i)
        {
            data.arena_warm[i] = i32_at(seg_warm, i * 4);
        }
        const auto arena_has = [](const std::vector<std::int32_t>& buf,
                                  std::uint32_t                    off,
                                  std::uint32_t                    len)
        { return static_cast<std::uint64_t>(off) + len <= buf.size(); };

        data.type_of_id.reserve(static_cast<std::size_t>(head->type_count));
        for (std::size_t i = 0; i < head->type_count; ++i)
        {
            data.type_of_id.emplace_back(
                str_at(u32_at(seg_types, i * 8), u32_at(seg_types, i * 8 + 4)));
        }
        for (std::size_t i = 1; i < data.type_of_id.size(); ++i)
        {
            if (!(data.type_of_id[i - 1] < data.type_of_id[i]))
            {
                return fail("type-order");
            }
        }

        data.var_domain.reserve(head->var_count);
        // 空串名是合法域条目（匿名槽会 intern 空串），洞的判定必须用独立
        // 的已写标记而非串内容。
        std::vector<std::string> var_names(
            static_cast<std::size_t>(head->next_var_id));
        std::vector<char> var_written(var_names.size(), 0);
        for (std::size_t i = 0; i < head->var_count; ++i)
        {
            const std::size_t off = i * 12;
            std::string_view  name =
                str_at(u32_at(seg_var, off), u32_at(seg_var, off + 4));
            const std::int32_t id = i32_at(seg_var, off + 8);
            if (id < 0 || static_cast<std::size_t>(id) >= var_written.size()
                || var_written[static_cast<std::size_t>(id)] != 0)
            {
                return fail("var-domain");
            }
            var_written[static_cast<std::size_t>(id)] = 1;
            var_names[static_cast<std::size_t>(id)].assign(name);
            data.var_domain.emplace_back(std::string{name}, id);
        }
        for (const char written : var_written)
        {
            if (written == 0)
            {
                return fail("var-gap");
            }
        }

        data.slots.reserve(static_cast<std::size_t>(head->slot_count));
        // 布局单源：槽记录字段偏移一律取自写侧 SlotPod（sizeof 已由
        // static_assert 钉为 slot_record_bytes），增删/挪动字段时读侧自动
        // 跟随，不再手抄魔法偏移。
        using detail::SlotPod;
        for (std::size_t i = 0; i < head->slot_count; ++i)
        {
            const std::size_t off = i * slot_record_bytes;
            SlotRec           s;
            s.owner_key = str_at(
                u32_at(seg_slots, off + offsetof(SlotPod, owner_off)),
                u32_at(seg_slots, off + offsetof(SlotPod, owner_len)));
            s.var_name = str_at(
                u32_at(seg_slots, off + offsetof(SlotPod, var_off)),
                u32_at(seg_slots, off + offsetof(SlotPod, var_len)));
            s.init_off = u32_at(seg_slots, off + offsetof(SlotPod, init_off));
            s.init_len = u32_at(seg_slots, off + offsetof(SlotPod, init_len));
            s.confidence =
                i32_at(seg_slots, off + offsetof(SlotPod, confidence));
            s.source = i32_at(seg_slots, off + offsetof(SlotPod, source));
            s.reason = str_at(
                u32_at(seg_slots, off + offsetof(SlotPod, reason_off)),
                u32_at(seg_slots, off + offsetof(SlotPod, reason_len)));
            s.related_var = str_at(
                u32_at(seg_slots, off + offsetof(SlotPod, relvar_off)),
                u32_at(seg_slots, off + offsetof(SlotPod, relvar_len)));
            s.related_key = str_at(
                u32_at(seg_slots, off + offsetof(SlotPod, relkey_off)),
                u32_at(seg_slots, off + offsetof(SlotPod, relkey_len)));
            s.param_index =
                i32_at(seg_slots, off + offsetof(SlotPod, param_index));
            s.warm_off = u32_at(seg_slots, off + offsetof(SlotPod, warm_off));
            s.warm_len = u32_at(seg_slots, off + offsetof(SlotPod, warm_len));
            s.warm_confidence =
                i32_at(seg_slots, off + offsetof(SlotPod, warm_confidence));
            s.warm_source =
                i32_at(seg_slots, off + offsetof(SlotPod, warm_source));
            s.warm_reason = str_at(
                u32_at(seg_slots, off + offsetof(SlotPod, wreason_off)),
                u32_at(seg_slots, off + offsetof(SlotPod, wreason_len)));
            s.alias_src = str_at(
                u32_at(seg_slots, off + offsetof(SlotPod, alias_off)),
                u32_at(seg_slots, off + offsetof(SlotPod, alias_len)));
            s.ret_src = str_at(
                u32_at(seg_slots, off + offsetof(SlotPod, ret_off)),
                u32_at(seg_slots, off + offsetof(SlotPod, ret_len)));
            const std::uint32_t statics_off =
                u32_at(seg_slots, off + offsetof(SlotPod, statics_off));
            const std::uint32_t files_off =
                u32_at(seg_slots, off + offsetof(SlotPod, files_off));
            s.has_outparam_flow =
                (u32_at(seg_slots, off + offsetof(SlotPod, flags)) & 1U) != 0;
            s.derived_seed =
                (u32_at(seg_slots, off + offsetof(SlotPod, flags)) & 2U) != 0;
            s.truncated =
                (u32_at(seg_slots, off + offsetof(SlotPod, tail)) & 0xFFU) != 0;
            if (s.owner_key.empty())
            {
                return fail("slot-owner");
            }
            if (!arena_has(data.arena_init, s.init_off, s.init_len)
                || !arena_has(data.arena_warm, s.warm_off, s.warm_len))
            {
                return fail("slot-seg");
            }
            if (static_cast<std::uint64_t>(statics_off) + 4
                > seg_statics.size())
            {
                return fail("statics");
            }
            const std::uint32_t n_statics = u32_at(seg_statics, statics_off);
            if (static_cast<std::uint64_t>(statics_off) + 4
                    + static_cast<std::uint64_t>(n_statics) * 8
                > seg_statics.size())
            {
                return fail("statics");
            }
            s.static_types.reserve(n_statics);
            for (std::uint32_t k = 0; k < n_statics; ++k)
            {
                const std::size_t soff = statics_off + 4 + k * 8;
                s.static_types.emplace_back(str_at(
                    u32_at(seg_statics, soff),
                    u32_at(seg_statics, soff + 4)));
            }
            if (static_cast<std::uint64_t>(files_off) + 4
                > seg_slotfiles.size())
            {
                return fail("slotfiles");
            }
            const std::uint32_t n_files = u32_at(seg_slotfiles, files_off);
            if (static_cast<std::uint64_t>(files_off) + 4
                    + static_cast<std::uint64_t>(n_files) * 8
                > seg_slotfiles.size())
            {
                return fail("slotfiles");
            }
            s.files.reserve(n_files);
            for (std::uint32_t k = 0; k < n_files; ++k)
            {
                s.files.push_back(fid_at(seg_slotfiles, files_off + 4 + k * 8));
            }
            data.slots.push_back(std::move(s));
        }

        data.row_alias.reserve(static_cast<std::size_t>(head->row_alias_count));
        for (std::size_t i = 0; i < head->row_alias_count; ++i)
        {
            const std::size_t   off = i * 12;
            const std::uint32_t slot = u32_at(seg_alias, off);
            if (slot >= data.slots.size())
            {
                return fail("alias-slot");
            }
            data.row_alias.emplace_back(
                slot,
                std::string{str_at(
                    u32_at(seg_alias, off + 4),
                    u32_at(seg_alias, off + 8))});
        }
        return result;
    }

    /**
     * @brief 快照路径约定：`<db_path>`.snap（与库同目录同基名，多配置库与
     *        多库并存天然隔离）。
     */
    [[nodiscard]]
    inline auto snapshot_path(const std::string& db_path)
        -> std::filesystem::path
    {
        std::filesystem::path p{db_path};
        p += ".snap";
        return p;
    }

} // namespace cg::analysis::snap

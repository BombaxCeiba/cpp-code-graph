module;
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
// read_wrap_log 经 sqlpp23 读 capture.db，表模型由 import
// cg.msbuild.tables 取得。残留的 sqlite3 C API 面是白名单接缝：
// SQLITE_OPEN_* 旗标宏（宏不跨模块边界）与 sqlite3_busy_timeout（连接器
// 无超时旋钮），include 面由 cg_core 的 PUBLIC cg::sqlite3 链接传递；
// extern "C" 声明重复无 ODR 风险。
#include <sqlite3.h>

/**
 * @module cg.msbuild.pipeline.wraplog
 *
 * @brief wrap 采集日志域：WrapLog 的逐行分拣装载、compile_commands 与 PCH
 * sidecar 的产物清单写盘。
 */
export module cg.msbuild.pipeline.wraplog;

import std;
import cg.util;
import cg.util.cstd;
import cg.msbuild.translate; // ClInvocation / CdbEntry（wrap 日志载荷类型）
import cg.msbuild.wrap;      // detail::env_wide / fs_path_from_utf8
import sqlpp23.core;
import sqlpp23.sqlite3;
import cg.msbuild.pipeline.configs;
import cg.msbuild.pipeline.registry;
import cg.msbuild.tables;

export namespace cg::msbuild
{
    namespace json = cg::util::json;
    /**
     * @brief 一份解析后的 wrap 模式捕获：记录到的编译器调用加包装器写
     * 下的每条 error 行（rsp 展开失败、环境变量缺失），以及读侧跳过的
     * 坏行计数。
     *
     * @internal 错误是数据不是异常：由调用方上报，诚实的 TU 图景来自
     * 实际捕获到的内容；坏行同理——计数加 stderr 告警，绝不令整轮翻译
     * 失败。
     */
    struct WrapLog
    {
        std::vector<ClInvocation> invocations;
        std::vector<std::string>  errors;
        int bad_rows = 0;    // 解析失败被跳过的 capture 行（stderr 已告警）
        std::string db_path; // 实际读取的 capture.db（零捕获报错时指路）
    };

    namespace detail
    {

        /**
         * @brief 消费一条预编译 capture SELECT：逐行分拣进 WrapLog。
         *
         * @internal 迭代中途的失败（连接器以异常报告 step 错误）降级为一行
         * stderr 告警并保留已收集的行——与旧直调形态"step 非 ROW 即 break、
         * 部分结果保留"逐字对齐。可空列（include_env/message）的 NULL 读作
         * 空串，同旧 column_text 对 nullptr 的折返。argv_json 解析失败按坏行
         * 计数并继续（错误是数据不是异常）。
         */
        template <typename Sel>
        inline void consume_capture_rows(
            sqlpp::sqlite3::connection& db,
            Sel&                        sel,
            WrapLog&                    out,
            const std::string&          db_path)
        {
            try
            {
                for (const auto& r : db(sel))
                {
                    const std::string kind{r.kind};
                    if (kind == "error")
                    {
                        out.errors.push_back(
                            r.message.has_value() ? std::string{*r.message}
                                                  : std::string{});
                        continue;
                    }
                    const std::string argv_json{r.argvJson};
                    ClInvocation      inv;
                    try
                    {
                        const json::Document doc =
                            json::Document::parse(argv_json);
                        const json::ValueView argv_view = doc.root();
                        if (!argv_view.is_array())
                        {
                            throw std::runtime_error(
                                "argv_json is not an array");
                        }
                        const std::size_t count = argv_view.size();
                        for (std::size_t i = 0; i < count; ++i)
                        {
                            if (const auto v = argv_view.at(i);
                                v && v->is_string())
                            {
                                inv.argv_utf8.emplace_back(*v->as_string());
                            }
                        }
                    }
                    catch (const std::exception&)
                    {
                        ++out.bad_rows;
                        continue;
                    }
                    inv.cwd = std::string{r.cwd};
                    inv.include_env = r.includeEnv.has_value()
                                          ? std::string{*r.includeEnv}
                                          : std::string{};
                    // 逐次调用的真实编译器：混合工具集下每个 TU
                    // 的兼容版本来源。
                    inv.real_cl = std::string{r.realCl};
                    out.invocations.push_back(std::move(inv));
                }
            }
            catch (const std::exception& e)
            {
                std::println(
                    std::cerr,
                    "ccg msbuild: capture db read failed at {}: {}",
                    db_path,
                    e.what());
            }
        }

    } // namespace detail

    /**
     * @brief 读取一份 wrap 模式捕获（capture.db 的 capture 表）。
     *
     * path 参数接受 CCG_CLWRAP_LOG 的值（jsonl 名，经与写侧同一函数
     * detail::capture_db_of_log_path 推导出 db 位置）或 capture.db 直接
     * 路径。db 缺失读作空捕获（调用方上报"零捕获"）；行内 argv_json 解析
     * 失败按坏行计数、stderr 告警（含 capture.db 完整路径）并继续——
     * 单行损坏不再令整轮翻译抛错。token 非空时只读本运行 token 的行：
     * prune 删旧库是主路径，token 过滤是纵深防御（prune 因句柄占用/ACL
     * 失败时，上一轮的行绝不混入本次翻译）；空 token（默认）读全表，供
     * 测试与直读调用面。读侧永远可以信任返回值里带出的 db_path。
     *
     * @internal 只读打开（不带 CREATE：库缺失时它就是"空捕获"信号，连接
     * 器把 open 失败报告为异常，就地捕获降级）。READWRITE 而非 READONLY
     * 是刻意的：崩溃残留的 WAL 需要恢复权才能 checkpoint，且本进程对
     * .ccg 目录本就有写权。查询经 sqlpp23 类型化语句：本 pin "参数名字
     * 标签每语句只能出现一次"，旧 WHERE (?1='' OR token=?1) 的同参数双绑
     * 不可表达，按 token 空否走两条预编译变体（无 WHERE 全表 / token 等
     * 值过滤），语义与旧单语句逐字一致；表缺失时 prepare 报异常，降级
     * 文案与旧 sqlite3_errmsg 口径相同。
     */
    [[nodiscard]]
    inline auto read_wrap_log(
        const std::string& path,
        const std::string& token = {}) -> WrapLog
    {
        WrapLog           out;
        const std::string db_path = detail::capture_db_of_log_path(path);
        out.db_path = db_path;
        std::optional<sqlpp::sqlite3::connection> db;
        try
        {
            sqlpp::sqlite3::connection_config cfg;
            cfg.path_to_database = db_path;
            cfg.flags = SQLITE_OPEN_READWRITE;
            db.emplace(cfg);
        }
        catch (const std::exception&)
        {
            std::println(
                std::cerr,
                "ccg msbuild: capture db absent or unreadable: {}",
                db_path);
            return out;
        }
        sqlite3_busy_timeout(db->native_handle(), 5000);
        const cap::Capture c{};
        try
        {
            // 六列清单单点：两条变体只在 WHERE/参数绑定位形上分叉（参数
            // 名字标签每语句一次的限制见函数注释），列集演化不再两处同步。
            auto base = sqlpp::select(
                            c.kind,
                            c.argvJson,
                            c.cwd,
                            c.includeEnv,
                            c.realCl,
                            c.message)
                            .from(c);
            if (token.empty())
            {
                auto sel = db->prepare(base.order_by(c.id.asc()));
                detail::consume_capture_rows(*db, sel, out, db_path);
            }
            else
            {
                auto sel =
                    db->prepare(base.where(c.token == sqlpp::parameter(c.token))
                                    .order_by(c.id.asc()));
                sel.parameters.token = token;
                detail::consume_capture_rows(*db, sel, out, db_path);
            }
        }
        catch (const std::exception& e)
        {
            std::println(
                std::cerr,
                "ccg msbuild: capture db {} has no capture "
                "table: {}",
                db_path,
                e.what());
            return out;
        }
        if (out.bad_rows != 0)
        {
            std::println(
                std::cerr,
                "ccg msbuild: capture db {} has {} bad row(s) skipped "
                "(counted, not fatal)",
                db_path,
                out.bad_rows);
        }
        return out;
    }

    /**
     * @brief 把适配器条目序列化为 compile_commands.json（arguments 形状
     * ——加载器 documented 的输入；argv[0] "clang-cl" 按设计在加载时被
     * 丢弃）。
     *
     * @internal 每条目一个对象、经共享写出器保持转义统一；顶层由手工拼
     * 接——写出器的根按构造只能是对象。与注册表同法原子替换。
     */
    inline void write_compile_commands(
        const std::string&           path,
        const std::vector<CdbEntry>& entries)
    {
        std::string text;
        text.reserve(entries.size() * 256 + 16);
        text += "[\n";
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            const CdbEntry& e = entries[i];
            json::Array     args;
            for (const std::string& a : e.arguments)
            {
                args.add(a);
            }
            json::Object entry;
            entry.set("directory", e.directory);
            entry.set("file", e.file);
            entry.set("arguments", std::move(args));
            text += json::stringify(std::move(entry));
            if (i + 1 < entries.size())
            {
                text += ',';
            }
            text += '\n';
        }
        text += "]\n";
        detail::atomic_write_text(path, text, "compile_commands.json");
    }

    /**
     * @brief MSBuild PCH sidecar 的一台 producer（`/Yc` 调用）。
     */
    struct PchProducerRecord
    {
        std::string              pch_key;       // 绝对 /Fp
        std::string              source;        // producer TU（绝对）
        std::string              umbrella;      // /Yc 头拼写
        std::string              umbrella_path; // 可解析时绝对，否则 ""
        std::string              cwd;           // 记录的构建目录
        std::vector<std::string> argv; // 展开后的逐字 argv（含 argv[0]）
        std::string              command_digest;
    };

    struct PchConsumerRecord
    {
        std::string file;    // consumer TU（绝对）
        std::string pch_key; // 绝对 /Fp
        std::string pch_header;
        std::string command_digest;
    };

    /**
     * @brief 与 compile_commands.json 同批原子写 PCH sidecar。
     *
     * @internal cdb 保持剥离 `/Yc /Yu /Fp`（clangd/第三方兼容；producer
     * 不会被索引两次）。sidecar 携带 `cdb_sha256`，加载侧校验不符即忽略
     * ——两份世界观绝不混用。raw wrap log 每次提取前会被删除，因此这是
     * 唯一的持久捕获。
     */
    inline void write_pch_sidecar(
        const std::string&                    path,
        const std::string&                    cdb_path,
        const std::string&                    config,
        const std::string&                    platform,
        const std::vector<PchProducerRecord>& producers,
        const std::vector<PchConsumerRecord>& consumers,
        const std::vector<std::string>&       not_using)
    {
        namespace json = cg::util::json;
        std::string cdb_sha;
        try
        {
            cdb_sha = cg::util::sha256_file_hex(cdb_path);
        }
        catch (const std::exception&)
        {
            return; // cdb 未写成：没有可信 sidecar 可写
        }
        json::Object root;
        root.set("version", static_cast<std::int64_t>(1));
        root.set("generated_by", "ccg msbuild");
        root.set("config", config);
        root.set("platform", platform);
        root.set("cdb_sha256", cdb_sha);
        json::Array producer_list;
        for (const PchProducerRecord& p : producers)
        {
            json::Object o;
            o.set("pch_key", p.pch_key);
            o.set("source", p.source);
            o.set("umbrella", p.umbrella);
            o.set("umbrella_path", p.umbrella_path);
            o.set("cwd", p.cwd);
            o.set("command_digest", p.command_digest);
            json::Array args;
            for (const std::string& a : p.argv)
            {
                args.add(a);
            }
            o.set("argv", std::move(args));
            producer_list.add(std::move(o));
        }
        root.set("producers", std::move(producer_list));
        json::Array consumer_list;
        for (const PchConsumerRecord& c : consumers)
        {
            json::Object o;
            o.set("file", c.file);
            o.set("pch_key", c.pch_key);
            o.set("pch_header", c.pch_header);
            o.set("command_digest", c.command_digest);
            consumer_list.add(std::move(o));
        }
        root.set("consumers", std::move(consumer_list));
        json::Array unused;
        for (const std::string& f : not_using)
        {
            unused.add(f);
        }
        root.set("not_using", std::move(unused));
        detail::atomic_write_text(
            path,
            json::stringify(std::move(root)),
            "pch.json");
    }
} // namespace cg::msbuild

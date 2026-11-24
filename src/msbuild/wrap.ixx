module;
// clang-format off
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif
// clang-format on
// capture.db 的读写经 sqlpp23，表模型由 import cg.msbuild.tables 取得。
// 残留的 sqlite3 C API 面有两个白名单接缝：bootstrap_capture_db 的
// PRAGMA journal_mode 探测与幂等 DDL（sqlpp23 无 schema 迁移 DSL、PRAGMA
// 无 ORM 旋钮，经 connection 的原生句柄调用），以及 SQLITE_OPEN_* 旗标宏
// 与 sqlite3_busy_timeout（宏不跨模块边界，连接器自身无超时旋钮）。include
// 面由 cg_core 的 PUBLIC cg::sqlite3 链接传递（自取 amalgamation，头目录
// PUBLIC 暴露）。sqlite3.h 自带 extern "C"，函数声明跨单元重复无 ODR 风险，
// 与 db.store 的全局片段属主并行不悖。
#include <sqlite3.h>

/**
 * @module cg.msbuild.wrap
 *
 * @brief ccg.exe 内部的 MSBuild 编译类工具包装（wrap）模式：记录每次
 * 工具调用，转发给真实工具（--full-build）或零写盘空跑（记录模式，
 * 默认——只记 argv，不写任何产物）。
 *
 * 当 MSBuild 以 CLToolExe=ccg.exe + CLToolPath 调用编译工具时，每次编译
 * 都经过本模块（自经过验证的 C 原型包装器移植）；记录模式下 MSBuild 的
 * 编译类工具家族（cl/link/lib/rc/mt/ml64/…，见 stub_tool_family）以
 * 家族名副本改道进本模块——构建图照常走完，但不发生真编译，替身零写
 * 盘（桩产物没有任何消费者，写出反而会覆写硬编码路径上的真实
 * PDB/lib/obj/PCH）。
 *
 * @internal
 * 触发契约：环境变量 CCG_CLWRAP_LOG 已设 = wrap 模式（与 argv 内容无关）；
 * 未设 = 正常 CLI（仅一次环境探测开销，零其他影响——该钩子位于 cg.cli
 * run_main 的第一条语句，先于一切 CLI 代码路径）。CCG_CLWRAP_STUB=1
 * 追加选择记录模式（零写盘空跑、不转发）；CCG_CLWRAP_CL 是
 * "`<项目目录>`\t<cl 路径>" 前缀映射，逐次调用按 cwd 选 cl（混合工具集），
 * 未设置或未命中回退 CCG_CLWRAP_REAL_CL。
 * 捕获契约：每次调用向 capture.db 的 capture 表追加一行
 * （token/timestamp/kind/argv json/cwd/INCLUDE/real_cl/message）。
 * db 路径由 CCG_CLWRAP_LOG 的值推导（…/.ccg/msbuild/`<C>`-`<P>`/
 * clwrap-`<token>`.jsonl → …/.ccg/`<C>`-`<P>`/capture.db）；env 值仍指
 * jsonl 名是既有触发契约与推导锚点，jsonl 文件本身不再写。读侧
 * cg.msbuild.pipeline 经 detail::capture_db_of_log_path 同一函数推导，
 * 两侧路径永不分叉。
 * 全局模块片段说明：包装器经 GetCommandLineW/CommandLineToArgvW 持有
 * 真宽命令行、经 cg.util.platform 的子进程原语派生真实编译器；本模块
 * 是 cg.msbuild 中唯一包含 shellapi.h 的地方（CommandLineToArgvW 所
 * 在，链接 Shell32）。
 * 失败契约：一切失败路径都向 capture 表写一条 kind="error" 行并以
 * 退出码 1 结束——被包装的构建绝不静默失败或挂起；记录层自身打不开
 * （db 插入失败）则跳过该行 + 向 stderr 打一行告警（wrap 继承
 * MSBuild 的 stdio，构建输出可见，丢行可诊断），绝不杀死构建（空捕获
 * 由流水线层响亮上报）。
 */
export module cg.msbuild.wrap;

import std;
import cg.util;
import cg.util.cstd;
import cg.msbuild.translate;
import sqlpp23.core;
import sqlpp23.sqlite3;
import cg.msbuild.tables;

export namespace cg::msbuild
{

    namespace detail
    {

        /**
         * @brief 从 CCG_CLWRAP_LOG 的值推导 capture.db 的位置（写读两侧唯一
         * 真相；本函数被 cg.msbuild.pipeline 复用，签名跨平台）。
         *
         * 输入接受两种形态：jsonl 完整路径（`<root>`/.ccg/msbuild/`<C>`-`<P>`/
         * clwrap-`<token>`.jsonl，写侧 env 值与读侧 log 参数）或捕获目录
         * （`<root>`/.ccg/msbuild/`<C>`-`<P>`，prune 的入参）。两者都映射到配置
         * 目录 `<root>`/.ccg/`<C>`-`<P>`/capture.db。形状不匹配（手工设置的
         * env、 异构布局）时回退输入同目录的 capture.db——同一函数保证写读两侧
         * 永远落到同一个文件。
         */
        [[nodiscard]]
        inline auto capture_db_of_log_path(std::string_view path) -> std::string
        {
            const std::string forward = cg::util::to_forward_slashes(path);
            // 直接就是 db 路径的拼写（读侧兼容直接传 capture.db 的调用面）。
            if (forward.ends_with("/capture.db"))
            {
                return forward;
            }
            constexpr std::string_view marker{".ccg/msbuild/"};
            const std::size_t          at = forward.rfind(marker);
            if (at != std::string::npos)
            {
                const std::size_t begin = at + marker.size();
                const std::size_t end = forward.find('/', begin);
                // 目录形态（无后续 '/'）与文件形态（后跟 clwrap-*.jsonl）都取
                // marker 之后的首段 = `<C>`-`<P>`。
                if (end == std::string_view::npos
                    || (end > begin
                        && forward.find('/', end + 1)
                               == std::string_view::npos))
                {
                    const std::string_view stem =
                        std::string_view{forward}.substr(
                            begin,
                            end == std::string_view::npos
                                ? std::string_view::npos
                                : end - begin);
                    if (!stem.empty())
                    {
                        return std::format(
                            "{}.ccg/{}/capture.db",
                            forward.substr(0, at),
                            stem);
                    }
                }
            }
            // 回退：输入（jsonl 路径）同目录。prune 的目录入参形状必命中
            // marker，不会走到这里。
            const std::size_t slash = forward.find_last_of('/');
            return (slash == std::string::npos ? std::string{}
                                               : forward.substr(0, slash))
                   + "/capture.db";
        }

        /**
         * @brief 从 CCG_CLWRAP_LOG 的值提取本次运行 token（clwrap-`<token>`
         * .jsonl 的中段；capture 表按 token 记账行属于哪次运行）。
         *
         * 形状不匹配时返回 ""——token 是记账字段，推导不出不阻塞记录。
         */
        [[nodiscard]]
        inline auto capture_token_of_log_path(std::string_view path)
            -> std::string
        {
            const std::string      forward = cg::util::to_forward_slashes(path);
            const std::string_view name = cg::util::path_basename(forward);
            constexpr std::string_view prefix{"clwrap-"};
            constexpr std::string_view suffix{".jsonl"};
            if (name.size() > prefix.size() + suffix.size()
                && name.starts_with(prefix) && name.ends_with(suffix))
            {
                return std::string{name.substr(
                    prefix.size(),
                    name.size() - prefix.size() - suffix.size())};
            }
            return {};
        }

        /**
         * @brief argv 序列化为 JSON 数组文本（capture 表 argv_json 列的形状）。
         *
         * 编码经 cg.util.json（yyjson 门面）完成，转义由库保证（yyjson 紧凑
         * 写出的控制字符同为 `\u00XX` 小写十六进制，与旧 jsonl 时代的字节形状
         * 兼容；读侧 pipeline 经同一门面的 Document::parse 读回）。错误行的
         * argv_json 记 "[]" 占位，读侧对 kind="error" 不解析 argv。
         */
        [[nodiscard]]
        inline auto argv_to_json(const std::vector<std::string>& argv)
            -> std::string
        {
            cg::util::json::Array arr;
            for (const std::string& arg : argv)
            {
                arr.add(arg);
            }
            return cg::util::json::stringify(std::move(arr));
        }

        /**
         * @brief 记录模式（record mode）下被桩化的工具家族。
         *
         * 记录模式照常走完整个 MSBuild 构建图，但编译类工具不真跑：ccg.exe 以
         * 家族名（cl.exe/link.exe/…）的硬链接或副本住进桩目录，MSBuild 的
         * ToolPath 覆盖与 PATH 前置把它们改道进本模块；包装器只记 argv，零写
         * 盘——依赖产物存在性的下游步骤会响亮失败，那正是记录模式的预期
         * （部署类步骤复制占位物进真实树反而是泄漏源）。
         * midl.exe 刻意不在表中——它的 .h/.c 输出进入编译输入图，必须真跑。
         */
        inline constexpr std::string_view stub_tool_family[] = {
            "cl.exe",
            "clang-cl.exe",
            "link.exe",
            "lib.exe",
            "rc.exe",
            "mt.exe",
            "ml64.exe",
            "ml.exe",
            "armasm.exe",
            "marmasm.exe",
            "bscmake.exe",
            "xdcmake.exe",
            // ClangCL 工具集的同职能别名（llvm-lib 替 lib、llvm-rc 替 rc、
            // llvm-mt 替 mt、lld-link 替 link）：MSBuild 的 LLVM 平台工具集按
            // 这些名调用，缺桩即 MSB6004 断链。
            "llvm-lib.exe",
            "llvm-rc.exe",
            "llvm-mt.exe",
            "lld-link.exe"};

        /** @brief
         * 桩角色（按被调用镜像的基名识别；泄漏金丝雀按它选输出旗标表）。 */
        enum class StubRole
        {
            cl,      // cl.exe / clang-cl.exe（clang-cl 同 cl 语义）
            link,    // link.exe
            lib,     // lib.exe
            rc,      // rc.exe
            mt,      // mt.exe
            masm,    // ml64.exe / ml.exe / armasm.exe / marmasm.exe
            bscmake, // bscmake.exe
            xdcmake, // xdcmake.exe
            unknown  // 家族外（ccg.exe 直名包装：按 cl 对待，兼容现行真编通道）
        };

        /**
         * @brief 被调用镜像基名（小写）到桩角色的映射；家族外返回 unknown。
         */
        [[nodiscard]]
        inline auto stub_role_of_basename(std::string_view exe_name) -> StubRole
        {
            if (exe_name == "cl.exe" || exe_name == "clang-cl.exe")
            {
                return StubRole::cl;
            }
            if (exe_name == "link.exe" || exe_name == "lld-link.exe")
            {
                return StubRole::link;
            }
            if (exe_name == "lib.exe" || exe_name == "llvm-lib.exe")
            {
                return StubRole::lib;
            }
            if (exe_name == "rc.exe" || exe_name == "llvm-rc.exe")
            {
                return StubRole::rc;
            }
            if (exe_name == "mt.exe" || exe_name == "llvm-mt.exe")
            {
                return StubRole::mt;
            }
            if (exe_name == "ml64.exe" || exe_name == "ml.exe"
                || exe_name == "armasm.exe" || exe_name == "marmasm.exe")
            {
                return StubRole::masm;
            }
            if (exe_name == "bscmake.exe")
            {
                return StubRole::bscmake;
            }
            if (exe_name == "xdcmake.exe")
            {
                return StubRole::xdcmake;
            }
            return StubRole::unknown;
        }

        /**
         * @brief CCG_CLWRAP_CL 前缀映射中本次调用应选的 cl。
         *
         * 映射文本是若干 "`<项目目录>`\t<cl 绝对路径>" 行（'\n' 分隔，目录为
         * 正斜杠拼写）。按 cwd 取最长前缀命中（大小写与分隔符不敏感——目录
         * 两种拼写都算命中）；无命中返回 ""（调用方回退全局
         * CCG_CLWRAP_REAL_CL）。混合工具集 .sln 下每个工程各归各的 cl 由此
         * 保证，单一 real_cl 贯穿全链的结构性断点由此修复。
         */
        [[nodiscard]]
        inline auto select_cl_by_cwd(
            std::string_view map_text,
            std::string_view cwd) -> std::string
        {
            auto normalize = [](std::string_view value)
            {
                // 反斜杠统一 + ASCII 折叠（目录比较口径）；尾分隔符抹平：目录键
                // 与 cwd 的结尾斜杠有无不影响前缀命中。
                std::string out =
                    cg::util::ascii_lower(cg::util::to_forward_slashes(value));
                while (out.size() > 3 && out.back() == '/')
                {
                    out.pop_back();
                }
                return out;
            };
            const std::string want = normalize(cwd);
            if (want.empty())
            {
                return {};
            }
            std::string best;
            std::size_t best_len = 0;
            std::size_t start = 0;
            while (start <= map_text.size())
            {
                const std::size_t      end = map_text.find('\n', start);
                const std::string_view line = map_text.substr(
                    start,
                    end == std::string_view::npos ? std::string_view::npos
                                                  : end - start);
                const std::size_t tab = line.find('\t');
                if (tab != std::string_view::npos && tab + 1 < line.size())
                {
                    const std::string dir = normalize(line.substr(0, tab));
                    const bool        prefix =
                        want.size() >= dir.size()
                        && (want.size() == dir.size()
                            || want[dir.size()] == '/')
                        && want.compare(0, dir.size(), dir) == 0;
                    if (prefix && dir.size() > best_len)
                    {
                        best = std::string{line.substr(tab + 1)};
                        best_len = dir.size();
                    }
                }
                if (end == std::string_view::npos)
                {
                    break;
                }
                start = end + 1;
            }
            return best;
        }

        /**
         * @brief UTF-8 路径拼写转 std::filesystem::path。
         *
         * @internal Windows 上 fs::path(std::string) 会把 UTF-8 经 ANSI 代码页
         * 重解释（非 ASCII 路径已知的乱码类），宽字符往返才是精确的；非
         * Windows 的窄路径即原生拼写。本模块写侧（open_capture_db 的父目录
         * 兜底）与读侧（cg.msbuild.pipeline 的注册表/产物落盘）共用本单点，
         * 转换纪律只此一处。
         */
        [[nodiscard]]
        inline auto fs_path_from_utf8(const std::string& path)
            -> std::filesystem::path
        {
#if defined(_WIN32)
            return std::filesystem::path{cg::util::platform::widen_utf8(path)};
#else
            return std::filesystem::path{path};
#endif
        }

#if defined(_WIN32)

        /**
         * @brief 按名取环境变量值；未设置时返回 std::nullopt。
         *
         * @internal 空值与未设置经此 API 无法区分，刻意按未设置处理——wrap
         * 契约是"变量存在且非空"。
         */
        [[nodiscard]]
        inline auto env_wide(const wchar_t* name) -> std::optional<std::wstring>
        {
            const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
            if (needed == 0)
            {
                return std::nullopt;
            }
            std::wstring value(static_cast<std::size_t>(needed), L'\0');
            const DWORD  got =
                GetEnvironmentVariableW(name, value.data(), needed);
            if (got == 0 || got >= needed)
            {
                return std::nullopt; // 两次探测之间被并发修改
            }
            value.resize(got);
            return value;
        }

        /** @brief capture 表的一行载荷（kind=invocation 或 error）。 */
        struct CaptureRow
        {
            bool        is_error = false;
            std::string argv_json; // invocation 行：["a","b",…]（含 argv[0]）
            std::string cwd;       // 原生反斜杠拼写（与旧 jsonl 契约一致）
            std::string
                include_env;     // INCLUDE 未设置时 ""（读侧与旧 null 同义）
            std::string real_cl; // 本次调用实际选用的 cl；未知 ""
            std::string message; // error 行的错误文本
        };

        /**
         * @brief capture 插入失败的响亮降级：一行 stderr，令丢行可诊断。
         *
         * @internal wrap 继承 MSBuild 的 stdio，这行告警直达构建输出（并发
         * 冷启动的实测教训：PRAGMA WAL 与 DDL 同批失败曾被静默吞掉——调用点
         * static_cast`<void>` ——整 invocation 行无痕丢失、MSBuild rc=0、翻译
         * 静默少索引一个 TU、无任何信号）。绝不抛出：记录层打不开不能杀死
         * 被包装的构建（空捕获由流水线层响亮上报）。
         */
        inline void capture_insert_failed(const std::string& db_path)
        {
            std::println(
                std::cerr,
                "ccg wrap: capture insert failed for {}",
                db_path);
        }

        /**
         * @brief 打开一条 capture.db 连接（父目录兜底 + busy_timeout）。
         *
         * @internal 父目录（`<root>`/.ccg/`<C>`-`<P>`/）不一定存在：run_extract
         * 只创建 env 锚点目录（.ccg/msbuild/`<C>`-`<P>`/），-o 外置布局下配置
         * 目录没有别人创建，而 SQLite 只建文件不建目录——先兜底（幂等，
         * 失败照常落到 open 的诚实空）。窄字符串不能直构 fs::path
         *（Windows ANSI 代码页重解释），父目录经 fs_path_from_utf8 的宽
         * 字符转换单点处理。连接经 sqlpp23 连接器构造（SQLITE_OPEN_* 旗标走
         * connection_config），打开失败在连接器内部表现为异常——吞掉并返回
         * nullopt，调用方按"记录层打不开"契约降级，绝不令异常外逃。busy
         * 超时无 ORM 旋钮，经原生句柄武装（与旧直调形态同一条 C API）。
         * 失败返回 nullopt。
         */
        [[nodiscard]]
        inline auto open_capture_db(const std::string& db_path)
            -> std::optional<sqlpp::sqlite3::connection>
        {
            const std::filesystem::path parent =
                fs_path_from_utf8(db_path).parent_path();
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            try
            {
                sqlpp::sqlite3::connection_config cfg;
                cfg.path_to_database = db_path;
                cfg.flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
                sqlpp::sqlite3::connection db{cfg};
                sqlite3_busy_timeout(db.native_handle(), 5000);
                return db;
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        /**
         * @brief 把连接的 journal 模式尽力而为地切到 WAL 并确保 capture 表
         * 存在（幂等 DDL）。建表失败返回 false。
         *
         * @internal 本函数是手写 SQL 的白名单接缝（PRAGMA 无 ORM 旋钮、DDL
         * 无 schema DSL——两项均属全仓免 ORM 理由注释的既有豁免），保持 C API
         * 直调形态：调用方传 sqlpp23 连接的原生句柄，WAL 探测/切换与幂等 DDL
         * 的语句序列与旧直调时期逐字一致，12 进程并发冷启动的竞态行为
         * （capture_db_ensure 预建 + 写者自建兜底）不变。
         * WAL 是吞吐优化不是正确性依赖：SQLite 对 journal_mode=WAL
         * 的切换在其它连接活动时不调用 busy handler 而直接回 SQLITE_BUSY
         *（并发冷启动实测：prune 删库后多个桩进程近乎同时 CREATE 同一个新
         * 库，WAL 切换即败）。所以先查当前模式、非 wal 才尝试切、切不动就
         * 放弃——默认 rollback journal + busy_timeout 仍保证串行正确写入。
         * 建表失败则不同：没有表就没有记录，如实返回 false。
         */
        inline auto bootstrap_capture_db(sqlite3* db) -> bool
        {
            bool          already_wal = false;
            sqlite3_stmt* mode = nullptr;
            if (sqlite3_prepare_v2(
                    db,
                    "PRAGMA journal_mode;",
                    -1,
                    &mode,
                    nullptr)
                == SQLITE_OK)
            {
                if (sqlite3_step(mode) == SQLITE_ROW)
                {
                    const unsigned char* const text =
                        sqlite3_column_text(mode, 0);
                    already_wal =
                        text != nullptr
                        && std::string_view{reinterpret_cast<const char*>(text)}
                               == "wal";
                }
                sqlite3_finalize(mode);
            }
            if (!already_wal)
            {
                char* switch_err = nullptr;
                static_cast<void>(sqlite3_exec(
                    db,
                    "PRAGMA journal_mode=WAL;",
                    nullptr,
                    nullptr,
                    &switch_err));
                sqlite3_free(switch_err);
            }
            char*      err = nullptr;
            const bool ok = sqlite3_exec(
                                db,
                                "CREATE TABLE IF NOT EXISTS capture ("
                                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                "token TEXT NOT NULL,"
                                "timestamp TEXT NOT NULL,"
                                "kind TEXT NOT NULL,"
                                "argv_json TEXT NOT NULL,"
                                "cwd TEXT NOT NULL,"
                                "include_env TEXT,"
                                "real_cl TEXT NOT NULL,"
                                "message TEXT);",
                                nullptr,
                                nullptr,
                                &err)
                            == SQLITE_OK;
            sqlite3_free(err);
            return ok;
        }

        /**
         * @brief 预建 capture.db：单一连接建库 + WAL + 幂等 DDL 后立即关闭。
         *
         * 由 run_extract 在 spawn 被包装构建前调用：库与表先于一切写者存在，
         * 桩进程并发冷启动时不再有人执行"CREATE 新库 + 切 WAL"的竞态窗口
         *（此前 12 进程同窗 open(CREATE) 曾以 ~8% 概率在 WAL PRAGMA 处
         * SQLITE_BUSY 而整行丢失）。写者仍有自建兜底（open_capture_db +
         * bootstrap_capture_db），预建失败只告警不中断。
         */
        inline void capture_db_ensure(const std::string& db_path)
        {
            auto db = open_capture_db(db_path);
            if (!db)
            {
                std::println(
                    std::cerr,
                    "ccg msbuild: cannot prepare capture db {}",
                    db_path);
                return;
            }
            if (!bootstrap_capture_db(db->native_handle()))
            {
                std::println(
                    std::cerr,
                    "ccg msbuild: cannot prepare capture db {}",
                    db_path);
            }
        }

        /**
         * @brief 即开即插即关地向 capture.db 的 capture 表追加一行。
         *
         * @internal 并发安全三件套：busy_timeout（多 cl 桩进程同窗写入时等锁）
         * + WAL（尽力而为，见 bootstrap_capture_db）+ 连接即开即关（句柄绝不
         * 跨调用驻留）。建表 DDL 幂等。插入经 sqlpp23 类型化语句（八列各占
         * 一个参数名字标签，满足本 pin "参数名字标签每语句一次"的限制），
         * 连接即开即关故语句随连接逐次预编译。一切失败返回 false 并向 stderr
         * 打一行告警（capture_insert_failed）、绝不抛出——try/catch 吞掉全部
         * ORM 异常（打开失败在 open_capture_db 内先行降级；语句构造/执行期的
         * 异常在此收敛），记录层打不开不能杀死被包装的构建（空捕获由流水线
         * 层响亮上报）。可空列的空值语义与旧直调形态逐字一致：include_env
         * 未设置写 NULL（绑定 nullopt），message 仅 error 行携带文本、
         * invocation 行写 NULL。
         */
        [[nodiscard]]
        inline auto capture_db_insert(
            const std::string& db_path,
            const std::string& token,
            const CaptureRow&  row) -> bool
        {
            const cap::Capture c{};
            try
            {
                auto db = open_capture_db(db_path);
                if (!db)
                {
                    capture_insert_failed(db_path);
                    return false;
                }
                if (!bootstrap_capture_db(db->native_handle()))
                {
                    capture_insert_failed(db_path);
                    return false;
                }
                using sqlpp::parameter;
                auto insert = db->prepare(
                    sqlpp::sqlite3::insert_into(c).set(
                        c.token = parameter(c.token),
                        c.timestamp = parameter(c.timestamp),
                        c.kind = parameter(c.kind),
                        c.argvJson = parameter(c.argvJson),
                        c.cwd = parameter(c.cwd),
                        c.includeEnv = parameter(c.includeEnv),
                        c.realCl = parameter(c.realCl),
                        c.message = parameter(c.message)));
                insert.parameters.token = token;
                insert.parameters.timestamp =
                    cg::util::cstd::iso8601_utc_timestamp();
                insert.parameters.kind =
                    std::string{row.is_error ? "error" : "invocation"};
                insert.parameters.argvJson =
                    row.is_error ? std::string{"[]"} : row.argv_json;
                insert.parameters.cwd = row.cwd;
                insert.parameters.includeEnv =
                    row.include_env.empty()
                        ? std::nullopt
                        : std::optional<std::string>{row.include_env};
                insert.parameters.realCl = row.real_cl;
                insert.parameters.message =
                    row.is_error ? std::optional<std::string>{row.message}
                                 : std::nullopt;
                static_cast<void>((*db)(insert));
                return true;
            }
            catch (const std::exception&)
            {
                capture_insert_failed(db_path);
                return false;
            }
        }

        /**
         * @brief wrap 模式的进程主体（Windows）。返回 ccg.exe 必须传播的子进程
         * 退出码；此处不会返回 nullopt（环境门控已先行通过）。
         *
         * @internal
         * argv 的事实来源是宽字符命令行（GetCommandLineW/CommandLineToArgvW），
         * 绝不用 main() 的窄 argv——ANSI 转换会破坏 unicode 路径拼写。cwd 生成
         * 两种拼写：正斜杠形供响应文件解析使用，capture 表保留原生反斜杠形。
         * @rsp 展开与适配器共享 expand_rsp：不可读或循环的响应文件是响亮的
         * 数据错误——写 error 行进 capture 表、令本次调用失败，绝不转发半
         * 展开的命令行、也绝不崩溃。capture 行的 argv[0] 是本可执行文件
         * （适配器会丢弃该 token）；裸的无参调用不产生行（既有契约如此）。
         * 记录模式（CCG_CLWRAP_STUB=1）：记录通道不变，跳过真实执行后以 0 退
         * 出——构建图走完但不发生真编译，替身零写盘（既不读输入也不写产物，
         * 只把 argv 记进 capture 表；依赖产物存在性的下游步骤响亮失败属预期，
         * 上层归类为提示而不是错误）。
         * real_cl 的选择：CCG_CLWRAP_CL 前缀映射按 cwd 最长前缀命中（混合
         * 工具集逐工程各归各），未命中回退 CCG_CLWRAP_REAL_CL；capture 行
         * real_cl 字段逐次记录实际选用值（读取侧无需改）。真编转发以继承
         * stdio、CRT 正确引用与 32766 字符硬上限的方式派生。
         */
        [[nodiscard]]
        inline auto wrap_main_win(const std::wstring& log_path) -> int
        {
            const bool stub_mode = [&]
            {
                const std::optional<std::wstring> v =
                    env_wide(L"CCG_CLWRAP_STUB");
                return v && *v == L"1";
            }();

            // 捕获存储：env 值（jsonl 名）是推导锚点，数据落 capture.db。
            const std::string log_utf8 =
                cg::util::platform::narrow_utf8(log_path);
            const std::string capture_db = capture_db_of_log_path(log_utf8);
            const std::string capture_token =
                capture_token_of_log_path(log_utf8);

            const std::optional<std::wstring> real_cl_env =
                env_wide(L"CCG_CLWRAP_REAL_CL");

            int     argc = 0;
            LPWSTR* wide_argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            std::vector<std::string> argv_utf8;
            if (wide_argv != nullptr)
            {
                for (int i = 0; i < argc; ++i)
                {
                    argv_utf8.push_back(
                        cg::util::platform::narrow_utf8(
                            std::wstring_view{wide_argv[i]}));
                }
                LocalFree(wide_argv);
            }

            const DWORD cwd_needed = GetCurrentDirectoryW(0, nullptr);
            std::string cwd_utf8;
            if (cwd_needed != 0)
            {
                std::wstring cwd(static_cast<std::size_t>(cwd_needed), L'\0');
                const DWORD  cwd_len =
                    GetCurrentDirectoryW(cwd_needed, cwd.data());
                if (cwd_len != 0 && cwd_len < cwd_needed)
                {
                    cwd.resize(cwd_len);
                    cwd_utf8 = cg::util::platform::narrow_utf8(cwd);
                }
            }
            const std::string cwd_slash =
                cg::util::to_forward_slashes(cwd_utf8);

            std::vector<std::string> expanded;
            try
            {
                expanded = expand_rsp(argv_utf8, cwd_slash);
            }
            catch (const std::exception& e)
            {
                CaptureRow row;
                row.is_error = true;
                row.message = "ccg wrap: response-file expansion failed: "
                              + std::string{e.what()};
                static_cast<void>(
                    capture_db_insert(capture_db, capture_token, row));
                return 1;
            }

            // real_cl 选择：前缀映射（逐工程）优先，全局变量回退。
            std::string real_cl_utf8;
            if (const std::optional<std::wstring> map =
                    env_wide(L"CCG_CLWRAP_CL"))
            {
                real_cl_utf8 = select_cl_by_cwd(
                    cg::util::platform::narrow_utf8(*map),
                    cwd_slash);
            }
            if (real_cl_utf8.empty() && real_cl_env)
            {
                real_cl_utf8 = cg::util::platform::narrow_utf8(*real_cl_env);
            }

            // INCLUDE 逐调用捕获（invocation 行载荷；读侧兼容版本推导用）。
            const std::string include_env = [&]
            {
                const std::optional<std::wstring> v = env_wide(L"INCLUDE");
                return v ? cg::util::platform::narrow_utf8(*v) : std::string{};
            }();

            if (expanded.size() > 1)
            {
                CaptureRow row;
                row.argv_json = argv_to_json(expanded);
                row.cwd = cwd_utf8;
                row.include_env = include_env;
                row.real_cl = real_cl_utf8;
                static_cast<void>(
                    capture_db_insert(capture_db, capture_token, row));
            }

            if (stub_mode)
            {
                // 零写盘空跑：argv 已记入 capture 表（上面），既不读输入也不写
                // 产物——桩产物没有任何消费者（下游编译工具同为替身；midl/生成
                // 器真跑与桩无关），写出反而会覆写硬编码路径上的真实
                // PDB/lib/obj/PCH。依赖产物存在性的部署类步骤（PostBuildEvent
                // 等）对缺失产物失败属记录模式预期（上层归类为提示）。
                return 0;
            }

            if (real_cl_utf8.empty())
            {
                CaptureRow row;
                row.is_error = true;
                row.message =
                    "ccg wrap: CCG_CLWRAP_REAL_CL is not set and CCG_CLWRAP_CL "
                    "has no cwd match; cannot forward the compiler invocation";
                static_cast<void>(
                    capture_db_insert(capture_db, capture_token, row));
                return 1;
            }

            // 脚本形态的 real_cl（.py 后缀）：CreateProcessW 不走 shell
            // 文件关联，.py 无法直接派生；前插 PATH 上的 python 解释器
            // （裸名交给 CreateProcessW 的标准 PATH 搜索），脚本路径降为
            // 首个参数，宽字符命令行经 GetCommandLineW 全程无损。刻意不用
            // cmd.exe 批处理垫：垫层会把 %VAR% 按环境变量展开、把 ^ 与
            // |、<、> 一类令牌重新解析，转发即粉碎。capture 行的 real_cl
            // 仍记原始值——解释器前缀只属于拉起形态，不属于记录契约。
            const std::string real_cl_lower =
                cg::util::ascii_lower(real_cl_utf8);
            std::wstring cmdline;
            if (real_cl_lower.ends_with(".py"))
            {
                cmdline += L"python ";
            }
            cmdline += cg::util::platform::quote_for_command_line(
                cg::util::platform::widen_utf8(real_cl_utf8));
            for (std::size_t i = 1; i < expanded.size(); ++i)
            {
                cmdline += L' ';
                cmdline += cg::util::platform::quote_for_command_line(
                    cg::util::platform::widen_utf8(expanded[i]));
            }
            if (cmdline.size() > 32766)
            {
                CaptureRow row;
                row.is_error = true;
                row.message =
                    "ccg wrap: expanded command line exceeds the CreateProcessW "
                    "32767-character limit";
                static_cast<void>(
                    capture_db_insert(capture_db, capture_token, row));
                return 1;
            }

            // wrap 透传形态的捕获原语配置：不建管道、不设
            // STARTF_USESTDHANDLES， 子进程按句柄继承直通本进程的
            // stdio（MSBuild 的重定向管道因此原样
            // 延伸到真实编译器）；无窗口/环境块定制——包装器与调用者共享控制台
            // 且继承完整环境是既有形态。
            cg::util::platform::ChildRunSpec spec;
            spec.cmdline = std::move(cmdline);
            spec.inherit_parent_stdio = true;
            spec.create_no_window = false;
            const cg::util::platform::ChildRunResult run =
                cg::util::platform::run_child(spec);
            if (!run.started)
            {
                CaptureRow row;
                row.is_error = true;
                row.message = std::format(
                    "ccg wrap: CreateProcessW failed for the real compiler "
                    "(error {})",
                    run.spawn_error);
                static_cast<void>(
                    capture_db_insert(capture_db, capture_token, row));
                return 1;
            }
            return run.exit_code;
        }

#endif // defined(_WIN32)

    } // namespace detail

    /**
     * @brief wrap 模式门控：cg.cli 作为 run_main 第一条语句调用的唯一钩子。
     *
     * CCG_CLWRAP_LOG 未设置时返回 std::nullopt（正常 CLI，仅一次环境探测
     * 开销）；已设置时本进程作为 MSBuild 的 CL 包装器运行完毕，返回值即
     * ccg.exe 应退出的代码。
     *
     * @internal
     * 刻意忽略 main() 的窄 argv——Windows 上宽字符命令行是唯一无损拼写；
     * 其他平台环境契约永不生效，CLI 照常继续。
     */
    [[nodiscard]]
    inline auto try_wrap_main() -> std::optional<int>
    {
#if defined(_WIN32)
        const std::optional<std::wstring> log_path =
            detail::env_wide(L"CCG_CLWRAP_LOG");
        if (!log_path)
        {
            return std::nullopt;
        }
        return detail::wrap_main_win(*log_path);
#else
        return std::nullopt;
#endif
    }

} // namespace cg::msbuild



/**
 * @module cg.clang.pch.sidecar
 *
 * @brief PCH sidecar 域：生产者/消费者/装载结构、sidecar 读取、生产者记录合并与
 * C++ 判定、include 目录提取。
 */
export module cg.clang.pch.sidecar;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;
import cg.clang.driver;
import cg.clang.compile_db;
import cg.clang.bmi;
import cg.clang.pch.paths;

export namespace cg::clang
{

    namespace detail
    {

        /** @brief sidecar 里的一台 producer（MSBuild `/Yc` 调用）。 */
        struct SidecarProducer
        {
            std::string              pch_key;
            std::string              source;
            std::string              umbrella;
            std::string              umbrella_path;
            std::string              cwd;
            std::vector<std::string> argv;
        };

        struct SidecarConsumer
        {
            std::string file;
            std::string pch_key;
            std::string pch_header;
        };

        struct Sidecar
        {
            std::vector<SidecarProducer> producers;
            std::vector<SidecarConsumer> consumers;
        };

        /**
         * @brief 读取并校验 MSBuild sidecar。
         *
         * @internal `cdb_sha256` 不符 → 忽略 sidecar、纯文本回退（cdb 被重新
         * 生成而 sidecar 落后时，绝不让两份世界观混用）。
         */
        [[nodiscard]]
        inline auto load_sidecar(
            const std::string& path,
            const std::string& cdb_path) -> std::optional<Sidecar>
        {
            namespace json = cg::util::json;
            const std::optional<std::string> text = read_text(path);
            if (!text)
            {
                return std::nullopt;
            }
            try
            {
                const json::Document  doc = json::Document::parse(*text);
                const json::ValueView root = doc.root();
                if (!root.is_object())
                {
                    return std::nullopt;
                }
                if (const auto v = root.get("version");
                    v && v->is_number() && *v->as_int() != 1)
                {
                    return std::nullopt;
                }
                std::string expect;
                if (const auto v = root.get("cdb_sha256"); v && v->is_string())
                {
                    expect = std::string{*v->as_string()};
                }
                std::string actual;
                try
                {
                    actual = cg::util::sha256_file_hex(cdb_path);
                }
                catch (const std::exception&)
                {
                    return std::nullopt;
                }
                if (expect.empty() || expect != actual)
                {
                    return std::nullopt;
                }
                Sidecar                              out;
                const std::optional<json::ValueView> producers =
                    root.get("producers");
                if (producers && producers->is_array())
                {
                    for (std::size_t i = 0; i < producers->size(); ++i)
                    {
                        const auto e = producers->at(i);
                        if (!e || !e->is_object())
                        {
                            continue;
                        }
                        SidecarProducer p;
                        p.pch_key =
                            cg::util::norm_path(json_string_of(*e, "pch_key"));
                        p.source =
                            cg::util::norm_path(json_string_of(*e, "source"));
                        p.umbrella = json_string_of(*e, "umbrella");
                        p.umbrella_path = cg::util::norm_path(
                            json_string_of(*e, "umbrella_path"));
                        p.cwd = cg::util::norm_path(json_string_of(*e, "cwd"));
                        if (const auto av = e->get("argv");
                            av && av->is_array())
                        {
                            for (std::size_t k = 0; k < av->size(); ++k)
                            {
                                if (const auto x = av->at(k);
                                    x && x->is_string())
                                {
                                    p.argv.emplace_back(*x->as_string());
                                }
                            }
                        }
                        if (!p.pch_key.empty() && !p.source.empty())
                        {
                            out.producers.push_back(std::move(p));
                        }
                    }
                }
                const std::optional<json::ValueView> consumers =
                    root.get("consumers");
                if (consumers && consumers->is_array())
                {
                    for (std::size_t i = 0; i < consumers->size(); ++i)
                    {
                        const auto e = consumers->at(i);
                        if (!e || !e->is_object())
                        {
                            continue;
                        }
                        SidecarConsumer c;
                        c.file =
                            cg::util::norm_path(json_string_of(*e, "file"));
                        c.pch_key =
                            cg::util::norm_path(json_string_of(*e, "pch_key"));
                        c.pch_header = json_string_of(*e, "pch_header");
                        if (!c.file.empty() && !c.pch_key.empty())
                        {
                            out.consumers.push_back(std::move(c));
                        }
                    }
                }
                return out;
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        /** @brief 一条已发现的生产记录（GNU cdb 或 MSBuild sidecar）。 */
        struct ProducerRec
        {
            std::string              key; // fold(绝对 PCH 路径 / pch_key)
            std::string              display_key; // 原始绝对路径
            std::string              input;       // producer TU（绝对）
            std::string              directory;   // 记录的构建目录
            std::string              umbrella;    // 伞头拼写（诊断/闭包种子）
            std::vector<std::string> forced;      // 记录的强制包含（绝对）
            std::vector<std::string>
                 argv; // 重放 argv（无 argv[0]、无输出旗标）
            bool cl_mode = false;
            bool cxx = true; // producer TU 的语言（影响搜索目录与环境变量）
            bool from_sidecar = false;
        };

        /** @brief 生产记录按 key 首插或整条覆写（overwrite 门控）。 */
        inline void upsert_producer(
            std::unordered_map<std::string, ProducerRec>& producers,
            ProducerRec                                   rec,
            bool                                          overwrite)
        {
            if (rec.key.empty())
            {
                return;
            }
            const auto it = producers.find(rec.key);
            if (it == producers.end())
            {
                producers.emplace(rec.key, std::move(rec));
            }
            else if (overwrite)
            {
                it->second = std::move(rec);
            }
        }

        /**
         * @brief producer TU 是否为 C++。
         *
         * @internal 优先级：显式 `-x`/`/Tc`/`/Tp` > 输入文件扩展名 > 默认 C++。
         * 语言决定使用哪条驱动器搜索链（C++ vs C）以及哪个环境变量
         * （CPLUS_INCLUDE_PATH vs C_INCLUDE_PATH）。
         */
        [[nodiscard]]
        inline auto producer_is_cxx(
            const std::vector<std::string>& argv,
            const std::string&              input) -> bool
        {
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                const std::string& a = argv[i];
                if (a == "-x" && i + 1 < argv.size())
                {
                    const std::string& lang = argv[i + 1];
                    if (lang == "c" || lang == "c-header"
                        || lang == "cpp-output")
                    {
                        return false;
                    }
                    if (lang == "c++" || lang == "c++-header")
                    {
                        return true;
                    }
                }
                if (a == "/Tc" || a.starts_with("/Tc"))
                {
                    return false;
                }
                if (a == "/Tp" || a.starts_with("/Tp"))
                {
                    return true;
                }
            }
            const std::string ext = cg::util::ascii_lower(
                std::filesystem::path{input}.extension().string());
            return !(ext == ".c" || ext == ".i");
        }

        /** @brief 从 argv 收集 include 搜索目录（已绝对化）。 */
        [[nodiscard]]
        inline auto include_dirs_of(
            const std::vector<std::string>& argv,
            const std::string& directory) -> std::vector<std::string>
        {
            std::vector<std::string> out;
            for (std::size_t i = 0; i < argv.size(); ++i)
            {
                for (const std::string_view prefix :
                     {"-I", "-isystem", "-iquote", "-idirafter", "-imsvc"})
                {
                    if (const auto v = flag_value(argv, i, prefix))
                    {
                        push_nonempty_unique(out, resolve_from(*v, directory));
                    }
                }
                for (const std::string_view prefix : {"/I", "/external:I"})
                {
                    if (const auto v = flag_value(argv, i, prefix))
                    {
                        push_nonempty_unique(out, resolve_from(*v, directory));
                    }
                }
            }
            return out;
        }

        /** @brief 按顺序合并目录并去重。 */
        inline void append_include_dirs(
            std::vector<std::string>&       out,
            const std::vector<std::string>& dirs)
        {
            for (const std::string& dir : dirs)
            {
                push_nonempty_unique(out, dir);
            }
        }

    } // namespace detail
} // namespace cg::clang



/**
 * @module cg.clang.pch.manifest
 *
 * @brief PCH manifest 域：条目/清单/捐赠命中结构、manifest 的 JSON 读写、argv
 * 重放、跨配置捐赠者查找与工件缓存失效。
 */
export module cg.clang.pch.manifest;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;
import cg.clang.driver;
import cg.clang.compile_db;
import cg.clang.bmi;
import cg.clang.pch.paths;
import cg.clang.pch.sidecar;
import cg.clang.pch.model;

export namespace cg::clang
{

    namespace detail
    {

        /** @brief 清单条目。 */
        struct PchEntry
        {
            std::string                                      slot_key;
            std::string                                      pch_key;
            std::string                                      producer_file;
            std::string                                      producer_input;
            std::string                                      producer_cwd;
            std::string                                      producer_digest;
            std::string                                      dialect;
            std::string                                      closure_hash;
            std::vector<std::pair<std::string, std::string>> closure;
            /** @brief 从兄弟配置复制时的 donor 影子绝对路径（本地构建为空）。
             */
            std::string  donor;
            std::int64_t pch_size = 0;
            /** @brief 影子字节的 sha256：同路径内容替换必须失配（内容
             * 面有效性判据）。 */
            std::string pch_hash;
        };

        struct PchManifest
        {
            std::string           engine;
            std::string           engine_key;
            std::vector<PchEntry> entries;
        };

        /** @brief 跨配置 donor 命中：影子绝对路径与台账条目。 */
        struct DonorHit
        {
            std::string path;
            PchEntry    entry;
        };

        [[nodiscard]]
        inline auto pch_manifest_to_json(const PchManifest& m) -> std::string
        {
            namespace json = cg::util::json;
            json::Object root;
            root.set("engine", m.engine);
            root.set("engine_key", m.engine_key);
            json::Array entries;
            for (const PchEntry& e : m.entries)
            {
                json::Object o;
                o.set("slot_key", e.slot_key);
                o.set("pch_key", e.pch_key);
                o.set("producer_file", e.producer_file);
                o.set("producer_input", e.producer_input);
                o.set("producer_cwd", e.producer_cwd);
                o.set("producer_digest", e.producer_digest);
                o.set("dialect", e.dialect);
                o.set("closure_hash", e.closure_hash);
                o.set("donor", e.donor);
                o.set("pch_size", e.pch_size);
                o.set("pch_hash", e.pch_hash);
                json::Array closure;
                for (const auto& [path, hash] : e.closure)
                {
                    json::Object c;
                    c.set("path", path);
                    c.set("hash", hash);
                    closure.add(std::move(c));
                }
                o.set("closure", std::move(closure));
                entries.add(std::move(o));
            }
            root.set("entries", std::move(entries));
            return json::stringify(std::move(root));
        }

        [[nodiscard]]
        inline auto pch_manifest_from_json(const std::string& text)
            -> std::optional<PchManifest>
        {
            namespace json = cg::util::json;
            try
            {
                const json::Document  doc = json::Document::parse(text);
                const json::ValueView root = doc.root();
                if (!root.is_object())
                {
                    return std::nullopt;
                }
                PchManifest out;
                out.engine = json_string_of(root, "engine");
                out.engine_key = json_string_of(root, "engine_key");
                const auto entries = root.get("entries");
                if (!entries || !entries->is_array())
                {
                    return std::nullopt;
                }
                for (std::size_t i = 0; i < entries->size(); ++i)
                {
                    const auto e = entries->at(i);
                    if (!e || !e->is_object())
                    {
                        continue;
                    }
                    PchEntry entry;
                    entry.slot_key = json_string_of(*e, "slot_key");
                    entry.pch_key = json_string_of(*e, "pch_key");
                    entry.producer_file = json_string_of(*e, "producer_file");
                    entry.producer_input = json_string_of(*e, "producer_input");
                    entry.producer_cwd = json_string_of(*e, "producer_cwd");
                    entry.producer_digest =
                        json_string_of(*e, "producer_digest");
                    entry.dialect = json_string_of(*e, "dialect");
                    entry.closure_hash = json_string_of(*e, "closure_hash");
                    entry.donor = json_string_of(*e, "donor");
                    entry.pch_hash = json_string_of(*e, "pch_hash");
                    if (const auto v = e->get("pch_size"); v && v->is_number())
                    {
                        entry.pch_size = *v->as_int();
                    }
                    if (const auto closure = e->get("closure");
                        closure && closure->is_array())
                    {
                        for (std::size_t k = 0; k < closure->size(); ++k)
                        {
                            const auto c = closure->at(k);
                            if (!c || !c->is_object())
                            {
                                continue;
                            }
                            const auto p = c->get("path");
                            const auto h = c->get("hash");
                            if (p && p->is_string() && h && h->is_string())
                            {
                                entry.closure.emplace_back(
                                    *p->as_string(),
                                    *h->as_string());
                            }
                        }
                    }
                    if (!entry.slot_key.empty() && !entry.pch_key.empty())
                    {
                        out.entries.push_back(std::move(entry));
                    }
                }
                return out;
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        /**
         * @brief 影子构建的重放 argv。
         *
         * @internal
         * 与记录 argv 的最小偏离：输出旗标全部重定向（`/Fo`、`/Fd` 入 slot
         * 临时目录）、构建副作用旗（`-MD/-MMD/-MF`、`-MJ`、
         * `--serialize-diagnostics`、`/sourceDependencies`、`-ftime-trace`、
         * `/showIncludes`、`/MP`、`/Gm`、`/GL`、`/analyze`、`/WX`）剥离、追加
         * `-Xclang -detailed-preprocessing-record`。其余旗标逐字保留。影子构建
         * 必须只读：任何会把文件写进构建树的记录旗标都不得原样重放。
         */
        [[nodiscard]]
        inline auto replay_argv(
            const ProducerRec& p,
            const std::string& shadow,
            const std::string& slot_dir) -> std::vector<std::string>
        {
            std::vector<std::string> argv;
            argv.push_back(driver_info().driver);
            if (p.cl_mode)
            {
                argv.emplace_back("--driver-mode=cl");
            }
            // cl 的 `/Yc` producer 是普通编译命令上的旗标；loader 在 PCH 感知
            // 解析中去掉 `/c`，重放时必须补回，否则 clang-cl 会走编译+链接。
            if (p.cl_mode)
            {
                const bool has_c =
                    std::ranges::find(p.argv, "/c") != p.argv.end()
                    || std::ranges::find(p.argv, "-c") != p.argv.end();
                if (!has_c)
                {
                    argv.emplace_back("/c");
                }
            }
            for (std::size_t i = 0; i < p.argv.size(); ++i)
            {
                const std::string& a = p.argv[i];
                // loader 可能已把 cl 方言写进 argv；重放只在最前面放一次。
                if (a == "--driver-mode=cl")
                {
                    continue;
                }
                if (a == "-MD" || a == "-MMD" || a == "/showIncludes"
                    || a == "/MP" || a == "/Gm" || a == "/GL" || a == "/analyze"
                    || a == "/WX" || a == "-ftime-trace"
                    || a.starts_with("-ftime-trace=")
                    || a.starts_with("-Werror"))
                {
                    continue;
                }
                if (a == "-MF")
                {
                    ++i;
                    continue;
                }
                // 预处理器/汇编/调试转储：都会在记录目录里写文件。剥离掉对
                // AST 形状没有影响（影子只需要 PCH 字节）。
                if (a == "-save-temps" || a.starts_with("-save-temps=")
                    || a == "--save-temps" || a.starts_with("--save-temps=")
                    || a.starts_with("-fdump-")
                    || a == "-fsave-optimization-record")
                {
                    continue;
                }
                if (a == "-foptimization-record-file")
                {
                    ++i;
                    continue;
                }
                if (a.starts_with("-foptimization-record-file="))
                {
                    continue;
                }
                if (a == "/P" || a == "/EP" || a == "/E" || a == "/FA")
                {
                    continue;
                }
                if (a == "/Fi" || a == "/Fa" || a == "/Fe" || a == "/Fm")
                {
                    // 分写值只有在下一 token 不是旗标时才是输出路径（cl
                    // 型守卫）。
                    if (cg::clang::detail::cl_value_follows(p.argv, i))
                    {
                        ++i;
                    }
                    continue;
                }
                // /Fi* /FA* /Fa* /Fe* /Fm*：/FA 可无值（/FA、/FAs、/FAcs）。
                if ((a.starts_with("/Fi") || a.starts_with("/FA")
                     || a.starts_with("/Fa") || a.starts_with("/Fe")
                     || a.starts_with("/Fm"))
                    && a.size() > 3)
                {
                    continue;
                }
                // 输出副作用旗：值与旗标一并剥离，绝不写进构建树。
                if (a == "-MJ")
                {
                    ++i;
                    continue;
                }
                if (a.starts_with("-MJ") && a.size() > 3)
                {
                    continue;
                }
                if (a == "--serialize-diagnostics")
                {
                    ++i;
                    continue;
                }
                if (a.starts_with("--serialize-diagnostics="))
                {
                    continue;
                }
                // MSVC 模块带值开关（/ifcOutput /scanDependencies
                // /sourceDependencies /exportHeader /ifcSearchDir
                // /reference）： 拼写归属经 msvc_module_switch_classify 对
                // cg.util.msvcswitch 单点清单判定（全仓一份——本分支此前只
                // 认识 /sourceDependencies 一份手写拼写，与剥离层各自维护正是
                // 参数逃逸复盘暴露的结构性缺口；大小写与前缀放宽与剥离层同口
                // 径）。冒号连写形态的值已随 token 整体剥除；两段式吞值保持
                // PCH 守卫——紧随 token 是旗标或源文件时绝不吞（源文件是重
                // 放的输入，不是开关的值）。剥除的是 clang 家工具无对应实现
                // 的模块开关与其产物路径（裸值形态的 .ifc 路径若穿透，会被
                // clang-cl 当作额外输入文件）。
                if (const cg::clang::detail::MsvcSwitchMatch mod =
                        cg::clang::detail::msvc_module_switch_classify(a);
                    mod.kind == cg::clang::detail::MsvcModuleSwitchKind::valued)
                {
                    if (!mod.joined
                        && cg::clang::detail::cl_header_value_follows(
                            p.argv,
                            i))
                    {
                        ++i; // 紧随其后的输出文件/路径值
                    }
                    continue;
                }
                if (a == "/Fo" || a == "/Fd")
                {
                    ++i;
                    continue;
                }
                if ((a.starts_with("/Fo") || a.starts_with("/Fd"))
                    && a.size() > 3)
                {
                    continue;
                }
                if (a == "/Fp")
                {
                    ++i;
                    continue;
                }
                if (a.starts_with("/Fp") && a.size() > 3)
                {
                    continue;
                }
                argv.push_back(a);
            }
            argv.emplace_back("-Xclang");
            argv.emplace_back("-detailed-preprocessing-record");
            if (p.cl_mode)
            {
                argv.push_back(std::format("/Fp{}", shadow));
                argv.push_back(std::format("/Fo{}/producer.obj", slot_dir));
                argv.push_back(std::format("/Fd{}/producer.pdb", slot_dir));
            }
            else
            {
                argv.emplace_back("-o");
                argv.push_back(shadow);
            }
            // 索引语境的告警降级（与 clang.tu parse() 的 extra 同款）：
            // non-pod-varargs 是 DefaultError（与 /WX 无关——上面的剥离名单已
            // 去掉 /WX 与 -Werror*），非平凡类型传 varargs 的合法 MSVC producer
            // 头会让影子构建直接失败；只降级错误升级、保留警告本体，两个方言
            // 都接受该开关。
            argv.emplace_back("-Wno-error=non-pod-varargs");
            // /WX- 兜底追加在所有记录旗标之后（后写胜出），兜住剥离白名单未
            // 覆盖的等价写法，确保影子构建永不被警告升级阻断；GNU 驱动不接受
            // /WX-，严格门控在 cl 方言。
            if (p.cl_mode)
            {
                argv.emplace_back("/WX-");
            }
            // 记录 argv 已含输入（MSBuild sidecar 的逐字 argv）时不再追加；
            // GNU cdb 的 loader 丢弃了位置输入，这里补回。
            bool              has_input = false;
            const std::string want = cg::util::norm_path(p.input);
            for (const std::string& a : argv)
            {
                if (!a.empty() && a[0] != '-' && a[0] != '/'
                    && cg::util::norm_path(
                           (std::filesystem::path{p.directory} / a)
                               .lexically_normal()
                               .generic_string())
                           == want)
                {
                    has_input = true;
                    break;
                }
            }
            if (!has_input)
            {
                argv.push_back(p.input);
            }
            return argv;
        }

        /**
         * @brief 跨配置 donor：兄弟配置目录里同一 slot 的影子。
         *
         * @internal 影子字节是引擎 + producer 命令 + 闭包的纯函数，故同
         * engine_key 下的字节可跨配置重定位。命中条件保守：donor 台账里
         * 的 slot_key 全等且 donor 字节的 sha256 与台账一致（尺寸非零、
         * 闭包非空）；绝不共享句柄，只复制。匹配只看生产者身份
         * （slot_key 同 = 同引擎/方言/摘要）；闭包与 pch_key 由调用方按
         * 本盘现状重验——donor 的依赖集描述的是 donor 盘，必须逐文件重
         * 哈希通过才采用。
         *
         * @param opt 只读 ccg_config_dir（空则无 donor）。
         */
        [[nodiscard]]
        inline auto find_cross_config_donor(
            const PchOptions&  opt,
            const std::string& engine_key,
            const std::string& engine_version,
            std::string_view   slot_key) -> std::optional<DonorHit>
        {
            namespace fs = std::filesystem;
            if (opt.ccg_config_dir.empty())
            {
                return std::nullopt;
            }
            const fs::path cfg =
                fs::path{opt.ccg_config_dir}.lexically_normal();
            std::error_code lec;
            for (const fs::directory_entry& sibling :
                 fs::directory_iterator(cfg.parent_path(), lec))
            {
                std::error_code sec;
                if (!sibling.is_directory(sec)
                    || fold(sibling.path().lexically_normal().generic_string())
                           == fold(cfg.generic_string()))
                {
                    continue;
                }
                const fs::path donor_engine =
                    sibling.path() / "pch" / engine_key;
                const fs::path  donor_manifest = donor_engine / "manifest.json";
                std::error_code mec;
                if (!fs::is_regular_file(donor_manifest, mec))
                {
                    continue;
                }
                const auto text = read_text(
                    donor_manifest.lexically_normal().generic_string());
                if (!text)
                {
                    continue;
                }
                const auto other = pch_manifest_from_json(*text);
                if (!other.has_value() || other->engine != engine_version)
                {
                    continue;
                }
                for (const PchEntry& e : other->entries)
                {
                    if (e.slot_key != slot_key || e.pch_hash.empty()
                        || e.pch_size <= 0 || e.closure.empty())
                    {
                        continue; // donor 记录必须带非零尺寸与闭包
                    }
                    const fs::path donor =
                        donor_engine / std::string{slot_key} / "shadow.pch";
                    std::error_code cec;
                    const auto      size = fs::file_size(donor, cec);
                    if (cec || size == 0)
                    {
                        continue;
                    }
                    const std::string hash = cg::clang::detail::hash_source(
                        donor.lexically_normal().generic_string());
                    if (hash.empty() || hash != e.pch_hash)
                    {
                        continue;
                    }
                    DonorHit hit;
                    hit.path = cg::util::norm_path(
                        donor.lexically_normal().generic_string());
                    hit.entry = e;
                    return hit;
                }
            }
            return std::nullopt;
        }

    } // namespace detail

    /**
     * @brief 让指定的影子 PCH 失效：原子重写清单并删除 slot 目录。
     *
     * @param shadow_paths 盘上影子路径
     *        （`.../pch/<engine_key>/<slot_key>/shadow.pch`）。
     * @return 从清单移除的条目数。
     *
     * @internal pch-unusable 说明缓存影子无法被本引擎加载；继续复用只会
     * 永远失败。重读-修改-原子替换清单（无锁、并发安全到"最多丢一次
     * 重建"），随后删 slot 目录，下一次规划必定本地重建。
     */
    [[nodiscard]]
    inline auto invalidate_cached_pch_artifacts(
        const std::vector<std::string>& shadow_paths) -> std::size_t
    {
        namespace fs = std::filesystem;
        struct Target
        {
            std::string engine_dir;
            std::string slot;
        };
        std::map<std::string, std::vector<Target>> groups;
        for (const std::string& raw : shadow_paths)
        {
            const fs::path shadow = fs::path{cg::util::norm_path(
                fs::path{raw}.lexically_normal().generic_string())};
            const fs::path slot_dir = shadow.parent_path();
            const fs::path engine_dir = slot_dir.parent_path();
            // 只接受 `<config>`/pch/`<engine_key>`/`<slot>`/shadow.pch 布局。
            if (engine_dir.parent_path().filename().generic_string() != "pch")
            {
                continue;
            }
            groups[cg::util::norm_path(
                       engine_dir.lexically_normal().generic_string())]
                .push_back(
                    Target{
                        engine_dir.lexically_normal().generic_string(),
                        slot_dir.filename().generic_string()});
        }
        std::size_t removed_entries = 0;
        for (const auto& [engine_dir, targets] : groups)
        {
            std::unordered_set<std::string> slots;
            for (const Target& target : targets)
            {
                slots.insert(target.slot);
            }
            const fs::path manifest_path{
                fs::path{engine_dir} / "manifest.json"};
            if (const auto text =
                    detail::read_text(manifest_path.generic_string()))
            {
                if (auto parsed = detail::pch_manifest_from_json(*text);
                    parsed.has_value())
                {
                    const std::size_t before = parsed->entries.size();
                    std::erase_if(
                        parsed->entries,
                        [&](const detail::PchEntry& e)
                        { return slots.contains(e.slot_key); });
                    removed_entries += before - parsed->entries.size();
                    if (parsed->entries.size() != before)
                    {
                        // 合并并发写者的新增 slot（被失效的 slot 不复活）。
                        std::unordered_set<std::string> present;
                        for (const detail::PchEntry& e : parsed->entries)
                        {
                            present.insert(e.slot_key);
                        }
                        if (const auto fresh = detail::read_text(
                                manifest_path.generic_string()))
                        {
                            if (const auto disk =
                                    detail::pch_manifest_from_json(*fresh);
                                disk.has_value())
                            {
                                for (detail::PchEntry e : disk->entries)
                                {
                                    if (slots.contains(e.slot_key)
                                        || !present.insert(e.slot_key).second)
                                    {
                                        continue;
                                    }
                                    parsed->entries.push_back(std::move(e));
                                }
                            }
                        }
                        (void)cg::util::platform::atomic_write_file(
                            manifest_path,
                            detail::pch_manifest_to_json(*parsed));
                    }
                }
            }
            for (const Target& target : targets)
            {
                std::error_code ec;
                fs::remove_all(fs::path{engine_dir} / target.slot, ec);
            }
        }
        return removed_entries;
    }
} // namespace cg::clang

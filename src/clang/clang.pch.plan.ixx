

/**
 * @module cg.clang.pch.plan
 *
 * @brief PCH 计划主函数 plan_pch：捐赠复用判定、影子通道构建与产物落盘编排。
 */
export module cg.clang.pch.plan;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.capi;
import cg.clang.driver;
import cg.clang.compile_db;
import cg.clang.bmi;
import cg.clang.pch.model;
import cg.clang.pch.paths;
import cg.clang.pch.sidecar;
import cg.clang.pch.incdirs;
import cg.clang.pch.depfile;
import cg.clang.pch.inject;
import cg.clang.pch.manifest;

export namespace cg::clang
{

    /**
     * @brief 规划 PCH 消费：发现、构建/复用影子、改写消费者 argv。
     *
     * @param commands 本次要解析的条目（update 时可能是过滤后的子集）。
     * @param coverage 全量编译数据库条目（发现 producer 的权威集合）。
     * @param opt PCH 扫描选项（配置目录等输入，donor 定位的前提）。
     */
    [[nodiscard]]
    inline auto plan_pch(
        std::vector<CompileCommand>        commands,
        const std::vector<CompileCommand>& coverage,
        const PchOptions&                  opt) -> PchPlanResult
    {
        namespace fs = std::filesystem;
        using detail::fold;
        PchPlanResult out;
        out.commands = std::move(commands);

        const auto has_data = [](const CompileCommand& c)
        { return c.pch_involved || c.pch_create || c.pch_use; };
        std::optional<detail::Sidecar> sidecar;
        const bool                     sidecar_file =
            !opt.sidecar_path.empty() && detail::is_file(opt.sidecar_path);
        if (sidecar_file)
        {
            sidecar =
                detail::load_sidecar(opt.sidecar_path, opt.compile_db_path);
        }
        const bool cmd_data = std::ranges::any_of(out.commands, has_data)
                              || std::ranges::any_of(coverage, has_data);
        const bool sidecar_data =
            sidecar.has_value()
            && (!sidecar->producers.empty() || !sidecar->consumers.empty());
        const auto mode = cg::util::cstd::get_env("CCG_PCH_MODE");
        const bool disabled = mode.has_value() && *mode == "off";
        if (disabled)
        {
            return out;
        }
        if (!cmd_data && !sidecar_data)
        {
            // 无 PCH 数据：零变化门禁——绝不建 pch 目录/清单。无效或陈旧
            // 的 pch.json 在此被静默忽略：它不得让无 PCH 工程留下任何
            // PCH 痕迹（note、空 manifest、缓存目录都不能出现）。
            return out;
        }
        const bool replay_degraded =
            opt.replay_degraded
            || (mode.has_value() && *mode == "replay-degraded");

        // 全量 cdb 重新加载（仅当本配置的条目里能看见消费者而 producer
        // 可能因根过滤缺席时）。发现只认构建系统记录。
        std::vector<CompileCommand> discovery = coverage;
        const bool                  coverage_has_consumer = std::ranges::any_of(
            discovery,
            [](const CompileCommand& c) { return c.pch_use; });
        if (!coverage_has_consumer && !opt.compile_db_path.empty())
        {
            try
            {
                discovery = cg::clang::load(opt.compile_db_path, {});
            }
            catch (const std::exception&)
            {
                discovery = coverage;
            }
        }

        std::unordered_map<std::string, detail::ProducerRec> producers;

        for (const CompileCommand& c : discovery)
        {
            if (!c.pch_create || c.pch_output.empty())
            {
                continue;
            }
            detail::ProducerRec rec;
            rec.display_key = cg::util::norm_path(c.pch_output);
            rec.key = fold(c.pch_output);
            rec.input = c.file;
            rec.directory = c.directory;
            rec.umbrella = c.pch_header;
            rec.forced = c.forced_includes;
            rec.argv = c.argv;
            // loader 的结构化 forced_includes 不含 `-imacros`（它不是
            // 强制包含而是宏注入），这里再从记录的 argv 补一次种子；两者
            // 求并集，重复无害。
            for (const std::string& f :
                 detail::forced_includes_of(rec.argv, rec.directory))
            {
                if (std::ranges::find(rec.forced, f) == rec.forced.end())
                {
                    rec.forced.push_back(f);
                }
            }
            // loader 已从 /Yc//Yu//Fp 或 --driver-mode=cl 推断方言；重放与
            // producer 跳过判定必须用同一信号（raw MSVC cdb 没有
            // --driver-mode=cl，只靠 argv[0]=cl.exe）。
            rec.cl_mode = detail::is_cl_command(c);
            rec.cxx = detail::producer_is_cxx(rec.argv, rec.input);
            detail::upsert_producer(producers, std::move(rec), false);
        }

        // MSBuild sidecar：producers 覆盖 cdb（cdb 保持剥离 /Yc /Fp）。
        std::vector<detail::SidecarConsumer> sidecar_consumers;
        if (sidecar_file)
        {
            if (sidecar.has_value())
            {
                for (const detail::SidecarProducer& p : sidecar->producers)
                {
                    detail::ProducerRec rec;
                    rec.display_key = p.pch_key;
                    rec.key = fold(p.pch_key);
                    rec.input = p.source;
                    rec.directory = p.cwd;
                    rec.umbrella = p.umbrella;
                    rec.argv.clear();
                    for (std::size_t i = 1; i < p.argv.size(); ++i)
                    {
                        if (i == 1 && p.argv[i] == "--driver-mode=cl")
                        {
                            continue; // 重放自行补模式旗标
                        }
                        rec.argv.push_back(p.argv[i]);
                    }
                    // 旁车 argv 是 /Yc 命令的逐字记录，没有过 cdb 载入的
                    // 宏值归一化；与消费者命令行共用同一函数后，影子 PCH
                    // 烙进的宏值才能与之按字节一致（装载期宏值比对任一侧
                    // 带原始空白即拒载、整组 TU 降级文本重放）。
                    cg::clang::detail::normalize_macro_values(rec.argv);
                    // `/FI` 与 umbrella 是彼此独立的 PCH 构建输入（自建
                    // 夹具实证）。逐字 argv 里的强制包含必须进入闭包，否则改
                    // 了被强制包含的头仍会复用陈旧影子。
                    rec.forced = detail::forced_includes_of(rec.argv, p.cwd);
                    if (!p.umbrella_path.empty()
                        && std::ranges::find(rec.forced, p.umbrella_path)
                               == rec.forced.end())
                    {
                        rec.forced.push_back(p.umbrella_path);
                    }
                    rec.cl_mode = true;
                    rec.cxx = detail::producer_is_cxx(rec.argv, rec.input);
                    rec.from_sidecar = true;
                    detail::upsert_producer(producers, std::move(rec), true);
                }
                sidecar_consumers = sidecar->consumers;
            }
            // 无效/陈旧的 sidecar 静默忽略：cdb 自身的记录（若有）继续
            // 生效；没有 cdb 记录时就是纯文本解析，不留 PCH 痕迹。
        }
        // 旁车消费者按文件归属索引（同文件多记录后者胜）：只留识别面
        // （关联键与伞头），file 是数据源组织信息，留在 SidecarConsumer
        // 不进判定。
        std::unordered_map<std::string, cg::clang::detail::SidecarPchFacts>
            sidecar_by_file;
        for (const detail::SidecarConsumer& c : sidecar_consumers)
        {
            sidecar_by_file.insert_or_assign(
                fold(c.file),
                cg::clang::detail::SidecarPchFacts{
                    .pch_key = c.pch_key,
                    .pch_header = c.pch_header});
        }

        // 影子缓存根：`<config>`/pch/`<engine_key>`/
        const DriverInfo& dinfo = driver_info();
        if (dinfo.driver.empty())
        {
            out.notes.push_back(
                "pch: no clang driver found - text fallback (a shadow PCH must "
                "be built with the engine's own driver)");
        }
        const detail::EngineIdentity ident = detail::engine_identity();
        const std::string            engine_key = ident.key;
        const std::string            engine_version =
            ident.version + "\n" + ident.driver + "\n" + ident.stat;
        std::string engine_dir;
        if (!opt.ccg_config_dir.empty())
        {
            engine_dir = cg::util::norm_path(
                (fs::path{opt.ccg_config_dir} / "pch" / engine_key)
                    .lexically_normal()
                    .generic_string());
            out.cache_dir = engine_dir;
        }

        detail::PchManifest manifest;
        const std::string   manifest_path =
            engine_dir.empty()
                ? std::string{}
                : cg::util::norm_path((fs::path{engine_dir} / "manifest.json")
                                          .lexically_normal()
                                          .generic_string());
        if (!manifest_path.empty())
        {
            if (const auto text = detail::read_text(manifest_path))
            {
                if (auto parsed = detail::pch_manifest_from_json(*text);
                    parsed.has_value())
                {
                    manifest = std::move(*parsed);
                    if (manifest.engine != engine_version)
                    {
                        // 引擎变化 = 世界观变化：整目录作废。
                        out.notes.push_back(
                            "pch: engine changed - shadow cache wiped");
                        if (!opt.dry_run)
                        {
                            std::error_code ec;
                            for (const fs::directory_entry& e :
                                 fs::directory_iterator(engine_dir, ec))
                            {
                                fs::remove_all(e.path(), ec);
                            }
                        }
                        manifest = detail::PchManifest{};
                    }
                }
            }
        }
        manifest.engine = engine_version;
        manifest.engine_key = engine_key;

        std::unordered_map<std::string, detail::PchEntry> prior;
        for (detail::PchEntry& e : manifest.entries)
        {
            prior.insert_or_assign(e.slot_key, e);
        }
        std::unordered_set<std::string> live_slots;

        // 只构建被消费者引用的 producer。命令侧的引用面经唯一判定点
        // （resolve_pch_consumer：cdb 令牌识别与旁车记录合并）得出；
        // 旁车记录自身的引用声明全量保留——其 TU 可能不在 discovery
        // （根过滤外），producer 的引用不能因命令缺席而丢失。
        std::unordered_map<std::string, bool> referenced;
        for (const CompileCommand& c : discovery)
        {
            const cg::clang::detail::SidecarPchFacts* facts = nullptr;
            if (const auto it = sidecar_by_file.find(fold(c.file));
                it != sidecar_by_file.end())
            {
                facts = &it->second;
            }
            const cg::clang::detail::PchConsumerVerdict verdict =
                cg::clang::detail::resolve_pch_consumer(c, facts);
            if (verdict.use && !verdict.path.empty())
            {
                referenced[fold(verdict.path)] = true;
            }
        }
        for (const detail::SidecarConsumer& c : sidecar_consumers)
        {
            referenced[fold(c.pch_key)] = true;
        }

        struct ShadowUse
        {
            std::string path;
            std::string identity;
            std::string note;
            bool        ok = false;
        };
        std::unordered_map<std::string, ShadowUse> shadows;

        for (const auto& [key, rec] : producers)
        {
            if (!referenced.contains(key))
            {
                continue;
            }
            ShadowUse use;
            if (dinfo.driver.empty() || engine_dir.empty())
            {
                use.note = "pch-producer-missing: no engine driver/cache dir";
                shadows.emplace(key, std::move(use));
                continue;
            }
            std::vector<std::string> inc_dirs;
            if (rec.cl_mode)
            {
                // cl 方言真实顺序（实测）：
                //   /I → CPATH → resource dir → -imsvc → INCLUDE
                // 自包含 argv（`/X` + 全量 -imsvc）下资源目录之外的一切
                // 环境/自动探测都必须关掉：闭包只用 argv 目录 + resource。
                const detail::IncludeDirsByKind kinds =
                    detail::include_dirs_by_kind(rec.argv, rec.directory);
                detail::append_include_dirs(inc_dirs, kinds.normal);
                const bool self_contained =
                    detail::is_self_contained_cl(rec.argv);
                if (!self_contained)
                {
                    detail::append_include_dirs(
                        inc_dirs,
                        detail::cpath_include_dirs(rec.cxx, rec.directory));
                }
                const std::string resource = detail::resource_include_dir();
                if (!resource.empty())
                {
                    inc_dirs.push_back(resource);
                }
                detail::append_include_dirs(inc_dirs, kinds.imsvc);
                if (!self_contained)
                {
                    detail::append_include_dirs(
                        inc_dirs,
                        detail::include_env_dirs(rec.directory));
                    // 自动探测链是回放的最后一层兜底；resource 已在前面，
                    // 去重使其不会重复。
                    detail::append_include_dirs(
                        inc_dirs,
                        cg::clang::driver_include_dirs(rec.cxx, true));
                }
            }
            else
            {
                inc_dirs = detail::include_dirs_of(rec.argv, rec.directory);
                // GNU 链：命令行 → CPATH/语言变量 → 驱动器默认（libc++）。
                detail::append_include_dirs(
                    inc_dirs,
                    detail::cpath_include_dirs(rec.cxx, rec.directory));
                detail::append_include_dirs(
                    inc_dirs,
                    cg::clang::driver_include_dirs(rec.cxx, false));
            }
            std::vector<std::string> seeds = rec.forced;
            if (!rec.input.empty())
            {
                seeds.push_back(rec.input);
            }
            if (!rec.umbrella.empty())
            {
                // 伞头先按记录目录解析；MSBuild 记录的伞头常是裸名，回退到
                // 记录的 include 搜索目录。
                std::string umbrella_path =
                    detail::resolve_from(rec.umbrella, rec.directory);
                if (!detail::is_file(umbrella_path))
                {
                    for (const std::string& dir : inc_dirs)
                    {
                        const std::string candidate =
                            detail::resolve_from(rec.umbrella, dir);
                        if (detail::is_file(candidate))
                        {
                            umbrella_path = candidate;
                            break;
                        }
                    }
                }
                if (detail::is_file(umbrella_path))
                {
                    seeds.push_back(std::move(umbrella_path));
                }
            }
            const std::string pd = detail::producer_digest(rec);
            const std::string slot_key = detail::hash_text(
                                             std::format(
                                                 "pch-v3\n{}\n{}\n{}",
                                                 engine_key,
                                                 rec.cl_mode ? "cl" : "gnu",
                                                 pd))
                                             .substr(0, 16);
            // 依赖集（缓存闭包）来自上一轮影子构建的 clang 真实解析：台账
            // 闭包逐文件字节重哈希（读不到即失配），全部一致 pch_key 才可
            // 能匹配。首跑/台账缺失走构建路径——构建运行附带依赖发射（同一
            // 遍预处理：宏 #include、搜索路径与大小写由真编译器按构建语义
            // 消解）。
            std::vector<std::pair<std::string, std::string>> closure_files;
            bool closure_from_ledger = false;
            if (const auto pit = prior.find(slot_key);
                pit != prior.end() && !pit->second.closure.empty())
            {
                closure_from_ledger = true;
                closure_files.reserve(pit->second.closure.size());
                for (const auto& dep : pit->second.closure)
                {
                    closure_files.emplace_back(
                        dep.first,
                        cg::clang::detail::hash_source(dep.first));
                }
            }
            std::string       closure_hash = detail::sha_of_list(closure_files);
            std::string       pch_key = detail::hash_text(
                                            std::format(
                                                "pch-id-v3\n{}\n{}\ndpr=1",
                                                slot_key,
                                                closure_hash))
                                            .substr(0, 32);
            const std::string slot_dir =
                cg::util::norm_path((fs::path{engine_dir} / slot_key)
                                        .lexically_normal()
                                        .generic_string());
            const std::string shadow =
                cg::util::norm_path((fs::path{slot_dir} / "shadow.pch")
                                        .lexically_normal()
                                        .generic_string());

            std::error_code fsec;
            const auto      fsize = fs::file_size(fs::path{shadow}, fsec);
            bool            reuse = false;
            std::string     current_hash;
            if (!opt.force && closure_from_ledger && !fsec && fsize > 0)
            {
                if (const auto it = prior.find(slot_key);
                    it != prior.end() && it->second.pch_key == pch_key
                    && it->second.closure_hash == closure_hash
                    && it->second.pch_size == static_cast<std::int64_t>(fsize)
                    && !it->second.pch_hash.empty())
                {
                    // 同路径内容替换（构建系统的 PCH 被外部改写）必须失配：
                    // 字节哈希是身份，mtime 不是。
                    current_hash = cg::clang::detail::hash_source(shadow);
                    if (!current_hash.empty()
                        && current_hash == it->second.pch_hash)
                    {
                        reuse = true;
                    }
                }
            }
            if (reuse)
            {
                use.ok = true;
                use.path = shadow;
                // 身份只认字节哈希：内容相同的重建（mtime 变化）不得让
                // 全部消费者 TU 变 stale。
                use.identity = detail::hash_text(
                                   std::format(
                                       "{}|{}|{}",
                                       engine_key,
                                       pch_key,
                                       current_hash))
                                   .substr(0, 32);
                ++out.reused;
                live_slots.insert(slot_key);
                shadows.emplace(key, std::move(use));
                continue;
            }
            // 跨配置 donor：本配置台账 miss（首跑）时从兄弟配置复制同一
            // 生产者产出，避免重复构建。donor 记录自带闭包——逐文件在本盘
            // 重哈希全部通过才采用（依赖集漂移即弃用，落回本地构建）。
            std::optional<detail::DonorHit> donor;
            bool                            donor_adopted = false;
            if (!opt.force && !opt.dry_run)
            {
                donor = detail::find_cross_config_donor(
                    opt,
                    engine_key,
                    engine_version,
                    slot_key);
            }
            if (donor.has_value())
            {
                std::vector<std::pair<std::string, std::string>> adopted;
                adopted.reserve(donor->entry.closure.size());
                bool deps_ok = true;
                for (const auto& dep : donor->entry.closure)
                {
                    const std::string h =
                        cg::clang::detail::hash_source(dep.first);
                    if (h.empty() || h != dep.second)
                    {
                        deps_ok = false;
                        break;
                    }
                    adopted.emplace_back(dep.first, h);
                }
                if (deps_ok)
                {
                    closure_files = std::move(adopted);
                    closure_from_ledger = true;
                    closure_hash = detail::sha_of_list(closure_files);
                    pch_key = detail::hash_text(
                                  std::format(
                                      "pch-id-v3\n{}\n{}\ndpr=1",
                                      slot_key,
                                      closure_hash))
                                  .substr(0, 32);
                    donor_adopted = true;
                }
            }
            if (donor_adopted)
            {
                if (!detail::create_dir(slot_dir))
                {
                    use.note =
                        "pch-build-failed: cannot create cache directory";
                    shadows.emplace(key, std::move(use));
                    continue;
                }
                // 复制到唯一 tmp，校验尺寸/哈希后才原子替换正式影子：
                // 半截 donor 副本绝不落到正式名上。
                const fs::path donor_tmp =
                    cg::util::platform::unique_temp_path(fs::path{shadow});
                std::error_code cec;
                fs::copy_file(
                    fs::path{donor->path},
                    donor_tmp,
                    fs::copy_options::overwrite_existing,
                    cec);
                if (!cec)
                {
                    const auto stat = cg::clang::detail::stat_source(
                        donor_tmp.generic_string());
                    const std::string copied_hash =
                        cg::clang::detail::hash_source(
                            donor_tmp.generic_string());
                    // 复制后尺寸必须与 donor 台账一致；有哈希则复核。
                    const bool size_ok = stat.ok && stat.size > 0
                                         && stat.size == donor->entry.pch_size;
                    const bool hash_ok =
                        !copied_hash.empty()
                        && copied_hash == donor->entry.pch_hash;
                    if (size_ok && hash_ok
                        && cg::util::platform::atomic_replace(
                            donor_tmp,
                            fs::path{shadow}))
                    {
                        detail::PchEntry entry = donor->entry;
                        entry.producer_file = rec.display_key;
                        entry.producer_input = rec.input;
                        entry.producer_cwd = rec.directory;
                        entry.producer_digest = pd;
                        entry.dialect = rec.cl_mode ? "cl" : "gnu";
                        // 记录 donor 来源，台账可审计跨配置复制。
                        entry.donor = donor->path;
                        entry.pch_size = stat.size;
                        // 复制后按本地字节重算，台账永远描述本配置盘上的影子。
                        entry.pch_hash = copied_hash;
                        entry.pch_key = pch_key;
                        prior.insert_or_assign(slot_key, entry);
                        live_slots.insert(slot_key);
                        use.ok = true;
                        use.path = shadow;
                        use.identity = detail::hash_text(
                                           std::format(
                                               "{}|{}|{}",
                                               engine_key,
                                               pch_key,
                                               entry.pch_hash))
                                           .substr(0, 32);
                        ++out.reused;
                        ++out.donated;
                        out.tu_notes.push_back(
                            std::format(
                                "{}: pch-donor: shadow copied from {}",
                                rec.input,
                                donor->path));
                        shadows.emplace(key, std::move(use));
                        continue;
                    }
                    // donor 字节不符/替换失败：删 tmp，落回本地重建
                    // （正式影子从未被触碰）。
                    cg::util::platform::remove_file(donor_tmp);
                }
                // 复制失败：落回本地构建，不把失败写进台账。
            }
            if (opt.dry_run)
            {
                use.ok = true;
                use.path = shadow;
                use.identity = "dry:" + pch_key;
                ++out.built;
                live_slots.insert(slot_key);
                shadows.emplace(key, std::move(use));
                continue;
            }
            if (!detail::create_dir(slot_dir))
            {
                use.note = "pch-build-failed: cannot create cache directory";
                shadows.emplace(key, std::move(use));
                continue;
            }
            // 影子也先写唯一 tmp，exit 0 且尺寸 >0 才原子替换正式名：
            // 失败时旧影子原样保留，绝不出现半截缓存。
            const fs::path shadow_tmp_path =
                cg::util::platform::unique_temp_path(fs::path{shadow});
            const std::string shadow_tmp = shadow_tmp_path.generic_string();
            std::vector<std::string> replay =
                detail::replay_argv(rec, shadow_tmp, slot_dir);
            // 依赖发射：与影子构建同一遍预处理（宏 #include、搜索路径、
            // 大小写由真编译器消解）。cl 方言走 /showIncludes 的 stdout；
            // gnu 方言走 -MD -MF，依赖文件落在 slot 缓存目录（过渡产物）。
            std::string dep_make;
            if (rec.cl_mode)
            {
                replay.emplace_back("/showIncludes");
            }
            else
            {
                dep_make = (fs::path{slot_dir} / "deps.d").generic_string();
                replay.emplace_back("-MD");
                replay.emplace_back("-MF");
                replay.emplace_back(dep_make);
            }
            if (cg::util::cstd::get_env("CCG_PCH_DEBUG").has_value())
            {
                std::print(std::cerr, "PCH-REPLAY {}:", rec.input);
                for (const std::string& a : replay)
                {
                    std::print(std::cerr, " [{}]", a);
                }
                std::println(std::cerr);
            }
            const std::vector<std::string> replay_args{
                replay.begin() + 1,
                replay.end()};
            const cg::clang::detail::ProcResult proc =
                cg::clang::detail::run_tool(
                    replay.front(),
                    replay_args,
                    rec.directory);
            std::error_code sec;
            std::int64_t    produced_size = 0;
            if (proc.started && proc.exit_code == 0
                && fs::is_regular_file(shadow_tmp_path, sec))
            {
                const auto size = fs::file_size(shadow_tmp_path, sec);
                if (!sec)
                {
                    produced_size = static_cast<std::int64_t>(size);
                }
            }
            const bool built = produced_size > 0
                               && cg::util::platform::atomic_replace(
                                   shadow_tmp_path,
                                   fs::path{shadow});
            if (!built)
            {
                cg::util::platform::remove_file(shadow_tmp_path);
                use.note = std::format(
                    "pch-build-failed: {}",
                    proc.started ? detail::first_line_of(
                                       proc.err.empty() ? proc.out : proc.err)
                                 : std::string{"driver did not start"});
                if (use.note.ends_with(": "))
                {
                    use.note += "unknown error";
                }
                shadows.emplace(key, std::move(use));
                continue;
            }
            const auto stat = cg::clang::detail::stat_source(shadow);
            // 构建期发射的依赖清单 → 真实闭包（种子 + 全部被包含文件，
            // 大小写折叠去重、逐文件字节哈希），并据此重定身份键。
            std::vector<std::string> emitted_deps;
            if (rec.cl_mode)
            {
                emitted_deps = detail::parse_show_includes(proc.out);
            }
            else if (!dep_make.empty())
            {
                if (const auto dep_text = detail::read_text(dep_make))
                {
                    emitted_deps = detail::parse_make_depfile(*dep_text);
                }
                cg::util::platform::remove_file(
                    fs::path{dep_make}); // 过渡产物，不留缓存目录
            }
            detail::PchEntry entry;
            entry.slot_key = slot_key;
            entry.producer_file = rec.display_key;
            entry.producer_input = rec.input;
            entry.producer_cwd = rec.directory;
            entry.producer_digest = pd;
            entry.dialect = rec.cl_mode ? "cl" : "gnu";
            entry.closure = detail::assemble_closure(seeds, emitted_deps);
            entry.closure_hash = detail::sha_of_list(entry.closure);
            pch_key = detail::hash_text(
                          std::format(
                              "pch-id-v3\n{}\n{}\ndpr=1",
                              slot_key,
                              entry.closure_hash))
                          .substr(0, 32);
            entry.pch_key = pch_key;
            entry.donor.clear();
            entry.pch_size = stat.ok ? stat.size : 0;
            entry.pch_hash = cg::clang::detail::hash_source(shadow);
            prior.insert_or_assign(slot_key, entry);
            live_slots.insert(slot_key);
            use.ok = true;
            use.path = shadow;
            use.identity = detail::hash_text(
                               std::format(
                                   "{}|{}|{}",
                                   engine_key,
                                   pch_key,
                                   entry.pch_hash))
                               .substr(0, 32);
            ++out.built;
            shadows.emplace(key, std::move(use));
        }

        // 消费者改写（只动本批 commands）。GNU producer 是生成的
        // `<target>`.dir/cmake_pch.hxx.cxx（内容只有注释），从解析集剔除。
        std::vector<CompileCommand> rewritten;
        rewritten.reserve(out.commands.size());
        for (CompileCommand& c : out.commands)
        {
            if (c.pch_create)
            {
                if (!detail::is_cl_command(c))
                {
                    ++out.producers_skipped;
                    continue;
                }
                c.argv = detail::strip_yc(c.argv);
                c.pch.status = PchUse::Status::none;
                c.pch.identity.clear();
                rewritten.push_back(std::move(c));
                continue;
            }
            // 消费者身份经唯一判定点合并（cdb 令牌识别 vs 旁车记录），
            // 不再就地拼。判定结果整组写回：cdb 已判定的条目函数原样
            // 返回其识别面（等价于原代码的跳过不写），旁车命中的条目
            // 取得 pch_key/pch_header 并补 pch_involved——与原合并语义
            // 逐条一致。
            const cg::clang::detail::SidecarPchFacts* facts = nullptr;
            if (const auto it = sidecar_by_file.find(fold(c.file));
                it != sidecar_by_file.end())
            {
                facts = &it->second;
            }
            const cg::clang::detail::PchConsumerVerdict verdict =
                cg::clang::detail::resolve_pch_consumer(c, facts);
            c.pch_use = verdict.use;
            c.pch_path = verdict.path;
            c.pch_header = verdict.header;
            c.pch_involved = c.pch_involved || verdict.sidecar_sourced;
            if (!c.pch_use)
            {
                if (c.pch_involved)
                {
                    // 隐式 PCH（`-include` 头旁有 .pch/.gch）：loader 已把
                    // -include 改写为转发形式绕开自动加载；没有记录的
                    // producer → 文本回退。
                    c.pch.status = PchUse::Status::text_fallback;
                    c.pch.identity = "text";
                    c.pch.note =
                        "pch-producer-missing: implicit PCH beside a "
                        "-include header - text fallback (the -include flag "
                        "was rewritten to bypass the implicit load)";
                    ++out.text_fallback;
                    out.tu_notes.push_back(
                        std::format(
                            "{}: {}",
                            cg::util::norm_path(c.file),
                            c.pch.note));
                }
                rewritten.push_back(std::move(c));
                continue;
            }
            ++out.consumers;
            const std::string key = fold(c.pch_path);
            const auto        sit = shadows.find(key);
            const bool linked = producers.contains(key) && sit != shadows.end()
                                && sit->second.ok;
            if (linked)
            {
                c.argv = detail::inject_shadow(c.argv, sit->second.path);
                c.pch_fallback_argv = detail::strip_include_pch(c.argv);
                c.pch.status = PchUse::Status::shadow;
                c.pch.identity = sit->second.identity;
                c.pch.shadow_path = sit->second.path;
                c.pch.note.clear();
                rewritten.push_back(std::move(c));
                continue;
            }
            const auto pit = producers.find(key);
            if (pit == producers.end())
            {
                // 没有记录的生产命令：禁止合成 recipe，直接文本回退。
                if (replay_degraded && !c.pch_path.empty()
                    && detail::is_file(c.pch_path))
                {
                    c.argv =
                        detail::inject_shadow(std::move(c.argv), c.pch_path);
                    c.pch.status = PchUse::Status::replay_degraded;
                    c.pch.identity = "replay-degraded";
                    c.pch.note = "replay-degraded (graph incomplete: no "
                                 "DetailedPreprocessingRecord)";
                    ++out.replay_degraded;
                    out.tu_notes.push_back(
                        std::format(
                            "{}: {}",
                            cg::util::norm_path(c.file),
                            c.pch.note));
                    rewritten.push_back(std::move(c));
                    continue;
                }
                c.argv = detail::strip_include_pch(std::move(c.argv));
                c.pch_fallback_argv = c.argv;
                c.pch.status = PchUse::Status::text_fallback;
                c.pch.identity = "text";
                const bool recorded_exists =
                    !c.pch_path.empty() && detail::is_file(c.pch_path);
                c.pch.note =
                    recorded_exists
                        ? "pch-rejected: the recorded PCH was not produced by "
                          "this engine - never consumed directly; text fallback"
                        : (c.pch_path.empty()
                               ? "pch-producer-missing: no recorded producer "
                                 "command - text fallback (recipe synthesis is "
                                 "forbidden)"
                               : "pch-missing: the recorded PCH (and a "
                                 "producer command) is absent - text fallback");
                ++out.text_fallback;
                out.tu_notes.push_back(
                    std::format(
                        "{}: {}",
                        cg::util::norm_path(c.file),
                        c.pch.note));
                rewritten.push_back(std::move(c));
                continue;
            }
            c.argv = detail::strip_include_pch(std::move(c.argv));
            c.pch_fallback_argv = c.argv;
            c.pch.status = PchUse::Status::text_fallback;
            c.pch.identity = "text";
            const std::string reason =
                sit != shadows.end() && !sit->second.note.empty()
                    ? sit->second.note
                    : std::string{"pch-build-failed"};
            c.pch.note = std::format("{} - text fallback", reason);
            ++out.text_fallback;
            out.tu_notes.push_back(
                std::format("{}: {}", cg::util::norm_path(c.file), c.pch.note));
            rewritten.push_back(std::move(c));
        }
        out.commands = std::move(rewritten);

        // 清单与孤儿清理。
        if (!manifest_path.empty() && !opt.dry_run)
        {
            // 宽限期：只有显式 --force/--full（opt.purge）且目标 mtime
            // 超过 1 小时才允许删除；增量/update 从不删缓存产物。
            constexpr std::chrono::hours grace{1};
            detail::PchManifest          next;
            next.engine = engine_version;
            next.engine_key = engine_key;
            std::unordered_set<std::string> dropped;
            for (const auto& [slot, entry] : prior)
            {
                if (live_slots.contains(slot))
                {
                    next.entries.push_back(entry);
                    continue;
                }
                if (!opt.purge)
                {
                    // 增量：结转所有旧条目，不删除任何目录。
                    next.entries.push_back(entry);
                    continue;
                }
                const fs::path slot_dir =
                    fs::path{engine_dir} / std::string{slot};
                if (detail::mtime_older_than_grace(slot_dir, grace))
                {
                    dropped.insert(slot);
                }
                else
                {
                    next.entries.push_back(entry);
                }
            }
            std::unordered_set<std::string> present;
            for (const detail::PchEntry& e : next.entries)
            {
                present.insert(e.slot_key);
            }
            // 乐观合并：并发规划器可能刚写入了它自己的 slot 条目；重读
            // 盘上清单并入未知条目，绝不复活本趟显式丢弃的 slot。
            if (const auto text = detail::read_text(manifest_path))
            {
                if (const auto disk = detail::pch_manifest_from_json(*text);
                    disk.has_value() && disk->engine == engine_version)
                {
                    for (detail::PchEntry e : disk->entries)
                    {
                        if (present.contains(e.slot_key)
                            || dropped.contains(e.slot_key))
                        {
                            continue;
                        }
                        present.insert(e.slot_key);
                        next.entries.push_back(std::move(e));
                    }
                }
            }
            if (detail::create_dir(engine_dir))
            {
                const std::string text = detail::pch_manifest_to_json(next);
                if (!cg::util::platform::atomic_write_file(
                        fs::path{manifest_path},
                        text))
                {
                    out.notes.push_back(
                        "pch: manifest write failed (the previous manifest was "
                        "kept)");
                }
            }
            if (opt.purge)
            {
                // 引用集来自重新读取的完整清单（写入后的盘上版本），
                // 目标必须超过宽限期才删除。清单读不回时不做任何回收：
                // 没有完整引用集就无法证明某个 slot 是孤儿。
                std::unordered_set<std::string> survivors;
                bool                            have_disk_manifest = false;
                if (const auto text = detail::read_text(manifest_path))
                {
                    if (const auto disk = detail::pch_manifest_from_json(*text);
                        disk.has_value() && disk->engine == engine_version)
                    {
                        for (const detail::PchEntry& e : disk->entries)
                        {
                            survivors.insert(e.slot_key);
                        }
                        have_disk_manifest = true;
                    }
                }
                for (const detail::PchEntry& e : next.entries)
                {
                    survivors.insert(e.slot_key);
                }
                if (have_disk_manifest)
                {
                    std::error_code ec;
                    for (const fs::directory_entry& e :
                         fs::directory_iterator(engine_dir, ec))
                    {
                        std::error_code isec;
                        if (!e.is_directory(isec))
                        {
                            continue;
                        }
                        const std::string slot =
                            e.path().filename().generic_string();
                        if (survivors.contains(slot)
                            || !detail::mtime_older_than_grace(e.path(), grace))
                        {
                            continue;
                        }
                        fs::remove_all(e.path(), ec);
                    }
                }
            }
        }

        out.notes.push_back(
            std::format(
                "shadow PCH: {} built, {} reused ({} donor), {} text fallbacks, "
                "{} unusable, {} replay-degraded, "
                "{} consumer TU(s), {} producer TU(s) skipped (cache {})",
                out.built,
                out.reused,
                out.donated,
                out.text_fallback,
                out.unusable,
                out.replay_degraded,
                out.consumers,
                out.producers_skipped,
                engine_dir.empty() ? std::string{"<none>"} : engine_dir));
        return out;
    }
} // namespace cg::clang

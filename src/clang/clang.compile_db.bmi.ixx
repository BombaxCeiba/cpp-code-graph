

/**
 * @module cg.clang.compile_db.bmi
 *
 * @brief BMI 映射域：BmiMap 结构与 JSON
 * 读取缓存、按命令求映射、模块喂给补供与兜底 argv。
 */
export module cg.clang.compile_db.bmi;

import std;
import cg.util;
import cg.util.cstd;
import cg.clang.driver;
import cg.clang.compile_db.model;
import cg.clang.compile_db.tokens;

export namespace cg::clang
{

    /**
     * @brief 一个构建目标的命名模块 BMI 映射。
     *
     * CMake 4.4+ 在目标的 .modmap 响应文件旁写 CXXModules.json，携带权威
     * 的模块名到 BMI 路径对应（"modules" 对象）、逐模块 requires 闭包
     * （"usages"）与查找拼写（"references"）。
     *
     * @internal
     * .modmap 展开之后的第二条消费通道：为 rsp 未携带投喂的模块 TU 供给
     * feeds，并为失败分类提供 pcm 到模块名的反查。usages 是消费编译必须
     * 投喂的精确集合（契约维度：整模块再导出链下，消费者自身的
     * requires 只列直接 import，能解析的是传递闭包）。
     */
    struct BmiMap
    {
        std::vector<ModuleFeed> feeds; // 模块名 -> 绝对 bmi 路径
        /** @brief feeds 的反向视图（绝对 pcm 路径 -> 模块名）。 */
        std::unordered_map<std::string, std::string> module_of_pcm;
        /** @brief 逐模块的 P1689 requires 闭包（"usages"），即上述契约
         *  下消费编译应投喂的精确集合。 */
        std::unordered_map<std::string, std::vector<std::string>> usages;
    };

    namespace detail
    {

        /**
         * @brief 读一份 CXXModules.json。
         *
         * @internal 任何问题（文件缺失、坏 JSON、意外形状）都返回空 BmiMap：
         * 此映射是加速器，绝不是闸门。CMake 为真实目标写绝对 bmi 路径，但其
         * 合成重扫目标（@synth_N）的路径相对构建根——相对路径锚定在 JSON
         * 自身目录上，绝不相对进程 cwd 解析。
         */
        [[nodiscard]]
        auto read_bmi_map(const std::string& json_path) -> BmiMap
        {
            namespace fs = std::filesystem;
            namespace json = cg::util::json;
            BmiMap          out;
            std::error_code ec;
            if (!fs::is_regular_file(fs::path{json_path}, ec))
            {
                return out;
            }
            try
            {
                const auto text = cg::util::read_text_file(json_path);
                if (!text.has_value())
                {
                    return out;
                }
                const json::Document  doc = json::Document::parse(*text);
                const json::ValueView root = doc.root();
                if (!root.is_object())
                {
                    return out;
                }
                const auto modules = root.get("modules");
                if (!modules || !modules->is_object())
                {
                    return out;
                }
                modules->for_each_member(
                    [&](std::string_view name, const json::ValueView& entry)
                    {
                        if (name.empty() || !entry.is_object())
                        {
                            return;
                        }
                        const auto bmi = entry.get("bmi");
                        if (!bmi || !bmi->is_string())
                        {
                            return;
                        }
                        ModuleFeed feed;
                        feed.name = std::string{name};
                        fs::path pcm{std::string{*bmi->as_string()}};
                        if (pcm.is_relative())
                        {
                            pcm = fs::path{json_path}.parent_path() / pcm;
                        }
                        feed.pcm = cg::util::norm_path(
                            pcm.lexically_normal().generic_string());
                        if (feed.pcm.empty())
                        {
                            return;
                        }
                        out.module_of_pcm.emplace(feed.pcm, feed.name);
                        out.feeds.push_back(std::move(feed));
                    });
                const auto usages = root.get("usages");
                if (usages && usages->is_object())
                {
                    usages->for_each_member(
                        [&](std::string_view name, const json::ValueView& list)
                        {
                            if (name.empty() || !list.is_array())
                            {
                                return;
                            }
                            std::vector<std::string>& closure =
                                out.usages[std::string{name}];
                            for (std::size_t i = 0; i < list.size(); ++i)
                            {
                                if (const auto v = list.at(i);
                                    v && v->is_string())
                                {
                                    closure.emplace_back(*v->as_string());
                                }
                            }
                        });
                }
            }
            catch (const std::exception&)
            {
                return BmiMap{};
            }
            return out;
        }

        /**
         * @brief 逐路径缓存。
         *
         * @internal 一个目标的全部模块条目共享同一份姊妹 CXXModules.json，
         * 且 load() 在测试中可能反复运行。
         */
        auto bmi_map_cache() -> std::unordered_map<std::string, BmiMap>&
        {
            static std::unordered_map<std::string, BmiMap> cache;
            return cache;
        }

    } // namespace detail

    /**
     * @brief 加载锚定在条目 .modmap 响应文件目录上的 BMI 映射。
     *
     * 先目标自身的 CXXModules.json，再同一 CMakeFiles/ 层级下每个姊妹
     * 目标的（提供 `std` 的 `cmake_cxx_std` 工具目标位于真实目标旁）。
     * 映射中已有的名字（来自本目标或更早姊妹）跳过——绝不在以不同 pcm
     * 构建同一模块名的两个目标之间猜测，目标自身的映射胜出。无解析时
     * 返回空映射；按锚目录缓存；线程安全。
     *
     * @internal
     * 确定性顺序：合并保留每模块名首个见到的映射，而目录迭代顺序未定，
     * 故先收集再排序。同名同 pcm 是无操作；同名不同 pcm 跳过。
     */
    [[nodiscard]]
    auto bmi_map_of(const CompileCommand& cc) -> const BmiMap&
    {
        namespace fs = std::filesystem;
        static std::mutex      mutex;
        const std::scoped_lock lock{mutex};
        auto&                  cache = detail::bmi_map_cache();
        if (cc.module_map.empty())
        {
            if (const auto it = cache.find("__none__"); it != cache.end())
            {
                return it->second;
            }
            return cache.emplace("__none__", BmiMap{}).first->second;
        }
        const std::string anchor_key = "merged:"
                                       + fs::path{cc.module_map}
                                             .parent_path()
                                             .lexically_normal()
                                             .generic_string();
        if (const auto it = cache.find(anchor_key); it != cache.end())
        {
            return it->second;
        }
        BmiMap merged = detail::read_bmi_map(
            (fs::path{cc.module_map}.parent_path() / "CXXModules.json")
                .lexically_normal()
                .generic_string());
        std::error_code ec;
        const fs::path  siblings =
            fs::path{cc.module_map}.parent_path().parent_path();
        if (fs::is_directory(siblings, ec))
        {
            std::vector<fs::path> dirs;
            for (const fs::directory_entry& entry :
                 fs::directory_iterator(siblings, ec))
            {
                if (entry.is_directory(ec))
                {
                    dirs.push_back(entry.path());
                }
            }
            std::ranges::sort(dirs);
            for (const fs::path& dir : dirs)
            {
                const BmiMap other =
                    detail::read_bmi_map((dir / "CXXModules.json")
                                             .lexically_normal()
                                             .generic_string());
                for (const ModuleFeed& feed : other.feeds)
                {
                    if (merged.module_of_pcm.contains(feed.pcm))
                    {
                        continue;
                    }
                    const bool name_conflict = std::ranges::any_of(
                        merged.feeds,
                        [&](const ModuleFeed& own)
                        { return own.name == feed.name; });
                    if (name_conflict)
                    {
                        continue;
                    }
                    merged.module_of_pcm.emplace(feed.pcm, feed.name);
                    merged.feeds.push_back(feed);
                }
            }
        }
        return cache.emplace(anchor_key, std::move(merged)).first->second;
    }

    /**
     * @brief 为重放 argv 未携带投喂的模块接口单元补 -fmodule-file。
     *
     * （被剥离或生成器过期的 .modmap 响应文件。）条目自身的
     * -fmodule-output 命名它产出的 BMI，目标的 CXXModules.json 把该 pcm
     * 反查回模块名并携带其 P1689 requires 闭包——恰是消费编译需要的投喂
     * 集。只投喂磁盘上存在且**非空**的 pcm（0 字节/半截产物是失败构建的
     * 残骸，投喂它只会让消费者以同样的错误失败）。返回获得投喂的条目数
     * （0 = modmap 通道已覆盖全部，即 CMake 的常态）。
     *
     * @internal
     * 不做超额投喂：libclang 会急切校验被投喂的 pcm，而姊妹/合成目标可能
     * 携带以不同方式构建的 BMI。绝不投喂单元自己的接口 pcm；合成重扫
     * 目标（@synth_N）与从未构建的可选目标不得以指向缺失文件的映射毒化
     * 解析。
     */
    auto supply_module_feeds(std::vector<CompileCommand>& commands)
        -> std::size_t
    {
        namespace fs = std::filesystem;
        std::size_t augmented = 0;
        for (CompileCommand& cc : commands)
        {
            if (!is_module_tu(cc) || !cc.module_feeds.empty()
                || cc.module_map.empty() || cc.module_output.empty())
            {
                continue;
            }
            const BmiMap& map = bmi_map_of(cc);
            const auto    own = map.module_of_pcm.find(cc.module_output);
            if (own == map.module_of_pcm.end())
            {
                continue;
            }
            const auto req = map.usages.find(own->second);
            if (req == map.usages.end())
            {
                continue;
            }
            bool fed_any = false;
            for (const std::string& name : req->second)
            {
                const auto feed_it = std::ranges::find_if(
                    map.feeds,
                    [&](const ModuleFeed& feed) { return feed.name == name; });
                if (feed_it == map.feeds.end()
                    || feed_it->pcm == cc.module_output)
                {
                    continue; // 绝不投喂单元自己的接口
                }
                std::error_code ec;
                if (!fs::is_regular_file(fs::path{feed_it->pcm}, ec) || ec)
                {
                    continue;
                }
                const auto feed_size =
                    fs::file_size(fs::path{feed_it->pcm}, ec);
                if (ec || feed_size == 0)
                {
                    continue; // 0 字节/半截 BMI 不得被投喂（复用）
                }
                cc.argv.push_back(
                    std::format(
                        "-fmodule-file={}={}",
                        feed_it->name,
                        feed_it->pcm));
                cc.module_feeds.push_back(*feed_it);
                fed_any = true;
            }
            if (fed_any)
            {
                ++augmented;
            }
        }
        return augmented;
    }

    /**
     * @brief 单个 TU 无可用条目时的基准旗标。
     *
     * -std=c++23 加 clang 驱动器为本安装报告的 include 搜索列表
     * （cg.clang.driver 讯问所得、按报告顺序）——绝不是烤死的路径。
     * 这与目录级兜底扫描无关：`ccg index` 只接受编译数据库，本函数只
     * 服务自建管线中没有记录权威的单元（如 cl 路由的模块单元）。
     *
     * @internal
     * $CCG_FALLBACK_FLAGS（';' 分隔的裸 argv token，如 "-isystem;
     * `<vendor>`/include;-DUSE_STD_FORMAT"）给这些合成向量追加自带依赖所需
     * 的 include 根与定义——自建管线的扫描、生产与解析全都骑同一份列表。
     * 讯问不到驱动器时响亮失败：错误的静默猜测比可行动错误更糟。
     */
    [[nodiscard]]
    auto fallback_argv() -> std::vector<std::string>
    {
        const DriverInfo& info = driver_info();
        if (info.driver.empty() || info.cxx_include_dirs.empty())
        {
            throw std::runtime_error(
                "ccg: cannot build fallback compiler flags: no clang driver "
                "found or it reported no include search list (looked beside "
                "the loaded LLVM engine library and under its installation "
                "prefix, then via CCG_LLVM_ROOT, then on PATH). Set "
                "CCG_LLVM_ROOT, pass `ccg index --llvm-root <prefix>`, or "
                "configure `ccg config set llvm-path <prefix>` to name the "
                "LLVM installation containing clang++/clang.");
        }
        std::vector<std::string> argv{"-std=c++23"};
        for (const std::string& dir : info.cxx_include_dirs)
        {
            argv.emplace_back("-I");
            argv.push_back(dir);
        }
        if (const auto env = cg::util::cstd::get_env("CCG_FALLBACK_FLAGS"))
        {
            std::stringstream stream{*env};
            std::string       token;
            while (std::getline(stream, token, ';'))
            {
                if (!token.empty())
                {
                    argv.push_back(std::move(token));
                }
            }
        }
        return argv;
    }
} // namespace cg::clang

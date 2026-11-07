module;
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// objbase 提供 IUnknown/GUID 与 interface 宏（CLR 宿主 COM 接口自
// 声明的基类与类型来源）；winioctl 提供 reparse 点解析。
#include <objbase.h>
#include <winioctl.h>
#else
// POSIX 分支：进程内 C# 对象宿主路线整体不可用（Windows 专属能力），
// 全部入口返回不可用。
#endif

/**
 * @module cg.util.cshost
 *
 * @brief C# 对象宿主的进程内装载与对象桥（Select-String 引擎面与
 *        MSBuild 求值面共宿主）：加载前微秒级预筛只对命中命令付费；
 *        Select-String 面走四候选降级链（装机版 dotnet 直载 → 独立
 *        dotnet → 商店版复制【含时间戳自刷新】→ 5.1），求值面钉 51 线
 *        （mscoree + CLR 4.0）。
 *
 * 本模块持有 CLR 宿主与 hostfxr 的全部平台 API 与降级链路由；工程
 * 其他任何地方不得接触这些宿主接口。对象协议：垫片侧两类
 * （SelectStringCore / MsbuildEval）各配静态工厂与句柄表，宿主经
 * ObjCreate/ObjCall/ObjDestroy 三件按句柄操作对象——51 线经命名共享
 * 内存（ExecuteInDefaultAppDomain 只接受单字符串参数），7 线经
 * [UnmanagedCallersOnly] 结构体桥（size + abi_version 双字段前缀，
 * 未来加字段时旧垫片按 size 判据直接暴露不匹配）。两线的垫片（构建期
 * 从同一份 C# 源码编译成 ccg-cshost51/7.dll，随可执行文件同目录分发
 * ——发布面经安装规则、开发树经构建期拷贝，装载唯一路线是同目录直载）
 * 持有互不可见的句柄表（各归各的 AppDomain / 默认 ALC）。
 *
 * 生命周期双侧联控（计数在 C++ 宿主侧、拆除动作在垫片侧）：句柄由
 * CsRef 以计数 1 接管，拷贝 = AddRef、析构 = Release，归零触发
 * ObjDestroy 的垫片拆除三件套（条目移除 → OnFinalRelease 钩子 →
 * GCHandle.Free）。垫片侧句柄 token 折入代次号，条目移除或槽复用后
 * 旧 token 永久失效（stale-handle 专用错误码），两侧对拆除的表决
 * 闭合；Release 归零后句柄已死，再对同号 AddRef 是错误用法（归零即
 * 失效纪律）。看门狗超时路径的会话走放弃分支（abandon）：只弃计数、
 * 不拆除——垫片孤儿线程仍在使用对象，此刻 GCHandle.Free 等于对托管
 * 对象的 use-after-free。
 *
 * 通道回传上限按句柄类分级：SelectStringCore 句柄沿用 64KB 视图
 * （行为零变化），MsbuildEval 句柄用 4MB 大视图（单工程事件量级）；
 * 垫片从视图容量自证档位。MsbuildEval 仅 51 线承载（VS 自带
 * Microsoft.Build.dll 是 net472 程序集，CoreCLR 装不下 .NET Framework
 * 家族），7 线的 ObjCreate 请求它返回"不被本线承载"专用错误码。
 *
 * @internal
 * 全局模块片段说明：Windows 分支自声明 CLR 宿主的最小 COM 接口集
 * （方法序与 GUID 逐字取自 Windows SDK 的 metahost.h / mscoree.h，
 * 避免依赖 NETFXSDK 的存在性）与 hostfxr 的最小函数指针类型（取自
 * 官方 hostfxr.h / coreclr_delegates.h 规格）；POSIX 分支无平台头。
 * 任何降级链失败都走快速否决：返回不可用，绝不抛出、绝不崩溃。
 * 协议载荷的 JSON 构造经 cg.util.json 门面（自带库纪律，严禁手写
 * 序列化）。
 */

export module cg.util.cshost;

import std;
// 可执行文件自身定位（随包垫片的同目录探测）；与伞头无关的兄弟
// 模块直连，DAG 上 platform 只依赖 std，无环。
import cg.util.platform;
// 环境变量读取单点（测试钩子 CCG_PSHOST_FORBID）；cstd 只依赖 std，无环。
import cg.util.cstd;
// 对象协议命令载荷（ObjCreate/ObjCall 的 JSON）经 json 门面构造；
// json 只依赖 std 与 bridge，无环。
import cg.util.json;

export namespace cg::util::cshost
{

    /**
     * @brief 对象协议错误码（与垫片侧 ObjectHost 逐值对齐；负数专用，
     *        0 成功、正数为句柄 token）。
     *
     * -1..-20 是垫片回传的协议码；-30 起是宿主侧补充（线不可用、命令
     * 超出通道档位），与垫片码区间隔离便于归因。
     */
    enum class ObjErr : std::int32_t
    {
        Ok = 0,           ///< 成功
        Channel = -1,     ///< 共享内存/结构体通道异常
        Payload = -2,     ///< 命令载荷非法（坏 JSON/形参缺失/长度越界）
        UnknownClass = -3,   ///< ObjCreate 类名未知
        Construct = -4,      ///< 静态工厂构造异常（诊断文本随通道回传）
        TableFull = -5,      ///< 垫片句柄表满
        Abi = -6,            ///< 结构体桥 ABI 不匹配（尺寸/版本判据）
        BadHandle = -10,     ///< 句柄值形态非法（非数字/越界）
        StaleHandle = -11,   ///< 句柄已失效（拆除后或从未存在的 token）
        UnknownMethod = -12, ///< 方法名未知或签名不符协议
        MethodFailure = -13, ///< 实例方法执行异常（诊断文本随通道回传）
        Overflow = -14,      ///< 结果超出该句柄级的通道视图容量
        NotOnLine = -20,     ///< 类不被本线承载（7 线请求 MsbuildEval）
        LineUnavailable = -30, ///< 宿主侧：目标线的引擎不可用
        CmdTooLarge = -31,    ///< 宿主侧：命令载荷超出通道档位容量
    };

    /** @brief 协议面（宿主内两面各自独立记账与择线）。 */
    enum class Face : std::uint8_t
    {
        SelectString, ///< Select-String 服务面（四候选降级链择线）
        Evaluation,   ///< MSBuild 求值面（钉 51 线）
    };

    /** @brief 垫片侧可构造的对象类（决定通道视图档位与线承载面）。 */
    enum class ObjClass : std::uint8_t
    {
        SelectStringCore, ///< 结构化查找意图提取（51/7 两线都承载）
        MsbuildEval,      ///< MSBuild 工程求值会话（仅 51 线承载）
    };

    /** @brief ObjCall 的命名实参（键 = 垫片侧方法形参名，值 = 字符串）。 */
    struct ObjArg
    {
        std::string_view name;
        std::string_view value;
    };

    /**
     * @brief 一次通道调用的结果。
     *
     * rc 为 0 时 payload 是方法返回 JSON；rc 为负错误码时 payload 是
     * 诊断文本（只取证不解析）。
     */
    struct ObjOutcome
    {
        std::int32_t rc{static_cast<std::int32_t>(ObjErr::Ok)};
        std::string  payload;

        /** @brief 是否成功。 */
        [[nodiscard]]
        auto ok() const noexcept -> bool
        {
            return rc == 0;
        }
    };

    /** @brief 引擎路由（降级链的最终落点，含四候选细分）。 */
    enum class Route : std::uint8_t
    {
        None = 0,          ///< 全链失败（快速否决）
        Clr51,             ///< 5.1 主路径：mscoree + CLR 4.0 + GAC 引擎
        HostfxrInstall,    ///< 候选一：装机版 dotnet 的 hostfxr + pwsh 目录
        HostfxrStandalone, ///< 候选二：DOTNET_ROOT 独立 dotnet
        HostfxrStoreCopy,  ///< 候选三：商店版 pwsh 复制布局（自刷新缓存）
    };

    /**
     * @brief 经进程内引擎解析一段 PowerShell 命令文本。
     *
     * Select-String 服务面的便捷入口：内部开一个 SelectStringCore
     * 短会话（ObjCreate → ObjCall("Extract") → 引用计数归零拆除），
     * 通道成本与历史单函数协议同为共享内存往返级。
     *
     * @param ps_script_utf8 完整命令文本（UTF-8）。
     * @return shim 产出的结构化查找请求 JSON 文本；预筛未命中、引擎
     *         不可用或通道失败时返回 std::nullopt（调用方按空手处理）。
     *
     * 加载前预筛在引擎初始化之前完成（微秒级字节扫描），未提及
     * Select-String/sls 的命令不付任何引擎成本。
     */
    [[nodiscard]]
    auto parse_via_engine(std::string_view ps_script_utf8)
        -> std::optional<std::string>;

    /** @brief 最近一次 Select-String 面实际使用的路由（诊断与测试）。 */
    [[nodiscard]]
    auto last_route() -> Route;

    /** @brief Select-String 面降级链的过程诊断（每候选一行结论，分号连接）。 */
    [[nodiscard]]
    auto last_detail() -> std::string_view;

    /**
     * @brief 指定面的过程诊断（求值面装载失败经此取指引性文本）。
     *
     * @param face 目标面（Select-String 面等价 last_detail）。
     */
    [[nodiscard]]
    auto face_detail(Face face) -> std::string_view;

    /**
     * @brief 降级链状态复位（仅测试钩子）。
     *
     * 配合 CCG_PSHOST_FORBID 注入重走降级链；已加载的引擎句柄刻意不
     * 卸载（CLR 不可卸载、hostfxr 委托生命周期属进程级），复位只清除
     * "已解析"记账令下一次调用重走候选序列。产品代码不得调用。
     */
    void reset_for_tests();

} // namespace cg::util::cshost

namespace cg::util::cshost
{
    /**
     * 句柄盒：C++ 侧引用计数的宿主载体（模块内部类型，导出面只见
     * CsRef）。refs 为 0/1 闸的计数器；stale 是看门狗短路闩——置位后
     * 归零析构不再触发 ObjDestroy（垫片孤儿线程仍在使用对象）。
     */
    struct HandleBox
    {
        Face                       face{};
        ObjClass                   cls{};   // 决定 51 线通道视图档位
        std::int32_t               token{}; // 垫片侧句柄（正数）
        std::atomic<std::uint32_t> refs{1};
        std::atomic<bool>          stale{false};
    };

    /** 引用计数归零的收尾（Windows 侧拆除句柄并释放盒子；POSIX 只删盒子）。 */
    void drop_handle_last(HandleBox* box);
}

export namespace cg::util::cshost
{

    /**
     * @brief 对象句柄的 RAII 值类型（计数参与权：拷贝 = AddRef、
     *        析构 = Release，归零析构触发垫片拆除三件套）。
     *
     * 语义纪律：Release 归零后句柄已死，继续持有或复活同号句柄是
     * 错误用法（COM/DAS 同款"归零即失效"）；看门狗超时会话先
     * abandon() 再放手——归零只弃计数、不拆除（垫片孤儿线程仍在
     * 使用对象）。abandon 后 call 一律拒绝（会话已放弃）。
     *
     * 多对象并存各自独立计数互不串扰（如 --all 多配置循环每配置
     * 一个 MsbuildEval 会话 = 各一个 CsRef，结束各自归零各自拆）。
     */
    class CsRef
    {
    public:
        /** @brief 空句柄（不持有任何对象）。 */
        CsRef() = default;

        CsRef(const CsRef& other) noexcept;
        CsRef(CsRef&& other) noexcept;
        CsRef& operator=(const CsRef& other) noexcept;
        CsRef& operator=(CsRef&& other) noexcept;
        ~CsRef();

        /** @brief 是否持有活句柄。 */
        [[nodiscard]]
        auto valid() const noexcept -> bool;

        /** @brief 所属协议面。 */
        [[nodiscard]]
        auto face() const noexcept -> Face;

        /** @brief 对象类。 */
        [[nodiscard]]
        auto obj_class() const noexcept -> ObjClass;

        /** @brief 垫片侧句柄 token（诊断与协议测试）。 */
        [[nodiscard]]
        auto token() const noexcept -> std::int32_t;

        /**
         * @brief 看门狗短路：标记放弃（stale 闩）。
         *
         * 此后 call 一律拒绝；析构归零只弃计数、不调 ObjDestroy——
         * 垫片孤儿线程仍在使用对象，此刻拆除等于对托管对象的
         * use-after-free，对象与孤儿线程一起活到进程退出。放弃是
         * 会话级单向动作，作用于整个句柄盒。
         */
        void abandon() noexcept;

        /**
         * @brief 经句柄调用实例方法（ObjCall 的 RAII 面）。
         *
         * @param method 垫片侧公有实例方法名（全 string 形参、string
         *               返回的协议方法）。
         * @param args   命名实参表。
         * @return 成功时 payload 为方法返回 JSON；失败时 rc 携带专用
         *         错误码、payload 为诊断文本。
         */
        [[nodiscard]]
        auto call(
            std::string_view        method,
            std::span<const ObjArg> args) const -> ObjOutcome;

    private:
        friend struct CsRefFactory;

        explicit CsRef(HandleBox* box) noexcept
            : box_{box}
        {
        }

        HandleBox* box_ = nullptr;
    };

    /**
     * @brief 开一个 SelectStringCore 会话（四候选降级链择线）。
     *
     * @return 可用引擎时返回计数 1 的句柄；全链失败返回 std::nullopt
     *         （诊断经 face_detail(Face::SelectString)）。
     */
    [[nodiscard]]
    auto open_select_string() -> std::optional<CsRef>;

    /**
     * @brief 开一个 MsbuildEval 求值会话（钉 51 线，不走降级链）。
     *
     * VS 自带 Microsoft.Build.dll 是 net472 程序集，CoreCLR（7 线）
     * 装不下 .NET Framework 家族程序集，求值面只由 51 线承载；51 线
     * 不可用（垫片缺席 / CLR 起不来）= 调用方按指引性报错处理。
     *
     * @param assembly_dir MSBuild.exe 同目录（VS 自带程序集的解析根，
     *                     经 ObjCreate 构造参数递入；垫片的依赖解析
     *                     只认这个根）。
     * @return 会话句柄；构造失败（目录无效/引擎程序集缺失——垫片回
     *         ErrConstruct）或 51 线不可用时返回 std::nullopt。
     */
    [[nodiscard]]
    auto open_msbuild_eval(std::wstring_view assembly_dir)
        -> std::optional<CsRef>;

    /**
     * @brief 对象协议底层通道原语（显式面，RAII 之外的无状态形态）。
     *
     * 求值客户端与协议单测的消费面：直接以裸 token 驱动三件（不经
     * 引用计数），用于分派方法的显式控制与陈旧句柄的对抗性验证。
     * 生产路径应优先 CsRef（open_* + call），防泄漏防复活。
     *
     * @return token（正数）或负错误码。
     */
    auto obj_create(
        Face                face,
        ObjClass            cls,
        std::wstring_view   assembly_dir) -> std::int32_t;

    /**
     * @brief ObjCall 通道原语：按句柄与方法名反射分派实例方法。
     *
     * @param cls  句柄类（51 线据此选通道视图档位：SelectStringCore
     *             小档 64KB、MsbuildEval 大档 4MB）。
     * @param token 垫片侧句柄。
     * @return 成功时 payload 为方法返回 JSON；失败时 rc 携带专用
     *         错误码、payload 为诊断文本。
     */
    auto obj_call(
        Face                     face,
        ObjClass                 cls,
        std::int32_t             token,
        std::string_view         method,
        std::span<const ObjArg>  args) -> ObjOutcome;

    /**
     * @brief ObjDestroy 通道原语：垫片拆除三件套（条目移除 →
     *        OnFinalRelease → GCHandle.Free）。
     *
     * 正常生命周期由 CsRef 归零路径独占调用；本原语的直接调用属于
     * 协议测试面（陈旧句柄验证）。坏句柄/已失效句柄返回专用错误码。
     */
    auto obj_destroy(Face face, std::int32_t token) -> std::int32_t;

} // namespace cg::util::cshost

// ---------------------------------------------------------------------------
// CsRef 成员定义（平台无关：只操作句柄盒与 obj_* 原语；拆除动作经
// drop_handle_last 分支供给）。
// ---------------------------------------------------------------------------
namespace cg::util::cshost
{

    /** 句柄接管的模块内部工厂（CsRef 的友元面：open_* 经它把裸句柄
     *  装进 CsRef；私有构造不落进导出面）。 */
    struct CsRefFactory
    {
        static auto adopt(HandleBox* box) -> CsRef { return CsRef{box}; }
    };

    CsRef::CsRef(const CsRef& other) noexcept
        : box_{other.box_}
    {
        if (box_ != nullptr)
        {
            box_->refs.fetch_add(1, std::memory_order_relaxed);
        }
    }

    CsRef::CsRef(CsRef&& other) noexcept
        : box_{std::exchange(other.box_, nullptr)}
    {
    }

    CsRef& CsRef::operator=(const CsRef& other) noexcept
    {
        if (this != &other)
        {
            // 先增后减：自赋值安全，且源仍持有时盒不会中途归零。
            if (other.box_ != nullptr)
            {
                other.box_->refs.fetch_add(1, std::memory_order_relaxed);
            }
            if (box_ != nullptr
                && box_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                drop_handle_last(box_);
            }
            box_ = other.box_;
        }
        return *this;
    }

    CsRef& CsRef::operator=(CsRef&& other) noexcept
    {
        if (this != &other)
        {
            if (box_ != nullptr
                && box_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                drop_handle_last(box_);
            }
            box_ = std::exchange(other.box_, nullptr);
        }
        return *this;
    }

    CsRef::~CsRef()
    {
        if (box_ != nullptr
            && box_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            drop_handle_last(box_);
        }
    }

    auto CsRef::valid() const noexcept -> bool { return box_ != nullptr; }

    auto CsRef::face() const noexcept -> Face
    {
        return box_ != nullptr ? box_->face : Face::SelectString;
    }

    auto CsRef::obj_class() const noexcept -> ObjClass
    {
        return box_ != nullptr ? box_->cls : ObjClass::SelectStringCore;
    }

    auto CsRef::token() const noexcept -> std::int32_t
    {
        return box_ != nullptr ? box_->token : 0;
    }

    void CsRef::abandon() noexcept
    {
        if (box_ != nullptr)
        {
            // 会话级单向闩；与归零析构的跨线程顺序由 refs 的原子
            // 读写链保证（seq_cst 省去精细论证，abandon 是一次性动作）。
            box_->stale.store(true, std::memory_order_seq_cst);
        }
    }

    auto CsRef::call(
        std::string_view        method,
        std::span<const ObjArg> args) const -> ObjOutcome
    {
        if (box_ == nullptr)
        {
            return ObjOutcome{
                .rc = static_cast<std::int32_t>(ObjErr::LineUnavailable)};
        }
        if (box_->stale.load(std::memory_order_seq_cst))
        {
            return ObjOutcome{
                .rc      = static_cast<std::int32_t>(ObjErr::StaleHandle),
                .payload = "session abandoned (watchdog short-circuit)"};
        }
        return obj_call(box_->face, box_->cls, box_->token, method, args);
    }

} // namespace cg::util::cshost

// ---------------------------------------------------------------------------
// Windows 实现。
// ---------------------------------------------------------------------------
#if defined(_WIN32)

namespace cg::util::cshost
{

    namespace
    {

// —— reparse 数据缓冲（user-mode 的 winioctl.h 不提供 REPARSE_DATA_BUFFER，
//    官方文档指向内核侧 Ntifs.h；此处按该结构的标准布局自声明，仅用于
//    读 APPEXECLINK 执行别名的重解析数据）。
#ifndef REPARSE_DATA_BUFFER_HEADER
#define REPARSE_DATA_BUFFER_HEADER
        struct CcgReparseDataBuffer
        {
            DWORD ReparseTag;
            WORD  ReparseDataLength;
            WORD  Reserved;
            union
            {
                struct
                {
                    WORD  SubstituteNameOffset;
                    WORD  SubstituteNameLength;
                    WORD  PrintNameOffset;
                    WORD  PrintNameLength;
                    ULONG Flags;
                    WCHAR PathBuffer[1];
                } SymbolicLinkReparseBuffer;
                struct
                {
                    UCHAR DataBuffer[1];
                } GenericReparseBuffer;
            };
        };
#endif // REPARSE_DATA_BUFFER_HEADER

        // —— CLR 宿主最小 COM 接口自声明（方法序逐字对齐 SDK 头，见模块注释）。

        interface IMyICLRMetaHost : public IUnknown
        {
            virtual HRESULT STDMETHODCALLTYPE
            GetRuntime(LPCWSTR pwzVersion, REFIID riid, LPVOID * ppRuntime) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetVersionFromFile(
                LPCWSTR pwzFilePath,
                LPWSTR  pwzBuffer,
                DWORD * pcchBuffer) = 0;
            virtual HRESULT STDMETHODCALLTYPE EnumerateInstalledRuntimes(
                IUnknown * *ppEnumerator) = 0;
            virtual HRESULT STDMETHODCALLTYPE EnumerateLoadedRuntimes(
                HANDLE hndProcess,
                IUnknown * *ppEnumerator) = 0;
            virtual HRESULT STDMETHODCALLTYPE RequestRuntimeLoadedNotification(
                void* pCallbackFunction) = 0;
            virtual HRESULT STDMETHODCALLTYPE QueryLegacyV2RuntimeBinding(
                REFIID riid,
                LPVOID * ppUnk) = 0;
            virtual HRESULT STDMETHODCALLTYPE ExitProcess(INT32 iExitCode) = 0;
        };

        interface IMyICLRRuntimeInfo : public IUnknown
        {
            virtual HRESULT STDMETHODCALLTYPE GetVersionString(
                LPWSTR pwzBuffer,
                DWORD * pcchBuffer) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetRuntimeDirectory(
                LPWSTR pwzBuffer,
                DWORD * pcchBuffer) = 0;
            virtual HRESULT STDMETHODCALLTYPE IsLoaded(
                HANDLE hndProcess,
                BOOL * pbLoaded) = 0;
            virtual HRESULT STDMETHODCALLTYPE LoadErrorString(
                UINT   iResourceID,
                LPWSTR pwzBuffer,
                DWORD * pcchBuffer,
                LONG iLocaleID) = 0;
            virtual HRESULT STDMETHODCALLTYPE LoadLibrary(
                LPCWSTR pwzDllName,
                HMODULE * phndModule) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetProcAddress(
                LPCSTR pszProcName,
                LPVOID * ppProc) = 0;
            virtual HRESULT STDMETHODCALLTYPE
            GetInterface(REFCLSID rclsid, REFIID riid, LPVOID * ppUnk) = 0;
            virtual HRESULT STDMETHODCALLTYPE IsLoadable(BOOL * pbLoadable) = 0;
            virtual HRESULT STDMETHODCALLTYPE SetDefaultStartupFlags(
                DWORD   dwStartupFlags,
                LPCWSTR pwzHostConfigFile) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetDefaultStartupFlags(
                DWORD * pdwStartupFlags,
                LPWSTR pwzHostConfigFile,
                DWORD * pcchHostConfigFile) = 0;
            virtual HRESULT STDMETHODCALLTYPE BindAsLegacyV2Runtime() = 0;
            virtual HRESULT STDMETHODCALLTYPE IsStarted(
                BOOL * pbStarted,
                DWORD * pdwStartupFlags) = 0;
        };

        interface IMyICLRRuntimeHost : public IUnknown
        {
            virtual HRESULT STDMETHODCALLTYPE Start() = 0;
            virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
            virtual HRESULT STDMETHODCALLTYPE SetHostControl(
                void* pHostControl) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetCLRControl(
                void** pCLRControl) = 0;
            virtual HRESULT STDMETHODCALLTYPE UnloadAppDomain(
                DWORD dwAppDomainId,
                BOOL  fWaitUntilDone) = 0;
            virtual HRESULT STDMETHODCALLTYPE ExecuteInAppDomains(
                DWORD dwAppDomainId,
                void* pCallback,
                void* cookie) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetCurrentAppDomainId(
                DWORD * pdwAppDomainId) = 0;
            virtual HRESULT STDMETHODCALLTYPE ExecuteApplication(
                LPCWSTR pwzAppFullName,
                DWORD   dwManifestPaths,
                LPCWSTR * ppwzManifestPaths,
                DWORD dwActivationData,
                LPCWSTR * ppwzActivationData,
                int* pReturnValue) = 0;
            virtual HRESULT STDMETHODCALLTYPE ExecuteInDefaultAppDomain(
                LPCWSTR pwzAssemblyPath,
                LPCWSTR pwzTypeName,
                LPCWSTR pwzMethodName,
                LPCWSTR pwzArgument,
                DWORD * pReturnValue) = 0;
        };

        // GUID（逐字取自 SDK metahost.h / mscoree.h 的 EXTERN_GUID 定义）。
        constexpr GUID CLSID_CLR_META_HOST = {
            0x9280188d,
            0x0e8e,
            0x4867,
            {0xb3, 0x0c, 0x7f, 0xa8, 0x38, 0x84, 0xe8, 0xde}};
        constexpr GUID IID_CLR_META_HOST = {
            0xd332db9e,
            0xb9b3,
            0x4125,
            {0x82, 0x07, 0xa1, 0x48, 0x84, 0xf5, 0x32, 0x16}};
        constexpr GUID IID_CLR_RUNTIME_INFO = {
            0xbd39d1d2,
            0xba2f,
            0x486a,
            {0x89, 0xb0, 0xb4, 0xb0, 0xcb, 0x46, 0x68, 0x91}};
        constexpr GUID CLSID_CLR_RUNTIME_HOST = {
            0x90f1a06e,
            0x7712,
            0x4762,
            {0x86, 0xb5, 0x7a, 0x5e, 0xba, 0x6b, 0xdb, 0x02}};
        constexpr GUID IID_CLR_RUNTIME_HOST = {
            0x90f1a06c,
            0x7712,
            0x4762,
            {0x86, 0xb5, 0x7a, 0x5e, 0xba, 0x6b, 0xdb, 0x02}};

        // —— hostfxr 最小函数指针类型（官方 hostfxr.h / coreclr_delegates.h
        //    规格的自声明；char_t 在 Windows 上是 wchar_t）。

        using HxHandle = void*;

        using HxInitFn = std::int32_t(__stdcall*)(
            const wchar_t* runtime_config,
            const void*    parameters,
            HxHandle*      host_context);
        using HxDelegateFn = std::int32_t(__stdcall*)(
            HxHandle host_context,
            int      delegate_type,
            void**   delegate);
        using HxCloseFn = std::int32_t(__stdcall*)(HxHandle host_context);
        using HxLoadAssemblyFn = std::int32_t(__stdcall*)(
            const wchar_t* assembly_path,
            const wchar_t* type_name,
            const wchar_t* method_name,
            const wchar_t* delegate_type_name,
            void*          reserved,
            void**         delegate);

        constexpr int HDT_LOAD_ASSEMBLY_AND_GET_FUNCTION_POINTER = 5;

        /// UNMANAGEDCALLERSONLY_METHOD 哨兵（coreclr_delegates.h 定义为
        /// (const char_t*)-1）。指针整转不可用于常量表达式，经函数取值。
        [[nodiscard]]
        auto unmanaged_callers_only_marker() -> const wchar_t*
        {
            return reinterpret_cast<const wchar_t*>(
                static_cast<std::uintptr_t>(-1));
        }

        // —— 7 线结构体桥（与 Cshost7Entry.cs 的 CcgCsCallV1 同构镜像；
        //    LayoutKind.Sequential 自然对齐，x64 上无填充）。未来加字段时
        //    旧垫片按 size 判据直接暴露 ABI 不匹配，绝不误读。
        struct NativeCallV1
        {
            std::uint32_t size;        // 调用方填结构体总字节数（判据字段）
            std::uint32_t abi_version;  // 协议版本（当前 1）
            void*         cmd;          // 载荷指针（UTF-8 JSON / token 串）
            std::uint64_t cmd_len;      // 载荷字节数
            void*         out_slot;     // 出参槽：垫片就地写回结果指针
        };
        static_assert(sizeof(NativeCallV1) == 32, "结构体桥布局漂移");
        static_assert(offsetof(NativeCallV1, out_slot) == 24, "出参槽偏移漂移");

        /// 7 线对象协议入口的函数指针形态（cdecl；x64 单一调用约定）。
        using ObjFn7 = int (*)(void* args, int args_size);

        constexpr std::uint32_t kAbiVersion7 = 1;

        // —— 51 线共享内存通道布局（与 Cshost51Entry.cs 逐常量一致）。
        //    结果区上限按句柄类分级：小档（SelectStringCore 与 ObjCreate）
        //    沿用历史单函数协议常量、行为零变化；大档（MsbuildEval 句柄）
        //    服务单工程事件量级。垫片从视图容量自证档位（容量 ≥ 大档
        //    全量即按大档解释），坏句柄路径不产生档位歧义。
        struct ChannelLayout
        {
            std::size_t cmd_cap;   // 命令区 wchar 容量
            std::size_t res_off;   // 结果区偏移（字节）
            std::size_t view_bytes; // 映射总大小（字节）
            std::size_t res_cap;   // 结果区容量（字节）
        };

        constexpr std::size_t CMD_OFF = 8; // 头部两个 int32

        constexpr ChannelLayout small_view{
            4096,
            CMD_OFF + 2 * 4096,
            CMD_OFF + 2 * 4096 + 65536,
            65536,
        };
        constexpr ChannelLayout large_view{
            65536,
            CMD_OFF + 2 * 65536,
            CMD_OFF + 2 * 65536 + 4 * 1024 * 1024,
            4 * 1024 * 1024,
        };

        /// 进程级引擎状态：一次性解析（成功与失败都记账，绝不重试慢路径）。
        /// Select-String 面与求值面各持一个实例（择线策略不同：前者四候选
        /// 降级链、后者钉 51 线），同进程内两面的 CLR/hostfxr 装载互不干扰。
        struct EngineState
        {
            std::mutex  mutex;
            bool        resolved = false;
            Route       route = Route::None;
            std::string detail;
            // 5.1 路线句柄（加载后不卸载：CLR 不可卸载，钩子进程短命无妨）。
            IMyICLRRuntimeHost* clr = nullptr;
            std::wstring        shim51_path;
            // 7 路线对象协议三件的函数指针（hostfxr 委托，进程级不卸载）。
            ObjFn7 create7  = nullptr;
            ObjFn7 call7    = nullptr;
            ObjFn7 destroy7 = nullptr;
            std::atomic<std::uint64_t> map_seq{0};
        };

        auto shell_state() -> EngineState&
        {
            static EngineState state; // Select-String 面
            return state;
        }

        auto eval_state() -> EngineState&
        {
            static EngineState state; // 求值面（钉 51 线）
            return state;
        }

        /**
         * @brief 加载前微秒级预筛：命令文本是否提及 Select-String / sls。
         *
         * 字节级大小写不敏感扫描（UTF-8 多字节序列不含 ASCII 字母字节）；
         * 三字母别名 sls 要求词边界（前后字节不是字母/数字/下划线/连字符），
         * 避免把普通子串误当命中。预筛只是成本闸门，误命中最多白付一次
         * 引擎解析，方向安全。
         */
        [[nodiscard]]
        auto mentions_search_cmdlet(std::string_view text) -> bool
        {
            constexpr std::string_view needle = "select-string";
            if (text.size() < 3)
            {
                return false;
            }
            const auto is_word_byte = [](char c)
            {
                const unsigned char u = static_cast<unsigned char>(c);
                return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
                       || (u >= '0' && u <= '9') || u == '_' || u == '-';
            };
            const auto lower = [](char c)
            {
                return static_cast<char>(
                    c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            };
            // 循环上界按三字母别名（短于 select-string 的命令也必须能命中，
            // 实测教训：上界取长针时 "sls foo" 整条被跳过）。
            for (std::size_t i = 0; i + 3 <= text.size(); ++i)
            {
                if (i + needle.size() <= text.size())
                {
                    bool match = true;
                    for (std::size_t j = 0; j < needle.size(); ++j)
                    {
                        if (lower(text[i + j]) != needle[j])
                        {
                            match = false;
                            break;
                        }
                    }
                    if (match)
                    {
                        return true;
                    }
                }
                // 独立词 "sls"。
                if (lower(text[i]) == 's' && lower(text[i + 1]) == 'l'
                    && lower(text[i + 2]) == 's')
                {
                    const bool left_ok = i == 0 || !is_word_byte(text[i - 1]);
                    const bool right_ok =
                        i + 3 >= text.size() || !is_word_byte(text[i + 3]);
                    if (left_ok && right_ok)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        /// 读宽字符环境变量（512 缓冲）；未设置、读取失败或装不下时按缺失
        /// 处理（返回 nullopt）。本模块内一切系统位置都从环境变量推导
        /// （机器绝对路径禁令），溢出即缺失是该推导的既定口径。
        [[nodiscard]]
        auto env_var(const wchar_t* name) -> std::optional<std::wstring>
        {
            std::wstring raw(512, L'\0');
            const DWORD  n = GetEnvironmentVariableW(name, raw.data(), 512);
            if (n == 0 || n >= 512)
            {
                return std::nullopt;
            }
            return raw.substr(0, n);
        }

        /// %LOCALAPPDATA%（拿不到时空路径，调用方按失败处理）。
        [[nodiscard]]
        auto local_app_data() -> std::filesystem::path
        {
            const auto base = env_var(L"LOCALAPPDATA");
            return base.has_value() ? std::filesystem::path{*base}
                                    : std::filesystem::path{};
        }

        /// cshost 缓存根（%LOCALAPPDATA%/ccg/cshost；历史 pshost 目录在
        /// 模块更名后成孤儿，一次性重复制成本）。
        [[nodiscard]]
        auto cache_root() -> std::filesystem::path
        {
            return local_app_data() / L"ccg" / L"cshost";
        }

        /// 标准安装前缀下的子目录（%ProgramFiles%\<tail>；拿不到时空路径，
        /// 调用方按候选缺失处理）。机器绝对路径禁令（可移植性闸门）要求
        /// 一切系统位置从环境变量推导。
        [[nodiscard]]
        auto program_files_sub(const std::wstring& tail)
            -> std::filesystem::path
        {
            const auto root = env_var(L"ProgramFiles");
            return root.has_value() ? std::filesystem::path{*root} / tail
                                    : std::filesystem::path{};
        }

        /// 可执行文件同目录的垫片 dll（唯一装载路线：同目录直载）。
        ///
        /// 发布面（cmake/Install.cmake 的安装规则）与开发构建树
        /// （src/CMakeLists.txt 的 POST_BUILD 拷贝）都把
        /// ccg-cshost51/7.dll 放在可执行文件同级目录，存在且尺寸
        /// 非零即直载；缺席（构建机工具线不完整或布局被拆散）时该
        /// 路线不可用，诊断经 note_shim_missing 进 detail 链。
        [[nodiscard]]
        auto bundled_shim(const wchar_t* stem)
            -> std::optional<std::filesystem::path>
        {
            const std::filesystem::path exe =
                cg::util::platform::executable_path();
            if (exe.empty())
            {
                return std::nullopt; // 平台查询失败：无法定位同目录
            }
            const std::filesystem::path dll =
                exe.parent_path() / (std::wstring{stem} + L".dll");
            std::error_code ec;
            if (!std::filesystem::is_regular_file(dll, ec) || ec)
            {
                return std::nullopt;
            }
            const auto size = std::filesystem::file_size(dll, ec);
            if (ec || size == 0)
            {
                return std::nullopt; // 零尺寸按缺席处理（半成品占位）
            }
            return dll;
        }

        /// 垫片缺席诊断（英文，进 detail 诊断链）：点名期望的
        /// dll 文件名与找过的可执行文件目录，并给出修复途径（同目录
        /// 直载是唯一装载路线，缺席即整条引擎路线不可用）。降级链的
        /// 多条候选共享同一缺席事实，重复装载只记一次。
        void note_shim_missing(EngineState& state, const wchar_t* stem)
        {
            const std::string dll_name =
                cg::util::platform::narrow_utf8(std::wstring_view{stem})
                + ".dll";
            const std::string tag = "shim " + dll_name + " missing";
            if (state.detail.find(tag) != std::string::npos)
            {
                return;
            }
            const std::filesystem::path dir =
                cg::util::platform::executable_path().parent_path();
            std::string text = tag;
            text += dir.empty()
                        ? "; executable directory unavailable"
                        : "; looked in \""
                              + cg::util::platform::narrow_utf8(
                                    dir.wstring())
                              + "\"";
            text += "; reinstall the published layout or rebuild so the "
                    "shim dll is copied beside the executable";
            if (!state.detail.empty())
            {
                state.detail += "; ";
            }
            state.detail += text;
        }

        // ---------------------------------------------------------------------------
        // hostfxr 路线：候选计划探测。
        // ---------------------------------------------------------------------------

        struct HostfxrPlan
        {
            std::filesystem::path hostfxr;
            std::filesystem::path rtconfig;
            std::filesystem::path pshome; // SMA 全家桶目录（CCG_PSHOME）
            std::string           name;   // 候选名（诊断）
        };

        /// 目录下指定子模式取语义最高版本（版本目录名按数值分量比较）。
        [[nodiscard]]
        auto latest_version_dir(
            const std::filesystem::path& parent,
            const wchar_t* child_prefix) -> std::optional<std::filesystem::path>
        {
            std::error_code ec;
            if (!std::filesystem::exists(parent, ec))
            {
                return std::nullopt;
            }
            std::optional<std::vector<std::uint64_t>> best_key;
            std::filesystem::path                     best;
            for (const auto& entry :
                 std::filesystem::directory_iterator(parent, ec))
            {
                if (ec || !entry.is_directory())
                {
                    continue;
                }
                const std::wstring name = entry.path().filename().wstring();
                if (!name.starts_with(child_prefix))
                {
                    continue;
                }
                // 数值分量键（"10.0.14" → {10,0,14}；非数值段计 0）。
                std::vector<std::uint64_t> key;
                std::uint64_t              number = 0;
                bool                       any = false;
                for (const wchar_t ch : name)
                {
                    if (ch >= L'0' && ch <= L'9')
                    {
                        number =
                            number * 10 + static_cast<std::uint64_t>(ch - L'0');
                        any = true;
                    }
                    else if (ch == L'.' && any)
                    {
                        key.push_back(number);
                        number = 0;
                        any = false;
                    }
                    else if (any)
                    {
                        break;
                    }
                }
                if (any)
                {
                    key.push_back(number);
                }
                if (key.empty())
                {
                    continue;
                }
                if (!best_key.has_value() || key > *best_key)
                {
                    best_key = key;
                    best = entry.path();
                }
            }
            if (best_key.has_value())
            {
                return best;
            }
            return std::nullopt;
        }

        /// 文件 ProductVersion 的前三个数值段（"10.0.14"）。
        [[nodiscard]]
        auto product_version(const std::filesystem::path& file)
            -> std::optional<std::wstring>
        {
            const std::wstring path = file.wstring();
            DWORD              handle = 0;
            DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
            if (size == 0)
            {
                return std::nullopt;
            }
            std::vector<unsigned char> buffer(size);
            if (!GetFileVersionInfoW(path.c_str(), 0, size, buffer.data()))
            {
                return std::nullopt;
            }
            void* fixed_info = nullptr;
            UINT  fixed_len = 0;
            if (!VerQueryValueW(buffer.data(), L"\\", &fixed_info, &fixed_len)
                || fixed_len < sizeof(VS_FIXEDFILEINFO))
            {
                return std::nullopt;
            }
            const auto* info = static_cast<const VS_FIXEDFILEINFO*>(fixed_info);
            const DWORD major = info->dwFileVersionMS >> 16;
            const DWORD minor = info->dwFileVersionMS & 0xFFFF;
            const DWORD build = info->dwFileVersionLS >> 16;
            return std::format(L"{}.{}.{}", major, minor, build);
        }

        /// 框架依赖形态的 runtimeconfig 内容（探测到的版本写入 framework）。
        [[nodiscard]]
        auto rtconfig_text(const std::wstring& framework_version) -> std::string
        {
            const std::string version{
                framework_version.begin(),
                framework_version.end()};
            const std::string major = version.substr(0, version.find('.'));
            return std::format(
                "{{\"runtimeOptions\":{{\"tfm\":\"net{}.0\",\"framework\":"
                "{{\"name\":\"Microsoft.NETCore.App\",\"version\":\"{}\"}}}}}}",
                major,
                version);
        }

        [[nodiscard]]
        auto write_text_file(
            const std::filesystem::path& path,
            std::string_view             content) -> bool
        {
            HANDLE file = CreateFileW(
                path.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            DWORD      written = 0;
            const BOOL ok = WriteFile(
                file,
                content.data(),
                static_cast<DWORD>(content.size()),
                &written,
                nullptr);
            CloseHandle(file);
            return ok && written == content.size();
        }

        /// 从一个 dotnet 根（host/fxr + shared 布局）构造候选计划。
        [[nodiscard]]
        auto plan_from_dotnet_root(
            const std::filesystem::path& dotnet_root,
            const wchar_t* candidate_name) -> std::optional<HostfxrPlan>
        {
            const auto fxr =
                latest_version_dir(dotnet_root / L"host" / L"fxr", L"");
            if (!fxr.has_value()
                || !std::filesystem::exists(*fxr / L"hostfxr.dll"))
            {
                return std::nullopt;
            }
            const auto shared = latest_version_dir(
                dotnet_root / L"shared" / L"Microsoft.NETCore.App",
                L"");
            if (!shared.has_value())
            {
                return std::nullopt;
            }
            // SMA 全家桶：pwsh 安装目录（7 的 SMA 不在 dotnet 目录里）。
            const std::filesystem::path pshome = program_files_sub(
                L"PowerShell"
                L"\\7");
            if (pshome.empty()
                || !std::filesystem::exists(
                    pshome / L"System.Management.Automation.dll"))
            {
                return std::nullopt;
            }
            HostfxrPlan plan;
            plan.hostfxr = *fxr / L"hostfxr.dll";
            plan.pshome = pshome;
            plan.rtconfig =
                cache_root()
                / (std::wstring{candidate_name} + L"-rtconfig.json");
            std::error_code ec;
            std::filesystem::create_directories(cache_root(), ec);
            if (!write_text_file(
                    plan.rtconfig,
                    rtconfig_text(shared->filename().wstring())))
            {
                return std::nullopt;
            }
            plan.name = "hostfxr-";
            for (const wchar_t ch : std::wstring_view{candidate_name})
            {
                plan.name.push_back(static_cast<char>(ch)); // 恒为 ASCII 标识
            }
            return plan;
        }

        /// 商店版 pwsh 执行别名的 reparse 解析（APPEXECLINK 的第二串是包
        /// 目录完整路径；格式无官方文档，按 NUL 分段取第一段含盘符路径）。
        [[nodiscard]]
        auto resolve_appexec_link(const std::filesystem::path& alias)
            -> std::optional<std::filesystem::path>
        {
            HANDLE file = CreateFileW(
                alias.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return std::nullopt;
            }
            std::vector<unsigned char> buffer(
                4096 + offsetof(CcgReparseDataBuffer, GenericReparseBuffer));
            DWORD      returned = 0;
            const BOOL ok = DeviceIoControl(
                file,
                FSCTL_GET_REPARSE_POINT,
                nullptr,
                0,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &returned,
                nullptr);
            CloseHandle(file);
            if (!ok || returned < sizeof(CcgReparseDataBuffer))
            {
                return std::nullopt;
            }
            const auto* reparse =
                reinterpret_cast<const CcgReparseDataBuffer*>(buffer.data());
            const wchar_t* data = reinterpret_cast<const wchar_t*>(
                reparse->GenericReparseBuffer.DataBuffer);
            const std::size_t data_chars =
                (returned
                 - offsetof(CcgReparseDataBuffer, GenericReparseBuffer))
                / sizeof(wchar_t);
            // 按 NUL 分段；找第一段形如盘符加反斜杠开头的完整路径。
            std::size_t i = 0;
            while (i < data_chars)
            {
                std::size_t end = i;
                while (end < data_chars && data[end] != L'\0')
                {
                    ++end;
                }
                const std::wstring_view segment{data + i, end - i};
                if (segment.size() >= 3 && segment[1] == L':'
                    && segment[2] == L'\\')
                {
                    return std::filesystem::path{segment};
                }
                i = end + 1;
            }
            return std::nullopt;
        }

        /// 两文件时间戳比对（商店版自刷新的核心探测，微秒级）。
        [[nodiscard]]
        auto same_mtime(
            const std::filesystem::path& a,
            const std::filesystem::path& b) -> bool
        {
            WIN32_FILE_ATTRIBUTE_DATA aa{};
            WIN32_FILE_ATTRIBUTE_DATA bb{};
            if (!GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &aa)
                || !GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &bb))
            {
                return false;
            }
            return aa.ftLastWriteTime.dwLowDateTime
                       == bb.ftLastWriteTime.dwLowDateTime
                   && aa.ftLastWriteTime.dwHighDateTime
                          == bb.ftLastWriteTime.dwHighDateTime;
        }

        /// 把源的 LastWriteTime 抄给目标（镜像回退路径专用）。
        ///
        /// copy_file 不保留修改时间，而商店版复制路线的新鲜度判据是
        /// 源/缓存两个标志性文件的时间戳比对——回退
        /// 拷贝若不带时间戳，判据恒假，每次引擎使用都会整目录重摆（秒级
        /// 千文件 IO，且重摆质量波动会漏注入）。硬链接天然共享时间戳，
        /// 无需此步。
        void copy_mtime(
            const std::filesystem::path& src,
            const std::filesystem::path& dst)
        {
            WIN32_FILE_ATTRIBUTE_DATA from{};
            if (!GetFileAttributesExW(
                    src.c_str(),
                    GetFileExInfoStandard,
                    &from))
            {
                return;
            }
            const HANDLE file = CreateFileW(
                dst.c_str(),
                FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return;
            }
            SetFileTime(file, nullptr, nullptr, &from.ftLastWriteTime);
            CloseHandle(file);
        }

        /// 递归物化目录树：优先硬链接（同卷，省 284MB 级复制），失败回退
        /// 拷贝（拷贝必须抄回源时间戳，理由见 copy_mtime）。
        void mirror_tree(
            const std::filesystem::path& src,
            const std::filesystem::path& dst)
        {
            std::error_code ec;
            std::filesystem::create_directories(dst, ec);
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(src, ec))
            {
                if (ec)
                {
                    return;
                }
                const auto target =
                    dst / std::filesystem::relative(entry.path(), src, ec);
                if (ec)
                {
                    return;
                }
                if (entry.is_directory())
                {
                    std::filesystem::create_directories(target, ec);
                }
                else if (entry.is_regular_file())
                {
                    std::filesystem::create_hard_link(target, entry.path(), ec);
                    if (ec)
                    {
                        ec.clear();
                        std::filesystem::copy_file(
                            entry.path(),
                            target,
                            std::filesystem::copy_options::overwrite_existing,
                            ec);
                        copy_mtime(entry.path(), target);
                    }
                }
            }
        }

        /// 候选三：商店版 pwsh 复制布局（含时间戳自刷新）。
        [[nodiscard]]
        auto plan_store_copy() -> std::optional<HostfxrPlan>
        {
            const auto alias =
                local_app_data() / L"Microsoft" / L"WindowsApps" / L"pwsh.exe";
            auto linked = resolve_appexec_link(alias);
            if (!linked.has_value())
            {
                return std::nullopt;
            }
            // reparse 数据给出的是 pwsh.exe 的完整文件路径（实测段 3），
            // 布局探测以包目录为源。
            const std::filesystem::path src = linked->parent_path();
            if (!std::filesystem::exists(src / L"pwsh.exe")
                || !std::filesystem::exists(src / L"hostfxr.dll"))
            {
                return std::nullopt;
            }
            const std::filesystem::path store = cache_root() / L"store";
            const std::filesystem::path pwsh_copy = store / L"pwsh";
            const std::filesystem::path ver_file = store / L"ver.txt";

            // 自刷新判定：两个标志性文件时间戳与源一致且布局完整即新鲜。
            std::optional<std::wstring> ver;
            {
                std::error_code ec;
                if (std::filesystem::exists(ver_file, ec))
                {
                    std::ifstream in{ver_file, std::ios::binary};
                    std::string   narrow(
                        (std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>{});
                    if (!narrow.empty())
                    {
                        ver = std::wstring{narrow.begin(), narrow.end()};
                    }
                }
            }
            const bool fresh =
                ver.has_value()
                && same_mtime(src / L"pwsh.exe", pwsh_copy / L"pwsh.exe")
                && same_mtime(src / L"hostfxr.dll", pwsh_copy / L"hostfxr.dll")
                && std::filesystem::exists(
                    store / L"dotnet" / L"host" / L"fxr" / *ver
                    / L"hostfxr.dll");

            if (!fresh)
            {
                // Store 静默升级导致时间戳漂移：整目录重摆，绝不用陈旧引擎。
                std::error_code ec;
                std::filesystem::remove_all(store, ec);
                mirror_tree(src, pwsh_copy);
                const auto raw = product_version(pwsh_copy / L"hostfxr.dll");
                if (!raw.has_value())
                {
                    return std::nullopt;
                }
                ver = *raw;
                const auto fxr_dir =
                    store / L"dotnet" / L"host" / L"fxr" / *ver;
                const auto shared_dir = store / L"dotnet" / L"shared"
                                        / L"Microsoft.NETCore.App" / *ver;
                std::filesystem::create_directories(fxr_dir, ec);
                std::filesystem::copy_file(
                    pwsh_copy / L"hostfxr.dll",
                    fxr_dir / L"hostfxr.dll",
                    ec);
                mirror_tree(pwsh_copy, shared_dir);
                // 两把身份钥匙（沙盒实证：缺任一 hostfxr 拒启动框架）。
                static_cast<void>(write_text_file(
                    shared_dir / L"Microsoft.NETCore.App.runtimeconfig.json",
                    rtconfig_text(*ver)));
                const std::filesystem::path deps =
                    shared_dir / L"Microsoft.NETCore.App.deps.json";
                bool deps_ok = false;
                // 优先从任何系统 dotnet 框架目录借身份文件（沙盒验证形态）。
                if (const auto system_shared = latest_version_dir(
                        program_files_sub(
                            L"dotnet"
                            L"\\shared")
                            / L"Microsoft.NETCore.App",
                        L"");
                    system_shared.has_value())
                {
                    std::error_code copy_ec;
                    std::filesystem::copy_file(
                        *system_shared / L"Microsoft.NETCore.App.deps.json",
                        deps,
                        std::filesystem::copy_options::overwrite_existing,
                        copy_ec);
                    deps_ok = !copy_ec;
                }
                if (!deps_ok)
                {
                    // 借不到时的最小身份骨架（未沙盒实证，初始化失败即自然
                    // 降级到 5.1，方向安全）。
                    deps_ok = write_text_file(
                        deps,
                        std::format(
                            "{{\"runtimeTarget\":{{\"name\":"
                            "\".NETCoreApp,Version=v{0}\"}},\"targets\":"
                            "{{\".NETCoreApp,Version=v{0}\":{{}}}}}}",
                            std::string{ver->begin(), ver->end()}));
                }
                if (!deps_ok)
                {
                    return std::nullopt;
                }
                const std::string narrow_ver{ver->begin(), ver->end()};
                static_cast<void>(write_text_file(ver_file, narrow_ver));
                static_cast<void>(write_text_file(
                    store / L"rtconfig.json",
                    rtconfig_text(*ver)));
            }

            HostfxrPlan plan;
            plan.hostfxr =
                store / L"dotnet" / L"host" / L"fxr" / *ver / L"hostfxr.dll";
            plan.rtconfig = store / L"rtconfig.json";
            plan.pshome = pwsh_copy;
            plan.name = "hostfxr-store";
            return plan;
        }

        // ---------------------------------------------------------------------------
        // 初始化与通道。
        // ---------------------------------------------------------------------------

        /// hostfxr 候选初始化（成功后 EngineState 持对象协议三件的
        /// 函数指针）。
        [[nodiscard]]
        auto init_hostfxr(
            EngineState&       state,
            const HostfxrPlan& plan,
            Route              route) -> bool
        {
            if (state.create7 != nullptr && state.call7 != nullptr
                && state.destroy7 != nullptr)
            {
                // 已装委托复用：hostfxr 的运行时初始化是进程级一次
                // （同进程二次 hostfxr_initialize_for_runtime_config 必
                // 失败），而委托指向已加载程序集的代码、context 关闭后
                // 仍有效——reset_for_tests 的"引擎句柄不卸载、只重走
                // 记账"语义靠复用兑现，否则复位后重走降级链会假性
                // miss 并静默降级 5.1。
                state.route = route;
                return true;
            }
            if (std::error_code ec; !std::filesystem::exists(plan.hostfxr, ec))
            {
                return false;
            }
            const auto shim = bundled_shim(L"ccg-cshost7");
            if (!shim.has_value())
            {
                note_shim_missing(state, L"ccg-cshost7");
                return false;
            }
            HMODULE lib = LoadLibraryW(plan.hostfxr.c_str());
            if (lib == nullptr)
            {
                return false;
            }
            const auto init = reinterpret_cast<HxInitFn>(
                GetProcAddress(lib, "hostfxr_initialize_for_runtime_config"));
            const auto get_delegate = reinterpret_cast<HxDelegateFn>(
                GetProcAddress(lib, "hostfxr_get_runtime_delegate"));
            const auto close = reinterpret_cast<HxCloseFn>(
                GetProcAddress(lib, "hostfxr_close"));
            if (init == nullptr || get_delegate == nullptr || close == nullptr)
            {
                return false;
            }
            // shim 的 AssemblyResolve 按 CCG_PSHOME 找 SMA 全家桶。
            SetEnvironmentVariableW(L"CCG_PSHOME", plan.pshome.c_str());

            HxHandle context = nullptr;
            if (init(plan.rtconfig.c_str(), nullptr, &context) != 0
                || context == nullptr)
            {
                return false;
            }
            void* loader = nullptr;
            if (get_delegate(
                    context,
                    HDT_LOAD_ASSEMBLY_AND_GET_FUNCTION_POINTER,
                    &loader)
                    != 0
                || loader == nullptr)
            {
                close(context);
                return false;
            }
            const auto load = reinterpret_cast<HxLoadAssemblyFn>(loader);
            // 对象协议三件逐一取 [UnmanagedCallersOnly] 原生指针；三件
            // 必须齐活才算该线可用（残缺线不可分派）。
            void* create  = nullptr;
            void* call    = nullptr;
            void* destroy = nullptr;
            if (load(
                    shim->c_str(),
                    L"CcgCsHost7.NativeEntry, ccg-cshost7",
                    L"ObjCreate",
                    unmanaged_callers_only_marker(),
                    nullptr,
                    &create)
                    != 0
                || create == nullptr
                || load(
                       shim->c_str(),
                       L"CcgCsHost7.NativeEntry, ccg-cshost7",
                       L"ObjCall",
                       unmanaged_callers_only_marker(),
                       nullptr,
                       &call)
                       != 0
                || call == nullptr
                || load(
                       shim->c_str(),
                       L"CcgCsHost7.NativeEntry, ccg-cshost7",
                       L"ObjDestroy",
                       unmanaged_callers_only_marker(),
                       nullptr,
                       &destroy)
                       != 0
                || destroy == nullptr)
            {
                close(context);
                return false;
            }
            close(context); // 委托已取出，上下文可关（hostfxr 契约）
            state.create7 = reinterpret_cast<ObjFn7>(create);
            state.call7 = reinterpret_cast<ObjFn7>(call);
            state.destroy7 = reinterpret_cast<ObjFn7>(destroy);
            state.route = route;
            return true;
        }

        /// 5.1 主路径初始化（mscoree → CLR 4.0 → GAC 引擎）。
        ///
        /// 各步失败都把含错误码数值的原因追加进 detail 链（与
        /// note_shim_missing 同款追加记法）：求值面钉此线，失败即整面
        /// 不可用，无降级链兜底，诊断必须能指认卡在哪一步。
        [[nodiscard]]
        auto init_clr51(EngineState& state) -> bool
        {
            const auto note = [&state](const std::string& text)
            {
                if (!state.detail.empty())
                {
                    state.detail += "; ";
                }
                state.detail += text;
            };
            const auto shim = bundled_shim(L"ccg-cshost51");
            if (!shim.has_value())
            {
                note_shim_missing(state, L"ccg-cshost51");
                return false;
            }
            HMODULE mscoree = LoadLibraryW(L"mscoree.dll");
            if (mscoree == nullptr)
            {
                const DWORD gle = GetLastError();
                note(std::format(
                    "clr51: LoadLibraryW(mscoree.dll) failed "
                    "(GetLastError={})",
                    static_cast<unsigned>(gle)));
                return false;
            }
            using CreateInstanceFn =
                HRESULT(__stdcall*)(REFCLSID, REFIID, LPVOID*);
            const auto create_instance = reinterpret_cast<CreateInstanceFn>(
                GetProcAddress(mscoree, "CLRCreateInstance"));
            if (create_instance == nullptr)
            {
                const DWORD gle = GetLastError();
                note(std::format(
                    "clr51: CLRCreateInstance export missing "
                    "(GetLastError={})",
                    static_cast<unsigned>(gle)));
                return false;
            }
            IMyICLRMetaHost* meta = nullptr;
            const HRESULT hr_meta = create_instance(
                CLSID_CLR_META_HOST,
                IID_CLR_META_HOST,
                reinterpret_cast<LPVOID*>(&meta));
            if (FAILED(hr_meta))
            {
                note(std::format(
                    "clr51: CLRCreateInstance failed (hr=0x{:08x})",
                    static_cast<unsigned>(hr_meta)));
                return false;
            }
            IMyICLRRuntimeInfo* info = nullptr;
            const HRESULT hr_runtime = meta->GetRuntime(
                L"v4.0.30319",
                IID_CLR_RUNTIME_INFO,
                reinterpret_cast<LPVOID*>(&info));
            if (FAILED(hr_runtime))
            {
                note(std::format(
                    "clr51: GetRuntime(v4.0.30319) failed (hr=0x{:08x})",
                    static_cast<unsigned>(hr_runtime)));
                return false;
            }
            IMyICLRRuntimeHost* host = nullptr;
            const HRESULT hr_host = info->GetInterface(
                CLSID_CLR_RUNTIME_HOST,
                IID_CLR_RUNTIME_HOST,
                reinterpret_cast<LPVOID*>(&host));
            if (FAILED(hr_host))
            {
                note(std::format(
                    "clr51: GetInterface(runtime host) failed "
                    "(hr=0x{:08x})",
                    static_cast<unsigned>(hr_host)));
                return false;
            }
            const HRESULT hr_start = host->Start();
            if (FAILED(hr_start))
            {
                note(std::format(
                    "clr51: ICLRRuntimeHost::Start failed (hr=0x{:08x})",
                    static_cast<unsigned>(hr_start)));
                return false;
            }
            state.clr = host;
            state.shim51_path = shim->wstring();
            state.route = Route::Clr51;
            return true;
        }

        /// 测试钩子：CCG_PSHOST_FORBID 逗号列表禁用指定路线（降级链注入）。
        /// 环境变量读取经仓内单点封装（MSVC 弃用警告在封装内规避）。
        [[nodiscard]]
        auto forbid_list() -> std::string
        {
            const auto value = cg::util::cstd::get_env("CCG_PSHOST_FORBID");
            return value.has_value() ? std::move(*value) : std::string{};
        }

        [[nodiscard]]
        auto route_forbidden(const char* name) -> bool
        {
            const std::string list = forbid_list();
            if (list.empty())
            {
                return false;
            }
            std::string_view items{list};
            if (items == "all")
            {
                return true;
            }
            std::size_t pos = 0;
            while (pos <= items.size())
            {
                const std::size_t      comma = items.find(',', pos);
                const std::string_view token = items.substr(
                    pos,
                    comma == std::string_view::npos ? std::string_view::npos
                                                    : comma - pos);
                if (token == name || token == "all")
                {
                    return true;
                }
                if (comma == std::string_view::npos)
                {
                    break;
                }
                pos = comma + 1;
            }
            return false;
        }

        /// 一次性解析降级链（装机版→独立 dotnet→商店版复制→5.1）。
        void resolve_chain(EngineState& state)
        {
            state.resolved = true;
            state.route = Route::None;
            state.detail.clear();
            const auto note = [&state](const std::string& text)
            {
                if (!state.detail.empty())
                {
                    state.detail += "; ";
                }
                state.detail += text;
            };
            const bool hostfxr_all = route_forbidden("hostfxr");
            if (!hostfxr_all && !route_forbidden("hostfxr-install"))
            {
                if (const auto plan = plan_from_dotnet_root(
                        program_files_sub(L"dotnet"),
                        L"install");
                    plan.has_value()
                    && init_hostfxr(state, *plan, Route::HostfxrInstall))
                {
                    note("install:ok");
                    return;
                }
                note("install:miss");
            }
            if (!hostfxr_all && !route_forbidden("hostfxr-standalone"))
            {
                const wchar_t* roots[] = {L"DOTNET_ROOT", L"DOTNET_ROOT_64"};
                for (const wchar_t* var : roots)
                {
                    const auto root = env_var(var);
                    if (!root.has_value())
                    {
                        continue;
                    }
                    if (const auto plan = plan_from_dotnet_root(
                            std::filesystem::path{*root},
                            L"standalone");
                        plan.has_value()
                        && init_hostfxr(state, *plan, Route::HostfxrStandalone))
                    {
                        note("standalone:ok");
                        return;
                    }
                }
                note("standalone:miss");
            }
            if (!hostfxr_all && !route_forbidden("hostfxr-store"))
            {
                if (const auto plan = plan_store_copy();
                    plan.has_value()
                    && init_hostfxr(state, *plan, Route::HostfxrStoreCopy))
                {
                    note("store:ok");
                    return;
                }
                note("store:miss");
            }
            if (!route_forbidden("clr51"))
            {
                if (init_clr51(state))
                {
                    note("clr51:ok");
                    return;
                }
                note("clr51:miss");
            }
            note("none");
        }

        // ---------------------------------------------------------------------------
        // 51 线共享内存通道：命令区进（UTF-16 JSON）、结果区出（UTF-8）。
        // ---------------------------------------------------------------------------

        /**
         * @brief 51 线带载荷通道（ObjCreate/ObjCall）。
         *
         * 每调用动态建命名映射（Local\ccgcs_<pid>_<seq>，进程内瞬态命名
         * 对象）；垫片按视图容量自证档位读取。rc 语义：0 成功（ObjCreate
         * 时正数 token 也经 rc 回传）、负数为垫片协议码或宿主侧通道码。
         */
        auto invoke_51(
            EngineState&           state,
            const ChannelLayout&   layout,
            const wchar_t*         method,
            std::wstring_view      command_wide) -> ObjOutcome
        {
            if (command_wide.size() > layout.cmd_cap)
            {
                return ObjOutcome{
                    .rc = static_cast<std::int32_t>(ObjErr::CmdTooLarge)};
            }
            const std::uint64_t seq = state.map_seq.fetch_add(1);
            const std::wstring  map_name = std::format(
                L"Local\\ccgcs_{}_{}",
                GetCurrentProcessId(),
                seq);
            HANDLE map = CreateFileMappingW(
                INVALID_HANDLE_VALUE,
                nullptr,
                PAGE_READWRITE,
                0,
                static_cast<DWORD>(layout.view_bytes),
                map_name.c_str());
            if (map == nullptr)
            {
                return ObjOutcome{
                    .rc = static_cast<std::int32_t>(ObjErr::Channel)};
            }
            void* view = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            if (view == nullptr)
            {
                CloseHandle(map);
                return ObjOutcome{
                    .rc = static_cast<std::int32_t>(ObjErr::Channel)};
            }
            ObjOutcome outcome;
            *static_cast<std::int32_t*>(view) =
                static_cast<std::int32_t>(command_wide.size());
            if (!command_wide.empty())
            {
                memcpy(
                    static_cast<char*>(view) + CMD_OFF,
                    command_wide.data(),
                    command_wide.size() * sizeof(wchar_t));
            }
            DWORD         ret = 0xFFFFFFFF;
            const HRESULT hr = state.clr->ExecuteInDefaultAppDomain(
                state.shim51_path.c_str(),
                L"CcgCsHost.Entry",
                method,
                map_name.c_str(),
                &ret);
            if (SUCCEEDED(hr))
            {
                outcome.rc = static_cast<std::int32_t>(ret);
                // 结果区在成功与失败两种形态都有产出（失败为诊断文本，
                // 溢出路径垫片不写头部、len 保持 0）。
                const std::int32_t len =
                    *(reinterpret_cast<const std::int32_t*>(view) + 1);
                if (len >= 0
                    && static_cast<std::size_t>(len) <= layout.res_cap)
                {
                    const char* begin =
                        static_cast<const char*>(view) + layout.res_off;
                    outcome.payload.assign(begin, static_cast<std::size_t>(len));
                }
                else if (len > 0)
                {
                    outcome.rc = static_cast<std::int32_t>(ObjErr::Channel);
                }
            }
            else
            {
                outcome.rc = static_cast<std::int32_t>(ObjErr::Channel);
            }
            UnmapViewOfFile(view);
            CloseHandle(map);
            return outcome;
        }

        /**
         * @brief 51 线拆除通道（ObjDestroy 的参数是 token 十进制串，
         *         直接作 ExecuteInDefaultAppDomain 实参，不经共享内存）。
         */
        auto invoke_destroy_51(EngineState& state, std::int32_t token)
            -> std::int32_t
        {
            const std::wstring token_text = std::format(L"{}", token);
            DWORD              ret = 0xFFFFFFFF;
            const HRESULT      hr = state.clr->ExecuteInDefaultAppDomain(
                state.shim51_path.c_str(),
                L"CcgCsHost.Entry",
                L"ObjDestroy",
                token_text.c_str(),
                &ret);
            if (FAILED(hr))
            {
                return static_cast<std::int32_t>(ObjErr::Channel);
            }
            return static_cast<std::int32_t>(ret);
        }

        /**
         * @brief 7 线结构体桥通道：载荷指针 + 字节数进出，出参槽就地
         *        写回结果指针（宿主读后 CoTaskMemFree）。
         */
        auto invoke_7(ObjFn7 fn, std::string_view payload_utf8) -> ObjOutcome
        {
            NativeCallV1 call{};
            call.size = sizeof(NativeCallV1);
            call.abi_version = kAbiVersion7;
            call.cmd = const_cast<char*>(payload_utf8.data());
            call.cmd_len = payload_utf8.size();
            ObjOutcome outcome;
            outcome.rc = fn(&call, static_cast<int>(sizeof(NativeCallV1)));
            if (call.out_slot != nullptr)
            {
                outcome.payload.assign(
                    static_cast<const char*>(call.out_slot));
                CoTaskMemFree(call.out_slot);
            }
            return outcome;
        }

        /// 面到状态实例的映射（两面独立记账与择线）。
        auto face_state(Face face) -> EngineState&
        {
            return face == Face::Evaluation ? eval_state() : shell_state();
        }

        /**
         * @brief 面的一次性择线（持锁解析；通道调用不持锁——垫片句柄表
         *        自带并发安全，51 线映射名经原子序号天然不冲突）。
         *
         * @return 择线成功（route 非 None）时返回状态实例；全链失败返回
         *         nullptr（诊断已进该面的 detail 链）。
         */
        auto ensure_face(Face face) -> EngineState*
        {
            EngineState&           state = face_state(face);
            const std::scoped_lock lock{state.mutex};
            if (!state.resolved)
            {
                if (face == Face::Evaluation)
                {
                    // 求值面钉 51 线：不走降级链（net472 程序集只有 CLR4
                    // 家族装得下）；clr51 的 FORBID 注入同获尊重（测试
                    // 可借此模拟 51 线缺席的装载失败形态）。
                    state.resolved = true;
                    state.route = Route::None;
                    state.detail.clear();
                    // 结局短码追加而非整体覆盖：init_clr51 失败时
                    // note_shim_missing 已把缺席指引写进 detail 链，
                    // 覆盖会让用户只剩一句无行动信息的短码（记法与
                    // resolve_chain 的 note 对齐）。
                    const auto note = [&state](const std::string& text)
                    {
                        if (!state.detail.empty())
                        {
                            state.detail += "; ";
                        }
                        state.detail += text;
                    };
                    if (!route_forbidden("clr51") && init_clr51(state))
                    {
                        note("clr51:ok");
                    }
                    else
                    {
                        note("clr51:miss");
                    }
                }
                else
                {
                    resolve_chain(state);
                }
            }
            return state.route == Route::None ? nullptr : &state;
        }

        /// 对象类到垫片侧类名（ObjCreate 构造 JSON 的 "class" 值）。
        [[nodiscard]]
        constexpr auto class_name_of(ObjClass cls) -> std::string_view
        {
            return cls == ObjClass::SelectStringCore ? "SelectStringCore"
                                                     : "MsbuildEval";
        }

        /// ObjCreate 构造 JSON（经 json 门面构造；MsbuildEval 附程序集
        /// 目录的 UTF-8 形态——垫片反序列化还原为 UTF-16 串）。
        [[nodiscard]]
        auto build_create_payload(ObjClass cls, std::wstring_view assembly_dir)
            -> std::string
        {
            cg::util::json::Object req;
            req.set("class", class_name_of(cls));
            if (cls == ObjClass::MsbuildEval)
            {
                req.set(
                    "assembly_dir",
                    cg::util::platform::narrow_utf8(assembly_dir));
            }
            return cg::util::json::stringify(std::move(req));
        }

        /// ObjCall 命令 JSON（句柄 + 方法名 + 命名实参表）。
        [[nodiscard]]
        auto build_call_payload(
            std::int32_t             token,
            std::string_view         method,
            std::span<const ObjArg>  args) -> std::string
        {
            cg::util::json::Object argv;
            for (const ObjArg& arg : args)
            {
                argv.set(arg.name, arg.value);
            }
            cg::util::json::Object req;
            req.set("handle", token);
            req.set("method", method);
            req.set("args", std::move(argv));
            return cg::util::json::stringify(std::move(req));
        }

        /// 句柄类对应的 51 线通道档位（小档 64KB / 大档 4MB）。
        [[nodiscard]]
        constexpr auto layout_of(ObjClass cls) -> const ChannelLayout&
        {
            return cls == ObjClass::MsbuildEval ? large_view : small_view;
        }

    } // namespace

    auto obj_create(
        Face              face,
        ObjClass          cls,
        std::wstring_view assembly_dir) -> std::int32_t
    {
        EngineState* state = ensure_face(face);
        if (state == nullptr)
        {
            return static_cast<std::int32_t>(ObjErr::LineUnavailable);
        }
        const std::string payload = build_create_payload(cls, assembly_dir);
        ObjOutcome        outcome;
        if (state->route == Route::Clr51)
        {
            outcome = invoke_51(
                *state,
                small_view, // 构造载荷短（类名 + 目录），恒小档
                L"ObjCreate",
                cg::util::platform::widen_utf8(payload));
        }
        else
        {
            outcome = invoke_7(state->create7, payload);
        }
        if (outcome.rc > 0)
        {
            return outcome.rc; // 正数即句柄 token
        }
        return outcome.rc == 0
                   ? static_cast<std::int32_t>(ObjErr::Channel)
                   : outcome.rc;
    }

    auto obj_call(
        Face                     face,
        ObjClass                 cls,
        std::int32_t             token,
        std::string_view         method,
        std::span<const ObjArg>  args) -> ObjOutcome
    {
        EngineState* state = ensure_face(face);
        if (state == nullptr)
        {
            return ObjOutcome{
                .rc = static_cast<std::int32_t>(ObjErr::LineUnavailable)};
        }
        const std::string payload = build_call_payload(token, method, args);
        if (state->route == Route::Clr51)
        {
            return invoke_51(
                *state,
                layout_of(cls),
                L"ObjCall",
                cg::util::platform::widen_utf8(payload));
        }
        return invoke_7(state->call7, payload);
    }

    auto obj_destroy(Face face, std::int32_t token) -> std::int32_t
    {
        EngineState* state = ensure_face(face);
        if (state == nullptr)
        {
            return static_cast<std::int32_t>(ObjErr::LineUnavailable);
        }
        if (state->route == Route::Clr51)
        {
            return invoke_destroy_51(*state, token);
        }
        const std::string token_text = std::format("{}", token);
        return invoke_7(state->destroy7, token_text).rc;
    }

    auto open_select_string() -> std::optional<CsRef>
    {
        const std::int32_t token =
            obj_create(Face::SelectString, ObjClass::SelectStringCore, {});
        if (token <= 0)
        {
            return std::nullopt;
        }
        HandleBox* box = new HandleBox{};
        box->face = Face::SelectString;
        box->cls = ObjClass::SelectStringCore;
        box->token = token;
        return CsRefFactory::adopt(box);
    }

    auto open_msbuild_eval(std::wstring_view assembly_dir)
        -> std::optional<CsRef>
    {
        const std::int32_t token = obj_create(
            Face::Evaluation,
            ObjClass::MsbuildEval,
            assembly_dir);
        if (token <= 0)
        {
            return std::nullopt;
        }
        HandleBox* box = new HandleBox{};
        box->face = Face::Evaluation;
        box->cls = ObjClass::MsbuildEval;
        box->token = token;
        return CsRefFactory::adopt(box);
    }

    void drop_handle_last(HandleBox* box)
    {
        if (!box->stale.load(std::memory_order_seq_cst))
        {
            // 归零拆除：垫片三件套（条目移除 → OnFinalRelease →
            // GCHandle.Free）。失败（垫片侧已先行失效等）不再有重试
            // 语义——token 已在垫片侧死亡，计数盒就地回收。
            static_cast<void>(obj_destroy(box->face, box->token));
        }
        // 看门狗短路路径（stale 闩）：只弃计数、不拆除——垫片孤儿
        // 线程仍在使用对象，拆除等于对托管对象的 use-after-free；
        // 对象与孤儿线程一起活到进程退出。
        delete box;
    }

    auto parse_via_engine(std::string_view ps_script_utf8)
        -> std::optional<std::string>
    {
        if (!mentions_search_cmdlet(ps_script_utf8))
        {
            return std::nullopt; // 预筛：未提及查找 cmdlet，零成本退场
        }
        // Select-String 面短会话：开句柄 → 单次 ObjCall → RAII 析构
        // 归零拆除（通道成本与历史单函数协议同为共享内存往返级）。
        std::optional<CsRef> session = open_select_string();
        if (!session.has_value())
        {
            return std::nullopt;
        }
        const ObjArg argv[]{{"script", ps_script_utf8}};
        ObjOutcome   outcome = session->call("Extract", argv);
        if (!outcome.ok())
        {
            return std::nullopt; // 通道/方法失败：快速否决（既定口径）
        }
        return std::move(outcome.payload);
    }

    auto last_route() -> Route { return shell_state().route; }

    auto last_detail() -> std::string_view { return shell_state().detail; }

    auto face_detail(Face face) -> std::string_view
    {
        return face_state(face).detail;
    }

    void reset_for_tests()
    {
        for (EngineState* state : {&shell_state(), &eval_state()})
        {
            const std::scoped_lock lock{state->mutex};
            state->resolved = false;
            state->route = Route::None;
            state->detail.clear();
        }
    }

} // namespace cg::util::cshost

// ---------------------------------------------------------------------------
// POSIX 桩：路线不可用（Windows 专属能力）。
// ---------------------------------------------------------------------------
#else

namespace cg::util::cshost
{

    void drop_handle_last(HandleBox* box)
    {
        // 无垫片可拆，只回收计数盒（stale 闩语义同 Windows：不拆除）。
        delete box;
    }

    auto parse_via_engine(std::string_view /*ps_script_utf8*/)
        -> std::optional<std::string>
    {
        return std::nullopt;
    }

    auto last_route() -> Route { return Route::None; }

    auto last_detail() -> std::string_view { return {}; }

    auto face_detail(Face /*face*/) -> std::string_view { return {}; }

    void reset_for_tests() {}

    auto open_select_string() -> std::optional<CsRef>
    {
        return std::nullopt;
    }

    auto open_msbuild_eval(std::wstring_view /*assembly_dir*/)
        -> std::optional<CsRef>
    {
        return std::nullopt;
    }

    auto obj_create(
        Face /*face*/,
        ObjClass /*cls*/,
        std::wstring_view /*assembly_dir*/) -> std::int32_t
    {
        return static_cast<std::int32_t>(ObjErr::LineUnavailable);
    }

    auto obj_call(
        Face /*face*/,
        ObjClass /*cls*/,
        std::int32_t /*token*/,
        std::string_view /*method*/,
        std::span<const ObjArg> /*args*/) -> ObjOutcome
    {
        return ObjOutcome{
            .rc = static_cast<std::int32_t>(ObjErr::LineUnavailable)};
    }

    auto obj_destroy(Face /*face*/, std::int32_t /*token*/) -> std::int32_t
    {
        return static_cast<std::int32_t>(ObjErr::LineUnavailable);
    }

} // namespace cg::util::cshost

#endif // defined(_WIN32)

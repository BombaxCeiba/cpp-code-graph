module;
#if defined(_MSC_VER)
// _dupenv_s / gmtime_s 是微软 CRT 扩展名，不随 import std 的 std 模块
// 可达（实测 MSVC 14.51：C3861 找不到标识符），必须在全局模块片段
// 直接引入。非微软分支只用 std::getenv / std::gmtime（标准名，
// import std 已覆盖），无需任何 include。
#include <cstdlib>
#include <ctime>
#endif

/**
 * @module cg.util.cstd
 *
 * @brief C 标准库平台差异的唯一封装点：get_env / gmtime_utc /
 *        iso8601_utc_timestamp。MSVC 把 getenv / gmtime 家族标记为
 *        C4996 弃用（仅提示，行为不变），其余工具链无此标记。调用方
 *        一律经由本模块，不再直接触碰 std::getenv / std::gmtime。
 *
 * @internal
 * 微软侧走安全变体：_dupenv_s（把值拷进自有缓冲，读毕 free）与
 * gmtime_s；非微软侧保留 std::getenv / std::gmtime 原样（无弃用
 * 告警）。语义统一为值返回：get_env 用 optional 区分“未设置”与
 * “空串”；gmtime_utc 返回 std::tm 值，调用方不再持有指向运行库
 * 静态缓冲的指针。实测（MSVC 14.51）这两个扩展名不随 import std
 * 可达（C3861），故微软分支在全局模块片段回退 include（见文件头）。
 * 与 cg.util.platform 的分工：那边是操作系统探针，这边是 C 运行库差异。
 * 现有站点全部是启动期一次性读取，无线程顾虑。
 */
export module cg.util.cstd;

import std;

export namespace cg::util::cstd
{

    /**
     * @brief 读环境变量；未设置返回空 optional（与设置为空串区分）。
     */
    [[nodiscard]]
    inline auto get_env(const char* name) -> std::optional<std::string>
    {
#ifdef _MSC_VER
        char*       raw = nullptr;
        std::size_t len = 0;
        _dupenv_s(&raw, &len, name);
        if (raw == nullptr)
        {
            return std::nullopt;
        }
        std::string value{raw};
        std::free(raw);
        return value;
#else
        const char* raw = std::getenv(name);
        if (raw == nullptr)
        {
            return std::nullopt;
        }
        return std::string{raw};
#endif
    }

    /**
     * @brief UTC 日历换算（值返回）。输入来自系统时钟恒有效；零初始化
     *        保证任何失败路径下也没有未定义读。
     */
    [[nodiscard]]
    inline auto gmtime_utc(std::time_t moment) -> std::tm
    {
        std::tm parts{};
#ifdef _MSC_VER
        gmtime_s(&parts, &moment);
#else
        parts = *std::gmtime(&moment);
#endif
        return parts;
    }

    /**
     * @brief 当前 UTC 时间戳，"YYYY-MM-DDTHH:MM:SSZ"。
     *
     * MSBuild 注册表的溯源时间戳与 capture 表的行时间戳同形状（本函数
     * 是两者的唯一来源，收敛昔日两份逐字相同的本地实现）；std::format
     * 没有 tm 特化，封装在此而非调用方手拼 tm 字段。
     */
    [[nodiscard]]
    inline auto iso8601_utc_timestamp() -> std::string
    {
        const std::time_t now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        const std::tm parts = gmtime_utc(now);
        return std::format(
            "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
            parts.tm_year + 1900,
            parts.tm_mon + 1,
            parts.tm_mday,
            parts.tm_hour,
            parts.tm_min,
            parts.tm_sec);
    }

    /**
     * @brief 当前 Unix 纪元毫秒（system_clock 口径）。
     *
     * 失败退避、租约算术、审计时间戳与诊断打点共用同一时钟口径——
     * 昔日各模块各写一份同形表达式，收敛于此单点；毫秒精度与
     * std::int64_t 返回和被收敛的各本地副本逐字一致。
     */
    [[nodiscard]]
    inline auto epoch_ms() -> std::int64_t
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

} // namespace cg::util::cstd

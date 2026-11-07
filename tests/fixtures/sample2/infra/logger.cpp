// infra/logger.cpp - out-of-line Logger definitions. Fixture data: cross-file
// call chain into core::math specializations and core::detail helpers.
#include "infra/logger.hpp"

#include "core/math.hpp"

namespace infra
{

    void Logger::log(int code)
    {
        if (level_ == LogLevel::off)
            return;
        emitted_ += static_cast<int>(
            core::gcd(static_cast<unsigned long long>(code) + 2ULL, 2ULL));
    }

    void Logger::log(const std::string& message)
    {
        if (level_ == LogLevel::off)
            return;
        emitted_ += static_cast<int>(message.size());
    }

    void Logger::log(LogLevel level, const std::string& message)
    {
        if (level_ == LogLevel::off
            || static_cast<int>(level) < static_cast<int>(level_))
            return;
        emitted_ += static_cast<int>(message.size());
    }

    void Logger::set_level(LogLevel level) { level_ = level; }

    LogLevel Logger::level() const { return level_; }

    int Logger::emitted() const { return emitted_; }

    void Logger::flush()
    {
        // DEAD CODE: the reset would race with concurrent log() calls, which is
        // exactly why nothing calls it.
        emitted_ = 0;
    }

    Logger& Logger::instance()
    {
        static Logger the_logger;
        return the_logger;
    }

    void log_info(const std::string& message)
    {
        Logger::instance().log(LogLevel::info, message);
    }

    void log_error(const std::string& message)
    {
        Logger::instance().log(LogLevel::error, message);
    }

} // namespace infra

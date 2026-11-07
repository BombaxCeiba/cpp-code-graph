// infra/logger.hpp - leveled logging. Fixture data: singleton accessor,
// method overload set, virtual-free static dispatch, dead flush().
#ifndef SAMPLE2_INFRA_LOGGER_HPP
#define SAMPLE2_INFRA_LOGGER_HPP

#include <string>

#include "core/types.hpp"

namespace infra {

enum class LogLevel {
    off = 0,
    info = 1,
    warn = 2,
    error = 3,
};

class Logger {
public:
    // Overload set on log(): plain code, message, leveled message.
    void log(int code);
    void log(const std::string& message);
    void log(LogLevel level, const std::string& message);

    void set_level(LogLevel level);
    LogLevel level() const;

    int emitted() const;

    // DEAD METHOD: declared and defined, never called.
    void flush();

    static Logger& instance();

private:
    Logger() = default;
    LogLevel level_ = LogLevel::info;
    int emitted_ = 0;
};

// Convenience free functions routing into the singleton.
void log_info(const std::string& message);
void log_error(const std::string& message);

}  // namespace infra

#endif  // SAMPLE2_INFRA_LOGGER_HPP

#pragma once
// Exercises: project defines (DEMO_DEFINE), _DEBUG branch per configuration.
#include <string>

namespace demo {

#ifdef _DEBUG
inline const char* build_mode() { return "debug"; }
#else
inline const char* build_mode() { return "release"; }
#endif

#if DEMO_DEFINE != 42
#error "DEMO_DEFINE not propagated by the build"
#endif

inline std::string banner() {
    return std::string("msbdemo [") + build_mode() + "] define=" +
           std::to_string(DEMO_DEFINE);
}

}  // namespace demo

// Reachable ONLY through the "$(ProjectDir)inc2" IncludePath entry - the
// absolute-with-doubled-separator form the captured INCLUDE value carries.
// Also pulls a toolchain header so the fixture proves the MSVC STL keeps
// resolving through clang-cl's own toolchain discovery.
#pragma once

#include <cstddef>

namespace envmath {

constexpr int env_extra = 7;

std::size_t env_deep(std::size_t value);

}  // namespace envmath

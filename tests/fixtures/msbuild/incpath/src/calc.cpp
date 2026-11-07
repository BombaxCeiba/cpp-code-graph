// Both project headers arrive only via the INCLUDE environment dirs; the
// toolchain headers must keep resolving through clang-cl's self-configured
// MSVC roots (the IncludePath classification feeds ONLY the project dirs).
#include <cstddef>

#include "envextra.hpp"
#include "envmath.hpp"

namespace envmath
{

    int env_scale(int value) { return value * 3 + env_extra; }

    int env_offset() { return env_extra; }

    std::size_t env_deep(std::size_t value) { return value + 2; }

} // namespace envmath

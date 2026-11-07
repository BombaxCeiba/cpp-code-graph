// Quoted includes of files that live only in the INCLUDE-dir headers:
// resolved through the environment-delivered search dirs, not /I, not the
// source directory.
#include "envextra.hpp"
#include "envmath.hpp"

namespace envpath
{

    int demo_answer() { return envmath::env_scale(12); }

} // namespace envpath

int main() { return envpath::demo_answer() > 0 ? 0 : 1; }

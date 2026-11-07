// Config-specific probe: each
// configuration compiles exactly ONE of the two definitions, so per-db
// config isolation is a property the build itself proves (the ccg gate
// asserts debug_only_probe lands only in the Debug db and
// release_only_probe only in the Release db).
#include <string>

namespace demo
{

#ifdef _DEBUG
    std::string debug_only_probe() { return "debug"; }
#else
    std::string release_only_probe() { return "release"; }
#endif

} // namespace demo

// Per-TU _MSC_VER probe: projB is pinned to toolset family 14.44 while the
// wrapped real_cl is the highest toolset. Only the project's
// INCLUDE reveals 14.44, so the replay must use
// -fms-compatibility-version=19.44.x (_MSC_VER 1944).
#include <cstddef>
#if _MSC_VER != 1944
#error CCG_MSC_VER_MISMATCH_PROJB
#endif
int projB_probe() { return 1944; }
int main() { return projB_probe() == 1944 ? 0 : 1; }

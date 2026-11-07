// Per-TU _MSC_VER probe: projA is pinned to toolset family 14.51, so the
// replay must carry -fms-compatibility-version=19.51.x (_MSC_VER 1951).
#include <cstddef>
#if _MSC_VER != 1951
#error CCG_MSC_VER_MISMATCH_PROJA
#endif
int projA_probe() { return 1951; }
int main() { return projA_probe() == 1951 ? 0 : 1; }

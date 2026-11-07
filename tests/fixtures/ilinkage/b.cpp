// internal-linkage fixture, TU B: the twin definitions. The bodies
// differ so the two rows stay genuinely distinct symbols; the identity
// (name/namespace/signature) matches TU A's twins exactly.
// Parse-input DATA.
#include "shared.hpp"

namespace
{

    int dampen(int v) { return v + 1; }

} // namespace

static int seed_value(int v) { return dampen(v) * 2; }

template <typename T>
static T clone_leaf(T v)
{
    return v;
}

int b_entry(int v)
{
    return seed_value(v) - clone_leaf(v) + dampen(v) + shared_scale(v);
}

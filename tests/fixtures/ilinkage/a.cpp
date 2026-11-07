// internal-linkage fixture: per-TU twins of a static free function, an
// anonymous-namespace function and a static function template (the
// per-TU copy shape: same name, same signature, one definition per TU).
// Parse-input DATA.
#include "shared.hpp"

namespace
{

    int dampen(int v) { return v - 1; }

} // namespace

static int seed_value(int v) { return dampen(v) + 1; }

template <typename T>
static T clone_leaf(T v)
{
    return v;
}

int a_entry(int v)
{
    return seed_value(v) + clone_leaf(v) + dampen(v) + shared_scale(v);
}

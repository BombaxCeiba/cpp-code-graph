// Real TU pulling in the cyclic include pair (a.hpp <-> b.hpp) so the
// includes table records both edges. Parse-input DATA, never compiled.
#include "a.hpp"

int main()
{
    cycle::A a;
    a.b.x = 1;
    return a.b.x;
}

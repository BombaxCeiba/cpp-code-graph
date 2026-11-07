// Real TU pulling in the whole diamond. Parse-input DATA, never compiled.
#include "a.hpp"

int main()
{
    diamond::A a;
    a.b.d.x = 1;
    a.c.d.x = 2;
    return a.b.d.x + a.c.d.x;
}

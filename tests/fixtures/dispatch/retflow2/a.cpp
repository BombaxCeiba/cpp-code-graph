#include "a.h"
int I::f() { return 0; }
struct Impl : I
{
    int f() override;
};
int  Impl::f() { return 1; }
I*   make() { return new Impl; }
void set(I* p) { p->f(); }

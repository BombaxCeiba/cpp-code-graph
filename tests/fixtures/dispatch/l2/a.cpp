#include "a.h"
int  I::f() { return 0; }
int  Impl1::f() { return 1; }
int  Impl2::f() { return 2; }
I*   make() { return new Impl1; }
void take1(I* p) { p->f(); }
void take2(I* p) { p->f(); }

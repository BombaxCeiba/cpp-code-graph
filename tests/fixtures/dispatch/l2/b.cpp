#include "a.h"
int main()
{
    I* p = make();
    p->f();
    take1(new Impl1);
    take2(new Impl1);
    take2(new Impl2);
    return 0;
}

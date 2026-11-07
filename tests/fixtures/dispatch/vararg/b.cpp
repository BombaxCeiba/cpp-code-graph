#include "a.h"
struct Impl : I
{
    int f() override;
};
int Impl::f() { return 1; }
int main()
{
    I* p = new Impl;
    set(p);
    return 0;
}

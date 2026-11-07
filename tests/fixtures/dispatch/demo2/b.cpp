#include "a.h"
struct Impl : I
{
    int f() override;
};
int I::f() { return 0; }
int Impl::f() { return 1; }
int main()
{
    set(new Impl);
    return 0;
}

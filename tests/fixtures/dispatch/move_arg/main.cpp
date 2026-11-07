#include <utility>

struct I
{
    virtual int f();
    virtual ~I() = default;
};
struct Impl : I
{
    int f() override;
};

int I::f() { return 0; }
int Impl::f() { return 1; }

void take_move(I* p) { p->f(); }
void take_forward(I* p) { p->f(); }

int main()
{
    I* p = new Impl;
    I* q = new Impl;
    take_move(std::move(p));
    take_forward(std::forward<I*>(q));
    return 0;
}

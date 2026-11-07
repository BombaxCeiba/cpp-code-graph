#include <memory>

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

int main()
{
    auto p = std::make_unique<Impl>();
    return p->f();
}

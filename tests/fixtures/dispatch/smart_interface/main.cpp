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

int use_unique()
{
    std::unique_ptr<I> p = std::make_unique<Impl>();
    return p->f();
}

int use_shared()
{
    std::shared_ptr<I> p = std::make_shared<Impl>();
    return p->f();
}

int use_new()
{
    std::unique_ptr<I> p(new Impl);
    return p->f();
}

int main() { return use_unique() + use_shared() + use_new(); }

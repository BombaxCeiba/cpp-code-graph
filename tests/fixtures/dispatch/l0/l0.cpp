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
struct Obj
{
    int x = 0;
};

int I::f() { return 0; }
int Impl::f() { return 1; }

int probes()
{
    I*                    a = new Impl;                 // direct_new
    std::unique_ptr<Impl> b = std::make_unique<Impl>(); // make_unique
    Obj                   o{};                          // direct_aggregate
    Obj*                  po = &o;                      // address_of_object
    Impl*                 d = static_cast<Impl*>(a);    // cast_down
    (void)b;
    (void)po;
    const int from_new = a->f();
    const int from_cast = d->f();
    return from_new + from_cast;
}

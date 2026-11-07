#include <vector>

struct Base
{
    virtual int f();
    virtual ~Base() = default;
};
struct D1 : Base
{
    int f() override;
};
struct D2 : Base
{
    int f() override;
};

int Base::f() { return 0; }
int D1::f() { return 1; }
int D2::f() { return 2; }

int main()
{
    std::vector<Base*> v;
    for (Base* p : v)
    {
        p->f();
    }
    return 0;
}

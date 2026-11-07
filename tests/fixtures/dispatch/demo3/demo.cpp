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

int use(Base* b) { return b->f(); }

int main()
{
    D1 d1;
    D2 d2;
    return use(&d1) + use(&d2);
}

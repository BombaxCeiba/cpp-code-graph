struct I
{
    virtual int f();
    virtual ~I() = default;
};
struct Impl : I
{
    int f() override;
};
struct Other : I
{
    int f() override;
};

int I::f() { return 0; }
int Impl::f() { return 1; }
int Other::f() { return 2; }

int main()
{
    I*   p = new Impl;
    I*   q = new Other;
    bool same = (p == q);
    (void)same;
    return p->f();
}

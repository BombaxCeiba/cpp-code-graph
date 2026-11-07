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

I*   make() { return new Impl; }
void set(I* p) { p->f(); }

int main()
{
    set(make());
    return 0;
}

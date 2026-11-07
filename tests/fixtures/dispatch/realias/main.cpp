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
    I* p = new Impl;
    I* q = nullptr;
    q = p;
    return q->f();
}

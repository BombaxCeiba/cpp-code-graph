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

I* make() { return new Impl; }

void set_arg(I* p) { p->f(); }
void use_arg() { set_arg(static_cast<I*>(make())); }

void set_var(I* p) { p->f(); }
void use_var()
{
    Impl  obj;
    Impl* q = &obj;
    set_var(static_cast<I*>(q));
}

int use_init()
{
    I* p = static_cast<I*>(make());
    return p->f();
}

int main()
{
    use_arg();
    use_var();
    return use_init();
}

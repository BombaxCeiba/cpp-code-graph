struct I
{
    virtual int f();
    virtual ~I() = default;
};
struct Impl1 : I
{
    int f() override;
};
struct Impl2 : I
{
    int f() override;
};

I*   make();
void take1(I* p);
void take2(I* p);

struct I
{
    virtual int f();
    virtual ~I() = default;
};
void set(I* p);

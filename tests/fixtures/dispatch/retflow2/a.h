struct I
{
    virtual int f();
    virtual ~I() = default;
};
I*   make();
void set(I* p);

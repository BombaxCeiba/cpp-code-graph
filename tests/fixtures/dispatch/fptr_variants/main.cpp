struct S
{
    int (*fp)() = nullptr;
    int run() { return fp(); }
};

int freefn() { return 0; }

int use_param(int (*fp)()) { return fp(); }

int main()
{
    int (*fp)() = &freefn;
    int local = fp();
    S   s;
    int member = s.run();
    int param = use_param(&freefn);
    return local + member + param;
}

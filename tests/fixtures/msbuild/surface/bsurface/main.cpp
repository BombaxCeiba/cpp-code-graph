#include "obj.h"
#include "pch.h"
int main()
{
    std::vector<std::string> v{"bsurface"};
    std::printf(
        "sub=%d mul2=%d gen=%d msc=%d %s\n",
        sub(5, 2),
        mul2(6, 7),
        gen(),
        _MSC_FULL_VER,
        v[0].c_str());
    IMath* m = nullptr;
    (void)m;
    return 0;
}

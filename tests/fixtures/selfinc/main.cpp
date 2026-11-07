// Real TU pulling in the self-including header. Parse-input DATA.
#include "s.hpp"

int main()
{
    selfinc::S s;
    s.x = 1;
    return s.x;
}

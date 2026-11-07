#include <cstdio>

#include "util.hpp"

int main()
{
    std::printf("%s\n", demo::mode_name().c_str());
    std::printf("add: %d  answer: %d\n", demo::add(40, 2), demo::answer());
    return 0;
}

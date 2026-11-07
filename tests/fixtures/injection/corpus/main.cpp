#include "alpha/lookup.h"
#include "beta/lookup.h"

int main()
{
    return Alpha::lookup(1) + Beta::lookup(2);
}

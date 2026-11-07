// Fixture data: the includer whose quoted include is spelled with "..".
#include "../inc/counter.hpp"

int main()
{
    tally::Counter c{21};
    return c.doubled();
}

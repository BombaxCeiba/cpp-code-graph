// uses-fixture: defines the opaque record the library references through
// pointers (cross-TU deferred USES resolution) and drives the matrix from
// a second TU. Parse-input DATA.
#include "lib.hpp"

namespace uses
{

    class Opaque
    {
    public:
        int hidden = 7;
    };

} // namespace uses

int main()
{
    uses::Payload start;
    start.value = 3;
    const uses::Payload round = uses::echo(start);
    uses::Carrier       carrier;
    carrier.by_class.value = round.value;
    uses::Opaque opaque;
    uses::poke(&opaque);
    uses::A* a = nullptr;
    uses::B  b;
    uses::C  c;
    a = &b;
    const int steps = (a ? a->step() : 0) + uses::call_base(b)
                      + uses::call_mid(c) + uses::call_derived(c);
    return steps + carrier.by_class.value > 0 ? 0 : 1;
}

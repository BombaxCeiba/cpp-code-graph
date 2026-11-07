// The other half of the include-cycle fixture. Parse-input DATA only.
#ifndef SAMPLE1_CYCLE_B_HPP
#define SAMPLE1_CYCLE_B_HPP

#include "a.hpp"

namespace cycle {

struct B {
    int x;
};

}  // namespace cycle

#endif

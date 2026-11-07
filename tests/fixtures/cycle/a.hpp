// Include-cycle fixture (kept separate so sample1 stays acyclic). Reserved
// for the LATER `check --cycles` command; parse-input DATA only.
#ifndef SAMPLE1_CYCLE_A_HPP
#define SAMPLE1_CYCLE_A_HPP

#include "b.hpp"

namespace cycle {

struct A {
    B b;
};

}  // namespace cycle

#endif

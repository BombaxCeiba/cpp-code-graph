// Negative include-cycle fixture: the classic diamond a -> {b, c} -> d.
// No file reaches itself, so `ccg check --cycles` must report none.
// Parse-input DATA, never compiled.
#ifndef DIAMOND_A_HPP
#define DIAMOND_A_HPP

#include "b.hpp"
#include "c.hpp"

namespace diamond {

struct A {
    B b;
    C c;
};

}  // namespace diamond

#endif

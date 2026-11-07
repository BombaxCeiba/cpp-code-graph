// Self-include edge-case fixture: s.hpp #includes itself (legal once the
// guard is defined; the directive still lands in the includes table, so
// `ccg check --cycles` must report a one-file cycle group with a self
// edge). Parse-input DATA, never compiled.
#ifndef SELFINC_S_HPP
#define SELFINC_S_HPP

#include "s.hpp"

namespace selfinc {

struct S {
    int x;
};

}  // namespace selfinc

#endif

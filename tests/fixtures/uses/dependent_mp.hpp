// uses-fixture: the DEPENDENT member-pointer regression shape. A member
// pointer whose class half is a template parameter ('void (U::*)(void)')
// inside an uninstantiated template pattern is the input class that makes
// some libclang builds dereference NULL inside the class-type query of the
// declared-type walk - with no C-API way to detect the dependence before
// the call. The walk fences that query behind a hardware-fault guard, so
// indexing this header must (a) survive: the whole visit completes and the
// template's symbols land in the db, and (b) lose AT MOST the owning-record
// edge of the dependent pattern itself - every non-dependent member
// pointer elsewhere in the fixture keeps its USES edge. Parse-input DATA,
// never compiled.
#ifndef USES_DEPENDENT_MP_HPP
#define USES_DEPENDENT_MP_HPP

namespace uses {

template <class T>
struct probe_mp {
    template <class U>
    static char tester(void (U::*)(void));  // dependent member-pointer param
    static const bool value = sizeof(tester<T>(0)) == sizeof(char);
};

struct DepTarget { void poke(); };

// Forces probe_mp into the fixture's symbol surface (an out-of-line
// parameter keeps the template reachable from the main-file walk even if
// the header alone were pruned).
int dep_mp_witness(const probe_mp<DepTarget>& p);

}  // namespace uses

#endif

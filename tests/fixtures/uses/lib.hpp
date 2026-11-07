// uses-fixture: the USES extraction matrix (field/param/return/local x
// class/template/std-named), the dispatch-site callers for the override
// family chain, and cross-TU pointer usage of the opaque type. Parse-input
// DATA, never compiled.
#ifndef USES_LIB_HPP
#define USES_LIB_HPP

#include <functional>

#include "dependent_mp.hpp"
#include "types.hpp"

namespace uses {

struct Carrier {
    Payload by_class;                 // field x class
    Holder<Payload> by_template;      // field x template (primary fold)
    std::vector<Payload> by_std;      // field x std-named (Payload only)
    int Payload::* by_member_ptr;     // field x member-pointer class type
};

Payload echo(Payload p);                       // param+return x class
Holder<Payload> echo_tpl(Holder<Payload> h);   // param+return x template
std::vector<Payload> echo_std(std::vector<Payload> v);  // param+return x std

int probe_member(int Payload::* m);  // param x member-pointer class type
std::function<Payload()> make_cb();  // return x function-type template arg
void run_cb(std::function<void(const Payload&)> cb);  // param x callback arg

int Payload::* slot_ref;  // variable x member-pointer class type

void poke(Opaque* o);  // param x cross-TU incomplete (deferred resolution)

int call_base(const A& a);    // dispatch site through A::step
int call_mid(const B& b);     // dispatch site through B::step
int call_derived(const C& c); // dispatch site through C::step

}  // namespace uses

#endif

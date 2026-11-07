// uses-fixture: out-of-line definitions (param/return USES live here) and
// the local-variable USES matrix inside bodies. Parse-input DATA.
#include "lib.hpp"

namespace uses
{

    Payload echo(Payload p)
    {
        Payload              local_class;    // local x class
        Holder<Payload>      local_template; // local x template
        std::vector<Payload> local_std;      // local x std-named
        local_class.value = p.value;
        local_template.slot = p;
        local_std.push_back(p);
        return local_class;
    }

    Holder<Payload> echo_tpl(Holder<Payload> h) { return h; }

    std::vector<Payload> echo_std(std::vector<Payload> v) { return v; }

    int probe_member(int Payload::* m)
    {
        int Payload::* local_mp = m; // local x member-pointer class type
        return local_mp ? 1 : 0;
    }

    std::function<Payload()> make_cb()
    {
        Payload seeded;
        seeded.value = 1;
        return [seeded] { return seeded; };
    }

    void run_cb(std::function<void(const Payload&)> cb)
    {
        std::function<Payload()> local_fn; // local x function-type template arg
        (void)cb;
        (void)local_fn;
    }

    void poke(Opaque* o) { (void)o; }

    int call_base(const A& a) { return a.step(); }

    int call_mid(const B& b) { return b.step(); }

    int call_derived(const C& c) { return c.step(); }

    int dep_mp_witness(const probe_mp<DepTarget>& p) { return p.value ? 1 : 0; }

    int B::step() const { return 1; }

    int C::step() const { return 2; }

} // namespace uses

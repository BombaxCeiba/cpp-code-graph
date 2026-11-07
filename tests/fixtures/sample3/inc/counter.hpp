// Fixture data (parse input, never compiled): quoted include spelled with a
// ".." climb from src/, plus a same-line double call. Layout regression for
// the ".."-spelled include path bug and call-occurrence folding, and the
// is_constexpr flag probe below.
#pragma once

namespace tally {

class Counter {
public:
    explicit Counter(int start) : value_(start) {}
    int get() const;
    // Two authored calls of get() on ONE line: the edge must fold to
    // occurrences = 2, not 1.
    int doubled() const { return get() + get(); }

private:
    int value_;
};

// is_constexpr probe: FlagProbe's constructor has NO leading specifier
// (its declaration extent starts at the name), so the constexpr LOCAL in
// its body must not flag the constructor; limit() is a real constexpr
// method and must be flagged.
class FlagProbe {
public:
    FlagProbe(int v)
    {
        constexpr int kGuard = 1;
        value_ = v + kGuard;
    }

    constexpr int limit() const { return 7; }

private:
    int value_ = 0;
};

}  // namespace tally

// sim/engine.hpp - simulation driver. Fixture data: class template with an
// EXPLICIT SPECIALIZATION for OrderBook, a function template with an explicit
// specialization, recursion, and dead template code never instantiated.
#ifndef SAMPLE2_SIM_ENGINE_HPP
#define SAMPLE2_SIM_ENGINE_HPP

#include <string>
#include <vector>

#include "core/math.hpp"
#include "core/types.hpp"
#include "market/book.hpp"
#include "strategy/strategy.hpp"

namespace sim {

// Function template: run one instrument for n ticks at a fixed drift.
template <typename T>
T drift(T start, int ticks)
{
    if (ticks <= 0)
        return start;
    return drift<T>(start + T{1}, ticks - 1);
}

// EXPLICIT SPECIALIZATION of the function template: doubles compound.
template <>
inline double drift<double>(double start, int ticks)
{
    if (ticks <= 0)
        return start;
    return drift<double>(start * 1.0001 + 0.001, ticks - 1);
}

// Primary class template: drives any book-like object through strategies.
template <typename BookT>
class Engine {
public:
    explicit Engine(BookT& book)
        : book_(book)
    {
    }

    void attach(strategy::Strategy& strat)
    {
        strategies_.push_back(&strat);
    }

    int run(int ticks)
    {
        if (ticks <= 0)
            return 0;
        return tick(ticks - 1);
    }

    std::size_t strategies() const
    {
        return strategies_.size();
    }

protected:
    // Recursive tick: each tick prices through the drift specialization and
    // fans out to every attached strategy (virtual dispatch).
    virtual int tick(int index)
    {
        const core::Price px = drift<double>(100.0, index % 8);
        for (strategy::Strategy* strat : strategies_)
            strat->on_tick(px);
        if (index == 0)
            return 1;
        return 1 + tick(index - 1);
    }

private:
    BookT& book_;
    std::vector<strategy::Strategy*> strategies_;
};

// EXPLICIT SPECIALIZATION of the class template: the OrderBook engine skips
// the drift model and replays a fixed ladder walk instead.
template <>
class Engine<market::exec::OrderBook> {
public:
    explicit Engine(market::exec::OrderBook& book)
        : book_(book)
    {
    }

    void attach(strategy::Strategy& strat)
    {
        strategies_.push_back(&strat);
    }

    int run(int ticks);

    std::size_t strategies() const
    {
        return strategies_.size();
    }

private:
    market::exec::OrderBook& book_;
    std::vector<strategy::Strategy*> strategies_;
};

// DEAD TEMPLATE: never instantiated by anything in the fixture.
template <typename T>
T abandoned_projection(T value)
{
    return value * T{2};
}

}  // namespace sim

#endif  // SAMPLE2_SIM_ENGINE_HPP

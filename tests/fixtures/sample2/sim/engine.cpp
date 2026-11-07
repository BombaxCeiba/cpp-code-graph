// sim/engine.cpp - out-of-line run() for the OrderBook engine specialization.
// Fixture data: recursion through run itself, virtual fan-out to strategies.
#include "sim/engine.hpp"

#include <string>

#include "market/book.hpp"
#include "strategy/strategy.hpp"

namespace sim
{

    int Engine<market::exec::OrderBook>::run(int ticks)
    {
        if (ticks <= 0)
            return 0;
        const core::Price px = drift<double>(90.0, ticks % 6);
        for (strategy::Strategy* strat : strategies_)
            strat->on_tick(px);
        const std::size_t depth_before = book_.depth();
        const int         inner = run(ticks - 1);
        return 1 + inner + static_cast<int>(depth_before);
    }

} // namespace sim

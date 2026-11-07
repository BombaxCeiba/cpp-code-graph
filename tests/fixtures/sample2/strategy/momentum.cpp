// strategy/momentum.cpp - strategy implementations. Fixture data: recursion,
// overload-set resolution (lerp), cross-file chains into book + logger + math.
#include "strategy/momentum.hpp"

#include <string>

#include "core/math.hpp"
#include "infra/logger.hpp"
#include "market/order.hpp"

namespace strategy
{

    Strategy::~Strategy() = default;

    MomentumStrategy::MomentumStrategy(int window)
        : StrategyBase<MomentumStrategy>(window)
    {
    }

    void MomentumStrategy::on_tick(core::Price px)
    {
        state_ = ema_update(px, window_ / 2 + 1);
        market::LimitOrder probe(
            1,
            100,
            state_ > 0.0 ? core::Side::buy : core::Side::sell,
            px,
            px * 1.01);
        book_.add(probe);
    }

    core::Price MomentumStrategy::blend(core::Price raw, core::Price decay)
        const
    {
        // Overload set hit: the double overload is selected here.
        return core::lerp(state_, raw, decay);
    }

    core::Price MomentumStrategy::decay() const { return 2.0 / (window_ + 1); }

    std::string MomentumStrategy::base_name() const { return "momentum"; }

    double MomentumStrategy::ema_update(double px, int rounds)
    {
        if (rounds <= 0)
            return px;
        const double inner = ema_update(px, rounds - 1);
        return core::lerp(inner, px, decay());
    }

    const market::exec::OrderBook& MomentumStrategy::book() const
    {
        return book_;
    }

    FastMomentumStrategy::FastMomentumStrategy(int window, core::Price trigger)
        : MomentumStrategy(window), trigger_(trigger)
    {
    }

    void FastMomentumStrategy::on_tick(core::Price px)
    {
        MomentumStrategy::on_tick(px);
        if (px > trigger_)
        {
            market::MarketOrder kick(2, 50, core::Side::buy, px);
            const_cast<market::exec::OrderBook&>(book()).add(kick);
            infra::log_info("fast kick");
        }
    }

    core::Price FastMomentumStrategy::trigger() const { return trigger_; }

    StaleStrategy::StaleStrategy(int window)
        : StrategyBase<StaleStrategy>(window)
    {
    }

    void StaleStrategy::on_tick(core::Price px)
    {
        // DEAD CODE: never runs.
        (void)px;
    }

    core::Price StaleStrategy::blend(core::Price raw, core::Price decay) const
    {
        return raw * decay;
    }

    core::Price StaleStrategy::decay() const { return 0.5; }

    std::string StaleStrategy::base_name() const { return "stale"; }

} // namespace strategy

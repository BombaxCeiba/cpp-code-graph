// strategy/momentum.hpp - concrete strategies. Fixture data: CRTP leaf,
// one more inheritance level on top (FastMomentumStrategy), a dead class.
#ifndef SAMPLE2_STRATEGY_MOMENTUM_HPP
#define SAMPLE2_STRATEGY_MOMENTUM_HPP

#include <string>

#include "market/book.hpp"
#include "strategy/strategy.hpp"

namespace strategy {

// CRTP leaf: implements the hooks StrategyBase calls back into.
class MomentumStrategy : public StrategyBase<MomentumStrategy> {
public:
    explicit MomentumStrategy(int window);

    void on_tick(core::Price px) override;

    // CRTP hooks used by StrategyBase::smooth / name.
    core::Price blend(core::Price raw, core::Price decay) const;
    core::Price decay() const;
    std::string base_name() const;

    // Recursive exponential update: ema(n) blends ema(n-1) towards px.
    double ema_update(double px, int rounds);

    const market::exec::OrderBook& book() const;

private:
    market::exec::OrderBook book_;
    double state_ = 0.0;
};

// Fourth level of the chain: a momentum strategy with a shorter fuse.
// Strategy -> StrategyBase<MomentumStrategy> -> MomentumStrategy -> this.
class FastMomentumStrategy : public MomentumStrategy {
public:
    FastMomentumStrategy(int window, core::Price trigger);

    void on_tick(core::Price px) override;

    core::Price trigger() const;

private:
    core::Price trigger_ = 0.0;
};

// DEAD CLASS: an abandoned mean-reversion experiment; nothing instantiates
// it and nothing calls its methods.
class StaleStrategy : public StrategyBase<StaleStrategy> {
public:
    explicit StaleStrategy(int window);

    void on_tick(core::Price px) override;

    core::Price blend(core::Price raw, core::Price decay) const;
    core::Price decay() const;
    std::string base_name() const;
};

}  // namespace strategy

#endif  // SAMPLE2_STRATEGY_MOMENTUM_HPP

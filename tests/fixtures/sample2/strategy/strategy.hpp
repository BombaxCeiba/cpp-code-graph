// strategy/strategy.hpp - strategy interfaces. Fixture data: abstract
// interface plus a CRTP middle layer, the root of a CRTP inheritance chain.
#ifndef SAMPLE2_STRATEGY_STRATEGY_HPP
#define SAMPLE2_STRATEGY_STRATEGY_HPP

#include <string>

#include "core/types.hpp"

namespace strategy {

// Pure interface: every strategy reacts to ticks and reports itself.
class Strategy {
public:
    virtual ~Strategy();

    virtual void on_tick(core::Price px) = 0;
    virtual std::string name() const = 0;
};

// CRTP middle layer: shares boilerplate (window bookkeeping, naming) with
// every concrete strategy while staying on the Strategy interface.
// Chain: Strategy -> StrategyBase<D> -> MomentumStrategy -> FastMomentum.
template <typename Derived>
class StrategyBase : public Strategy {
public:
    explicit StrategyBase(int window)
        : window_(window)
    {
    }

    ~StrategyBase() override = default;

    int window() const
    {
        return window_;
    }

    // Helper the derived classes call; dispatches back through CRTP.
    core::Price smooth(core::Price raw) const
    {
        const Derived& self = static_cast<const Derived&>(*this);
        return self.blend(raw, self.decay());
    }

    std::string name() const override
    {
        const Derived& self = static_cast<const Derived&>(*this);
        return self.base_name() + "[" + std::to_string(window_) + "]";
    }

protected:
    int window_ = 1;
};

}  // namespace strategy

#endif  // SAMPLE2_STRATEGY_STRATEGY_HPP

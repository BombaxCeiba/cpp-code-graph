// market/order.hpp - order hierarchy. Fixture data: deep inheritance
// (Entity -> Tradeable -> Order -> LimitOrder/MarketOrder), pure virtuals,
// virtual overrides, const methods, protected fields.
#ifndef SAMPLE2_MARKET_ORDER_HPP
#define SAMPLE2_MARKET_ORDER_HPP

#include <string>

#include "core/meta.hpp"
#include "core/types.hpp"
#include "market/entity.hpp"

namespace market {

// Level 2: adds tradability facts.
class Tradeable : public Entity {
public:
    Tradeable(core::SymbolId id, core::Qty qty, core::Side side);

    core::Qty quantity() const;
    core::Side side() const;

    std::string kind() const override;

protected:
    core::Qty qty_ = 0;
    core::Side side_ = core::Side::buy;
};

// Level 3: the order concept itself. validate() is the polymorphic gate the
// book calls before insertion.
class Order : public Tradeable {
public:
    Order(core::SymbolId id, core::Qty qty, core::Side side, core::Price px);

    core::Price price() const;

    virtual bool validate() const;

    // Fee schedule: recursive pricing model delegating to core::ipow.
    double fee(int levels) const;

    std::string kind() const override;

protected:
    core::Price px_ = 0.0;
};

// Level 4a: price-capped limit order.
class LimitOrder : public Order {
public:
    LimitOrder(core::SymbolId id, core::Qty qty, core::Side side,
               core::Price px, core::Price cap);

    bool validate() const override;
    std::string kind() const override;

    core::Price cap() const;

private:
    core::Price cap_ = 0.0;
};

// Level 4b: immediate-or-cancel market order.
class MarketOrder : public Order {
public:
    MarketOrder(core::SymbolId id, core::Qty qty, core::Side side,
                core::Price ref_px);

    bool validate() const override;
    std::string kind() const override;

private:
    core::Price ref_px_ = 0.0;
};

// Free overload set over the order family: dispatch by static type.
std::string describe_order(const Order& order);
std::string describe_order(const LimitOrder& order);
std::string describe_order(const MarketOrder& order);

}  // namespace market

#endif  // SAMPLE2_MARKET_ORDER_HPP

// market/order.cpp - out-of-line order definitions. Fixture data: explicit
// base-class calls (Tradeable::kind from Order::kind), recursion via fee,
// cross-file calls into core::math and infra::logger.
#include "market/order.hpp"

#include <string>

#include "core/math.hpp"
#include "infra/logger.hpp"

namespace market
{

    Entity::Entity(core::SymbolId id) : id_(id) {}

    Entity::~Entity() = default;

    core::SymbolId Entity::id() const { return id_; }

    std::string Entity::kind() const { return "entity"; }

    Tradeable::Tradeable(core::SymbolId id, core::Qty qty, core::Side side)
        : Entity(id), qty_(qty), side_(side)
    {
    }

    core::Qty Tradeable::quantity() const { return qty_; }

    core::Side Tradeable::side() const { return side_; }

    std::string Tradeable::kind() const
    {
        return Entity::kind() + "/tradeable";
    }

    Order::Order(
        core::SymbolId id,
        core::Qty      qty,
        core::Side     side,
        core::Price    px)
        : Tradeable(id, qty, side), px_(px)
    {
    }

    core::Price Order::price() const { return px_; }

    bool Order::validate() const
    {
        if (px_ <= core::kEpsilon)
            return false;
        return qty_ > 0;
    }

    // Recursive fee model: fee(n) = fee(n-1) * decay + notional * ipow(rate,
    // n).
    double Order::fee(int levels) const
    {
        if (levels <= 0)
            return px_ * static_cast<double>(qty_);
        const double inner = fee(levels - 1);
        return inner * 0.999 + inner * core::ipow(0.001, levels / 2);
    }

    std::string Order::kind() const { return Tradeable::kind() + "/order"; }

    LimitOrder::LimitOrder(
        core::SymbolId id,
        core::Qty      qty,
        core::Side     side,
        core::Price    px,
        core::Price    cap)
        : Order(id, qty, side, px), cap_(cap)
    {
    }

    bool LimitOrder::validate() const
    {
        if (!Order::validate())
            return false;
        return px_ <= cap_ + core::kEpsilon;
    }

    std::string LimitOrder::kind() const { return "limit"; }

    core::Price LimitOrder::cap() const { return cap_; }

    MarketOrder::MarketOrder(
        core::SymbolId id,
        core::Qty      qty,
        core::Side     side,
        core::Price    ref_px)
        : Order(id, qty, side, ref_px), ref_px_(ref_px)
    {
    }

    bool MarketOrder::validate() const
    {
        if (!Order::validate())
            return false;
        return ref_px_ > core::kEpsilon;
    }

    std::string MarketOrder::kind() const { return "market"; }

    std::string describe_order(const Order& order)
    {
        return order.kind() + "@" + std::to_string(order.price());
    }

    std::string describe_order(const LimitOrder& order)
    {
        return order.kind() + "/cap" + std::to_string(order.cap());
    }

    std::string describe_order(const MarketOrder& order)
    {
        return order.kind() + "/ioc";
    }

} // namespace market

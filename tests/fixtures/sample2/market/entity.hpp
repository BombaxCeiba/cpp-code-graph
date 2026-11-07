// market/entity.hpp - root of the entity hierarchy. Fixture data: abstract
// base class, virtual method declared here and defined out-of-line elsewhere.
#ifndef SAMPLE2_MARKET_ENTITY_HPP
#define SAMPLE2_MARKET_ENTITY_HPP

#include <string>

#include "core/types.hpp"

namespace market {

// Level 1 of a four-deep inheritance chain:
//   Entity -> Tradeable -> Order -> LimitOrder / MarketOrder.
class Entity {
public:
    explicit Entity(core::SymbolId id);
    virtual ~Entity();

    core::SymbolId id() const;

    virtual std::string kind() const;

private:
    core::SymbolId id_ = core::kNullSymbol;
};

}  // namespace market

#endif  // SAMPLE2_MARKET_ENTITY_HPP

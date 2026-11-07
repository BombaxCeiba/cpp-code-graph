// core/types.hpp - shared value types and identifiers for the market sim.
// Fixture data for ccg integration tests: nested namespaces, enums, aliases,
// constants, and deliberate dead code.
#ifndef SAMPLE2_CORE_TYPES_HPP
#define SAMPLE2_CORE_TYPES_HPP

#include <cstdint>
#include <string>

namespace core {

using Price = double;
using Qty = std::int64_t;
using SymbolId = std::uint32_t;

inline constexpr int kMaxBookDepth = 16;
inline constexpr Price kEpsilon = 1e-9;
inline constexpr SymbolId kNullSymbol = 0;

enum class Side : std::uint8_t {
    buy = 0,
    sell = 1,
};

enum class OrdStatus {
    created,
    routed,
    filled,
    cancelled,
    rejected,
};

// Plain value type moved around the simulation loop.
struct Trade {
    SymbolId instrument = kNullSymbol;
    Price price = 0.0;
    Qty quantity = 0;
    Side aggressor = Side::buy;
};

// Nested namespace one level deep: internal helpers that the public headers
// never mention. make_tag is DEAD CODE (nothing calls it).
namespace detail {

inline std::string make_tag(SymbolId id, Side side)
{
    std::string text = side == Side::buy ? "B" : "S";
    text += std::to_string(id);
    return text;
}

struct IdMinter {
    SymbolId next = 1;

    SymbolId mint()
    {
        const SymbolId id = next;
        next += 1;
        return id;
    }
};

}  // namespace detail
}  // namespace core

#endif  // SAMPLE2_CORE_TYPES_HPP

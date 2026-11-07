// market/book.hpp - order book. Fixture data: nested namespace market::exec,
// nested struct, member template insert<OrderT>, friend function, overload
// set add(LimitOrder)/add(MarketOrder).
#ifndef SAMPLE2_MARKET_BOOK_HPP
#define SAMPLE2_MARKET_BOOK_HPP

#include <string>
#include <vector>

#include "core/types.hpp"
#include "market/order.hpp"

namespace market {
namespace exec {

struct BookLevel {
    core::Price price = 0.0;
    core::Qty quantity = 0;
};

class OrderBook {
public:
    // Overload set: insertion dispatches on static order type.
    bool add(const LimitOrder& order);
    bool add(const MarketOrder& order);

    // Member template over any order-like type (calls validate() through the
    // template parameter).
    template <typename OrderT>
    bool insert(const OrderT& order)
    {
        if (!order.validate())
            return false;
        return absorb(order.price(), order.quantity());
    }

    core::Price best_bid() const;
    core::Price best_ask() const;
    std::size_t depth() const;

    // Recursive rebalance keeps the level ladder sorted.
    void rebalance(std::size_t index);

    // Friend declaration: auditor dumps the ladder without going through
    // the public accessors.
    friend std::string dump_book(const OrderBook& book);

    struct Stats {
        std::size_t levels = 0;
        core::Qty volume = 0;
    };

    Stats stats() const;

private:
    bool absorb(core::Price px, core::Qty qty);

    std::vector<BookLevel> bids_;
    std::vector<BookLevel> asks_;
};

std::string dump_book(const OrderBook& book);

}  // namespace exec
}  // namespace market

#endif  // SAMPLE2_MARKET_BOOK_HPP

// market/book.cpp - book implementation. Fixture data: recursion (rebalance,
// insert), cross-file call chains into orders, math, and the logger.
#include "market/book.hpp"

#include <algorithm>
#include <string>

#include "core/math.hpp"
#include "infra/logger.hpp"

namespace market
{
    namespace exec
    {

        bool OrderBook::add(const LimitOrder& order)
        {
            infra::log_info("limit in");
            return insert(order);
        }

        bool OrderBook::add(const MarketOrder& order)
        {
            infra::log_info("market in");
            return insert(order);
        }

        bool OrderBook::absorb(core::Price px, core::Qty qty)
        {
            const double clamped = core::clamp_value(px, 0.0, 1.0e9);
            BookLevel    level;
            level.price = clamped;
            level.quantity = qty;
            if (qty > 0)
                bids_.push_back(level);
            else
                asks_.push_back(level);
            if (bids_.size() + asks_.size() > core::kMaxBookDepth)
            {
                infra::log_error("book overflow");
                return false;
            }
            rebalance(bids_.size() + asks_.size() - 1);
            return true;
        }

        core::Price OrderBook::best_bid() const
        {
            if (bids_.empty())
                return 0.0;
            core::Price best = bids_.front().price;
            for (const BookLevel& level : bids_)
                best = core::clamp_value(best, level.price, 1.0e12);
            return best;
        }

        core::Price OrderBook::best_ask() const
        {
            if (asks_.empty())
                return 0.0;
            core::Price best = asks_.front().price;
            for (const BookLevel& level : asks_)
                best = core::clamp_value(best, level.price, 1.0e12);
            return best;
        }

        std::size_t OrderBook::depth() const
        {
            return bids_.size() + asks_.size();
        }

        // Recursive bubble pass: each call fixes one position and recurses
        // down.
        void OrderBook::rebalance(std::size_t index)
        {
            if (index == 0)
                return;
            std::vector<BookLevel>& ladder = bids_;
            if (ladder[index].price > ladder[index - 1].price)
                std::swap(ladder[index], ladder[index - 1]);
            else
                return;
            rebalance(index - 1);
        }

        OrderBook::Stats OrderBook::stats() const
        {
            Stats out;
            out.levels = depth();
            for (const BookLevel& level : bids_)
                out.volume += level.quantity;
            for (const BookLevel& level : asks_)
                out.volume += level.quantity;
            return out;
        }

        std::string dump_book(const OrderBook& book)
        {
            std::string text = "book{b=";
            text += std::to_string(book.bids_.size());
            text += ",a=";
            text += std::to_string(book.asks_.size());
            text += "}";
            return text;
        }

    } // namespace exec
} // namespace market

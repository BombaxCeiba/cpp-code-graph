// main.cpp - wires the whole fixture together. Fixture data: the deepest
// cross-file call chain (main -> Engine::run -> tick/run -> on_tick ->
// OrderBook::add -> insert -> validate -> Order::validate), overload-set
// hits, CRTP dispatch, and one more dead function.
#include <string>

#include "core/math.hpp"
#include "core/meta.hpp"
#include "core/types.hpp"
#include "core/vec.hpp"
#include "infra/logger.hpp"
#include "market/book.hpp"
#include "market/order.hpp"
#include "sim/engine.hpp"
#include "strategy/momentum.hpp"

namespace
{

    // DEAD CODE: nothing in the fixture calls price_spread.
    double price_spread(core::Price bid, core::Price ask)
    {
        const double mid = core::lerp(bid, ask, 0.5);
        return ask - mid;
    }

} // namespace

int main()
{
    infra::Logger& log = infra::Logger::instance();
    log.set_level(infra::LogLevel::info);

    market::exec::OrderBook        book;
    strategy::MomentumStrategy     momentum{8};
    strategy::FastMomentumStrategy fast{4, 105.0};

    sim::Engine<market::exec::OrderBook> engine{book};
    engine.attach(momentum);
    engine.attach(fast);
    const int ticks = engine.run(24);

    market::LimitOrder  limit{3, 10, core::Side::buy, 101.0, 102.0};
    market::MarketOrder immediate{4, 5, core::Side::sell, 99.5};
    book.add(limit);
    book.add(immediate);

    // Overload set hit by static type: LimitOrder / Order / MarketOrder.
    const std::string a = market::describe_order(limit);
    const std::string b =
        market::describe_order(static_cast<const market::Order&>(immediate));
    const std::string c = market::describe_order(immediate);

    // CRTP dispatch: smooth -> blend -> core::lerp(double,double,double).
    const core::Price smoothed = momentum.smooth(101.5);

    // Explicit specializations exercised at runtime.
    const char* tag_d = core::type_tag<double>();
    const char* name_d = core::TypeName<double>::get();
    const char* name_i = core::TypeName<long long>::get();

    // Vector algebra through free operator overloads and the out-of-line
    // scaled() definition.
    const core::Vec3 up{1.0, 2.0, 3.0};
    const core::Vec3 blended = up.scaled(2.0) + up * 0.5 + 0.25 * up;

    // The friend function sees the private ladders.
    const std::string                    ladder = market::exec::dump_book(book);
    const market::exec::OrderBook::Stats st = book.stats();

    // Logger overload set: int, string, leveled.
    log.log(42);
    log.log("session complete");
    log.log(infra::LogLevel::warn, ladder);

    (void)a;
    (void)b;
    (void)c;
    (void)smoothed;
    (void)tag_d;
    (void)name_d;
    (void)name_i;

    return (ticks > 0 && blended.length() > 0.0 && st.levels > 0) ? 0 : 1;
}

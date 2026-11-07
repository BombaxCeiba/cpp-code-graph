// Fixture data: out-of-line member definition - its record scope must
// survive even though the class lives in a header reached via "..".
#include "../inc/counter.hpp"

namespace tally
{

    int Counter::get() const { return value_; }

} // namespace tally

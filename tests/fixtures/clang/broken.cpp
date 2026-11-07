// Fixture with a deliberate syntax error: cg_test_clang asserts that
// diagnostics at severity >= error are surfaced through the wrapper and
// that walking the (partially recovered) cursor tree does not crash.
// Parse-input DATA, not project source.
namespace broken
{

int missing_body(;  // deliberate syntax error

int fine() {
        return 1; }

} // namespace broken

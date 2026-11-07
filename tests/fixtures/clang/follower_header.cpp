// Fixture: a header-unit import whose physical line carries follower
// code (a grown #include would make the preprocessor DROP that code as
// "extra tokens at end of #include directive"), plus a comment-only
// follower line where the directive is legal.
import "follower_cfg.h"; int follower_impl() { return 7; }
import "follower_cfg.h"; // comment-only follower keeps the directive

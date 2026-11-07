#ifndef CCG_UMBRELLA_FAKE
#define CCG_UMBRELLA_FAKE
// Minimal stand-in for the MSVC umbrella header: rides the parse include
// path via -I, proving the emitted directive pulled content in.
inline int umbrella_marker() { return 5; }
#endif

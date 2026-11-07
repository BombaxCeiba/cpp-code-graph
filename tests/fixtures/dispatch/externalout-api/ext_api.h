// Out-of-index loader API declarations (dlopen zero-activation fixture):
// this header lives OUTSIDE the fixture's index roots, mirroring
// real code where Win32/POSIX loader prototypes sit in system headers.
// Before the fix, collect's under_roots guard on the CALLEE declaration
// dropped these call sites entirely, so the dlopen pairing never saw any
// API anchor. The site must now land in call_sites (evidence-only), never
// resolve to a library symbol, and never produce a calls edge.
#pragma once

extern "C" void* dlopen(const char*, int);
extern "C" void* GetProcAddress(void*, const char*);

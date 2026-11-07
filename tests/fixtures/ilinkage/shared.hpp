// internal-linkage fixture: external-linkage control. One shared definition
// (single symbols row, file = this header) must keep resolving from every
// TU - the per-TU scoping must not over-reach onto it. Parse-input DATA.
#pragma once

inline int shared_scale(int v)
{
    return v * 3;
}

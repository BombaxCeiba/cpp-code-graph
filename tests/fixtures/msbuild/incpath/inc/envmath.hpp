// Reachable ONLY through the relative IncludePath entry "inc" (delivered
// by MSBuild through the INCLUDE environment, never as a /I flag) - the
// delivery pattern this fixture pins for the adapter's classification.
#pragma once

namespace envmath {

int env_scale(int value);
int env_offset();

}  // namespace envmath

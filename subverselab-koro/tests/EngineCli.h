#pragma once

// The engine's own CLI and selftest (tools/engine_render.cpp, a verbatim copy of
// prototypes/CIFTE/tools/render.cpp made by tools/sync_engine.sh), compiled into our
// tools inside a namespace so its main() and helpers do not collide with ours.
// Everything it includes is included first, so the includes inside it are no-ops.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include "../Source/Engine.h"

#if defined (__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Weverything"
#endif
#include "../tools/wav.h"
namespace enginecli
{
#define main engine_cli_main
#include "../tools/engine_render.cpp"
#undef main
}
#if defined (__clang__)
 #pragma clang diagnostic pop
#endif

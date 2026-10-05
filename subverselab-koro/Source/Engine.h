#pragma once

// The KORO engine and its six MOODS, both verbatim copies from prototypes/CIFTE
// (tools/sync_engine.sh copies them and writes their md5 into EngineVersion.h).
// They are not edited here, so any compiler noise they make under JUCE's strict
// warning set is silenced at this one include instead of in the files.
#include "EngineVersion.h"

#if defined (__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Weverything"
#elif defined (__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wall"
 #pragma GCC diagnostic ignored "-Wextra"
#elif defined (_MSC_VER)
 #pragma warning (push, 0)
#endif

#include "CifteEngine.h"
#include "../tools/presets.h"

#if defined (__clang__)
 #pragma clang diagnostic pop
#elif defined (__GNUC__)
 #pragma GCC diagnostic pop
#elif defined (_MSC_VER)
 #pragma warning (pop)
#endif

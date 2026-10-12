#pragma once
// Source/CifteEngine.h was a verbatim copy of prototypes/CIFTE/Source/CifteEngine.h (md5 6071fef268c2dd35651f6202863f1553).
// 2026-10-04 (approved engine change): it now DIFFERS from the prototype by the click fixes (voice fades, per-sample MIX / HARMONY gains,
// interpolated TIMING offset), see STATUS.md.
// 2026-10-12 (family batch): MONO SAFE redesign in processLegacy (no 0.18 delay scaling; doubles go into the side signal, mid = input).
// Previous md5 7fd7e265f3b2bf5a1f47cb1d1478f4c5. MONO SAFE off is bit-identical (tests/MonoSafeTests.h). Do not run tools/sync_engine.sh without FORCE=1: it would overwrite the fixes. md5 of the file:
#define KORO_ENGINE_MD5 "3b9069e8ef23295da33196a3d25bc089"
#define KORO_MOODS_MD5 "d0a112d190e7aaeca5b9344916c90c64"

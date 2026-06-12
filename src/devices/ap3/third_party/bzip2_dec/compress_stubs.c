/*
 * SubuwuTuner addition 2026-06-11 (NOT part of upstream bzip2).
 *
 * The SubuwuTuner build only needs bzip2 decompress — the layer-4
 * step of the COBB AccessPort V3 `.ptm` cipher chain. The original
 * compress sources (`compress.c`, `blocksort.c`) were dropped per
 * the dep survey in `specs/cobb-ap3-tier3-dep-survey.md`.
 *
 * bzlib.c still references three compress-side entry points from
 * its `BZ2_bzCompress*` functions, which the linker requires
 * resolution for even if those code paths are never executed at
 * runtime (the SubuwuTuner consumer only calls
 * `BZ2_bzBuffToBuffDecompress` and the `BZ2_bzDecompress*` family).
 *
 * This file provides no-op stubs so the link succeeds. Calling any
 * of these at runtime would mean someone wired up the
 * `BZ2_bzCompress` path against this build — which the implementer
 * has documented they should not. The stubs are silent: they don't
 * abort, they just do nothing. (The compress paths in bzlib.c
 * already handle empty output buffers per the BZ_API contract.)
 *
 * See `THIRD_PARTY_NOTICES.md` for the full attribution.
 */

#include "bzlib_private.h"

void BZ2_blockSort(EState *s) {
    (void)s;
}

void BZ2_compressBlock(EState *s, Bool is_last_block) {
    (void)s;
    (void)is_last_block;
}

void BZ2_bsInitWrite(EState *s) {
    (void)s;
}

# `tests/fuzz/` — libFuzzer harnesses

Per `docs/08-testing-strategy.md` Tier 3. Three harnesses:

- `fuzz_rom_load.cpp`     — `st::Rom::load_bytes` on random byte
  sequences. ROM loader malformed-input class.
- `fuzz_ptm_decode.cpp`   — `st::library::decode_ptm_xml` on random
  byte sequences. PrivateData XML parser malformed-input class.
- `fuzz_toml_load.cpp`    — toml++'s `parse_file` via a temp file on
  random byte sequences. Definition-pack TOML loader.

## Build

The harnesses are gated behind `ST_BUILD_FUZZ_HARNESSES=ON` so the
default build doesn't link libFuzzer. With Clang ≥ 12:

```bash
cmake -B build/fuzz \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DST_BUILD_FUZZ_HARNESSES=ON \
    -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer,address,undefined"
cmake --build build/fuzz --target fuzz_rom_load fuzz_ptm_decode fuzz_toml_load
```

Each harness binary takes a corpus directory:

```bash
./build/fuzz/tests/fuzz/fuzz_ptm_decode tests/fuzz/corpus/ptm_decode/
```

## Initial corpus

`corpus/<harness>/` is empty in-tree — each harness's first run will
generate its own seeds. The corpus directory exists with a placeholder
`.gitkeep` so the path is committed.

Findings (crashes, hangs, sanitizer trips) belong in
`findings/fuzz-<date>/` off-tree following the existing analyst-
workspace pattern (`D:/Subuwu/findings/`).

## What these harnesses don't cover

- Datalog CSV replay (no parser surface today; the replay path is
  framing-aware and skips the CSV-parse failure modes the harness
  would target).
- `.ptm` cipher chain — fuzzing decrypt_ptm would exercise the
  AES + bzip2 + XTEA layers, but the cipher gate (default OFF)
  means most CI runs won't compile the fuzz target. A future
  cipher-on fuzz pass is the right scope for those layers.
- `.stune` project directory walker — the loader walks a directory
  structure; a fuzz harness would need a filesystem mock. Deferred.

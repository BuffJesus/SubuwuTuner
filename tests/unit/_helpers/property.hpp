// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// In-tree property-based testing harness — ship blocker #11 per
// docs/04-roadmap.md "Property-based tests on the codec layer (SSM
// framing, DVI codec, native SOF/seq/CRC) and the UDS state
// machine. RapidCheck (or in-tree property harness) — example-based
// Catch2 will miss bit-flip / reorder cases."
//
// Why in-tree instead of RapidCheck:
// - One fewer FetchContent dependency, one fewer CMake surface,
//   no MinGW / Apple-Clang portability question to chase.
// - Our codec tests don't need shrinking — a failing seed plus the
//   iteration index is enough to reproduce + bisect by hand.
// - The harness fits in ~80 lines and stays readable. RapidCheck
//   is excellent but heavier than we need for codec round-trips.
//
// Usage:
//
//   TEST_CASE("property: ...", "[property][...]") {
//       st::test::check_property(0xDEADBEEF, 200, [&](st::test::PropertyRng &rng) {
//           auto bytes = rng.bytes(0, 64);
//           // ... build a frame, decode it, assert invariants
//       });
//   }
//
// On any REQUIRE failure inside the lambda, Catch2's CAPTURE records
// the iteration index + seed so you can re-run deterministically by
// hard-coding the failing seed into a one-shot check_property call.

#ifndef ST_TEST_PROPERTY_HPP
#define ST_TEST_PROPERTY_HPP

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace st::test {

// Deterministic PRNG with explicit seed visibility. Wraps
// std::mt19937_64; we never expose the underlying engine type so
// callers can't depend on the implementation across versions of
// the standard library. Seed is bit-mixed by callers via the
// SplitMix64 constant in check_property() so consecutive iteration
// seeds aren't sequential.
class PropertyRng {
public:
    explicit PropertyRng(std::uint64_t seed) noexcept : rng_{seed}, seed_{seed} {}

    [[nodiscard]] std::uint64_t seed() const noexcept {
        return seed_;
    }

    [[nodiscard]] std::uint8_t byte() {
        return static_cast<std::uint8_t>(rng_() & 0xFFU);
    }

    [[nodiscard]] bool boolean() {
        return (rng_() & 1U) != 0U;
    }

    [[nodiscard]] std::uint16_t uint16() {
        return static_cast<std::uint16_t>(rng_() & 0xFFFFU);
    }

    [[nodiscard]] std::uint32_t uint32() {
        return static_cast<std::uint32_t>(rng_());
    }

    // Closed-interval uniform integer pick, inclusive on both ends.
    // Both lo and hi must be reachable; no swap on lo > hi (programmer error).
    [[nodiscard]] std::uint32_t uint32_in(std::uint32_t lo, std::uint32_t hi) {
        std::uniform_int_distribution<std::uint32_t> d{lo, hi};
        return d(rng_);
    }

    [[nodiscard]] std::size_t size_in(std::size_t lo, std::size_t hi) {
        std::uniform_int_distribution<std::size_t> d{lo, hi};
        return d(rng_);
    }

    // Random byte vector of length in [lo, hi]. Each byte is uniform 0..255.
    [[nodiscard]] std::vector<std::uint8_t> bytes(std::size_t lo, std::size_t hi) {
        auto const n = size_in(lo, hi);
        std::vector<std::uint8_t> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(byte());
        }
        return out;
    }

    // Pick one element of a span uniformly. Convenience for opcode /
    // enum-value pickers in tests. Empty span is a programmer error
    // (caller responsibility).
    template <typename T>
    [[nodiscard]] T pick(std::span<T const> options) {
        return options[size_in(0, options.size() - 1)];
    }

private:
    std::mt19937_64 rng_;
    std::uint64_t seed_;
};

// Run `body` `iterations` times with a fresh PropertyRng each iteration.
// Each iteration's seed is `base_seed + i * SplitMix64-constant` so seeds
// stay deterministic-but-decorrelated. Iteration index + seed are
// recorded via CAPTURE — on any REQUIRE failure inside body, Catch2's
// output will show both.
//
// To re-run a specific failing case during debugging:
//   check_property(<seed-from-failure-output>, 1, [](PropertyRng &rng) { ... });
template <typename Fn>
void check_property(std::uint64_t base_seed, std::size_t iterations, Fn &&body) {
    // SplitMix64 golden-ratio constant — same one common in hash
    // mixing literature. Avoids consecutive seeds correlating.
    constexpr std::uint64_t kStride = 0x9E3779B97F4A7C15ULL;
    for (std::size_t i = 0; i < iterations; ++i) {
        std::uint64_t const seed = base_seed + i * kStride;
        PropertyRng rng{seed};
        CAPTURE(i, seed);
        body(rng);
    }
}

} // namespace st::test

#endif // ST_TEST_PROPERTY_HPP

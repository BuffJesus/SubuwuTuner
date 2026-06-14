// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// T23 — per-version AES key dispatcher pinning. Confirms that the
// well-defined v220 / v230 / v240 keys are wired and the
// unsupported versions (v210 MapInternalKey with the
// uncertain-length published key, v9995 D3IK which uses a
// different wrapper) return an empty span so the higher-level
// decrypt_ptm surfaces a clear "format not supported" error
// instead of silently producing garbage plaintext.

#include "st/devices/ets/aes256_ctr.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#ifdef ST_ETS_HAVE_CIPHER

TEST_CASE("ap3 v230 MapInternalKey3 is the shipped default",
          "[devices][ap3][cipher][variant]") {
    auto const key = st::devices::ets::cipher::ptm_key_for_version(230U);
    REQUIRE(key.size() == 32);
    // First 8 bytes pinned to detect accidental table swaps.
    CHECK(key[0] == 'b');
    CHECK(key[1] == 'J');
    CHECK(key[2] == 'T');
    CHECK(key[3] == 'c');
}

TEST_CASE("ap3 v220 MapInternalKey2 is wired",
          "[devices][ap3][cipher][variant]") {
    auto const key = st::devices::ets::cipher::ptm_key_for_version(220U);
    REQUIRE(key.size() == 32);
    CHECK(key[0] == 'M');
    CHECK(key[1] == 'I');
    CHECK(key[2] == 'I');
    CHECK(key[3] == 'E');
}

TEST_CASE("ap3 v240 MapInternalKey4 is wired",
          "[devices][ap3][cipher][variant]") {
    auto const key = st::devices::ets::cipher::ptm_key_for_version(240U);
    REQUIRE(key.size() == 32);
    CHECK(key[0] == 'w');
    CHECK(key[1] == 'r');
    CHECK(key[2] == 'q');
    CHECK(key[3] == 'j');
}

TEST_CASE("ap3 v210 returns empty span (key length uncertain)",
          "[devices][ap3][cipher][variant]") {
    auto const key = st::devices::ets::cipher::ptm_key_for_version(210U);
    CHECK(key.empty());
}

TEST_CASE("ap3 v9995 returns empty span (D3En7 wrapper not implemented)",
          "[devices][ap3][cipher][variant]") {
    auto const key = st::devices::ets::cipher::ptm_key_for_version(9995U);
    CHECK(key.empty());
}

TEST_CASE("ap3 unknown versions return empty span",
          "[devices][ap3][cipher][variant]") {
    CHECK(st::devices::ets::cipher::ptm_key_for_version(0U).empty());
    CHECK(st::devices::ets::cipher::ptm_key_for_version(231U).empty());
    CHECK(st::devices::ets::cipher::ptm_key_for_version(999U).empty());
}

#else // !ST_ETS_HAVE_CIPHER

TEST_CASE("ap3 dispatcher returns empty when cipher is off",
          "[devices][ap3][cipher][variant]") {
    CHECK(st::devices::ets::cipher::ptm_key_for_version(230U).empty());
}

#endif

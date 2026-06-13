// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// AP3 live-hardware smoke test (checklist T1.7). Off by default — only
// runs when STT_ETS_LIVE_TEST=1 is set in the environment AND an AP3 is
// physically connected (Zadig + WinUSB on Windows, udev rule on Linux,
// per docs/install.md).
//
// The test exercises the Capability-A surface end-to-end:
//   1. query_state — non-empty raw response bodies
//   2. ls("/maps/") — decodes as a vector<FileInfo>
//   3. push a synthetic 50 KB file → read back → verify byte-identical
//      → remove
//
// The synthetic file lives at `/maps/_st_smoke_<unix_ts>.bin`. The
// underscore prefix is a deliberate signal to anyone manually browsing
// `/maps/` on the AP that this file is a SubuwuTuner internal probe and
// safe to delete. We always remove it at the end of the test.
//
// What this test will NOT do:
//   - Touch /settings, /backupcksum, or any file the AP firmware
//     consults during reflash. Capability A is opaque-blob-level only.
//   - Send any cmd byte outside the spec §6.0 allow-list. The codec's
//     PolicyDenied gate enforces that even if the test tries.
//   - Retry past one daze: if the AP wedges mid-test, the test fails
//     and the user replugs. We never poke a dazed device with a longer
//     sequence (that risks escalating the wedge).

#include "st/core/result.hpp"
#include "st/devices/ets/client.hpp"
#include "st/devices/ets/file_info.hpp"
#include "st/transport/byte_channel.hpp"
#include "st/transport/ets_channel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

bool live_test_enabled() {
    char const *v = std::getenv("STT_ETS_LIVE_TEST");
    return v != nullptr && std::string{v} == "1";
}

std::string smoke_filename() {
    auto const now = std::chrono::system_clock::now().time_since_epoch();
    auto const secs = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    return "_st_smoke_" + std::to_string(secs) + ".bin";
}

} // namespace

TEST_CASE("ap3 live: query_state returns non-empty bodies", "[.live][ap3]") {
    if (!live_test_enabled()) {
        SKIP("STT_ETS_LIVE_TEST not set — skipping live-hardware smoke");
    }
    auto channel = st::transport::ets::open_channel();
    REQUIRE(channel.has_value());
    st::devices::ets::Client client{**channel};
    auto state = client.query_state();
    REQUIRE(state.has_value());
    REQUIRE_FALSE(state->user_info_body.empty());
    REQUIRE_FALSE(state->firmware_body.empty());
    REQUIRE_FALSE(state->device_settings_body.empty());
}

TEST_CASE("ap3 live: ls /maps/ decodes to a non-empty file list", "[.live][ap3]") {
    if (!live_test_enabled()) {
        SKIP("STT_ETS_LIVE_TEST not set — skipping live-hardware smoke");
    }
    auto channel = st::transport::ets::open_channel();
    REQUIRE(channel.has_value());
    st::devices::ets::Client client{**channel};
    auto records = client.ls("/maps/");
    REQUIRE(records.has_value());
    // A working AP should have at least one tune file in /maps/. An
    // empty result is the strongest signal of either codec drift or a
    // wiped AP — either way the test should fail loudly.
    REQUIRE_FALSE(records->empty());
    for (auto const &rec : *records) {
        // Every record's name should be printable ASCII; an empty or
        // garbage name signals a decode that walked off a length-
        // prefix mismatch.
        REQUIRE_FALSE(rec.name.empty());
    }
}

TEST_CASE("ap3 live: push 50 KB -> read-back byte-identical -> remove",
          "[.live][ap3]") {
    if (!live_test_enabled()) {
        SKIP("STT_ETS_LIVE_TEST not set — skipping live-hardware smoke");
    }
    auto channel = st::transport::ets::open_channel();
    REQUIRE(channel.has_value());
    st::devices::ets::Client client{**channel};

    std::string const path = "/maps/" + smoke_filename();

    // 50 KB pseudo-random payload (deterministic per test run; not
    // cryptographic). Goal: a non-trivial body across the WinUSB
    // ~45 KB single-transfer cap so we exercise the loop-write path.
    std::vector<std::uint8_t> payload(50 * 1024);
    std::uint64_t seed = 0xDEADBEEFCAFEBABEULL;
    for (auto &b : payload) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        b = static_cast<std::uint8_t>(seed >> 56);
    }

    auto push = client.write_file(path, payload);
    REQUIRE(push.has_value());

    auto pulled = client.read_file(path);
    REQUIRE(pulled.has_value());
    REQUIRE(pulled->size() == payload.size());
    REQUIRE(std::memcmp(pulled->data(), payload.data(), payload.size()) == 0);

    auto rm = client.remove_file(path);
    REQUIRE(rm.has_value());
}

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Compile-time + minimal-runtime smoke tests for the extension seams in
// `st/core/ext/`. These tests deliberately implement each interface
// with a trivial stub and exercise it through a base-class pointer —
// the goal is to catch ABI / signature drift, not to test the
// (non-existent) production implementations.

#include <catch2/catch_test_macros.hpp>

#include <st/core/ext.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace st::ext;

// ---------------------------------------------------------------------
// Validator
// ---------------------------------------------------------------------

namespace {
class StubValidator final : public Validator {
public:
    std::string_view id() const noexcept override {
        return "stub-validator";
    }
    std::string_view display_name() const noexcept override {
        return "Stub Validator";
    }
    std::vector<st::Diagnostic> validate(std::span<std::uint8_t const> bytes,
                                         std::string_view ctx) const override {
        if (bytes.empty()) {
            return {st::Diagnostic{st::Severity::Warning, "stub", "empty buffer at " +
                                                                       std::string{ctx}}};
        }
        return {};
    }
};
} // namespace

TEST_CASE("ext::Validator implementations compile + dispatch", "[core][ext][validator]") {
    std::unique_ptr<Validator> v = std::make_unique<StubValidator>();
    REQUIRE(v->id() == "stub-validator");
    REQUIRE(v->display_name() == "Stub Validator");
    REQUIRE(v->validate(std::span<std::uint8_t const>{}, "test").size() == 1);
    std::array<std::uint8_t, 4> data{1, 2, 3, 4};
    REQUIRE(v->validate(data, "test").empty());
}

// ---------------------------------------------------------------------
// ChecksumStrategy
// ---------------------------------------------------------------------

namespace {
class StubChecksum final : public ChecksumStrategy {
public:
    std::string_view id() const noexcept override {
        return "stub-cksum";
    }
    st::Result<std::uint32_t> compute(std::span<std::uint8_t const> bytes) const override {
        std::uint32_t acc = 0;
        for (auto b : bytes) {
            acc += b;
        }
        return acc;
    }
    st::Status recompute_in_place(std::span<std::uint8_t> bytes) const override {
        if (bytes.empty()) {
            return st::failure(st::ErrorCode::InvalidArgument, "stub: empty buffer");
        }
        bytes[0] = static_cast<std::uint8_t>(bytes.size() & 0xFFU);
        return st::ok();
    }
    std::span<ChecksumStrategy::Range const> affected_ranges() const noexcept override {
        static constexpr std::array<ChecksumStrategy::Range, 1> ranges{{{0, 1}}};
        return ranges;
    }
};
} // namespace

TEST_CASE("ext::ChecksumStrategy implementations compile + dispatch",
          "[core][ext][checksum_strategy]") {
    std::unique_ptr<ChecksumStrategy> s = std::make_unique<StubChecksum>();
    std::array<std::uint8_t, 3> buf{1, 2, 3};
    auto c = s->compute(buf);
    REQUIRE(c.has_value());
    REQUIRE(*c == 6U);
    auto r = s->recompute_in_place(buf);
    REQUIRE(r.has_value());
    REQUIRE(buf[0] == 3U);
    REQUIRE(s->affected_ranges().size() == 1);
}

// ---------------------------------------------------------------------
// TransportDriver
// ---------------------------------------------------------------------

namespace {
class StubTransport final : public TransportDriver {
public:
    std::string_view id() const noexcept override {
        return "stub-transport";
    }
    std::string_view display_name() const noexcept override {
        return "Stub Transport";
    }
    st::Status open() override {
        open_ = true;
        return st::ok();
    }
    void close() noexcept override {
        open_ = false;
    }
    bool is_open() const noexcept override {
        return open_;
    }
    st::Status send(std::span<std::uint8_t const>) override {
        if (!open_) {
            return st::failure(st::ErrorCode::TransportUnavailable, "stub: not open");
        }
        return st::ok();
    }
    st::Result<std::vector<std::uint8_t>>
    receive(std::chrono::milliseconds) override {
        return std::vector<std::uint8_t>{0xAA, 0xBB};
    }

private:
    bool open_{false};
};
} // namespace

TEST_CASE("ext::TransportDriver implementations compile + dispatch",
          "[core][ext][transport_driver]") {
    std::unique_ptr<TransportDriver> t = std::make_unique<StubTransport>();
    REQUIRE_FALSE(t->is_open());
    REQUIRE(t->open().has_value());
    REQUIRE(t->is_open());
    std::array<std::uint8_t, 2> frame{1, 2};
    REQUIRE(t->send(frame).has_value());
    auto rx = t->receive(std::chrono::milliseconds{0});
    REQUIRE(rx.has_value());
    REQUIRE(rx->size() == 2);
    t->close();
    REQUIRE_FALSE(t->is_open());
}

// ---------------------------------------------------------------------
// KernelDescriptor (plain struct — pin layout)
// ---------------------------------------------------------------------

TEST_CASE("ext::KernelDescriptor compiles + defaults are off-by-default",
          "[core][ext][kernel_descriptor]") {
    KernelDescriptor desc{};
    REQUIRE(desc.isa == IsaTag::Unknown);
    REQUIRE(desc.wire == WireProtocol::Unknown);
    REQUIRE_FALSE(desc.supports_read);
    REQUIRE_FALSE(desc.supports_write);
    REQUIRE_FALSE(desc.supports_erase);
    REQUIRE_FALSE(desc.supports_verify_readback);
    REQUIRE(desc.max_block_bytes == 0U);

    desc.id = "subaru.va.sh2a.demo";
    desc.display_name = "Demo Kernel";
    desc.isa = IsaTag::Sh2a;
    desc.wire = WireProtocol::SsmCan;
    desc.supports_read = true;
    REQUIRE(desc.isa == IsaTag::Sh2a);
}

// ---------------------------------------------------------------------
// LogChannelSource
// ---------------------------------------------------------------------

namespace {
class StubLogChannel final : public LogChannelSource {
public:
    std::string_view id() const noexcept override {
        return "stub.channel";
    }
    std::string_view display_name() const noexcept override {
        return "Stub Channel";
    }
    std::string_view unit() const noexcept override {
        return "°C";
    }
    std::string_view producing_table_id() const noexcept override {
        return {};
    }
    std::string_view consuming_table_id() const noexcept override {
        return {};
    }
    st::Result<std::optional<LogSample>>
    read(std::chrono::milliseconds) override {
        return std::optional<LogSample>{LogSample{std::chrono::milliseconds{42}, 75.0}};
    }
};
} // namespace

TEST_CASE("ext::LogChannelSource implementations compile + dispatch",
          "[core][ext][log_channel_source]") {
    std::unique_ptr<LogChannelSource> s = std::make_unique<StubLogChannel>();
    auto r = s->read(std::chrono::milliseconds{10});
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    REQUIRE((*r)->value == 75.0);
}

// ---------------------------------------------------------------------
// RomFormat
// ---------------------------------------------------------------------

namespace {
class StubRomFormat final : public RomFormat {
public:
    std::string_view id() const noexcept override {
        return "stub-rom";
    }
    std::string_view canonical_extension() const noexcept override {
        return "stub";
    }
    bool looks_like(std::span<std::uint8_t const> head) const noexcept override {
        return !head.empty() && head[0] == 0xAAU;
    }
    st::Result<std::vector<std::uint8_t>>
    decode(std::span<std::uint8_t const> bytes) const override {
        return std::vector<std::uint8_t>{bytes.begin(), bytes.end()};
    }
    st::Result<std::vector<std::uint8_t>>
    encode(std::span<std::uint8_t const> rom) const override {
        return std::vector<std::uint8_t>{rom.begin(), rom.end()};
    }
};
} // namespace

TEST_CASE("ext::RomFormat implementations round-trip via decode/encode",
          "[core][ext][rom_format]") {
    std::unique_ptr<RomFormat> r = std::make_unique<StubRomFormat>();
    std::array<std::uint8_t, 4> probe{0xAA, 0xBB, 0xCC, 0xDD};
    REQUIRE(r->looks_like(probe));
    auto dec = r->decode(probe);
    REQUIRE(dec.has_value());
    auto enc = r->encode(*dec);
    REQUIRE(enc.has_value());
    REQUIRE(*enc == std::vector<std::uint8_t>(probe.begin(), probe.end()));
}

// ---------------------------------------------------------------------
// DefinitionSource
// ---------------------------------------------------------------------

namespace {
class StubDefSource final : public DefinitionSource {
public:
    std::string_view id() const noexcept override {
        return "stub-defs";
    }
    st::Result<std::vector<DefinitionPackSummary>> list() override {
        return std::vector<DefinitionPackSummary>{
            {"pack.a", "Pack A", "1.0.0", "tester"},
        };
    }
    st::Result<std::string> fetch(std::string_view id) override {
        if (id == "pack.a") {
            return std::string{"# stub TOML\n"};
        }
        return st::failure(st::ErrorCode::FileNotFound, std::string{id});
    }
};
} // namespace

TEST_CASE("ext::DefinitionSource implementations list + fetch",
          "[core][ext][definition_source]") {
    std::unique_ptr<DefinitionSource> s = std::make_unique<StubDefSource>();
    auto lst = s->list();
    REQUIRE(lst.has_value());
    REQUIRE(lst->size() == 1);
    REQUIRE((*lst)[0].id == "pack.a");

    auto body = s->fetch("pack.a");
    REQUIRE(body.has_value());
    REQUIRE(*body == "# stub TOML\n");

    auto missing = s->fetch("nope");
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(missing.error().code() == st::ErrorCode::FileNotFound);
}

// ---------------------------------------------------------------------
// UnitConverter
// ---------------------------------------------------------------------

namespace {
class CelsiusToFahrenheit final : public UnitConverter {
public:
    std::string_view id() const noexcept override {
        return "celsius-to-fahrenheit";
    }
    std::string_view storage_unit() const noexcept override {
        return "°C";
    }
    std::string_view display_unit() const noexcept override {
        return "°F";
    }
    double to_display(double c) const noexcept override {
        return c * 9.0 / 5.0 + 32.0;
    }
    double to_storage(double f) const noexcept override {
        return (f - 32.0) * 5.0 / 9.0;
    }
};
} // namespace

TEST_CASE("ext::UnitConverter round-trips within FP tolerance",
          "[core][ext][unit_converter]") {
    std::unique_ptr<UnitConverter> u = std::make_unique<CelsiusToFahrenheit>();
    REQUIRE(u->to_display(0.0) == 32.0);
    REQUIRE(u->to_display(100.0) == 212.0);
    // Round-trip.
    double const c = 37.0;
    double const round = u->to_storage(u->to_display(c));
    REQUIRE(std::abs(round - c) < 1e-9);
}

// ---------------------------------------------------------------------
// ActionHandler
// ---------------------------------------------------------------------

namespace {
class StubAction final : public ActionHandler {
public:
    std::string_view id() const noexcept override {
        return "stub.noop";
    }
    std::string_view display_name() const noexcept override {
        return "Stub Action";
    }
    st::Status execute(std::unordered_map<std::string, std::string> args) override {
        last_args_ = std::move(args);
        return st::ok();
    }
    std::unordered_map<std::string, std::string> last_args_{};
};
} // namespace

TEST_CASE("ext::ActionHandler dispatches + forwards args", "[core][ext][action]") {
    auto h = std::make_unique<StubAction>();
    ActionHandler *base = h.get();
    REQUIRE(base->is_available());
    auto s = base->execute({{"foo", "bar"}});
    REQUIRE(s.has_value());
    REQUIRE(h->last_args_.at("foo") == "bar");
}

// ---------------------------------------------------------------------
// HelpTopic
// ---------------------------------------------------------------------

namespace {
class StubHelp final : public HelpTopic {
public:
    std::string_view id() const noexcept override {
        return "stub.topic";
    }
    std::string_view title() const noexcept override {
        return "Stub Topic";
    }
    st::Result<std::string> markdown_body() const override {
        return std::string{"# Stub Topic\n\nSome help text."};
    }
};
} // namespace

TEST_CASE("ext::HelpTopic returns markdown body", "[core][ext][help_topic]") {
    std::unique_ptr<HelpTopic> h = std::make_unique<StubHelp>();
    auto md = h->markdown_body();
    REQUIRE(md.has_value());
    REQUIRE(md->find("Stub Topic") != std::string::npos);
}

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CORE_DIAGNOSTIC_HPP
#define ST_CORE_DIAGNOSTIC_HPP

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st {

// Severity of a diagnostic. Loosely ordered from least to most restrictive
// so callers can compare with `<` to escalate ("take stricter of two").
//
// Distinct from `st::policy::Action` (which is what the host SHOULD DO on
// a diagnostic); a Diagnostic is what HAPPENED. The mapping Diagnostic ->
// Action is per-context: a Blocker in the preflight pipeline always
// refuses; a Warning may be confirm-and-proceed under one profile and
// block under another.
enum class Severity {
    Info = 0,
    Warning = 1,
    Blocker = 2,
};

// A typed message produced by a validator, preflight check, or any other
// pipeline that reports findings rather than throwing. The `category` is a
// short stable identifier (e.g. "ecu_id_match", "battery_voltage"); the
// `message` is human-facing prose. Callers can group on category, render
// the message, and choose presentation based on severity.
class Diagnostic {
public:
    Diagnostic() = default;

    Diagnostic(Severity severity, std::string category, std::string message)
        : severity_{severity}, category_{std::move(category)}, message_{std::move(message)} {}

    [[nodiscard]] Severity severity() const noexcept {
        return severity_;
    }

    [[nodiscard]] std::string_view category() const noexcept {
        return category_;
    }

    [[nodiscard]] std::string_view message() const noexcept {
        return message_;
    }

    [[nodiscard]] bool is_blocker() const noexcept {
        return severity_ == Severity::Blocker;
    }

    [[nodiscard]] bool is_warning() const noexcept {
        return severity_ == Severity::Warning;
    }

    [[nodiscard]] bool is_info() const noexcept {
        return severity_ == Severity::Info;
    }

    friend bool operator==(Diagnostic const &lhs, Diagnostic const &rhs) noexcept {
        return lhs.severity_ == rhs.severity_ && lhs.category_ == rhs.category_ &&
               lhs.message_ == rhs.message_;
    }

private:
    Severity severity_{Severity::Info};
    std::string category_{};
    std::string message_{};
};

// A bag of diagnostics produced by one run of a pipeline. `ok()` is the
// canonical "should I proceed?" question: true iff no blockers are
// present. Warnings do NOT make `ok()` false — they require host
// acknowledgement but do not block.
class DiagnosticReport {
public:
    void push(Diagnostic d) {
        items_.push_back(std::move(d));
    }

    [[nodiscard]] std::vector<Diagnostic> const &items() const noexcept {
        return items_;
    }

    [[nodiscard]] bool ok() const noexcept {
        for (auto const &d : items_) {
            if (d.is_blocker()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return items_.size();
    }

    [[nodiscard]] std::vector<Diagnostic> blockers() const {
        std::vector<Diagnostic> out;
        for (auto const &d : items_) {
            if (d.is_blocker()) {
                out.push_back(d);
            }
        }
        return out;
    }

    [[nodiscard]] std::vector<Diagnostic> warnings() const {
        std::vector<Diagnostic> out;
        for (auto const &d : items_) {
            if (d.is_warning()) {
                out.push_back(d);
            }
        }
        return out;
    }

private:
    std::vector<Diagnostic> items_{};
};

} // namespace st

#endif // ST_CORE_DIAGNOSTIC_HPP

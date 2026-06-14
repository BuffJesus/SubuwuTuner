// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/library/filename_classifier.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::library {

namespace {

[[nodiscard]] bool icontains_word(std::string_view haystack,
                                   std::string_view needle) {
    // \b...\b semantics — needle must be bordered by ASCII-non-word
    // characters (or start/end of string). We avoid std::regex here:
    // <regex>'s build cost on this project's optimization flags is
    // noticeable (~100 KB and the engine is heavy for these short
    // patterns), and the heuristic is well-served by a hand-rolled
    // case-insensitive substring scan with word-boundary checks.
    if (needle.empty() || needle.size() > haystack.size()) {
        return false;
    }
    auto const is_word = [](char c) {
        // Deliberately treat `_` as a separator, not a word character.
        // Filename convention uses `_` as a delimiter ("Felix-on-FA24_v2")
        // so we want the token before/after it to count as bordered. This
        // is more permissive than the upstream Python `\b\w\b` semantics
        // but fits the .ptm corpus we actually classify.
        return std::isalnum(static_cast<unsigned char>(c)) != 0;
    };
    auto const ieq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        // Word-boundary left + right.
        if (i > 0 && is_word(haystack[i - 1])) {
            continue;
        }
        if (i + needle.size() < haystack.size() &&
            is_word(haystack[i + needle.size()])) {
            continue;
        }
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (!ieq(haystack[i + j], needle[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

// Find every word-bordered match of `needle` in `haystack`. Used by
// the variant tokens which can repeat (e.g. "Stage1+SF+93" has both
// SF and 93 as separate matches).
void find_word_matches(std::string_view haystack, std::string_view needle,
                        std::vector<std::string> &out) {
    auto const is_word = [](char c) {
        // Deliberately treat `_` as a separator, not a word character.
        // Filename convention uses `_` as a delimiter ("Felix-on-FA24_v2")
        // so we want the token before/after it to count as bordered. This
        // is more permissive than the upstream Python `\b\w\b` semantics
        // but fits the .ptm corpus we actually classify.
        return std::isalnum(static_cast<unsigned char>(c)) != 0;
    };
    auto const ieq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };
    if (needle.empty() || needle.size() > haystack.size()) {
        return;
    }
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (i > 0 && is_word(haystack[i - 1])) {
            continue;
        }
        if (i + needle.size() < haystack.size() &&
            is_word(haystack[i + needle.size()])) {
            continue;
        }
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (!ieq(haystack[i + j], needle[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            out.emplace_back(haystack.substr(i, needle.size()));
        }
    }
}

// Stage<N> match. Case-insensitive, returns canonical "StageN".
[[nodiscard]] std::string find_stage(std::string_view name) {
    for (int n = 0; n <= 9; ++n) {
        char buf[8];
        // "Stage0".."Stage9"
        buf[0] = 'S';
        buf[1] = 't';
        buf[2] = 'a';
        buf[3] = 'g';
        buf[4] = 'e';
        buf[5] = static_cast<char>('0' + n);
        buf[6] = '\0';
        std::string_view tok{buf, 6};
        if (icontains_word(name, tok)) {
            return std::string{tok};
        }
    }
    return {};
}

// WRK<N> (Fehr/DMann custom-tune lineage marker). Greedy on digits.
[[nodiscard]] std::vector<std::string>
find_wrk_runs(std::string_view name) {
    std::vector<std::string> out;
    auto const is_word = [](char c) {
        // Deliberately treat `_` as a separator, not a word character.
        // Filename convention uses `_` as a delimiter ("Felix-on-FA24_v2")
        // so we want the token before/after it to count as bordered. This
        // is more permissive than the upstream Python `\b\w\b` semantics
        // but fits the .ptm corpus we actually classify.
        return std::isalnum(static_cast<unsigned char>(c)) != 0;
    };
    auto const ieq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };
    std::string_view const prefix{"WRK"};
    for (std::size_t i = 0; i + prefix.size() < name.size(); ++i) {
        if (i > 0 && is_word(name[i - 1])) {
            continue;
        }
        bool match = true;
        for (std::size_t j = 0; j < prefix.size(); ++j) {
            if (!ieq(name[i + j], prefix[j])) {
                match = false;
                break;
            }
        }
        if (!match) {
            continue;
        }
        std::size_t j = i + prefix.size();
        std::size_t const digits_start = j;
        while (j < name.size() &&
               std::isdigit(static_cast<unsigned char>(name[j])) != 0) {
            ++j;
        }
        if (j == digits_start) {
            continue; // "WRK" alone, no digit
        }
        if (j < name.size() && is_word(name[j])) {
            continue; // ran into more letters
        }
        // Canonicalize the WRK prefix as upper-case while preserving
        // the digit run as-is.
        std::string canonical = "WRK";
        canonical += name.substr(digits_start, j - digits_start);
        out.push_back(std::move(canonical));
    }
    return out;
}

// v<N> through v<NNN> (e.g. "v401" — COBB OTS revision number;
// "v2" / "v3" — SubuwuTuner-built custom-tune iterations). Left
// boundary requires a non-word predecessor; right boundary requires
// a non-word successor or end-of-string.
[[nodiscard]] std::vector<std::string>
find_v_runs(std::string_view name) {
    std::vector<std::string> out;
    auto const is_word = [](char c) {
        // Deliberately treat `_` as a separator, not a word character.
        // Filename convention uses `_` as a delimiter ("Felix-on-FA24_v2")
        // so we want the token before/after it to count as bordered. This
        // is more permissive than the upstream Python `\b\w\b` semantics
        // but fits the .ptm corpus we actually classify.
        return std::isalnum(static_cast<unsigned char>(c)) != 0;
    };
    for (std::size_t i = 0; i + 1 < name.size(); ++i) {
        if (i > 0 && is_word(name[i - 1])) {
            continue;
        }
        if (name[i] != 'v' && name[i] != 'V') {
            continue;
        }
        std::size_t j = i + 1;
        std::size_t const digits_start = j;
        while (j < name.size() &&
               std::isdigit(static_cast<unsigned char>(name[j])) != 0) {
            ++j;
        }
        std::size_t const digit_count = j - digits_start;
        if (digit_count < 1 || digit_count > 3) {
            continue;
        }
        if (j < name.size() && is_word(name[j])) {
            continue;
        }
        std::string token = "v";
        token += name.substr(digits_start, digit_count);
        out.push_back(std::move(token));
    }
    return out;
}

void append_joined(std::string &acc, std::string_view tok) {
    if (tok.empty()) {
        return;
    }
    if (!acc.empty()) {
        acc += " / ";
    }
    acc.append(tok);
}

} // namespace

PtmFilenameClassification classify_ptm_filename(std::string_view name) {
    PtmFilenameClassification out;

    // Vendor — first-wins, ordered to match the Python heuristic so
    // results stay consistent between the inventory tool and the GUI.
    if (icontains_word(name, "COBB")) {
        out.vendor = "COBB";
    } else if (icontains_word(name, "NexGen")) {
        out.vendor = "NexGen";
    } else if (icontains_word(name, "DMann") ||
               icontains_word(name, "Fehr")) {
        // Fehr_DMann is the canonical pattern. `_` is treated as a
        // separator by is_word, so the trailing `_` after Fehr counts
        // as a word boundary and the match succeeds naturally.
        out.vendor = "Fehr"; // DMann is the tuner, Fehr is the customer/owner
    } else if (icontains_word(name, "NTM")) {
        out.vendor = "NTM";
    } else if (icontains_word(name, "Felix")) {
        out.vendor = "Felix";
    } else if (icontains_word(name, "Epifan")) {
        out.vendor = "EpifanSoft";
    } else if (!find_stage(name).empty()) {
        // Bare Stage<N> with no other vendor marker → COBB OTS by
        // convention. Comes last so an explicit vendor token wins.
        out.vendor = "COBB";
    }

    out.stage = find_stage(name);

    // Fuel grade.
    constexpr std::array<std::string_view, 7> kFuelTokens{
        "91", "93", "94", "E85", "E50", "E30", "Race",
    };
    for (auto const &fuel : kFuelTokens) {
        if (icontains_word(name, fuel)) {
            out.fuel_grade = std::string{fuel};
            break;
        }
    }

    // Variant tokens. WRK<N> and v<NN[N]> are pattern-matched; the
    // rest are word-bordered literals.
    constexpr std::array<std::string_view, 8> kVariantLiterals{
        "Redline", "BigSF", "SF",      "Economy",
        "Valet",   "HWG",   "LWG",     "ETune",
    };
    std::vector<std::string> hits;
    hits.reserve(8);
    auto wrk = find_wrk_runs(name);
    hits.insert(hits.end(),
                 std::make_move_iterator(wrk.begin()),
                 std::make_move_iterator(wrk.end()));
    auto vrev = find_v_runs(name);
    hits.insert(hits.end(),
                 std::make_move_iterator(vrev.begin()),
                 std::make_move_iterator(vrev.end()));
    for (auto const &lit : kVariantLiterals) {
        find_word_matches(name, lit, hits);
    }
    std::string joined;
    for (auto const &tok : hits) {
        append_joined(joined, tok);
    }
    out.variant = std::move(joined);

    return out;
}

} // namespace st::library

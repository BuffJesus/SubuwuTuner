// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Minimal subprocess runner for CLI integration tests. Wraps popen so
// tests can invoke the real subuwutuner-cli binary against fixtures.
// The binary path is injected at compile time via ST_CLI_BINARY_PATH
// from the test target's CMakeLists.txt — keeps the binary location
// out of test source (works across MSVC/MinGW/Linux build dirs).

#ifndef ST_TEST_HELPERS_CLI_RUNNER_HPP
#define ST_TEST_HELPERS_CLI_RUNNER_HPP

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#define ST_TEST_POPEN _popen
#define ST_TEST_PCLOSE _pclose
#else
#include <sys/wait.h>
#define ST_TEST_POPEN popen
#define ST_TEST_PCLOSE pclose
#endif

namespace st::test {

struct CliResult {
    int exit_code{-1};
    std::string stdout_text;
    bool spawned{false};
};

// Path to the CLI binary, injected by CMake. The tests CMakeLists
// adds a target_compile_definitions(... ST_CLI_BINARY_PATH=...)
// pointing at $<TARGET_FILE:subuwutuner-cli>. If the symbol isn't
// defined at compile time (e.g. running tests outside the project's
// CMake) cli_binary_path() returns empty and is_cli_available()
// returns false; callers skip rather than fail.
[[nodiscard]] inline std::string cli_binary_path() {
#ifdef ST_CLI_BINARY_PATH
    return std::string{ST_CLI_BINARY_PATH};
#else
    return {};
#endif
}

[[nodiscard]] inline bool is_cli_available() {
    auto const p = cli_binary_path();
    if (p.empty())
        return false;
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

// Quote a single argument for shell-level safety. Crude — wraps in
// double quotes + escapes embedded quotes. Adequate for the fixed
// fixture paths the integration tests pass. NOTE backslashes are NOT
// escaped: on POSIX, `\` is shell-special inside quotes (escapes the
// next char), but the fixture paths only ever contain forward slashes
// at the test layer. On Windows, `\` is part of a normal file path and
// must not be escaped. Don't pass paths containing literal backslashes
// from POSIX tests — use forward slashes (Windows accepts both).
[[nodiscard]] inline std::string quote(std::string const &s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// Run the CLI binary with the given argument string. `args` is appended
// to the binary path with one space. On Windows, popen invokes cmd.exe
// internally; on POSIX, /bin/sh. Captures stdout (stderr is left
// going to the terminal — the CLI surfaces its errors via stdout
// anyway). Returns CliResult{exit, captured} or spawned=false if
// _popen failed.
[[nodiscard]] inline CliResult cli_run(std::string const &args) {
    CliResult r;
    auto bin = cli_binary_path();
    if (bin.empty()) {
        return r;
    }
    std::string cmd = quote(bin) + " " + args;
#if defined(_WIN32)
    // _popen on Windows invokes `cmd /c <cmd>`. cmd strips the outer
    // pair of double quotes from <cmd> before parsing, which mangles
    // commands of the shape `"path" arg "path"` (cmd sees the outer
    // two quotes as the pair to strip, leaving the inner quoting
    // broken). The canonical workaround is to wrap the WHOLE command
    // in an extra pair so cmd's outer strip leaves the per-arg quotes
    // intact. See Microsoft Learn "Command-line parsing rules" + the
    // _popen docs; this is the documented escape.
    cmd = std::string{"\""} + cmd + "\"";
#endif
    std::FILE *p = ST_TEST_POPEN(cmd.c_str(), "r");
    if (p == nullptr) {
        return r;
    }
    r.spawned = true;
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), p) != nullptr) {
        r.stdout_text.append(buf.data());
    }
    int const raw = ST_TEST_PCLOSE(p);
#if defined(_WIN32)
    r.exit_code = raw;
#else
    // POSIX: pclose returns wait(2)-style status. Normal-termination
    // exit code lives in the high byte; signal-killed children surface
    // via WIFSIGNALED. We expose the raw negative -1 on pclose failure
    // and the exit code otherwise. Without WEXITSTATUS the comparison
    // `exit_code == 0` would silently miscompare on Linux/macOS.
    if (raw == -1) {
        r.exit_code = -1;
    } else if (WIFEXITED(raw)) {
        r.exit_code = WEXITSTATUS(raw);
    } else {
        r.exit_code = -1;
    }
#endif
    return r;
}

} // namespace st::test

#endif // ST_TEST_HELPERS_CLI_RUNNER_HPP

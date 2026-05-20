// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/transport/serial_byte_channel.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
// clang-format on

namespace st::transport {

namespace {

// Build the magic prefix Windows requires for COM ports above COM9.
// "\\\\.\\COM5" works for both COM1-COM9 and COM10+, so we always
// prefix for uniformity. If the user passed the full device path
// already (starts with "\\\\.\\") we leave it alone.
std::string canonicalize_com(std::string_view in) {
    if (in.size() >= 4 && in.substr(0, 4) == "\\\\.\\") {
        return std::string{in};
    }
    return std::string{"\\\\.\\"} + std::string{in};
}

// Last-error → human string. GetLastError() is a Win32 DWORD.
std::string last_error_string(DWORD code) {
    LPSTR buf = nullptr;
    DWORD const len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    std::string out;
    if (len > 0 && buf != nullptr) {
        // Strip the trailing CR/LF that FormatMessage appends.
        std::size_t n = len;
        while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' ')) {
            --n;
        }
        out.assign(buf, n);
    } else {
        out = "Win32 error " + std::to_string(code);
    }
    if (buf != nullptr)
        LocalFree(buf);
    return out;
}

// RAII wrapper around HANDLE so the SerialByteChannel destructor is
// noexcept by default + closes on early-exit.
class HandleOwner {
public:
    HandleOwner() = default;
    explicit HandleOwner(HANDLE h) noexcept : h_{h} {}
    ~HandleOwner() noexcept {
        reset();
    }

    HandleOwner(HandleOwner const &) = delete;
    HandleOwner &operator=(HandleOwner const &) = delete;
    HandleOwner(HandleOwner &&other) noexcept : h_{other.h_} {
        other.h_ = INVALID_HANDLE_VALUE;
    }
    HandleOwner &operator=(HandleOwner &&other) noexcept {
        if (this != &other) {
            reset();
            h_ = other.h_;
            other.h_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    void reset() noexcept {
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return h_;
    }
    [[nodiscard]] bool valid() const noexcept {
        return h_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE h_{INVALID_HANDLE_VALUE};
};

class WinSerialByteChannel : public IByteChannel {
public:
    explicit WinSerialByteChannel(HandleOwner h) noexcept : h_{std::move(h)} {}

    st::Status write_bytes(std::span<std::uint8_t const> bytes) override {
        if (bytes.empty())
            return st::ok();
        DWORD written = 0;
        BOOL ok =
            WriteFile(h_.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        if (!ok) {
            DWORD const e = GetLastError();
            return st::failure(st::ErrorCode::TransportUnavailable,
                               "serial write failed: " + last_error_string(e));
        }
        if (written != bytes.size()) {
            return st::failure(st::ErrorCode::TransportUnavailable,
                               "serial write short (" + std::to_string(written) + "/" +
                                   std::to_string(bytes.size()) + ")");
        }
        return st::ok();
    }

    Result<std::vector<std::uint8_t>> read_bytes(std::size_t max_bytes,
                                                 std::chrono::milliseconds timeout) override {
        // SetCommTimeouts was configured per `cfg.read_timeout_ms`
        // at open time. Honor the per-call timeout by adjusting it
        // here for this single read.
        adjust_read_timeout(timeout.count());

        std::vector<std::uint8_t> out(max_bytes);
        DWORD bytes_read = 0;
        BOOL ok =
            ReadFile(h_.get(), out.data(), static_cast<DWORD>(max_bytes), &bytes_read, nullptr);
        if (!ok) {
            DWORD const e = GetLastError();
            return st::failure(st::ErrorCode::TransportUnavailable,
                               "serial read failed: " + last_error_string(e));
        }
        out.resize(bytes_read);
        return out; // empty vector on timeout is the expected semantic
    }

private:
    void adjust_read_timeout(long long timeout_ms) noexcept {
        COMMTIMEOUTS to{};
        // ReadIntervalTimeout = MAXDWORD + ReadTotalTimeoutMultiplier=0
        // + ReadTotalTimeoutConstant=N is the "return-when-bytes-available
        // OR after N ms" pattern. Matches IByteChannel semantics.
        to.ReadIntervalTimeout = MAXDWORD;
        to.ReadTotalTimeoutMultiplier = 0;
        to.ReadTotalTimeoutConstant = (timeout_ms < 0) ? 0
                                      : (timeout_ms > MAXDWORD - 1)
                                          ? MAXDWORD - 1
                                          : static_cast<DWORD>(timeout_ms);
        to.WriteTotalTimeoutMultiplier = 0;
        to.WriteTotalTimeoutConstant = write_timeout_ms_;
        // Errors silently ignored — already-open ports may briefly
        // refuse SetCommTimeouts in the middle of an I/O. Worst case
        // the previous timeout applies for one more read.
        (void)SetCommTimeouts(h_.get(), &to);
    }

public:
    void set_write_timeout(std::uint32_t ms) noexcept {
        write_timeout_ms_ = ms;
    }

private:
    HandleOwner h_;
    std::uint32_t write_timeout_ms_{1000};
};

} // namespace

st::Result<std::unique_ptr<IByteChannel>> make_serial_byte_channel(SerialChannelConfig const &cfg) {
    if (cfg.device_path.empty()) {
        return st::failure(st::ErrorCode::InvalidArgument,
                           "make_serial_byte_channel: device_path is empty");
    }
    if (cfg.baud_rate == 0) {
        return st::failure(st::ErrorCode::InvalidArgument,
                           "make_serial_byte_channel: baud_rate is zero");
    }

    std::string const path = canonicalize_com(cfg.device_path);
    HandleOwner h{CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0,       // no sharing — serial ports are exclusive
                              nullptr, // default security
                              OPEN_EXISTING,
                              0, // synchronous I/O (no FILE_FLAG_OVERLAPPED)
                              nullptr)};
    if (!h.valid()) {
        DWORD const e = GetLastError();
        st::ErrorCode const code =
            (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? st::ErrorCode::FileNotFound
            : (e == ERROR_ACCESS_DENIED) ? st::ErrorCode::PermissionDenied
                                         : st::ErrorCode::TransportUnavailable;
        return st::failure(code, "serial open failed for " + path + ": " + last_error_string(e));
    }

    // Configure DCB (data control block) — baud, 8N1, no flow control.
    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(h.get(), &dcb)) {
        return st::failure(st::ErrorCode::TransportUnavailable,
                           "GetCommState failed: " + last_error_string(GetLastError()));
    }
    dcb.BaudRate = cfg.baud_rate;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h.get(), &dcb)) {
        return st::failure(st::ErrorCode::TransportUnavailable,
                           "SetCommState failed: " + last_error_string(GetLastError()));
    }

    // Initial timeouts — adjust_read_timeout() resets these per-read.
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = cfg.read_timeout_ms;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = cfg.write_timeout_ms;
    if (!SetCommTimeouts(h.get(), &to)) {
        return st::failure(st::ErrorCode::TransportUnavailable,
                           "SetCommTimeouts failed: " + last_error_string(GetLastError()));
    }

    // Clear any stale bytes left in the OS buffers from a previous session.
    PurgeComm(h.get(), PURGE_RXCLEAR | PURGE_TXCLEAR);

    auto ch = std::make_unique<WinSerialByteChannel>(std::move(h));
    ch->set_write_timeout(cfg.write_timeout_ms);
    return std::unique_ptr<IByteChannel>{std::move(ch)};
}

} // namespace st::transport

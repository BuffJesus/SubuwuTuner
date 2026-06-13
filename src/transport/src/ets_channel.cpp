// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/ets_channel.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/transport/byte_channel.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>

#ifdef ST_AP3_HAVE_LIBUSB
#    include <libusb.h>
#endif

namespace st::transport::ets {

#ifdef ST_AP3_HAVE_LIBUSB

namespace {

std::string libusb_error_name_string(int code) {
    char const *n = libusb_error_name(code);
    return n != nullptr ? std::string{n} : std::string{"unknown"};
}

// Lazily-evaluated env-var trace gate. Caches the env-var lookup so
// every bulk_transfer doesn't pay the std::getenv cost on the hot
// path. ST_ETS_TRACE_USB=1 — print a hexdump of every OUT/IN payload
// to stderr. Off by default. Format intended for direct diff against
// the captured-known-good fixtures under specs/fixtures/ap3/.
bool usb_trace_enabled() noexcept {
    static int cached = -1;
    if (cached == -1) {
        char const *v = std::getenv("ST_ETS_TRACE_USB");
        cached = (v != nullptr && std::string{v} == "1") ? 1 : 0;
    }
    return cached == 1;
}

void hexdump_to_stderr(char const *tag, std::uint8_t const *bytes, std::size_t n) {
    std::fprintf(stderr, "[ap3-trace] %s (%zu bytes)\n", tag, n);
    constexpr std::size_t kPerRow = 16;
    for (std::size_t i = 0; i < n; i += kPerRow) {
        std::fprintf(stderr, "  %04zx:", i);
        std::size_t const row = std::min(kPerRow, n - i);
        for (std::size_t j = 0; j < row; ++j) {
            std::fprintf(stderr, " %02x", bytes[i + j]);
        }
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
}

class LibusbChannel : public IByteChannel {
public:
    LibusbChannel(libusb_context *ctx, libusb_device_handle *handle, ChannelConfig cfg) noexcept
        : ctx_{ctx}, handle_{handle}, cfg_{cfg} {}

    ~LibusbChannel() override {
        if (handle_ != nullptr) {
            (void)libusb_release_interface(handle_, cfg_.interface_number);
            libusb_close(handle_);
        }
        if (ctx_ != nullptr) {
            libusb_exit(ctx_);
        }
    }

    LibusbChannel(LibusbChannel const &) = delete;
    LibusbChannel &operator=(LibusbChannel const &) = delete;
    LibusbChannel(LibusbChannel &&) = delete;
    LibusbChannel &operator=(LibusbChannel &&) = delete;

    // Reads the IN endpoint with short timeouts until the bus has been
    // quiet for `settle`. A prior session that crashed mid-transaction
    // can leave an unread response in the AP's IN buffer; the next
    // session's first read would pull those stale bytes and decode
    // them as a fresh (but wrong) packet. The analyst's `ap_pull_state`
    // tool always drains before its first send for this reason.
    void drain(std::chrono::milliseconds settle = std::chrono::milliseconds{500}) {
        auto last = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - last < settle) {
            std::uint8_t buf[512];
            int actual = 0;
            int const rc = libusb_bulk_transfer(handle_, cfg_.bulk_in_endpoint, buf,
                                                static_cast<int>(sizeof(buf)), &actual, 100U);
            if (rc == 0 && actual > 0) {
                last = std::chrono::steady_clock::now();
            }
            // LIBUSB_ERROR_TIMEOUT / no data is the expected idle case.
        }
    }

    st::Status write_bytes(std::span<std::uint8_t const> bytes) override {
        if (bytes.empty()) {
            return st::ok();
        }
        if (usb_trace_enabled()) {
            hexdump_to_stderr("OUT", bytes.data(), bytes.size());
        }
        // Windows WinUSB caps single DeviceIoControl transfers around
        // ~45 KB. The AP's state machine doesn't require packet
        // boundaries, so we loop bulk_transfer until wire_len total
        // has been sent. Per-call timeout of 5s is the upper bound
        // for one chunk; a 70 KB tune file uploads well under that.
        std::size_t off = 0;
        while (off < bytes.size()) {
            int actual = 0;
            // libusb takes a non-const buffer for OUT transfers.
            auto *buf = const_cast<std::uint8_t *>(bytes.data() + off);
            int const remaining = static_cast<int>(bytes.size() - off);
            int const rc = libusb_bulk_transfer(handle_, cfg_.bulk_out_endpoint, buf, remaining,
                                                &actual, kPerCallTimeoutMs);
            if (rc != 0 && actual == 0) {
                // The most common LIBUSB_ERROR_TIMEOUT-with-zero-progress
                // failure mode is the §4.2 firmware daze: a prior
                // malformed-body packet (typically u32-LE string lengths
                // in a VaultFileMetadata record where uleb128 was required, but
                // any body-shape error qualifies) wedges the AP's USB
                // state machine. Subsequent OUT transfers — even
                // body-less probes like cmd 0x28 — return Pipe error or
                // hang. clear_halt and libusb_reset_device do not
                // unstick the firmware; only unplugging and replugging
                // the AP recovers. We surface that hint here because
                // the user-facing error otherwise reads like a host
                // driver problem.
                return st::failure(st::ErrorCode::TransportUnavailable,
                                   "ap3: bulk_transfer OUT failed: " +
                                       libusb_error_name_string(rc) +
                                       " (if this persists after running again, the AP firmware "
                                       "may be dazed by an earlier malformed packet — unplug "
                                       "and replug the AccessPort; see docs/34 / spec §4.2)");
            }
            off += static_cast<std::size_t>(actual);
            if (actual == 0) {
                // The endpoint accepted nothing — treat as a hard
                // failure rather than spin forever. Real devices
                // would either accept partials or error.
                return st::failure(st::ErrorCode::TransportUnavailable,
                                   "ap3: bulk_transfer OUT returned 0 bytes with rc=" +
                                       libusb_error_name_string(rc));
            }
        }
        return st::ok();
    }

    Result<std::vector<std::uint8_t>>
    read_bytes(std::size_t max_bytes, std::chrono::milliseconds timeout) override {
        if (max_bytes == 0) {
            return std::vector<std::uint8_t>{};
        }
        std::vector<std::uint8_t> out(max_bytes);
        int actual = 0;
        unsigned int const ms =
            timeout.count() < 0 ? 0U : static_cast<unsigned int>(timeout.count());
        int const rc = libusb_bulk_transfer(handle_, cfg_.bulk_in_endpoint, out.data(),
                                            static_cast<int>(max_bytes), &actual, ms);
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            // Timeout is the expected idle case per IByteChannel
            // semantics. Return whatever (partial) bytes arrived.
            out.resize(static_cast<std::size_t>(actual));
            return out;
        }
        if (rc != 0) {
            return st::failure(st::ErrorCode::TransportUnavailable,
                               "ap3: bulk_transfer IN failed: " + libusb_error_name_string(rc));
        }
        out.resize(static_cast<std::size_t>(actual));
        if (usb_trace_enabled() && !out.empty()) {
            hexdump_to_stderr("IN", out.data(), out.size());
        }
        return out;
    }

private:
    // Caps an individual OUT chunk while we accumulate large writes.
    // Doesn't bound the total — the channel's higher caller owns the
    // total budget; this just keeps a single libusb call from hanging
    // forever on a wedged endpoint.
    static constexpr unsigned int kPerCallTimeoutMs = 5000U;

    libusb_context *ctx_{nullptr};
    libusb_device_handle *handle_{nullptr};
    ChannelConfig cfg_{};
};

} // namespace

Result<std::unique_ptr<IByteChannel>> open_channel(ChannelConfig const &cfg) {
    libusb_context *ctx = nullptr;
    if (int const rc = libusb_init(&ctx); rc != 0) {
        return st::failure(st::ErrorCode::TransportUnavailable,
                           "ap3: libusb_init failed: " + libusb_error_name_string(rc));
    }
    libusb_device_handle *handle =
        libusb_open_device_with_vid_pid(ctx, cfg.vendor_id, cfg.product_id);
    if (handle == nullptr) {
        libusb_exit(ctx);
        char buf[128];
        (void)std::snprintf(buf, sizeof(buf),
                            "ap3: no device matched VID 0x%04X PID 0x%04X (plugged in? "
                            "WinUSB driver bound on Windows? udev rule installed on Linux?)",
                            cfg.vendor_id, cfg.product_id);
        return st::failure(st::ErrorCode::TransportUnavailable, std::string{buf});
    }

    // On Linux/macOS the kernel may have bound a default driver. The
    // detach call returns 0 on success, NOT_FOUND when no driver was
    // attached (a perfectly fine state), or NOT_SUPPORTED on
    // platforms where the call is a no-op (Windows). All three are
    // acceptable; we error only on the rarer NO_DEVICE / etc.
    int const detach_rc = libusb_detach_kernel_driver(handle, cfg.interface_number);
    if (detach_rc != 0 && detach_rc != LIBUSB_ERROR_NOT_FOUND &&
        detach_rc != LIBUSB_ERROR_NOT_SUPPORTED) {
        libusb_close(handle);
        libusb_exit(ctx);
        return st::failure(st::ErrorCode::PermissionDenied,
                           "ap3: detach_kernel_driver failed: " +
                               libusb_error_name_string(detach_rc));
    }

    if (int const claim_rc = libusb_claim_interface(handle, cfg.interface_number); claim_rc != 0) {
        libusb_close(handle);
        libusb_exit(ctx);
        return st::failure(st::ErrorCode::PermissionDenied,
                           "ap3: claim_interface failed: " + libusb_error_name_string(claim_rc) +
                               " (Windows: rebind the AP to WinUSB via Zadig; "
                               "Linux: install a udev rule for VID 1a84 PID 0121)");
    }

    auto channel = std::unique_ptr<LibusbChannel>{new LibusbChannel(ctx, handle, cfg)};
    // Drain stale IN bytes left over from any prior session that
    // crashed mid-transaction. Without this, the first read pulls the
    // buffered tail of the previous session and decodes it as the
    // current command's response.
    channel->drain();
    return std::unique_ptr<IByteChannel>{std::move(channel)};
}

bool detect_present(std::uint16_t vendor_id, std::uint16_t product_id) noexcept {
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        return false;
    }
    libusb_device **list = nullptr;
    ssize_t const count = libusb_get_device_list(ctx, &list);
    bool found = false;
    if (count > 0 && list != nullptr) {
        for (ssize_t i = 0; i < count; ++i) {
            libusb_device_descriptor desc{};
            if (libusb_get_device_descriptor(list[i], &desc) == 0 &&
                desc.idVendor == vendor_id && desc.idProduct == product_id) {
                found = true;
                break;
            }
        }
    }
    if (list != nullptr) {
        libusb_free_device_list(list, 1);
    }
    libusb_exit(ctx);
    return found;
}

#else // !ST_AP3_HAVE_LIBUSB

Result<std::unique_ptr<IByteChannel>> open_channel(ChannelConfig const & /*cfg*/) {
    return st::failure(st::ErrorCode::NotImplemented,
                       "ap3: build configured without libusb (ST_ENABLE_AP3=OFF); "
                       "AP3 file-vault transport is unavailable");
}

bool detect_present(std::uint16_t /*vendor_id*/, std::uint16_t /*product_id*/) noexcept {
    return false;
}

#endif

} // namespace st::transport::ets

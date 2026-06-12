// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/cobb_ap_channel.hpp"

#include "st/core/error.hpp"
#include "st/core/result.hpp"
#include "st/transport/byte_channel.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#ifdef ST_AP3_HAVE_LIBUSB
#    include <libusb.h>
#endif

namespace st::transport::ap3 {

#ifdef ST_AP3_HAVE_LIBUSB

namespace {

std::string libusb_error_name_string(int code) {
    char const *n = libusb_error_name(code);
    return n != nullptr ? std::string{n} : std::string{"unknown"};
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

    st::Status write_bytes(std::span<std::uint8_t const> bytes) override {
        if (bytes.empty()) {
            return st::ok();
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
                // in a FileInfo2 record where uleb128 was required, but
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

    return std::unique_ptr<IByteChannel>{new LibusbChannel(ctx, handle, cfg)};
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

} // namespace st::transport::ap3

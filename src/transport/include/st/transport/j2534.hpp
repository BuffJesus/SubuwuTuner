// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::transport::j2534 — SAE J2534-1 v04.04 "PassThru" API binding.
//
// J2534 is the SAE-standardized vehicle-to-PC pass-thru API every
// commercial diagnostic adapter ships a vendor DLL for. SubuwuTuner
// targets it through Tactrix OpenPort 2.0 (and any other v04.04-
// compliant adapter the user has installed). The actual DLL is
// loaded at runtime — there's no compile-time vendor SDK dependency,
// so the project builds without any Tactrix headers present.
//
// This header exposes:
//   - Function-pointer typedefs for the J2534 v04.04 entry points
//     we drive (Open/Close/Connect/Disconnect/Read/Write/Ioctl plus
//     ReadVersion + GetLastError for diagnostics).
//   - PASSTHRU_MSG struct matching the SAE-defined wire layout
//     (uint32_t fields — `long` is 64-bit on Linux x64 and we MUST
//     match the DLL's 32-bit ABI).
//   - Protocol-ID, status-code, ioctl-ID, and SCONFIG-parameter
//     constants from the v04.04 spec.
//   - J2534Library — wraps a populated function table. Constructed
//     either from a test-injected FunctionTable (current path) or
//     from a DLL/SO path via platform-specific dynamic load (stub
//     today; lands when there's a vendor DLL to point at).
//   - status_message(uint32_t) — human-readable string for each
//     documented status code.
//
// Out of scope for this header (lands when there's a DLL to load
// against):
//   - The platform-specific dynamic loader (LoadLibraryA/dlopen).
//   - Windows-registry-driven adapter discovery
//     (HKLM\Software\PassThruSupport.04.04 + Wow6432Node mirror).
//   - The higher-level J2534Transport class implementing
//     st::transport::ITransport on top of the function table.

#ifndef ST_TRANSPORT_J2534_HPP
#define ST_TRANSPORT_J2534_HPP

#include "st/core/result.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace st::transport::j2534 {

// ---- ABI types ----------------------------------------------------

// J2534 v04.04 uses `unsigned long` for every status / id / size /
// flag value. On Windows (LLP64) and 32-bit Linux that's 32 bits; on
// 64-bit Linux/macOS it's 64 bits. Vendor DLLs are compiled against
// the Windows ABI, so we MUST use a 32-bit type to match the wire
// layout — never native `unsigned long`.
using u32 = std::uint32_t;

// PASSTHRU_MSG — the SAE J2534-1 message struct, byte-identical to
// the C definition in the spec. Total size = 24 + 4128 = 4152 bytes,
// no padding (every field is naturally 4-aligned). The vendor DLL
// reads/writes this layout directly via the ReadMsgs / WriteMsgs
// pointers, so any mismatch corrupts the bus.
//
// `Data` is fixed-size by the spec (max single-message payload),
// even though most messages are much shorter. `DataSize` is the
// valid prefix length.
struct PassThruMsg {
    u32 protocol_id{0};
    u32 rx_status{0};
    u32 tx_flags{0};
    u32 timestamp{0}; // adapter-side µs since some epoch
    u32 data_size{0};
    u32 extra_data_index{0};
    std::array<std::uint8_t, 4128> data{};
};

static_assert(sizeof(PassThruMsg) == 4152,
              "PassThruMsg must match the SAE J2534 v04.04 wire layout");

// SCONFIG / SCONFIG_LIST per the spec — driven into the DLL via
// PassThruIoctl(channel, SET_CONFIG, &SConfigList, nullptr).
struct SConfig {
    u32 parameter{0};
    u32 value{0};
};

struct SConfigList {
    u32 num_of_params{0};
    SConfig *config_ptr{nullptr};
};

// ---- Documented constants ----------------------------------------

// J2534 v04.04 protocol IDs. Subset present here is what we'll use;
// the spec also defines SCI_A_ENGINE / SCI_A_TRANS / SCI_B_ENGINE /
// SCI_B_TRANS (Chrysler SCI variants — unsupported on OpenPort 2.0
// and out of scope for Subaru).
enum class ProtocolId : u32 {
    J1850VPW = 0x01,
    J1850PWM = 0x02,
    ISO9141 = 0x03,  // SSM-over-K-Line (Subaru VA)
    ISO14230 = 0x04, // KWP2000 (Nissan + some non-Subaru)
    Can = 0x05,
    ISO15765 = 0x06, // SSM-over-CAN / UDS-over-CAN (Subaru VB)
};

// J2534 v04.04 status codes. `StatusNoError == 0` per spec so any
// non-zero return from a PassThru* call is a failure. Decoded to
// human-readable text by `status_message()`.
enum class Status : u32 {
    NoError = 0x00,
    NotSupported = 0x01,
    InvalidChannelId = 0x02,
    InvalidProtocolId = 0x03,
    NullParameter = 0x04,
    InvalidIoctlValue = 0x05,
    InvalidFlags = 0x06,
    Failed = 0x07,
    DeviceNotConnected = 0x08,
    Timeout = 0x09,
    InvalidMsg = 0x0A,
    InvalidTimeInterval = 0x0B,
    ExceededLimit = 0x0C,
    InvalidMsgId = 0x0D,
    DeviceInUse = 0x0E,
    InvalidIoctlId = 0x0F,
    BufferEmpty = 0x10,
    BufferFull = 0x11,
    BufferOverflow = 0x12,
    PinInvalid = 0x13,
    ChannelInUse = 0x14,
    MsgProtocolId = 0x15,
    InvalidFilterId = 0x16,
    NoFlowControl = 0x17,
    NotUnique = 0x18,
    InvalidBaudrate = 0x19,
    InvalidDeviceId = 0x1A,
};

// J2534 v04.04 ioctl IDs. Trimmed to the subset we'll actually use
// in v1.x — full list is in the SAE spec.
enum class IoctlId : u32 {
    GetConfig = 0x01,
    SetConfig = 0x02,
    ReadVbatt = 0x03, // battery voltage on DLC pin 16
    FiveBaudInit = 0x04,
    FastInit = 0x05,
    ClearTxBuffer = 0x07,
    ClearRxBuffer = 0x08,
    ClearPeriodicMsgs = 0x09,
    ClearMsgFilters = 0x0A,
    ReadProgVoltage = 0x0E,
};

// SCONFIG parameter IDs we'll set. K-Line SSM needs DataRate=4800,
// DataBits=8, Parity=0; CAN ISO-15765 needs ISO15765_BS and
// ISO15765_STmin for flow control.
enum class ConfigParam : u32 {
    DataRate = 0x01,
    Loopback = 0x03,
    Parity = 0x16,
    BitSamplePoint = 0x17,
    SyncJumpWidth = 0x18,
    DataBits = 0x20,
    Iso15765BS = 0x1E,
    Iso15765StMin = 0x1F,
};

// Decode a status code to a stable, short human-readable string.
// Returns "unknown J2534 status" for any value outside the enum;
// the value itself is not embedded in the returned string (call
// sites that want the numeric form should format it separately).
[[nodiscard]] std::string_view status_message(Status s) noexcept;

// Convenience overload — vendor DLLs return the raw u32 directly.
[[nodiscard]] std::string_view status_message(u32 raw) noexcept;

// ---- Function-pointer typedefs -----------------------------------

// J2534 v04.04 entry points. The vendor DLL exports these as C
// functions; we resolve them through the platform's dynamic loader
// and stash the addresses in J2534Library::FunctionTable.
//
// Calling convention: J2534 v04.04 specifies WINAPI (__stdcall on
// 32-bit Windows). On 64-bit Windows __stdcall is the same as the
// default x64 calling convention, so no decorator is needed for
// our typical Windows build. Linux/macOS naturally use the platform
// calling convention.

extern "C" {

using PtOpen_t = Status (*)(void *device_name, u32 *device_id);
using PtClose_t = Status (*)(u32 device_id);
using PtConnect_t = Status (*)(u32 device_id, u32 protocol_id, u32 flags, u32 baud,
                               u32 *channel_id);
using PtDisconnect_t = Status (*)(u32 channel_id);
using PtReadMsgs_t = Status (*)(u32 channel_id, PassThruMsg *msgs, u32 *num_msgs, u32 timeout_ms);
using PtWriteMsgs_t = Status (*)(u32 channel_id, PassThruMsg *msgs, u32 *num_msgs, u32 timeout_ms);
using PtIoctl_t = Status (*)(u32 channel_id, u32 ioctl_id, void *input, void *output);
using PtReadVersion_t = Status (*)(u32 device_id, char *firmware_ver, char *dll_ver, char *api_ver);
using PtGetLastError_t = Status (*)(char *error_description);

} // extern "C"

// ---- J2534Library ------------------------------------------------

// Owning wrapper around a resolved J2534 v04.04 vendor DLL.
//
// Two construction paths:
//
//   1. `J2534Library{table}` — test-injected function table. Used
//      by unit tests + by future code that wants to mock the
//      vendor DLL. The library does NOT take ownership of any
//      OS handle when constructed this way.
//
//   2. `J2534Library::load(dll_path)` — platform-specific dynamic
//      load. STUBBED today (returns NotImplemented). When wired up
//      it will own the OS handle and unload on destruction.
//
// All entry points stay accessible as raw function pointers via
// `functions()` — there's no value in re-wrapping every call with
// a member function that just forwards arguments. The higher-level
// J2534Transport class (not in this slice) will own the call-site
// ergonomics.
class J2534Library {
public:
    struct FunctionTable {
        PtOpen_t open{nullptr};
        PtClose_t close{nullptr};
        PtConnect_t connect{nullptr};
        PtDisconnect_t disconnect{nullptr};
        PtReadMsgs_t read_msgs{nullptr};
        PtWriteMsgs_t write_msgs{nullptr};
        PtIoctl_t ioctl{nullptr};
        PtReadVersion_t read_version{nullptr};
        PtGetLastError_t get_last_error{nullptr};

        // True iff every entry point we currently use is non-null.
        // ReadVersion + GetLastError are diagnostics-only and not
        // required for basic operation, so they're excluded from
        // the readiness check. A vendor DLL missing one of those is
        // a warning, not a fatal error — surfaced separately.
        [[nodiscard]] bool core_complete() const noexcept;
    };

    // Construct from an explicit function table. Caller is
    // responsible for keeping the pointed-to functions alive for
    // the J2534Library's lifetime. The library does not own any
    // OS handle in this path.
    explicit J2534Library(FunctionTable table) noexcept;

    // Move-only — owning an OS module handle requires single-owner
    // semantics. Copies would either leak the handle or double-unload.
    J2534Library(J2534Library const &) = delete;
    J2534Library &operator=(J2534Library const &) = delete;
    J2534Library(J2534Library &&) noexcept;
    J2534Library &operator=(J2534Library &&) noexcept;
    ~J2534Library();

    // Resolve a vendor DLL/SO from disk and populate the function
    // table from its exported symbols. NotImplemented in this slice
    // — lands when the developer's adapter arrives and there's a
    // real `op20pt32.dll` to point at.
    [[nodiscard]] static Result<J2534Library> load(std::filesystem::path const &dll_path);

    [[nodiscard]] FunctionTable const &functions() const noexcept {
        return table_;
    }

private:
    FunctionTable table_{};
    // Opaque handle to the loaded OS module. `nullptr` when the
    // library was constructed from an explicit FunctionTable (test
    // path). Owned — closed on destruction.
    void *module_handle_{nullptr};
};

} // namespace st::transport::j2534

#endif // ST_TRANSPORT_J2534_HPP

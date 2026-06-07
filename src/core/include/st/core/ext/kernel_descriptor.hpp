// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CORE_EXT_KERNEL_DESCRIPTOR_HPP
#define ST_CORE_EXT_KERNEL_DESCRIPTOR_HPP

#include <cstdint>
#include <string_view>

namespace st::ext {

// ISA of the target ECU. Used to dispatch codegen + brick-protection
// procedures. Extend after the existing values; do not renumber.
enum class IsaTag : std::uint8_t {
    Unknown = 0,
    Sh2a = 1,
    Rh850 = 2,
};

// Wire protocol family. Influences which transport drivers + ECU
// protocol stacks are valid for this kernel.
enum class WireProtocol : std::uint8_t {
    Unknown = 0,
    SsmKLine = 1,
    SsmCan = 2,
    UdsCan = 3,
    KwpKLine = 4,
};

// KernelDescriptor — static metadata describing one ECU family's
// flashing kernel. "Kernel" here is the small bootstrap binary uploaded
// into ECU RAM to enable extended flash operations on some platforms,
// PLUS the protocol + addressing knowledge required to use it.
//
// This is metadata only — actual kernel code is not exposed across the
// extension boundary. A future `KernelLoader` interface may follow.
struct KernelDescriptor {
    // Stable identifier (e.g. "subaru.va.sh2a.aftermarket.v3").
    std::string_view id{};

    // Display name (UI: "Subaru VA WRX MT — SH-2A — Stage Kernel v3").
    std::string_view display_name{};

    // ISA of the target ECU.
    IsaTag isa{IsaTag::Unknown};

    // Wire protocol used to talk to this kernel.
    WireProtocol wire{WireProtocol::Unknown};

    // Whether this kernel supports each operation. Off-by-default so
    // a new descriptor cannot accidentally claim capabilities it does
    // not implement.
    bool supports_read{false};
    bool supports_write{false};
    bool supports_erase{false};
    bool supports_verify_readback{false};

    // The maximum payload size, in bytes, this kernel can accept per
    // TransferData / TransferData-equivalent block. Zero means
    // "consult the ECU's reported value at runtime".
    std::uint32_t max_block_bytes{0};

    // Reference to documentation describing the kernel's behavior +
    // brick-recovery procedure. Path is relative to the `docs/`
    // directory shipped with the application, e.g.
    // "31-brick-protection-by-isa.md#sh2a".
    std::string_view doc_ref{};
};

} // namespace st::ext

#endif // ST_CORE_EXT_KERNEL_DESCRIPTOR_HPP

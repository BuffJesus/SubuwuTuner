// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/j2534_discovery.hpp"

#include "st/transport/j2534.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#endif

namespace st::transport::j2534 {

char const *registry_view_name(RegistryView v) noexcept {
    switch (v) {
    case RegistryView::Native64:
        return "native64";
    case RegistryView::Wow6432:
        return "wow6432";
    }
    return "unknown";
}

namespace {

// J2534-spec bitmask ↔ ProtocolId name. Subset of what shows up in
// `ProtocolsSupported`; entries not here render as "(unknown:
// 0xNN)" so future-spec protocols don't silently disappear.
//
// Bit positions correspond to (1 << (ProtocolId - 1)) per the
// J2534 v04.04 spec (J1850VPW=0x01 → bit 0, J1850PWM=0x02 → bit 1,
// ISO9141=0x03 → bit 2, etc.). Some older vendor DLLs use a flat
// "1 if any protocol" sentinel which we surface verbatim.
struct ProtoEntry {
    std::uint32_t mask;
    char const *name;
};
constexpr std::array<ProtoEntry, 6> kProtocolNames{{
    {1U << 0U, "J1850VPW"},
    {1U << 1U, "J1850PWM"},
    {1U << 2U, "ISO9141"},
    {1U << 3U, "ISO14230"},
    {1U << 4U, "CAN"},
    {1U << 5U, "ISO15765"},
}};

} // namespace

std::string format_protocols_supported(std::uint32_t mask) {
    if (mask == 0) {
        return "(none)";
    }
    std::string out;
    std::uint32_t consumed = 0;
    for (auto const &e : kProtocolNames) {
        if ((mask & e.mask) != 0) {
            if (!out.empty())
                out.append(", ");
            out.append(e.name);
            consumed |= e.mask;
        }
    }
    std::uint32_t const leftover = mask & ~consumed;
    if (leftover != 0) {
        if (!out.empty())
            out.append(", ");
        char buf[32];
        std::snprintf(buf, sizeof buf, "(unknown: 0x%08X)", leftover);
        out.append(buf);
    }
    return out;
}

#ifdef _WIN32

namespace {

// Open the J2534 root key under a specific registry view (32-bit
// or 64-bit). Returns HKEY (caller closes via RegCloseKey) or
// nullptr if the key isn't present. We never propagate the raw
// LSTATUS — "no such key" is the normal case for one of the two
// views on most installs.
[[nodiscard]] HKEY open_root(REGSAM view_flag) noexcept {
    HKEY hkey = nullptr;
    LSTATUS status = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\PassThruSupport.04.04", 0,
                                   KEY_READ | view_flag, &hkey);
    if (status != ERROR_SUCCESS) {
        return nullptr;
    }
    return hkey;
}

// Read a REG_SZ value from an open subkey. Returns the string or
// empty if the value is missing / wrong type / too large for our
// buffer. We cap at 1 KiB per value (J2534 names + paths are
// short).
[[nodiscard]] std::string read_string_value(HKEY hkey, char const *value_name) noexcept {
    char buf[1024]{};
    DWORD buf_size = static_cast<DWORD>(sizeof(buf) - 1);
    DWORD type = 0;
    LSTATUS status = RegQueryValueExA(hkey, value_name, nullptr, &type,
                                      reinterpret_cast<LPBYTE>(buf), &buf_size);
    if (status != ERROR_SUCCESS)
        return {};
    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return {};
    // RegQueryValueEx returns size in bytes including the NUL on
    // success; clamp + trim.
    if (buf_size > 0 && buf[buf_size - 1] == '\0')
        --buf_size;
    return std::string{buf, buf_size};
}

[[nodiscard]] std::uint32_t read_dword_value(HKEY hkey, char const *value_name) noexcept {
    DWORD value = 0;
    DWORD value_sz = sizeof(value);
    DWORD type = 0;
    LSTATUS status = RegQueryValueExA(hkey, value_name, nullptr, &type,
                                      reinterpret_cast<LPBYTE>(&value), &value_sz);
    if (status != ERROR_SUCCESS || type != REG_DWORD) {
        return 0;
    }
    return static_cast<std::uint32_t>(value);
}

// Walk one registry view and append every adapter subkey we find
// to `out`. `view_flag` is KEY_WOW64_64KEY or KEY_WOW64_32KEY;
// `view_tag` records which view in the AdapterInfo.
void walk_view(REGSAM view_flag, RegistryView view_tag, std::vector<AdapterInfo> &out) {
    HKEY root = open_root(view_flag);
    if (root == nullptr)
        return;

    DWORD index = 0;
    for (;;) {
        char subkey_name[256]{};
        DWORD subkey_size = static_cast<DWORD>(sizeof(subkey_name));
        LSTATUS status = RegEnumKeyExA(root, index, subkey_name, &subkey_size, nullptr, nullptr,
                                       nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS)
            break;
        if (status != ERROR_SUCCESS)
            break; // be conservative on weird errors
        ++index;

        HKEY sub = nullptr;
        LSTATUS open_sub = RegOpenKeyExA(root, subkey_name, 0, KEY_READ | view_flag, &sub);
        if (open_sub != ERROR_SUCCESS)
            continue;

        AdapterInfo info;
        info.subkey = subkey_name;
        info.name = read_string_value(sub, "Name");
        if (info.name.empty())
            info.name = info.subkey;
        info.function_library = read_string_value(sub, "FunctionLibrary");
        info.vendor = read_string_value(sub, "Vendor");
        info.protocols_supported = read_dword_value(sub, "ProtocolsSupported");
        info.view = view_tag;

        RegCloseKey(sub);
        // Only include entries that name an actual DLL — empty
        // FunctionLibrary means the vendor installer registered a
        // skeleton without filling it in (seen in the wild on
        // partial installs).
        if (!info.function_library.empty()) {
            out.push_back(std::move(info));
        }
    }
    RegCloseKey(root);
}

} // namespace

std::vector<AdapterInfo> discover_adapters() {
    std::vector<AdapterInfo> out;
    walk_view(KEY_WOW64_64KEY, RegistryView::Native64, out);
    walk_view(KEY_WOW64_32KEY, RegistryView::Wow6432, out);
    return out;
}

#else // !_WIN32

std::vector<AdapterInfo> discover_adapters() {
    // J2534 vendor DLLs are a Windows registration mechanism;
    // Linux/macOS J2534 stacks (e.g. Tactrix's own runtime) use a
    // different distribution channel that doesn't surface here.
    return {};
}

#endif

} // namespace st::transport::j2534

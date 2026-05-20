// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/transport/j2534.hpp"

#include "st/core/error.hpp"

#include <string>
#include <utility>

namespace st::transport::j2534 {

std::string_view status_message(Status s) noexcept {
    switch (s) {
    case Status::NoError:
        return "no error";
    case Status::NotSupported:
        return "operation not supported by this adapter";
    case Status::InvalidChannelId:
        return "invalid channel id";
    case Status::InvalidProtocolId:
        return "invalid protocol id";
    case Status::NullParameter:
        return "null parameter passed to PassThru call";
    case Status::InvalidIoctlValue:
        return "invalid ioctl value";
    case Status::InvalidFlags:
        return "invalid flags";
    case Status::Failed:
        return "vendor-defined failure (call PassThruGetLastError for details)";
    case Status::DeviceNotConnected:
        return "device not connected (USB unplugged or not powered)";
    case Status::Timeout:
        return "operation timed out";
    case Status::InvalidMsg:
        return "invalid message";
    case Status::InvalidTimeInterval:
        return "invalid time interval";
    case Status::ExceededLimit:
        return "exceeded an adapter resource limit";
    case Status::InvalidMsgId:
        return "invalid message id";
    case Status::DeviceInUse:
        return "device already in use by another process";
    case Status::InvalidIoctlId:
        return "invalid ioctl id";
    case Status::BufferEmpty:
        return "buffer empty (no messages available)";
    case Status::BufferFull:
        return "buffer full";
    case Status::BufferOverflow:
        return "buffer overflow (bytes were dropped)";
    case Status::PinInvalid:
        return "invalid pin specified";
    case Status::ChannelInUse:
        return "channel already in use";
    case Status::MsgProtocolId:
        return "message protocol id does not match channel";
    case Status::InvalidFilterId:
        return "invalid filter id";
    case Status::NoFlowControl:
        return "no flow control configured (CAN ISO-15765 needs one)";
    case Status::NotUnique:
        return "value not unique";
    case Status::InvalidBaudrate:
        return "invalid baud rate for this protocol";
    case Status::InvalidDeviceId:
        return "invalid device id";
    }
    return "unknown J2534 status";
}

std::string_view status_message(u32 raw) noexcept {
    return status_message(static_cast<Status>(raw));
}

bool J2534Library::FunctionTable::core_complete() const noexcept {
    return open != nullptr && close != nullptr && connect != nullptr && disconnect != nullptr &&
           read_msgs != nullptr && write_msgs != nullptr && ioctl != nullptr;
}

J2534Library::J2534Library(FunctionTable table) noexcept : table_(table) {}

J2534Library::J2534Library(J2534Library &&other) noexcept
    : table_(other.table_), module_handle_(other.module_handle_) {
    other.table_ = {};
    other.module_handle_ = nullptr;
}

J2534Library &J2534Library::operator=(J2534Library &&other) noexcept {
    if (this != &other) {
        // No OS handle to close yet (load() returns NotImplemented),
        // but the dtor path below is the real release point when
        // platform dynload lands. Mirror it here.
        if (module_handle_ != nullptr) {
            // TODO(transport_j2534): platform-specific FreeLibrary /
            // dlclose lands with load(). Today this branch is
            // unreachable because module_handle_ is always nullptr.
        }
        table_ = other.table_;
        module_handle_ = other.module_handle_;
        other.table_ = {};
        other.module_handle_ = nullptr;
    }
    return *this;
}

J2534Library::~J2534Library() {
    if (module_handle_ != nullptr) {
        // TODO(transport_j2534): same as above — release the OS
        // module handle once platform dynload lands.
    }
}

Result<J2534Library> J2534Library::load(std::filesystem::path const &dll_path) {
    std::string msg{"j2534::J2534Library::load: platform dynamic-load not yet "
                    "implemented (path '"};
    msg.append(dll_path.string());
    msg.append("'). Use the FunctionTable constructor for now; "
               "Windows LoadLibraryA + GetProcAddress lands when "
               "the developer's Tactrix adapter arrives and there's "
               "a real op20pt32.dll to point at.");
    return failure(ErrorCode::NotImplemented, std::move(msg));
}

} // namespace st::transport::j2534

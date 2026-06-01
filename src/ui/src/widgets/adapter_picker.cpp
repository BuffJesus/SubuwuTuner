// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Adapter picker — a small sub-form the Read ROM modal embeds to pick
// between J2534 / OBDX / Native / Trace (test) transports. Separated
// from the modal so future "scan & datalog" or "live tune" modals can
// reuse the same picker without duplicating its layout + tooltip
// language.

#include "widgets/adapter_picker.hpp"

#include "app_state.hpp"

#include "st/transport/factory.hpp"

#include <imgui.h>

namespace st::ui {

bool adapter_is_trace_mode(AdapterPickerState const &s) noexcept {
    return s.kind_idx == 3;
}

// Render the adapter-picker sub-form into the current ImGui window/popup.
// Returns true iff the form is filled in enough to enable a "go" button
// (kind selected + the matching path field non-empty). Intended to be
// called inside a modal/window started by the caller.
bool render_adapter_picker(AdapterPickerState &s) {
    char const *const labels[] = {"J2534", "OBDX", "Native", "Trace (test)"};
    ImGui::Combo("Adapter", &s.kind_idx, labels, IM_ARRAYSIZE(labels));
    if (s.kind_idx == 0) {
        ImGui::InputText("Vendor DLL path", s.dll_path, sizeof s.dll_path);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Path to your J2534 vendor DLL\n"
                              "(e.g. op20pt32.dll, MongoosePro_GM.dll).");
        }
        return s.dll_path[0] != '\0';
    }
    if (s.kind_idx == 3) {
        ImGui::InputText("Trace file (.uds)", s.trace_path, sizeof s.trace_path);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Path to a UDS trace file ('> req' / '< resp' pairs).\n"
                              "Lets you smoke-test the flow without an adapter — feeds\n"
                              "the canned exchanges into a MockTransport. Use this for\n"
                              "pre-OBDX testing or for replaying a recorded session.");
        }
        return s.trace_path[0] != '\0';
    }
    ImGui::InputText("Device path", s.device_path, sizeof s.device_path);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("USB CDC port for the adapter.\n"
                          "Windows: COM5, COM6 etc.   Linux: /dev/ttyACM0.");
    }
    return s.device_path[0] != '\0';
}

// Convert an AdapterPickerState to a TransportSpec ready for
// `st::transport::open_transport`. Pure function; no UI side effects.
// PRECONDITION: !adapter_is_trace_mode(s). Caller must branch on
// adapter_is_trace_mode first; trace mode does not go through the
// factory.
st::transport::TransportSpec adapter_picker_to_spec(AdapterPickerState const &s) {
    st::transport::TransportSpec spec;
    switch (s.kind_idx) {
    case 0:
        spec.kind = st::transport::Kind::J2534;
        break;
    case 2:
        spec.kind = st::transport::Kind::Native;
        break;
    default:
        spec.kind = st::transport::Kind::Obdx;
        break;
    }
    spec.dll_path = s.dll_path;
    spec.device_path = s.device_path;
    return spec;
}

} // namespace st::ui

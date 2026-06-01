// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Adapter-picker sub-form: J2534 / OBDX / Native / Trace (test) dropdown
// + matching path field. Shared between Read-ROM and the future Write-ROM
// modal so the picker UX stays a single source of truth.
//
// AdapterPickerState lives in app_state.hpp (it sits inside AppState).
// The helpers here are the render + spec-construction surface.

#ifndef ST_UI_WIDGETS_ADAPTER_PICKER_HPP
#define ST_UI_WIDGETS_ADAPTER_PICKER_HPP

#include "app_state.hpp" // AdapterPickerState

#include "st/transport/factory.hpp"

namespace st::ui {

// Returns true if `s` selects the trace-replay test mode (no real
// adapter — MockTransport + parse_uds_trace).
[[nodiscard]] bool adapter_is_trace_mode(AdapterPickerState const &s) noexcept;

// Render the adapter-picker sub-form into the current ImGui window or
// popup. Returns true iff the form is filled in enough to enable a "go"
// button (kind selected + the matching path field non-empty).
[[nodiscard]] bool render_adapter_picker(AdapterPickerState &s);

// Convert an AdapterPickerState to a TransportSpec ready for
// `st::transport::open_transport`. PRECONDITION: !adapter_is_trace_mode(s).
[[nodiscard]] st::transport::TransportSpec
adapter_picker_to_spec(AdapterPickerState const &s);

} // namespace st::ui

#endif

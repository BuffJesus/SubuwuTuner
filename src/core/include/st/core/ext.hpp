// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CORE_EXT_HPP
#define ST_CORE_EXT_HPP

// Umbrella header for the extension-interface seams under
// `st::core::ext`. Use the per-interface headers when only one is
// needed; this header pulls them all in for hosts that wire many
// interfaces at once (e.g. the action router or the plugin manifest
// reader, both currently in-tree).
//
// See `st/core/ext/README.md` for the rationale + stability promises.

#include "st/core/ext/action_handler.hpp"
#include "st/core/ext/checksum_strategy.hpp"
#include "st/core/ext/definition_source.hpp"
#include "st/core/ext/help_topic.hpp"
#include "st/core/ext/kernel_descriptor.hpp"
#include "st/core/ext/log_channel_source.hpp"
#include "st/core/ext/rom_format.hpp"
#include "st/core/ext/transport_driver.hpp"
#include "st/core/ext/unit_converter.hpp"
#include "st/core/ext/validator.hpp"

#endif // ST_CORE_EXT_HPP

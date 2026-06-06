// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::ai::Backend — Tier-2 LLM narration trait per docs/20.
//
// The drift classifier (Tier 1) hands a DriftDiagnosis to the explain
// layer; the explain layer wraps a Backend that turns the structured
// diagnosis into reader-friendly narration. The trait keeps the
// transport details (HTTPS to api.anthropic.com, HTTPS to api.openai
// .com, eventually local Ollama) out of the call site — the GUI panel
// just asks for prose.
//
// V1 transport: shell out to `curl` (cross-platform, no heavy HTTPS
// dep, sidesteps cert-bundle questions on Windows). The trait is
// deliberately small so a future-proof refactor to a native HTTPS
// client doesn't touch any caller.
//
// Privacy posture (docs/20 §"Privacy and safety posture"):
//   - Cloud calls show the exact prompt in a confirm dialog before
//     transmission. The Backend doesn't know about that dialog — the
//     GUI gates the call.
//   - Output is advisory. No Backend method returns a FlashPlan or
//     writes to a Project; the trait's only output is std::string
//     text.
//   - BackendInfo identifies the provenance so the GUI can render a
//     "Cloud · Anthropic claude-opus-4-7" badge.
//
// Clean-room boundary: the Backend implementations don't carry any
// provider-specific prose, marketing copy, or sample prompts beyond
// the protocol JSON shapes the providers publish. The user's prompt
// originates from the DriftDiagnosis the (open-source) classifier
// produced.

#ifndef ST_AI_BACKEND_HPP
#define ST_AI_BACKEND_HPP

#include "st/core/error.hpp"
#include "st/core/result.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace st::ai {

// Where the model runs. Drives the GUI badge ("Local" vs "Cloud") and
// the docs/20 §"Privacy" posture — Cloud is the channel that needs a
// pre-transmit confirm dialog.
enum class BackendLocality : std::uint8_t { Local, Cloud };

// Provider tag. Stub is the no-op backend used when ST_AI is OFF or
// the user hasn't configured an API key — `complete()` returns a
// stable "AI disabled" message so call sites don't need to special-
// case the missing path.
enum class BackendKind : std::uint8_t { Stub, Anthropic, OpenAI };

// Identity badge for the GUI. `name` is the user-visible string
// ("Anthropic Claude Opus 4.7"); `model` is the wire-level model id
// the request actually carries.
struct BackendInfo {
    std::string name;
    BackendLocality locality{BackendLocality::Local};
    BackendKind kind{BackendKind::Stub};
    std::string model;
};

class Backend {
public:
    virtual ~Backend() = default;

    // Run the prompt against the configured backend. system_prompt is
    // the persona / safety guard the user shouldn't have to type;
    // user_prompt is the per-request payload (the rendered
    // DriftDiagnosis, for now). Returns the assistant's text on
    // success or an error on transport / parse failure.
    [[nodiscard]] virtual Result<std::string>
    complete(std::string_view system_prompt, std::string_view user_prompt) = 0;

    [[nodiscard]] virtual BackendInfo info() const = 0;
};

// Stub backend — always returns the same string. Useful when ST_AI is
// off, the user hasn't entered a key, or in tests that exercise the
// call site without firing HTTPS.
[[nodiscard]] std::unique_ptr<Backend> make_stub_backend();

// Anthropic Messages API backend. POSTs to
// https://api.anthropic.com/v1/messages with the x-api-key header.
// `model` empty → default "claude-opus-4-7" (CLAUDE.md anchored).
[[nodiscard]] std::unique_ptr<Backend>
make_anthropic_backend(std::string api_key, std::string model = {});

// OpenAI Chat Completions backend. POSTs to
// https://api.openai.com/v1/chat/completions with the
// Authorization: Bearer header. `model` empty → default "gpt-4o".
[[nodiscard]] std::unique_ptr<Backend>
make_openai_backend(std::string api_key, std::string model = {});

// ---- Request body builders (exposed for tests) -----------------------
//
// Hand-rolled JSON; both providers accept the same minimal shape with
// trivial structural differences. The builders are pure functions so
// tests can pin the wire shape without going near the network.

[[nodiscard]] std::string
build_anthropic_request_body(std::string_view model, std::string_view system_prompt,
                             std::string_view user_prompt, int max_tokens = 1024);

[[nodiscard]] std::string
build_openai_request_body(std::string_view model, std::string_view system_prompt,
                          std::string_view user_prompt, int max_tokens = 1024);

// ---- Response parsers (exposed for tests) ----------------------------
//
// Extract the assistant text from a provider response body. Returns
// ErrorCode::ParseError + a short message on a malformed / unexpected-
// shape response. Both providers occasionally surface refusal blocks
// or tool-use blocks; v1 reports those as parse errors with the raw
// payload preserved in the error message so the GUI can hand the user
// a "show raw response" affordance.

[[nodiscard]] Result<std::string>
parse_anthropic_response(std::string_view body);

[[nodiscard]] Result<std::string>
parse_openai_response(std::string_view body);

// ---- curl shell-out --------------------------------------------------
//
// Run a POST against `url` with `headers` (each a full "Name: value"
// string) and `body` as the request payload. Returns the response
// body (stdout) on success. The body is written to a temporary file
// and passed via --data-binary @<tmp>; the tmp is removed on return.
// Errors: curl missing (popen failed), non-zero curl exit (network /
// auth), oversized request (body > kMaxBody).

[[nodiscard]] Result<std::string>
http_post_via_curl(std::string_view url,
                   std::vector<std::string> const &headers,
                   std::string_view body);

} // namespace st::ai

#endif // ST_AI_BACKEND_HPP

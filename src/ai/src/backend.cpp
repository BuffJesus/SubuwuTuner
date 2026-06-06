// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/ai/backend.hpp"

#include "st/core/json_util.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace st::ai {

namespace {

constexpr std::size_t kMaxBody = 1 << 20; // 1 MiB sanity cap on prompts

constexpr char const *kDefaultAnthropicModel = "claude-opus-4-7";
constexpr char const *kDefaultOpenAiModel = "gpt-4o";
constexpr char const *kAnthropicEndpoint = "https://api.anthropic.com/v1/messages";
constexpr char const *kOpenAiEndpoint = "https://api.openai.com/v1/chat/completions";
constexpr char const *kAnthropicVersion = "2023-06-01";

// Parse a JSON string at `pos` in `body`. On success returns the
// unescaped value and advances `pos` past the closing quote. On
// failure returns std::nullopt and leaves pos at the failure point.
// Handles \" \\ \n \r \t \uXXXX (ASCII range only — non-ASCII Unicode
// passes through as the literal UTF-8 bytes the provider sent).
struct StringResult {
    bool ok{false};
    std::string value;
};

StringResult parse_json_string(std::string_view body, std::size_t &pos) {
    StringResult r;
    if (pos >= body.size() || body[pos] != '"')
        return r;
    ++pos;
    while (pos < body.size()) {
        char const c = body[pos++];
        if (c == '"') {
            r.ok = true;
            return r;
        }
        if (c == '\\') {
            if (pos >= body.size())
                return r;
            char const esc = body[pos++];
            switch (esc) {
            case '"':
                r.value.push_back('"');
                break;
            case '\\':
                r.value.push_back('\\');
                break;
            case '/':
                r.value.push_back('/');
                break;
            case 'n':
                r.value.push_back('\n');
                break;
            case 'r':
                r.value.push_back('\r');
                break;
            case 't':
                r.value.push_back('\t');
                break;
            case 'b':
                r.value.push_back('\b');
                break;
            case 'f':
                r.value.push_back('\f');
                break;
            case 'u': {
                if (pos + 4 > body.size())
                    return r;
                std::uint32_t code = 0;
                for (int i = 0; i < 4; ++i) {
                    char const hex = body[pos++];
                    code <<= 4U;
                    if (hex >= '0' && hex <= '9')
                        code |= static_cast<std::uint32_t>(hex - '0');
                    else if (hex >= 'a' && hex <= 'f')
                        code |= static_cast<std::uint32_t>(hex - 'a' + 10);
                    else if (hex >= 'A' && hex <= 'F')
                        code |= static_cast<std::uint32_t>(hex - 'A' + 10);
                    else
                        return r;
                }
                if (code < 0x80) {
                    r.value.push_back(static_cast<char>(code));
                } else if (code < 0x800) {
                    r.value.push_back(static_cast<char>(0xC0 | (code >> 6)));
                    r.value.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else {
                    r.value.push_back(static_cast<char>(0xE0 | (code >> 12)));
                    r.value.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    r.value.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
                break;
            }
            default:
                return r;
            }
        } else {
            r.value.push_back(c);
        }
    }
    return r;
}

// Locate the first occurrence of `"<key>":` at the current nesting
// depth — naive scan that doesn't honor JSON structure rigorously
// but works for the well-shaped provider responses we care about.
// Returns the position immediately after the colon (skipping
// whitespace), or std::string_view::npos on miss.
std::size_t find_key_after(std::string_view body, std::string_view key,
                           std::size_t from) {
    std::string needle;
    needle.reserve(key.size() + 3);
    needle.push_back('"');
    needle.append(key);
    needle.append("\"");
    while (from < body.size()) {
        auto const at = body.find(needle, from);
        if (at == std::string_view::npos)
            return std::string_view::npos;
        std::size_t p = at + needle.size();
        // Skip whitespace; expect ':'.
        while (p < body.size() &&
               (body[p] == ' ' || body[p] == '\t' || body[p] == '\n' || body[p] == '\r'))
            ++p;
        if (p < body.size() && body[p] == ':') {
            ++p;
            while (p < body.size() &&
                   (body[p] == ' ' || body[p] == '\t' || body[p] == '\n' || body[p] == '\r'))
                ++p;
            return p;
        }
        from = at + needle.size();
    }
    return std::string_view::npos;
}

// Read the entire stream from a popen'd child process. Returns the
// concatenated stdout. Caller passes the open FILE* and the close
// callback (Windows uses _pclose, POSIX uses pclose).
std::string slurp_pipe(std::FILE *pipe) {
    std::string out;
    out.reserve(4096);
    char buf[4096];
    while (auto const n = std::fread(buf, 1, sizeof buf, pipe)) {
        out.append(buf, n);
    }
    return out;
}

} // namespace

// ============================================================================
// Request body builders
// ============================================================================

std::string build_anthropic_request_body(std::string_view model,
                                         std::string_view system_prompt,
                                         std::string_view user_prompt,
                                         int max_tokens) {
    std::string out;
    out.reserve(256 + system_prompt.size() + user_prompt.size());
    out.append("{\"model\":");
    st::json_escape(out, model.empty() ? std::string_view{kDefaultAnthropicModel} : model);
    out.append(",\"max_tokens\":");
    out.append(std::to_string(max_tokens));
    if (!system_prompt.empty()) {
        out.append(",\"system\":");
        st::json_escape(out, system_prompt);
    }
    out.append(",\"messages\":[{\"role\":\"user\",\"content\":");
    st::json_escape(out, user_prompt);
    out.append("}]}");
    return out;
}

std::string build_openai_request_body(std::string_view model,
                                      std::string_view system_prompt,
                                      std::string_view user_prompt, int max_tokens) {
    std::string out;
    out.reserve(256 + system_prompt.size() + user_prompt.size());
    out.append("{\"model\":");
    st::json_escape(out, model.empty() ? std::string_view{kDefaultOpenAiModel} : model);
    out.append(",\"max_tokens\":");
    out.append(std::to_string(max_tokens));
    out.append(",\"messages\":[");
    bool first = true;
    if (!system_prompt.empty()) {
        out.append("{\"role\":\"system\",\"content\":");
        st::json_escape(out, system_prompt);
        out.append("}");
        first = false;
    }
    if (!first)
        out.append(",");
    out.append("{\"role\":\"user\",\"content\":");
    st::json_escape(out, user_prompt);
    out.append("}");
    out.append("]}");
    return out;
}

// ============================================================================
// Response parsers
// ============================================================================
//
// Anthropic Messages response (success):
//   {"id":"msg_...","type":"message","role":"assistant",
//    "content":[{"type":"text","text":"..."}], ...}
//
// We find the first "text":"..." string that appears AFTER the
// "content":[ marker so plain top-level "type":"text" doesn't confuse
// the scan. Refusal blocks come back as type:"text" too, so a refusal
// surfaces as the response prose the same way a normal completion
// does — the user sees the model's refusal text instead of an opaque
// error.

Result<std::string> parse_anthropic_response(std::string_view body) {
    if (auto const err = find_key_after(body, "error", 0);
        err != std::string_view::npos) {
        // Error envelope — Anthropic surfaces { "error":{ "message":"..." } }.
        if (auto const msg = find_key_after(body, "message", err);
            msg != std::string_view::npos) {
            std::size_t pos = msg;
            auto sr = parse_json_string(body, pos);
            if (sr.ok) {
                return failure(ErrorCode::ParseError,
                               std::string{"Anthropic API error: "} + sr.value);
            }
        }
        return failure(ErrorCode::ParseError,
                       std::string{"Anthropic API error: "} + std::string{body});
    }
    auto const content_at = find_key_after(body, "content", 0);
    if (content_at == std::string_view::npos)
        return failure(ErrorCode::ParseError,
                       "parse_anthropic_response: no `content` key");
    auto const text_at = find_key_after(body, "text", content_at);
    if (text_at == std::string_view::npos)
        return failure(ErrorCode::ParseError,
                       "parse_anthropic_response: no `text` inside content");
    std::size_t pos = text_at;
    auto sr = parse_json_string(body, pos);
    if (!sr.ok)
        return failure(ErrorCode::ParseError,
                       "parse_anthropic_response: malformed `text` value");
    return sr.value;
}

// OpenAI ChatCompletion response (success):
//   {"id":"chatcmpl-...","choices":[{"index":0,"message":{
//        "role":"assistant","content":"..."}, "finish_reason":"stop"}], ...}

Result<std::string> parse_openai_response(std::string_view body) {
    if (auto const err = find_key_after(body, "error", 0);
        err != std::string_view::npos) {
        if (auto const msg = find_key_after(body, "message", err);
            msg != std::string_view::npos) {
            std::size_t pos = msg;
            auto sr = parse_json_string(body, pos);
            if (sr.ok) {
                return failure(ErrorCode::ParseError,
                               std::string{"OpenAI API error: "} + sr.value);
            }
        }
        return failure(ErrorCode::ParseError,
                       std::string{"OpenAI API error: "} + std::string{body});
    }
    auto const choices_at = find_key_after(body, "choices", 0);
    if (choices_at == std::string_view::npos)
        return failure(ErrorCode::ParseError,
                       "parse_openai_response: no `choices` key");
    auto const message_at = find_key_after(body, "message", choices_at);
    if (message_at == std::string_view::npos)
        return failure(ErrorCode::ParseError,
                       "parse_openai_response: no `message` inside choices");
    auto const content_at = find_key_after(body, "content", message_at);
    if (content_at == std::string_view::npos)
        return failure(ErrorCode::ParseError,
                       "parse_openai_response: no `content` inside message");
    std::size_t pos = content_at;
    auto sr = parse_json_string(body, pos);
    if (!sr.ok)
        return failure(ErrorCode::ParseError,
                       "parse_openai_response: malformed `content` value");
    return sr.value;
}

// ============================================================================
// curl shell-out
// ============================================================================

Result<std::string> http_post_via_curl(std::string_view url,
                                       std::vector<std::string> const &headers,
                                       std::string_view body) {
    if (body.size() > kMaxBody)
        return failure(ErrorCode::InvalidArgument,
                       "http_post_via_curl: body exceeds 1 MiB cap");

    // Write body to a temp file so we can pass it to curl via
    // --data-binary @<path>. Avoids cmdline-length limits + JSON
    // shell-quoting headaches.
    auto const ts =
        std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path const tmp_path =
        std::filesystem::temp_directory_path() /
        (std::string{"subuwutuner_ai_req_"} + std::to_string(ts) + ".json");
    {
        std::ofstream out{tmp_path, std::ios::binary};
        if (!out)
            return failure(ErrorCode::IoFailure,
                           std::string{"http_post_via_curl: cannot open "} +
                               tmp_path.string());
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
    }

    // Build the command. Quote every header + the URL; the temp path
    // is fully under our control so no further sanitization is needed
    // beyond surrounding quotes.
    std::string cmd;
    cmd.reserve(256 + headers.size() * 64);
    cmd.append("curl -sS -X POST");
    for (auto const &h : headers) {
        cmd.append(" -H \"");
        // Embedded quotes in a header value would break the shell
        // quoting; providers never legitimately have them, but defend
        // anyway by collapsing them.
        for (char c : h) {
            if (c == '"')
                cmd.push_back('\'');
            else
                cmd.push_back(c);
        }
        cmd.append("\"");
    }
    cmd.append(" --data-binary @\"");
    cmd.append(tmp_path.string());
    cmd.append("\" \"");
    cmd.append(url);
    cmd.append("\"");
    // Force stderr → stdout so transport errors land in the captured
    // response. curl -sS keeps it quiet on success but surfaces errors.
    cmd.append(" 2>&1");

#if defined(_WIN32)
    std::FILE *pipe = _popen(cmd.c_str(), "r");
#else
    std::FILE *pipe = popen(cmd.c_str(), "r");
#endif
    if (pipe == nullptr) {
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        return failure(ErrorCode::IoFailure,
                       "http_post_via_curl: popen failed (is curl on PATH?)");
    }
    std::string out = slurp_pipe(pipe);
#if defined(_WIN32)
    int const rc = _pclose(pipe);
#else
    int const rc = pclose(pipe);
#endif
    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
    if (rc != 0) {
        std::string msg{"http_post_via_curl: curl exited "};
        msg.append(std::to_string(rc));
        if (!out.empty()) {
            msg.append(" — ");
            msg.append(out);
        }
        return failure(ErrorCode::IoFailure, std::move(msg));
    }
    return out;
}

// ============================================================================
// Backend implementations
// ============================================================================

namespace {

class StubBackend final : public Backend {
public:
    Result<std::string> complete(std::string_view, std::string_view) override {
        return std::string{
            "AI narration is disabled in this build (Settings → AI is off "
            "or no API key was provided). The rules-based diagnosis above "
            "is the same one the CLI emits."};
    }
    BackendInfo info() const override {
        BackendInfo i;
        i.name = "Stub (disabled)";
        i.locality = BackendLocality::Local;
        i.kind = BackendKind::Stub;
        return i;
    }
};

class AnthropicBackend final : public Backend {
public:
    AnthropicBackend(std::string api_key, std::string model)
        : api_key_{std::move(api_key)}, model_{std::move(model)} {
        if (model_.empty())
            model_ = kDefaultAnthropicModel;
    }
    Result<std::string> complete(std::string_view system_prompt,
                                 std::string_view user_prompt) override {
        if (api_key_.empty())
            return failure(ErrorCode::InvalidArgument,
                           "AnthropicBackend: empty API key");
        auto const body =
            build_anthropic_request_body(model_, system_prompt, user_prompt);
        std::vector<std::string> headers{
            std::string{"x-api-key: "} + api_key_,
            std::string{"anthropic-version: "} + kAnthropicVersion,
            "content-type: application/json",
        };
        auto const resp = http_post_via_curl(kAnthropicEndpoint, headers, body);
        if (!resp.has_value())
            return failure(resp.error().code(), std::string{resp.error().message()});
        return parse_anthropic_response(*resp);
    }
    BackendInfo info() const override {
        BackendInfo i;
        i.name = std::string{"Anthropic "} + model_;
        i.locality = BackendLocality::Cloud;
        i.kind = BackendKind::Anthropic;
        i.model = model_;
        return i;
    }

private:
    std::string api_key_;
    std::string model_;
};

class OpenAiBackend final : public Backend {
public:
    OpenAiBackend(std::string api_key, std::string model)
        : api_key_{std::move(api_key)}, model_{std::move(model)} {
        if (model_.empty())
            model_ = kDefaultOpenAiModel;
    }
    Result<std::string> complete(std::string_view system_prompt,
                                 std::string_view user_prompt) override {
        if (api_key_.empty())
            return failure(ErrorCode::InvalidArgument,
                           "OpenAiBackend: empty API key");
        auto const body =
            build_openai_request_body(model_, system_prompt, user_prompt);
        std::vector<std::string> headers{
            std::string{"Authorization: Bearer "} + api_key_,
            "content-type: application/json",
        };
        auto const resp = http_post_via_curl(kOpenAiEndpoint, headers, body);
        if (!resp.has_value())
            return failure(resp.error().code(), std::string{resp.error().message()});
        return parse_openai_response(*resp);
    }
    BackendInfo info() const override {
        BackendInfo i;
        i.name = std::string{"OpenAI "} + model_;
        i.locality = BackendLocality::Cloud;
        i.kind = BackendKind::OpenAI;
        i.model = model_;
        return i;
    }

private:
    std::string api_key_;
    std::string model_;
};

} // namespace

std::unique_ptr<Backend> make_stub_backend() {
    return std::make_unique<StubBackend>();
}

std::unique_ptr<Backend> make_anthropic_backend(std::string api_key, std::string model) {
    return std::make_unique<AnthropicBackend>(std::move(api_key), std::move(model));
}

std::unique_ptr<Backend> make_openai_backend(std::string api_key, std::string model) {
    return std::make_unique<OpenAiBackend>(std::move(api_key), std::move(model));
}

} // namespace st::ai

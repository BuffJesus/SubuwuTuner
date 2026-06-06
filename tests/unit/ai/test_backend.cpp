// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// Backend trait coverage — request body shapes + response parsing.
// The HTTPS round-trip itself is not tested here (curl + a real API
// key + network). Tag [.live] is reserved for opt-in live tests that
// CI deliberately skips.

#include "st/ai/backend.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace ai = st::ai;

TEST_CASE("build_anthropic_request_body emits a minimal valid request",
          "[ai][backend][request]") {
    auto const body = ai::build_anthropic_request_body(
        "claude-opus-4-7", "you are an ECU drift narrator", "LTFT mean = -7%",
        512);
    REQUIRE(body.find("\"model\":\"claude-opus-4-7\"") != std::string::npos);
    REQUIRE(body.find("\"max_tokens\":512") != std::string::npos);
    REQUIRE(body.find("\"system\":\"you are an ECU drift narrator\"") !=
            std::string::npos);
    REQUIRE(body.find("\"messages\":[{\"role\":\"user\",\"content\":\"LTFT mean = -7%\"}]") !=
            std::string::npos);
}

TEST_CASE("build_anthropic_request_body defaults to claude-opus-4-7 on empty model",
          "[ai][backend][request][default]") {
    auto const body = ai::build_anthropic_request_body("", "sys", "user");
    REQUIRE(body.find("\"model\":\"claude-opus-4-7\"") != std::string::npos);
}

TEST_CASE("build_anthropic_request_body escapes embedded quotes + newlines",
          "[ai][backend][request][escape]") {
    auto const body = ai::build_anthropic_request_body(
        "m", "sys with \"quote\"", "user with\nnewline + \"q\"");
    REQUIRE(body.find("\\\"quote\\\"") != std::string::npos);
    REQUIRE(body.find("\\nnewline") != std::string::npos);
    // No raw newline survived into the wire body.
    REQUIRE(body.find('\n') == std::string::npos);
}

TEST_CASE("build_anthropic_request_body omits system when empty",
          "[ai][backend][request][edge]") {
    auto const body = ai::build_anthropic_request_body("m", "", "u");
    REQUIRE(body.find("\"system\"") == std::string::npos);
    REQUIRE(body.find("\"messages\":[{\"role\":\"user\"") != std::string::npos);
}

TEST_CASE("build_openai_request_body emits a minimal valid request",
          "[ai][backend][request]") {
    auto const body = ai::build_openai_request_body(
        "gpt-4o", "you are an ECU drift narrator", "LTFT mean = -7%", 512);
    REQUIRE(body.find("\"model\":\"gpt-4o\"") != std::string::npos);
    REQUIRE(body.find("\"max_tokens\":512") != std::string::npos);
    REQUIRE(body.find("\"role\":\"system\"") != std::string::npos);
    REQUIRE(body.find("\"role\":\"user\"") != std::string::npos);
}

TEST_CASE("build_openai_request_body defaults to gpt-4o on empty model",
          "[ai][backend][request][default]") {
    auto const body = ai::build_openai_request_body("", "sys", "user");
    REQUIRE(body.find("\"model\":\"gpt-4o\"") != std::string::npos);
}

TEST_CASE("build_openai_request_body skips system role when empty",
          "[ai][backend][request][edge]") {
    auto const body = ai::build_openai_request_body("m", "", "u");
    REQUIRE(body.find("\"role\":\"system\"") == std::string::npos);
    REQUIRE(body.find("\"role\":\"user\"") != std::string::npos);
}

TEST_CASE("parse_anthropic_response extracts content text",
          "[ai][backend][response]") {
    std::string const body =
        R"({"id":"msg_01abc","type":"message","role":"assistant",)"
        R"("content":[{"type":"text","text":"The LTFT drift suggests a vacuum leak."}],)"
        R"("model":"claude-opus-4-7","stop_reason":"end_turn"})";
    auto const r = ai::parse_anthropic_response(body);
    REQUIRE(r.has_value());
    REQUIRE(*r == "The LTFT drift suggests a vacuum leak.");
}

TEST_CASE("parse_anthropic_response unescapes \\n and \\\"",
          "[ai][backend][response][escape]") {
    std::string const body =
        R"({"id":"msg","type":"message","role":"assistant",)"
        R"("content":[{"type":"text","text":"line one\nline \"two\""}]})";
    auto const r = ai::parse_anthropic_response(body);
    REQUIRE(r.has_value());
    REQUIRE(*r == "line one\nline \"two\"");
}

TEST_CASE("parse_anthropic_response surfaces error envelopes",
          "[ai][backend][response][error]") {
    std::string const body =
        R"({"type":"error","error":{"type":"invalid_request_error",)"
        R"("message":"x-api-key header is required"}})";
    auto const r = ai::parse_anthropic_response(body);
    REQUIRE_FALSE(r.has_value());
    auto const msg = std::string{r.error().message()};
    REQUIRE(msg.find("x-api-key header is required") != std::string::npos);
}

TEST_CASE("parse_anthropic_response rejects malformed shape",
          "[ai][backend][response][error]") {
    auto const r = ai::parse_anthropic_response(R"({"foo":"bar"})");
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("parse_openai_response extracts choices[0].message.content",
          "[ai][backend][response]") {
    std::string const body =
        R"({"id":"chatcmpl-x","object":"chat.completion","choices":[)"
        R"({"index":0,"message":{"role":"assistant","content":"Suspected vacuum leak."},)"
        R"("finish_reason":"stop"}]})";
    auto const r = ai::parse_openai_response(body);
    REQUIRE(r.has_value());
    REQUIRE(*r == "Suspected vacuum leak.");
}

TEST_CASE("parse_openai_response surfaces error envelopes",
          "[ai][backend][response][error]") {
    std::string const body =
        R"({"error":{"message":"Incorrect API key provided","type":"invalid_request_error"}})";
    auto const r = ai::parse_openai_response(body);
    REQUIRE_FALSE(r.has_value());
    auto const msg = std::string{r.error().message()};
    REQUIRE(msg.find("Incorrect API key provided") != std::string::npos);
}

TEST_CASE("make_stub_backend returns a stable disabled-mode response",
          "[ai][backend][stub]") {
    auto b = ai::make_stub_backend();
    auto const r = b->complete("sys", "user");
    REQUIRE(r.has_value());
    REQUIRE(r->find("disabled") != std::string::npos);
    auto const info = b->info();
    REQUIRE(info.kind == ai::BackendKind::Stub);
    REQUIRE(info.locality == ai::BackendLocality::Local);
}

TEST_CASE("make_anthropic_backend BackendInfo reports cloud + model id",
          "[ai][backend][info]") {
    auto b = ai::make_anthropic_backend("fake-key", "claude-opus-4-7");
    auto const info = b->info();
    REQUIRE(info.kind == ai::BackendKind::Anthropic);
    REQUIRE(info.locality == ai::BackendLocality::Cloud);
    REQUIRE(info.model == "claude-opus-4-7");
    REQUIRE(info.name.find("Anthropic") != std::string::npos);
    REQUIRE(info.name.find("claude-opus-4-7") != std::string::npos);
}

TEST_CASE("make_openai_backend BackendInfo reports cloud + model id",
          "[ai][backend][info]") {
    auto b = ai::make_openai_backend("fake-key", "gpt-4o");
    auto const info = b->info();
    REQUIRE(info.kind == ai::BackendKind::OpenAI);
    REQUIRE(info.locality == ai::BackendLocality::Cloud);
    REQUIRE(info.model == "gpt-4o");
    REQUIRE(info.name.find("OpenAI") != std::string::npos);
    REQUIRE(info.name.find("gpt-4o") != std::string::npos);
}

TEST_CASE("make_anthropic_backend defaults to claude-opus-4-7 when model empty",
          "[ai][backend][info][default]") {
    auto b = ai::make_anthropic_backend("fake-key", "");
    REQUIRE(b->info().model == "claude-opus-4-7");
}

TEST_CASE("make_openai_backend defaults to gpt-4o when model empty",
          "[ai][backend][info][default]") {
    auto b = ai::make_openai_backend("fake-key", "");
    REQUIRE(b->info().model == "gpt-4o");
}

TEST_CASE("Anthropic backend short-circuits on empty API key",
          "[ai][backend][short-circuit]") {
    auto b = ai::make_anthropic_backend("", "claude-opus-4-7");
    auto const r = b->complete("sys", "user");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

TEST_CASE("OpenAI backend short-circuits on empty API key",
          "[ai][backend][short-circuit]") {
    auto b = ai::make_openai_backend("", "gpt-4o");
    auto const r = b->complete("sys", "user");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code() == st::ErrorCode::InvalidArgument);
}

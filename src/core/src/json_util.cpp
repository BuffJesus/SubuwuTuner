// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "st/core/json_util.hpp"

#include <cstdio>

namespace st {

void json_escape(std::string &out, std::string_view s) {
    out.push_back('"');
    for (char ch : s) {
        auto const u = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (u < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04X", u);
                out.append(buf);
            } else {
                out.push_back(ch);
            }
        }
    }
    out.push_back('"');
}

} // namespace st

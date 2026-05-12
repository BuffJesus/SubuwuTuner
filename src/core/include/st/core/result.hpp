// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubaruTuner Authors

#ifndef ST_CORE_RESULT_HPP
#define ST_CORE_RESULT_HPP

#include "st/core/error.hpp"

#include <expected>
#include <string>
#include <utility>

namespace st {

// Result<T> = std::expected<T, Error>. We re-export rather than alias so the
// helper factories below live in `st::` and pick up the right Error type by
// default.
template <typename T>
using Result = std::expected<T, Error>;

using Status = Result<void>;

// `failure(...)` builds a std::unexpected<Error>. The two-arg form takes a
// message; the one-arg form uses the default message for that code.
[[nodiscard]] inline std::unexpected<Error> failure(ErrorCode code) {
    return std::unexpected<Error>{Error{code}};
}

[[nodiscard]] inline std::unexpected<Error> failure(ErrorCode code, std::string message) {
    return std::unexpected<Error>{Error{code, std::move(message)}};
}

[[nodiscard]] inline std::unexpected<Error> failure(Error err) {
    return std::unexpected<Error>{std::move(err)};
}

// `ok()` for Status. For Result<T> just return the value directly.
[[nodiscard]] inline Status ok() noexcept { return Status{}; }

} // namespace st

// ST_TRY: monadic-style unwrap. Evaluates `expr`; if it holds an Error,
// returns it from the enclosing function (which must also be Result/Status).
// Otherwise binds the value to `name`. Uses GCC/Clang statement expressions;
// the MSVC path falls back to a verbose pattern caller must write by hand.
//
// Usage:
//   auto bytes = ST_TRY(read_file(path));
#if defined(__GNUC__) || defined(__clang__)
#  define ST_TRY(expr)                                                                              \
      __extension__({                                                                               \
          auto _st_try_tmp = (expr);                                                                \
          if (!_st_try_tmp.has_value()) {                                                           \
              return ::st::failure(std::move(_st_try_tmp).error());                                 \
          }                                                                                         \
          std::move(_st_try_tmp).value();                                                           \
      })
#endif

#endif // ST_CORE_RESULT_HPP

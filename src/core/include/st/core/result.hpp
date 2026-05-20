// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#ifndef ST_CORE_RESULT_HPP
#define ST_CORE_RESULT_HPP

#include "st/core/error.hpp"

#include <string>
#include <utility>

// Feature-detect std::expected. C++23 introduced it (P0323), but Apple Clang's
// libc++ shipped it late, and some library configurations under -std=c++23
// still don't expose it. When that's the case we fall back to TartanLlama's
// tl::expected, which is C++11-compatible and behaviourally compatible enough
// for our usage.
#if defined(__has_include)
#    if __has_include(<expected>)
#        include <expected>
#        if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#            define ST_USE_STD_EXPECTED 1
#        endif
#    endif
#endif

#ifndef ST_USE_STD_EXPECTED
#    define ST_USE_STD_EXPECTED 0
#    include <tl/expected.hpp>
#endif

namespace st {

#if ST_USE_STD_EXPECTED
template<typename T, typename E>
using expected = std::expected<T, E>;

template<typename E>
using unexpected = std::unexpected<E>;
#else
template<typename T, typename E>
using expected = tl::expected<T, E>;

template<typename E>
using unexpected = tl::unexpected<E>;
#endif

template<typename T>
using Result = expected<T, Error>;

using Status = Result<void>;

[[nodiscard]] inline unexpected<Error> failure(ErrorCode code) {
    return unexpected<Error>{Error{code}};
}

[[nodiscard]] inline unexpected<Error> failure(ErrorCode code, std::string message) {
    return unexpected<Error>{Error{code, std::move(message)}};
}

[[nodiscard]] inline unexpected<Error> failure(Error err) {
    return unexpected<Error>{std::move(err)};
}

[[nodiscard]] inline Status ok() noexcept {
    return Status{};
}

} // namespace st

// ST_TRY: monadic-style unwrap. Evaluates `expr`; if it holds an Error,
// returns it from the enclosing function (which must also be Result/Status).
// Otherwise binds the value to `name`. Uses GCC/Clang statement expressions;
// callers on MSVC fall back to manual has_value() / error() / value() checks.
//
// Usage:
//   auto bytes = ST_TRY(read_file(path));
#if defined(__GNUC__) || defined(__clang__)
#    define ST_TRY(expr)                                                                           \
        __extension__({                                                                            \
            auto _st_try_tmp = (expr);                                                             \
            if (!_st_try_tmp.has_value()) {                                                        \
                return ::st::failure(std::move(_st_try_tmp).error());                              \
            }                                                                                      \
            std::move(_st_try_tmp).value();                                                        \
        })
#endif

#endif // ST_CORE_RESULT_HPP

// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors

#include "zip_archive.hpp"

#include "st/core/error.hpp"

#include "miniz.h"

#include <utility>

namespace st::defs::detail {

struct ZipArchive::Impl {
    std::filesystem::path path;
    mz_zip_archive archive{};

    Impl() = default;
    Impl(Impl const &) = delete;
    Impl &operator=(Impl const &) = delete;
    Impl(Impl &&) = delete;
    Impl &operator=(Impl &&) = delete;
    ~Impl() {
        mz_zip_reader_end(&archive);
    }
};

ZipArchive::ZipArchive(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}

ZipArchive::ZipArchive(ZipArchive &&) noexcept = default;
ZipArchive &ZipArchive::operator=(ZipArchive &&) noexcept = default;
ZipArchive::~ZipArchive() = default;

Result<ZipArchive> ZipArchive::open(std::filesystem::path const &path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return failure(ErrorCode::InvalidArgument,
                       std::string{"zip_archive: file does not exist: "} +
                           path.string());
    }
    auto impl = std::make_unique<Impl>();
    impl->path = path;
    // miniz takes UTF-8 byte paths on Windows but std::filesystem stores
    // wide on Win32. Use string() which yields the native UTF-8/ANSI;
    // for the tested-clean fs paths this is fine. If we ever need wide
    // path support, mz_zip_reader_init_file is the swap.
    std::string const native = path.string();
    if (!mz_zip_reader_init_file(&impl->archive, native.c_str(), 0)) {
        auto const err = mz_zip_get_last_error(&impl->archive);
        return failure(ErrorCode::ParseError,
                       std::string{"zip_archive: miniz failed to open "} +
                           native + ": " +
                           mz_zip_get_error_string(err));
    }
    return ZipArchive{std::move(impl)};
}

std::vector<std::string> ZipArchive::entries() const {
    std::vector<std::string> out;
    auto &arch = impl_->archive;
    auto const n = mz_zip_reader_get_num_files(&arch);
    out.reserve(static_cast<std::size_t>(n));
    for (mz_uint i = 0; i < n; ++i) {
        if (mz_zip_reader_is_file_a_directory(&arch, i)) {
            continue;
        }
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&arch, i, &st)) {
            continue;
        }
        out.emplace_back(st.m_filename);
    }
    return out;
}

Result<std::string> ZipArchive::read_entry_string(std::string_view name) const {
    auto &arch = impl_->archive;
    // miniz expects a NUL-terminated name. string_view doesn't carry that
    // guarantee, so copy.
    std::string const key{name};
    int const idx = mz_zip_reader_locate_file(&arch, key.c_str(), nullptr, 0);
    if (idx < 0) {
        return failure(ErrorCode::FileNotFound,
                       std::string{"zip_archive: entry not found: "} + key);
    }
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&arch, static_cast<mz_uint>(idx), &st)) {
        return failure(ErrorCode::ParseError,
                       std::string{"zip_archive: file_stat failed for "} + key);
    }
    std::string out;
    out.resize(static_cast<std::size_t>(st.m_uncomp_size));
    if (st.m_uncomp_size == 0) {
        return out;
    }
    if (!mz_zip_reader_extract_to_mem(&arch, static_cast<mz_uint>(idx),
                                       out.data(), out.size(), 0)) {
        auto const err = mz_zip_get_last_error(&arch);
        return failure(ErrorCode::ParseError,
                       std::string{"zip_archive: extract failed for "} + key +
                           ": " + mz_zip_get_error_string(err));
    }
    return out;
}

} // namespace st::defs::detail

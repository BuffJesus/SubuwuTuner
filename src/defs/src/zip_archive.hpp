// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The SubuwuTuner Authors
//
// st::defs::detail::ZipArchive — RAII wrapper around miniz's read-side API.
//
// We use this only to read per-platform <platform>.zip archives produced by
// tools/build_definition_archives.py. The reader is **read-only**: no
// modify, no write, no streaming. The pack TOMLs inside an archive are at
// most a few hundred KB each, so we slurp entire entries into memory.
//
// Threading: each ZipArchive instance is unique-owned and not safe for
// concurrent access from multiple threads. The PackRegistry owns a
// per-platform ZipArchive and serializes access through its own mutex if
// it ever exposes concurrent loads (currently single-threaded callers
// only — load_pack is called once per pack open).

#ifndef ST_DEFS_DETAIL_ZIP_ARCHIVE_HPP
#define ST_DEFS_DETAIL_ZIP_ARCHIVE_HPP

#include "st/core/result.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace st::defs::detail {

class ZipArchive {
public:
    // Open the archive at `path`. Returns failure(InvalidArgument) if the
    // file is missing, failure(ParseError) if miniz cannot read the
    // central directory.
    [[nodiscard]] static Result<ZipArchive> open(std::filesystem::path const &path);

    // List every regular-file entry in the archive (directories and
    // zero-length placeholders are skipped). Order matches the central
    // directory order (lexicographic by archived file name in zips
    // produced by tools/build_definition_archives.py).
    [[nodiscard]] std::vector<std::string> entries() const;

    // Slurp `name`'s decompressed contents into a string. Returns
    // failure(NotFound) if the entry is not in the archive,
    // failure(ParseError) if miniz cannot decompress.
    [[nodiscard]] Result<std::string> read_entry_string(std::string_view name) const;

    ZipArchive() = delete;
    ZipArchive(ZipArchive const &) = delete;
    ZipArchive &operator=(ZipArchive const &) = delete;
    ZipArchive(ZipArchive &&) noexcept;
    ZipArchive &operator=(ZipArchive &&) noexcept;
    ~ZipArchive();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit ZipArchive(std::unique_ptr<Impl> impl) noexcept;
};

} // namespace st::defs::detail

#endif // ST_DEFS_DETAIL_ZIP_ARCHIVE_HPP

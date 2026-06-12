# AP3 host→AP REQUEST fixtures (analyst response, 2026-06-11)

These fixtures capture the byte sequences the **host sends to the AP** for the
file-vault commands. They were generated from the analyst's verified-live
toolkit (Frida-captured + live-AP-replay-tested) and unblock the SubuwuTuner
implementer after the 2026-06-11 live-test daze.

If you read only one thing in this file, read the **"Format mismatch summary"**
table below.

## Background

The implementer's handoff
(`findings/handoffs/HANDOFF-to-analyst-2026-06-11-out-fileinfo2-format-mismatch.md`)
reported that their encoder was pinned byte-for-byte against
`fileinfo2_known_good.bin`, but sending those bytes to the live AP dazed it
(spec §4.2). The implementer hypothesized — correctly — that
`fileinfo2_known_good.bin` is the AP **response** shape, not the host
**request** shape.

It's worse than that: `fileinfo2_known_good.bin` was generated from the spec
§7 description, which is itself wrong. The spec says **uleb128** string-length
prefixes; the on-wire format uses **u32 LE**. The "negative test" fixture
`fileinfo2_malformed_u32le.bin` is, in fact, **the shape the AP accepts** —
the labels are inverted.

These new request fixtures replace `fileinfo2_known_good.bin` as the source
of truth for the encoder. The implementer should pin their FileInfo2 encoder
against `cmd20_readfile_setup_request_known_good.bin` (or any of the
FileInfo2-bearing fixtures below), not the legacy file.

## Fixtures

All fixtures are complete packets — sync magic through CRC trailer. Send the
whole file to the AP's bulk OUT endpoint as-is.

| File | Cmd | Body kind | Input parameters | Size |
|---|---|---|---|---|
| `cmd20_backupcksum_request_known_good.bin` | 0x20 ReadFile setup | FileInfo2 | `name="backupcksum"`, `path="backupcksum"`, mtime=0, size=0 | 105B |
| `cmd20_readfile_setup_request_known_good.bin` | 0x20 ReadFile setup | FileInfo2 | `name="test.ptm"`, `path="/maps/"`, mtime=0, size=0 | 97B |
| `cmd21_getfile_request_known_good.bin` | 0x21 GetFile (step 2) | string | `local_path="C:/tmp/test.ptm"` | 65B |
| `cmd22_putfile_setup_request_known_good.bin` | 0x22 PutFile setup | FileInfo2 | `name="test.ptm"`, `path="maps/test.ptm"`, mtime=0x12345678, size=0 | 104B |
| `cmd25_removefile_request_known_good.bin` | 0x25 RemoveFile | FileInfo2 | `name="test.ptm"`, `path="maps/test.ptm"`, mtime=0, size=0 | 104B |
| `cmd26_listfiles_request_known_good.bin` | 0x26 ListFiles | **string, not FileInfo2** | `dir="maps"` | 54B |

**Cross-check:** the `backupcksum` fixture's CRC is `0x547a5f4a`. This matches
the value the analyst's `ap_readfile.py` self-test expects against a live
Frida capture of APManager. If your encoder produces a different CRC for the
same inputs, your body shape is wrong even if the byte count happens to
match.

## Wire format — host→AP, FileInfo2-bearing commands (0x20, 0x22, 0x25)

```
[02 00]                                ← sync magic
[u24 BE]                               ← wire_len = len(body) + 4
[00]                                   ← reserved
[cmd byte]                             ← 0x20 / 0x22 / 0x25

[boost envelope, 35 bytes]:
    [u32 LE = 22]                      ← length of "serialization::archive"
    [b"serialization::archive"]        ← 22 bytes
    [03 04 04 04 08]                   ← Boost format constants
    [u32 LE = 1]                       ← objcount = 1

[FileInfo2 class registration, 2 bytes]:
    [00 01]

[FileInfo2 body]:
    [u32 LE name_len] [name UTF-8 bytes]
    [27-byte metadata]:
        [u32 LE mtime] [u32 LE size] [19 zero bytes]    ← cmd 0x22 PutFile
        OR [27 zero bytes]                              ← cmd 0x20 / 0x25
    [u32 LE path_len] [path UTF-8 bytes]

[u32 BE CRC]                           ← CRC32-reflected of (header + body + 4 zero bytes)
```

## Wire format — host→AP, string-bearing commands (0x21, 0x26)

cmd 0x21 (the second step of ReadFile) and cmd 0x26 (ListFiles) do **not**
carry a FileInfo2. They carry a single Boost-archived UTF-8 string:

```
[02 00] [u24 BE] [00] [cmd byte]
[boost envelope, 35 bytes — same as above]
[u32 LE str_len] [str UTF-8 bytes]
[u32 BE CRC]
```

- For cmd 0x21, the string is the host's local destination path. APManager
  uses it as a session identifier; the AP doesn't write to it. Any non-empty
  string works.
- For cmd 0x26, the string is the directory name: `"maps"`, `"presets"`,
  `"datalog"`, or `"images"`.

**Important:** the implementer's handoff stated cmd 0x26 carries a FileInfo2
record. That is incorrect. The cmd 0x26 **response** from the AP contains a
`vector<FileInfo2>`, but the **request** is a single string.

## Format mismatch summary — implementer encoder vs actual wire

| Aspect | Implementer's current encoder | Actual wire format | Where the bug lives |
|---|---|---|---|
| Boost envelope size | 27 bytes (`u32+ "serialization::archive" + 03`) + 3 trailing zero bytes treated as "archive config" | **35 bytes**: `u32+ "serialization::archive" + 03 04 04 04 08 + u32_LE_objcount=1` | spec §12.3 "27 bytes" + `build_fixtures.py:42` |
| Class registration tag | `00 00 00` (3 bytes, labeled "class_reg_first") | `00 01` (2 bytes) | `build_fixtures.py:99` |
| String length prefix | **uleb128** | **u32 LE** | spec §7 + `build_fixtures.py:83-93` |
| Field order in FileInfo2 | `name, path, metadata` (metadata trailing) | `name, metadata, path` (metadata between) | spec §7 + `build_fixtures.py:106-114` |
| Metadata field types | `u64 LE mtime + u64 LE size + 11 zero bytes` | `u32 LE mtime + u32 LE size + 19 zero bytes` | spec §7 + `build_fixtures.py:111-113` |
| cmd 0x26 body | FileInfo2 | single Boost-archived string | implementer hypothesis in handoff |

The metadata-field-types row is subtle — the total metadata length is 27
bytes either way, but the AP firmware reads the first 8 bytes as `(u32 mtime,
u32 size)`, not `(u64 mtime)`. An encoder that uses `u64 LE mtime + u64 LE
size + 11 zeros` will set the AP's "size" field to the high 32 bits of mtime,
which on any modern timestamp is non-zero garbage. This may explain some of
the daze symptoms even if the lengths happen to line up.

## What to do with the legacy fixtures

`fileinfo2_known_good.bin` and `fileinfo2_malformed_u32le.bin` should be
treated as **misnamed**. The implementer can:

1. **Delete both** and replace `fileinfo2_known_good.bin` usage in unit tests
   with `cmd20_readfile_setup_request_known_good.bin` — strip the 7-byte
   header and 4-byte CRC trailer to recover the body, or pin the whole packet
   (preferred — exercises more of the codec).

2. Or **rename in place** for clarity:
   - `fileinfo2_known_good.bin` → `fileinfo2_uleb128_dazes_ap_negative.bin`
     — keep as a regression fixture: the encoder must NOT produce these bytes
   - `fileinfo2_malformed_u32le.bin` → keep deleted or rename to
     `fileinfo2_u32le_partial_correct_envelope_too_short.bin` — it has the
     right string-length prefix encoding but the wrong envelope; the AP also
     rejects this shape.

Either path is fine. The new request fixtures are the source of truth either
way.

## Spec §7 needs a correction

The implementer is encoding what spec §7 currently says, and the spec is
wrong. Either:

- The analyst should land a follow-up edit to `cobb-ap3-usb-protocol.md` §7
  changing `uleb128` to `u32 LE`, the field order to `name | metadata | path`,
  and the envelope to 35 bytes (not 27). The §4.2 daze example also needs
  inverting — the uleb128 shape dazes the AP, not the u32 LE shape.

- Or, until that edit lands, the implementer treats this README + the binary
  fixtures as the authoritative source and ignores spec §7.

This is queued as a follow-up. See
`findings/handoffs/HANDOFF-from-analyst-2026-06-11-fileinfo2-request-fixtures.md`
for the reply to the implementer's handoff that triggered this work.

## How these were generated

`build_request_fixtures.py` is self-contained — it inlines the analyst
toolkit's wire-format primitives (BOOST_MAGIC, FILEINFO2_CLASS_REG,
packet_crc) so the implementer can read the generator without needing to look
at the analyst-side Python. The generator asserts the `backupcksum` fixture
matches a Frida-captured reference body before emitting any fixture file, so
a future drift surfaces immediately.

Re-run with: `python build_request_fixtures.py` from this directory.

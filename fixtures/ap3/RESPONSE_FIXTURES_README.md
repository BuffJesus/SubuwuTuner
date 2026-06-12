# AP3 device-response fixtures

Pinned canonical examples of cmd 0x28 (UserInfo) and cmd 0x03 (DeviceSettings) responses from an AccessPort V3. **PII scrubbed for public-repo distribution** — the original raw captures live at `fixtures/private/ap3_live_captures/` for analyst regression testing.

## Files (public, scrubbed)

### `cmd28_userinfo_response.bin` (130 bytes)

Full response packet for cmd 0x28 (UserInfo). 7-byte header + 119-byte body + 4-byte CRC trailer. CRC has been recomputed after scrubbing — the fixture is a valid wire-format packet.

Per spec §6.13, the body layout is:

```
[0..29]    30-byte FileInfo2-style prefix
[30]       u8 archive flag = 0x08
[31..34]   u32 LE tracking ID = 1
[35..38]   u32 LE string length (= 80)
[39..118]  80 bytes ASCII payload (6 fields separated by \n):
             v1.7.6.0-28785
             AP3-SUB-004
             SUBTEST000
             Installed
             2017 USDM WRX MT CCF Gen3
             00000
```

The serial `SUBTEST000` is a placeholder; the original capture had a real device serial that was scrubbed for public distribution. Tests should pin against `SUBTEST000`.

**Recommended test:** `decode_user_info(body)` produces a struct with:
- `firmware_version = "v1.7.6.0-28785"`
- `ap_product_code = "AP3-SUB-004"`
- `ap_serial = "SUBTEST000"`
- `married = true` (because field 3 == "Installed")
- `vehicle = "2017 USDM WRX MT CCF Gen3"`

### `cmd03_devicesettings_response.bin` (147 bytes)

Full response packet for cmd 0x03 (DeviceSettings). 7-byte header + 136-byte body + 4-byte CRC trailer. CRC recomputed after scrubbing.

Per spec §15 (open question — hypothesis-pinned 2026-06-11), the body contains:

```
[0..29]    30-byte FileInfo2-style prefix
[30]       u8 archive flag = 0x08
[31..34]   u32 LE tracking ID = 1
[35]       u8 padding/separator
[36]       u8 = 0x0a
[37]       u8 AP_STATE — observed 0x01 (= Installed)
[38]       u8 CM1_STATE or related flag
[39]       u8 length prefix
[40..43]   4-byte install hash (scrubbed to 0xFFFFFFFF for public repo)
[44..47]   4-byte ASCII marker (observed "BBOC")
[48..]     length-prefixed strings (vehicle_id, language, etc.)
```

The 4-byte install-hash bytes at `[40..43]` are a per-device install token in the original capture; replaced with `FF FF FF FF` for public distribution. Tests should pin against the scrubbed value.

**Use case:** `Client::query_state` cmd 0x03 path. The `[37]` byte is the marriage-state signal; until an unmarried-AP capture confirms `0x00` there, prefer cmd 0x28 (UserInfo) as the canonical marriage source.

## Files (private, original)

`fixtures/private/ap3_live_captures/` contains the raw uncontested captures from the analyst's AP. Same format, same byte layout, but with the original device identifiers preserved. **Not for public test pinning** — used for analyst-side regression checks when working against the actual analyst hardware. Public CI never references these files.

## How to read these in tests

```cpp
std::vector<std::uint8_t> load_fixture(char const *name) {
    auto path = std::filesystem::path{"fixtures/ap3"} / name;
    std::ifstream f{path, std::ios::binary};
    return std::vector<std::uint8_t>{
        std::istreambuf_iterator<char>{f}, {}};
}

TEST_CASE("cmd 0x28 response parses correctly", "[devices][ap3]") {
    auto packet = load_fixture("cmd28_userinfo_response.bin");
    REQUIRE(packet.size() == 130);
    // Strip the 7-byte header + 4-byte CRC trailer:
    auto body = std::span{packet.data() + 7, packet.size() - 11};
    auto info = decode_user_info(body);
    REQUIRE(info.has_value());
    REQUIRE(info->firmware_version == "v1.7.6.0-28785");
    REQUIRE(info->ap_serial == "SUBTEST000");
    REQUIRE(info->married == true);
}
```

## Provenance

The original captures came from the analyst's AccessPort V3 over USB during the 2026-06-10/11 RE work. The user explicitly approved the public-repo distribution of the format + structure; the device-specific identifiers (serial, install token) have been replaced with placeholders to avoid distributing one user's PII in the project's public Apache 2.0 release.

Re-capture procedure (analyst-side): run `python ap_cli.py state` against a connected AP, capture the raw response bytes, scrub the device-specific identifiers using the procedure documented at `fixtures/private/ap3_live_captures/HOW_TO_REGENERATE.md` (analyst-side; not in public repo).

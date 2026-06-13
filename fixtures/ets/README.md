# AP3 test fixtures

These fixtures materialize the test vectors in
`specs/references/cobb-ap3-usb-protocol.md` §12 as binary files. They let
the SubuwuTuner implementer write unit tests for the AP3 codec WITHOUT
needing a live AP3 device.

## Fixtures

| File | Spec ref | What it is |
|---|---|---|
| `boost_archive_header.bin` | §12.3 | 27 bytes — the prefix every Boost-archived body begins with |
| `packet_cmd28_userinfo.bin` | §12.2 | Complete 50-byte cmd 0x28 packet, CRC = 0x4f1ff045 |
| `packet_cmd22_setup_ack.bin` | §12.5 | 12-byte cmd 0x22 ACK device-to-host packet |
| `packet_cmd23_putfile_prefix.bin` | §12.4 | First 7 bytes of cmd 0x23 for a 72173B file — wire_len 72177 BE, the load-bearing test for the u24 BE parser |
| `fileinfo2_known_good.bin` | §7 + §12.3 | A canonical FileInfo2 body for name="test.ptm" path="/maps/" |
| `fileinfo2_malformed_u32le.bin` | §4.2 + §7 | NEGATIVE test — same FileInfo2 with u32 LE string lengths instead of uleb128. The implementer's reader MUST reject this. |

## Critical tests to write against these

1. **CRC verifier.** Read `packet_cmd28_userinfo.bin`, compute CRC over `pkt[:-4] + b'\x00\x00\x00\x00'` per spec §5, compare to last 4 bytes BE. Expected: 0x4f1ff045. **If your CRC routine returns 0xb8b5c5b1 or any other value, the input-domain handling of the 4 CRC-slot bytes is wrong.**

2. **u24 BE wire_len.** Use `packet_cmd23_putfile_prefix.bin` for this. Bytes [2..4] are u24 BE = 0x0119f1 = 72177. **If your reader treats [4..5] as u16 LE, it will produce wire_len = 0xf100 = 61696 — a different value that fails any subsequent bounds check. This fixture is the load-bearing test for the u24 BE parser; the cmd 0x28 fixture's wire_len < 256 and would pass both correct and incorrect interpretations.**

3. **Boost-archive header bytes.** Hard-code these into your encoder; validate them on every incoming FileInfo2.

4. **FileInfo2 round-trip.** Read `fileinfo2_known_good.bin`, decode to (name, path, mtime, size, flags), re-encode, compare byte-identical to the fixture. **If the round-trip differs, your uleb128 encoder or your metadata layout is wrong.**

5. **FileInfo2 malformed-body rejection.** Read `fileinfo2_malformed_u32le.bin` and attempt to parse it through the same reader. **The reader MUST reject this body, NOT attempt to interpret it leniently.** This fixture has u32 LE string-length prefixes where uleb128 is required — the exact shape that dazed the live AP on 2026-06-11 (spec §4.2). A reader that "accepts and tries" this body will produce a malformed output that, if sent to a real AP, wedges the firmware until physical replug.

## How these were generated

See `build_fixtures.py`. Re-run if the fixtures get corrupted; they're
deterministic. The script asserts the cmd 0x28 packet matches the spec
§12.2 hex byte-for-byte before writing, so failures here surface a spec
drift immediately.

## What's NOT here

The 6 reference `.ptm` samples cited in spec §13.2 are owned-content
(your own tune library + acquired tunes). Per the project's data
distribution policy, they live at `fixtures/private/ap3-ptm-samples/`
on the analyst's machine; tests that depend on them skip gracefully
when absent. **Cipher round-trip tests should NOT use these public
fixtures** — generate them on demand from any locally available `.ptm`
once the implementer has the cipher source enabled.

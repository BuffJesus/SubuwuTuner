# 38 — Subaru SecurityAccess variant landscape

Cross-reference for everyone touching `st::ecu::subaru::ssmcan1_*` —
which round-key table, which S-box, which Feistel direction, which
generation, and where the bytes came from. Companion to:

- `docs/23-security-access.md` — algorithm description + plug-in seam
- `docs/37-subaru-flash-protocol.md` — flash sequence that consumes
  these keys
- `findings/re-2026-06-12-pm/cobb_sa_keys_extracted.md` — RE5b key
  extraction with cross-validation cred

## The variant matrix

ECU "generation" identifies the silicon family + Subaru's SSM era.
"Variant" identifies the install state — what installed it, which
calibration framework is overlaid. Both axes matter for SA: a single
ECU at one moment is exactly one (generation, variant) tuple, and
the SA round-key table follows from that tuple.

| Generation | Silicon | Subaru SSM | Years | Variants on file |
|---|---|---|---|---|
| Gen-A.1 | SH7055 | SSM-II | pre-2008 | Factory only (no aftermarket fleet) |
| Gen-A.2 | SH7058 | SSM-III | 2008–2014 | Factory · CobbFlash · CobbMafSd · Aftermarket · EcuTek |
| Gen-A.2 | SH-2A (LF79103P) | SSM-IV | 2015–2021 (VA WRX) | Factory · CobbFlash · CobbMafSd · Aftermarket · EcuTek |
| Gen-B | RH850 | SSM-V | 2022+ (VB WRX) | Factory · CobbFlash · CobbMafSd · Aftermarket |
| Gen-B | RH850 (newer) | SSM-VI | 2024+ | CobbFlash · CobbMafSd · Aftermarket |

Six variants × four-or-five generations, but **not every cell is
inhabited** — SSM-II has no aftermarket fleet on file, EcuTek hasn't
shipped Gen-B keys yet, etc. The implementer-side enums
(`SubaruInitVariant` × `SubaruEcuFamily`) enumerate the cells; the
runtime resolver in `Flasher::set_security_key_fn` is responsible
for picking the right (table, S-box, direction) tuple from the
(generation, variant) pair.

## What changes between variants

Three axes:

1. **Round-key table** (16 × `uint16_t`). The Feistel construction
   indexes a 16-entry table once per round. All Gen-A variants use a
   2-byte round-key; Gen-B doubles to a `uint32_t` table that
   we haven't pulled in yet.

2. **S-box** (32-entry × 4-bit lookup). Substitution table used by
   the round F function. EcuTek is the only variant that patches the
   S-box (entries 0–4); everything else uses the factory `kSBox`.

3. **Direction**. Factory uses **inverse Feistel** on the wordswapped
   wire seed (the tester unwinds the rounds the ECU ran forward).
   Aftermarket / COBB / EcuTek install a dispatcher patch that flips
   this — tester uses **forward Feistel + wordswap** on the wire
   seed directly. SSM-V factory reuses the L35 round-key set in
   reversed order, applied forward.

## The full variant catalog

Each row is callable as a `SecurityKeyFn` and selectable on the CLI
via `--sa-variant`.

| Function | CLI name | Table | S-box | Direction | Generation | Provenance |
|---|---|---|---|---|---|---|
| `ssmcan1_key_stub` | `default` / `factory` | `kSaTableL1` (= `kFeistelRoundKeysL1`) | `kSBox` | inverse + wordswap | SSM-III/IV factory | LF79103P dump @ 0x074338 |
| `ssmcan1_l1_aftermarket` | `aftermarket` / `aftermarket-l1` | `kSaTableL1Aftermarket` | `kSBox` | forward + wordswap | SSM-III/IV aftermarket | install-patched flash 0x074338 |
| `ssmcan1_l3_aftermarket` | `aftermarket-l3` | `kSaTableL35Aftermarket` (also SSM-V/VI COBB) | `kSBox` | forward + wordswap + L3 perms | SSM-III/IV aftermarket L3 | flash 0x074358 + dispatcher patches |
| `ssmcan1_l1_cobb_flash` | `cobb-flash` | `kSaTableCobbFlash` | `kSBox` | forward + wordswap | SSM-III/IV COBB-installed | `libFlashSubaru.so` RE5b |
| `ssmcan1_l1_cobb_maf_sd` | `cobb-maf-sd` | `kSaTableCobbMafSd` | `kSBox` | forward + wordswap | SSM-III/IV COBB MAF-SD mode | `libFlashSubaru.so` RE5b |
| `ssmcan1_l1_ecutek` | `ecutek` / `ecutek-l1` | `kSaTableL1` (factory!) | `kSBoxEcuTek` | forward + wordswap | SSM-III/IV EcuTek-installed | `libFlashSubaru.so:0x8b73c` |
| `ssmcan1_l35_ecutek` | `ecutek-l3` / `ecutek-l35` | `kSaTableL35` (factory!) | `kSBoxEcuTek` | forward + wordswap + L3 perms | SSM-III/IV EcuTek L3 | same |
| `ssmcan1_l1_ssmv_factory` | `ssmv-factory` | `kFeistelRoundKeysSSMVFactory` (= L35 reversed) | `kSBox` | forward + wordswap | SSM-V/VI factory | `libFlashSubaru.so` RE wave 3 §F3 |

Stub functions not yet implemented:
- `ssmk1_key_stub` — pre-2008 K-Line SSMK1 (no live K-Line capture rig
  in hand). Returns `NotImplemented`.
- `cy1_aes_key_stub` — Gen-B AES-128 SecurityAccess for RH850. The
  three universal master keys are recovered analyst-side; in-tree
  impl deferred pending an AES primitive choice + per-level plumbing.

## Cross-generation key reuse

A theme worth pinning explicitly — COBB ships **one** pair of keys
across the entire SH7058 + SH-2A + RH850 fleet they tune:

- `SSM-III COBB_CF == SSM-IV COBB_CF == kSaTableCobbFlash` (16-bit
  table, Gen-A.2 wire format)
- `SSM-III COBB_MAF_SD == SSM-IV COBB_MAF_SD == kSaTableCobbMafSd`
- `SSM-V COBB == SSM-VI COBB == kSaTableL35Aftermarket` (the Gen-A.2
  aftermarket L35 table is byte-identical to the Gen-B COBB key —
  COBB reuses the Fehr-active framework's key on Gen-B too)

Implementer side: one constant per variant covers both Gen-A.2 and
Gen-B for COBB. The `SubaruEcuFamily` enum is still load-bearing —
the wire-format width (16-bit per round-key vs 32-bit), the F
function's bit-shuffling rules, and the SA exchange's seed/key byte
width all change at the generation boundary. The reuse is at the
*round-key table content* level, not at the SA exchange protocol
level.

## Cross-validation cred

The implementer's pre-RE5b SA recovery (`kFeistelRoundKeysL35` from
the 2017 WRX LF79103P ROM dump) was independently re-extracted via
Capstone disasm of `libFlashSubaru.so:0x8b654` and decoded as LE u16
pairs — byte-identical match. Two independent extraction paths
converging is the strongest cross-check available short of live
bench-rig SA validation.

Similarly, the EcuTek S-box (`kSBoxEcuTek`) was extracted from
`libFlashSubaru.so:0x8b73c` (RE wave 5 §ε1) and the factory S-box
(`kSBox`) was independently cross-validated against the same
binary's `subaru_feistel` S-box at `0x8aa78` (RE wave 4 §δ). Two
factory-S-box extractions agreeing byte-for-byte raises confidence
in the analyst's extraction methodology for the EcuTek variant
where the implementer can't yet cross-check against a captured
(seed, key) pair.

## What's missing

- **EcuTek fixture pin**. Determinism + distinctness tests for
  EcuTek L1 + L35 cover regression; a captured (seed, key) pair from
  a live EcuTek-installed ECU would tighten this to a bytes-identical
  fixture pin. Analyst follow-up task ζ2.
- **Gen-B AES path** (RH850 SecurityAccess). The Gen-B SA uses
  AES-128 ECB with three universal master keys recovered analyst-
  side. In-tree impl is `NotImplemented` pending an AES primitive
  choice — the tiny-AES-c port that lands the `.ptm` cipher chain is
  the natural home for this, but the per-level key plumbing is
  separate work.
- **Bench-rig live validation**. None of the variants have been
  validated by issuing an actual UDS 0x27 exchange against a real
  ECU in the matching install state. The bench rig coming online
  closes this loop (docs/28 Phase 5.5).

## Cross-references

- `docs/23-security-access.md` — algorithm + plug-in seam
- `docs/37-subaru-flash-protocol.md` — flash sequence consuming
  these keys
- `src/ecu/src/subaru_security.cpp` — the round-key tables and
  wrappers
- `src/ecu/include/st/ecu/subaru_security.hpp` — the public
  function declarations callers see
- `findings/re-2026-06-12-pm/cobb_sa_keys_extracted.md` — RE5b key
  extraction provenance
- `findings/re-2026-06-12-pm/RE_wave5_findings.md §ε1` — EcuTek S-box
  differential

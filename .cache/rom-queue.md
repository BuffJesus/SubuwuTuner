# Plaintext ROM corpus / RE queue

> Inventory only. This report does not copy ROMs, edit definitions,
> run Ghidra, prove calibration semantics, or authorize ECU writes.

- Plaintext corpus files matched to packs: **126**
- Unique defined CIDs: **28**
- Unmatched binary files: **4230**
- Definition packs considered: **714**

## Best next targets

These are the highest-leverage untouched CIDs: multiple sibling images
make signature naming and table-address reconciliation substantially more
useful than a one-off decompile.

| CID | Family | Plaintext images | Definition | Existing artifacts | Why it matters |
|---|---:|---:|---|---|---|
| `EZ1GC00C` | `EZ1G` | 7 | `ez1gc00c.toml` | none | 10-image NA Forester family; good cross-CID naming corpus. |
| `EZ1GB20I` | `EZ1G` | 1 | `ez1gb20i.toml` | none | 10-image NA Forester family; good cross-CID naming corpus. |

## Family roll-up

| Family | Defined CIDs | Plaintext images | Named/indexed CIDs | Example CIDs |
|---|---:|---:|---:|---|
| `AZ1G` | 4 | 10 | 0 | AZ1G101M, AZ1G202G, AZ1G300F, AZ1G401V |
| `A2ZJ` | 3 | 3 | 0 | A2ZJ201D, A2ZJ500I, A2ZJ500M |
| `A8DH` | 3 | 26 | 3 | A8DH101I, A8DH201X, A8DH202X |
| `A2TB` | 2 | 6 | 0 | A2TB001N, A2TB100K |
| `A4TF` | 2 | 6 | 0 | A4TF800F, A4TF810F |
| `EZ1G` | 2 | 8 | 0 | EZ1GB20I, EZ1GC00C |
| `LF75` | 2 | 22 | 2 | LF75404H, LF75404S |
| `LF79` | 2 | 25 | 2 | LF79100P, LF79101P |
| `A2WC` | 1 | 1 | 0 | A2WC522S |
| `A2WF` | 1 | 1 | 0 | A2WF101K |
| `A4TH` | 1 | 3 | 0 | A4TH100H |
| `EP5G` | 1 | 1 | 1 | EP5G600A |
| `LF9D` | 1 | 1 | 1 | LF9D012H |
| `LF9G` | 1 | 6 | 1 | LF9G003T |
| `LF9L` | 1 | 5 | 1 | LF9L000E |
| `LV9N` | 1 | 2 | 0 | LV9N001D |

## Interpretation

The current supported-corpus mirror is intentionally narrow, but the
private plaintext corpus is not. The next useful RE expansion is therefore
not another isolated VA pass: it is a reusable family baseline for an older
A-series family with many sibling images, followed by definition promotion
only after byte-verified table/address evidence exists.

Recommended order:

1. EP5G600A — finish the existing analyst seed and make it the first older
   family with named functions plus a reproducible family index.
2. EZ1G / A8DH — use the sibling volume to promote shared functions and
   identify which definitions are truly stable across model years.
3. LF79 / LF9C / LF9D — deepen VA coverage only where it improves the
   tuning software: checksum/flash gates, live DIDs, and calibration roles.
4. Treat every unlisted family as research-only until its image format,
   architecture, checksum, and writable-region boundaries are independently
   established.

# `st/core/ext/` — Extension Interface Seams

> Header-only abstract interfaces that downstream modules implement against. **No dynamic loading.** **No service registry yet.** Just the design contracts that future plugins (or in-tree implementations) target.

This directory exists so the seams between major subsystems are visible, reviewed, and stable *before* a third-party plugin host is built. The idea-level inspiration comes from the plugin / service-registry pattern common in long-lived desktop applications; the C++ here is original.

See `docs/02-architecture.md` (the extension-seam discussion) for the rationale.

## Interfaces in this directory

| Header | Purpose |
|---|---|
| `validator.hpp` | Validates a byte buffer in some structural way. Returns `Diagnostic`s. |
| `checksum_strategy.hpp` | Computes / recomputes a ROM family's checksum bytes in place. |
| `transport_driver.hpp` | Sends + receives raw byte frames over a transport. |
| `kernel_descriptor.hpp` | Metadata for a per-vehicle / per-family flashing kernel. |
| `log_channel_source.hpp` | Produces timestamped samples for a single logged parameter. |
| `rom_format.hpp` | Reads + writes a ROM container format (`.bin`, `.hex`, `.srec`, ...). |
| `definition_source.hpp` | Lists + fetches definition packs from a source (filesystem, URL, registry). |
| `unit_converter.hpp` | Bidirectional conversion between display + storage representations. |
| `action_handler.hpp` | Executes a named user action. |
| `help_topic.hpp` | Provides in-app help content for a topic ID. |

All interfaces:

- Use `std::span<std::uint8_t const>` and `std::string_view` at the boundary (no downstream-module types).
- Are pure virtual abstract classes with virtual destructors.
- Return `st::Result<T>` / `st::Status` from fallible operations.
- Are `[[nodiscard]]` on non-void returns.
- Use snake_case for method names.

## How to implement an interface

```cpp
#include <st/core/ext/checksum_strategy.hpp>

namespace my_module {
class MyChecksum final : public st::ext::ChecksumStrategy {
public:
    std::string_view id() const noexcept override { return "my-family-v1"; }
    st::Result<std::uint32_t>
    compute(std::span<std::uint8_t const> bytes) const override { /* ... */ }
    st::Status
    recompute_in_place(std::span<std::uint8_t> bytes) const override { /* ... */ }
};
} // namespace my_module
```

In-tree implementations stay in their own module's `src/`. Future out-of-tree plugins would link against `st::core` only.

## How to NOT use this directory

- Do not put concrete types here. Interfaces only.
- Do not depend on downstream modules (`st::rom`, `st::flash`, `st::ecu`, …) here — that would create cycles.
- Do not add a service-registry, plugin-loader, or DLL-hosting type here. That is a separate slice that will live in its own subdirectory if and when it ships.

## Stability

These headers are intended to be source-stable across minor versions once v1.0 ships. Changes to method signatures or virtual layouts after v1.0 are ABI-breaking and need a new major version. Adding new interface methods after v1.0 should be done by adding a new pure-virtual interface (e.g. `ChecksumStrategyV2`) rather than mutating the existing one.

Pre-1.0: free to evolve. Tests pin current shapes via compile-time smoke tests in `tests/unit/core/test_ext_interfaces.cpp`.

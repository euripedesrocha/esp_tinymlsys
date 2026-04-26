# API Reference

## Namespace

All public types and functions are under `esp::memory`.

## Types

### `enum class capabilities : uint32_t`

Heap capability flags for Espressif allocation.

Available flags:

- `not_specified`
- `addressable_8bit`
- `addressable_32bit`
- `dma`
- `executable`
- `from_spiram`
- `internal`
- `retention_dma`
- `rtc`
- `simd`

Bitwise `|` is supported.

### `class resource`

`resource` derives from `std::pmr::memory_resource`.

Public surface:

- `explicit resource(capabilities requested_caps = capabilities::not_specified, std::pmr::memory_resource* upstream_resource = std::pmr::get_default_resource())`
- `capabilities caps() const noexcept`
- `std::size_t minimum_alignment() const noexcept`

Behavior:

- allocates with `heap_caps_aligned_alloc`
- frees with `std::free`
- throws `std::bad_alloc` on failure
- does not fall back to upstream allocation on failure

### `class aligned_resource<...>`

Internal building block for profile-based resources.

Public consumers should use `accelerator_resource`.

### `using accelerator_resource`

Target-aware resource for accelerator/vector-oriented memory.

Current minimum-alignment policy:

- `ESP32-S3`: 16 bytes
- `ESP32-P4`: 16 bytes
- others: `alignof(std::max_align_t)`

## Functions

### `uint32_t get_caps(capabilities cap)`

Returns the raw ESP-IDF capability bitmask.

### `std::size_t accelerator_alignment()`

Returns the same target-aware minimum alignment used by
`accelerator_resource`.

Useful for:

- `alignas(...)`
- static scratch buffers
- monotonic/pool resource backing storage
- custom allocators

### `capabilities accelerator_capabilities()`

Returns the preferred capability mask for accelerator-oriented memory.

Current behavior:

- maps to `capabilities::simd`

## Usage Pattern

```cpp
#include <memory_resource>
#include <vector>

#include "esp_memory_resource.hpp"

auto caps = esp::memory::capabilities::dma |
            esp::memory::capabilities::internal |
            esp::memory::accelerator_capabilities();

esp::memory::accelerator_resource mem{caps};
std::pmr::vector<float> values{&mem};
values.resize(256);
```

## Design Notes

- The API is intentionally small for now.
- Alignment policy is exposed as a single public construct:
  `accelerator_alignment()`.
- Additional public alignment/resource variants can be added later if real SoC
  use cases justify them.

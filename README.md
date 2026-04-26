# esp-memory

`esp-memory` is a small ESP-IDF component published as `euripedesrocha/esp_memory`
that exposes `std::pmr::memory_resource`
implementations backed by `heap_caps_*` allocation.

The current public API is intentionally small:

- `esp::memory::resource`
- `esp::memory::accelerator_resource`
- `esp::memory::accelerator_alignment()`
- `esp::memory::accelerator_capabilities()`
- `esp::memory::capabilities`

This component is meant to support higher-level DSP, ML, and systems code that
needs STL/PMR-friendly access to Espressif heap capabilities and alignment
rules.

> [!WARNING]
> This component is still a work in progress and has not been published to the
> ESP Component Registry yet.

## Requirements

- ESP-IDF `>= 6.0`
- C++23

## Install

Add the component from the ESP Component Registry:

```yaml
dependencies:
  euripedesrocha/esp_memory: "^0.1.0"
```

Or add it locally during development:

```yaml
dependencies:
  euripedesrocha/esp_memory:
    override_path: ../../path/to/esp_memory
```

## API Overview

### `esp::memory::resource`

`resource` is a `std::pmr::memory_resource` that allocates with Espressif heap
capabilities.

```cpp
#include <memory_resource>
#include <vector>

#include "esp_memory_resource.hpp"

esp::memory::resource mem{
    esp::memory::capabilities::dma | esp::memory::capabilities::internal};

std::pmr::vector<std::byte> buffer{&mem};
buffer.resize(1024);
```

### `esp::memory::accelerator_resource`

`accelerator_resource` is a `resource` with a target-aware minimum alignment
suited for vectorized or accelerator-oriented buffers.

Today the policy is:

- `ESP32-S3`: minimum 16-byte alignment
- `ESP32-P4`: minimum 16-byte alignment
- other targets: `alignof(std::max_align_t)`

The requested alignment from the caller is still honored when it is stronger
than the resource minimum.

```cpp
esp::memory::accelerator_resource mem{
    esp::memory::capabilities::dma | esp::memory::capabilities::internal |
    esp::memory::accelerator_capabilities()};
```

### `esp::memory::accelerator_alignment()`

Use this when you need the same target-aware alignment outside PMR allocation,
for example with static storage, `std::array`, monotonic buffers, or pool
backing storage.

```cpp
#include <array>
#include <cstddef>

#include "esp_memory_resource.hpp"

alignas(esp::memory::accelerator_alignment()) std::array<std::byte, 1024> scratch{};
```

## Failure Semantics

Allocation is strict.

If the requested capabilities or alignment cannot be satisfied,
`esp::memory::resource` throws `std::bad_alloc`. It does not silently fall back
to the upstream PMR resource.

That behavior is deliberate: capability-sensitive allocations should either
succeed with the requested policy or fail explicitly.

## Validation

The repo includes:

- a static-analysis and warning-policy app under [test_apps/verify](test_apps/verify)
- functional tests for real hardware under [test_apps/catch2](test_apps/catch2)

Run the static validation flow with:

```sh
idf.py -C test_apps/verify verify
```

That command runs:

- `clang-tidy` against the library
- a GCC static-analyzer build

Run the hardware-backed functional tests with:

```sh
pytest test_apps/catch2/pytest_catch2_console.py -v
```

## Example

A simple PMR example is available under [examples/memory_resource](examples/memory_resource).

## Notes

- This component is allocator-focused. It does not provide containers or DSP/ML
  algorithms by itself.
- `accelerator_capabilities()` currently maps to `MALLOC_CAP_SIMD` when the
  target and ESP-IDF expose it.
- The public namespace is `esp::memory`.

## Reference

See [docs/api.md](docs/api.md) for a compact API reference.

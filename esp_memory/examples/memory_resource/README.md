# Memory Resource Example

This example demonstrates usage of the `esp::memory::resource` family with C++ PMR containers in ESP-IDF.

- `esp::memory::resource` honors the alignment requested by the PMR container/type.
- `esp::memory::accelerator_resource` adds a target-aware minimum alignment suited for vector and accelerator-oriented buffers.
- `esp::memory::accelerator_alignment()` exposes that same target-aware alignment for `alignas(...)`, static buffers, and custom arenas.
- Allocation is strict: if the requested heap capabilities or alignment cannot be satisfied, the resource throws `std::bad_alloc` instead of falling back to the upstream PMR resource.
- The example logs a compact before/after heap summary for the requested capability mask and verifies that the returned buffer pointer satisfies `accelerator_alignment()`.

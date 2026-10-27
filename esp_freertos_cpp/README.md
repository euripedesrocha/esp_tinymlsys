# esp_freertos_cpp

C++26 wrappers for ESP-IDF FreeRTOS synchronization primitives, with automatic
resource cleanup, chrono timeouts, and support for standard C++ lock guards.

Requires ESP-IDF 6.0 or newer and a compiler supporting C++26 contracts. The
component exports its C++26 and `-fcontracts` build requirements to consumers.

| Header                                  | Facilities                                  |
| --------------------------------------- | ------------------------------------------- |
| `esp_freertos_cpp/chrono.hpp`           | `tick_duration` and `to_ticks`              |
| `esp_freertos_cpp/semaphore.hpp`        | `binary_semaphore` and `isr_release_result` |
| `esp_freertos_cpp/event_group.hpp`      | `event_group` and `clear_on_exit`           |
| `esp_freertos_cpp/critical_section.hpp` | `spinlock` for standard lock guards         |

## Ownership

Semaphores and event groups own their native handles and release them at
destruction. They are noncopyable and immovable. Construct and destroy them from
task context, and keep them alive until all tasks and callbacks using them have
finished. Allocation failure violates a contract rather than returning an empty
wrapper. Applications control their contract-violation handler.

## Binary semaphore

A `binary_semaphore` starts empty. `release()` adds a token and returns `false`
if one is already available. `acquire()` waits indefinitely and returns `false`
if FreeRTOS aborts the wait. `try_acquire()` attempts immediate acquisition;
`try_acquire_for()` waits for a chrono duration. All four operations are task-only.

```cpp
#include <chrono>
#include "esp_freertos_cpp/semaphore.hpp"

esp::freertos::binary_semaphore ready;

bool wait_for_signal() {
    return ready.try_acquire_for(std::chrono::milliseconds{100});
}
```

`binary_semaphore::release_from_isr()` returns whether release succeeded and
whether a higher-priority task was awakened. Call it only from an ISR and perform
any required ISR yield in the caller.

## Event group

`event_group` provides `set()`, `clear()`, and `get_bits()`, plus `wait_any()` and
`wait_all()` with timed variants `wait_any_for()` and `wait_all_for()`. All
operations are task-only. Masks use the lower 24 bits, and waits require a
nonzero mask.

Waits return a `std::optional<EventBits_t>` containing the observed bits when
the condition is satisfied, or `std::nullopt` otherwise. Pass
`clear_on_exit::clear` to consume the requested bits on success, or
`clear_on_exit::preserve` to leave them set.

## Chrono timeouts

`tick_duration` represents one FreeRTOS tick. `to_ticks()` rounds positive
durations upward so a positive timeout never becomes an immediate attempt.
Nonpositive durations become zero ticks. The rounded count must be below
`portMAX_DELAY`; exceeding that range violates a contract. Semaphore and
event-group timed waits use this conversion.

## Spinlock

`spinlock` owns a native `portMUX_TYPE`. Its `lock()` and `unlock()` methods enter
and exit a critical section. Use standard guards, for example:

```cpp
#include <mutex>
#include "esp_freertos_cpp/critical_section.hpp"

esp::freertos::spinlock mutex;
int count = 0;

void increment() {
    std::lock_guard guard{mutex};
    ++count;
}
```

It supports `std::lock_guard`, the basic operations of `std::unique_lock`, and
single-lock `std::scoped_lock`. Ports with nonblocking critical-section entry
also expose `try_lock()`, supporting `std::try_to_lock` and multiple-lock
`std::scoped_lock`. IDF's Linux port lacks that API, so `try_lock()` is unavailable
there. Standard multi-lock algorithms may yield on contention; keep them out of
ISRs. Keep critical sections short and do not block or yield while holding a
spinlock. A spinlock is noncopyable and immovable, and initializes without
allocating a native resource.

`lock()` and `unlock()` select the native task or ISR critical-section API for
the caller. Use from an ISR does not by itself guarantee operation while flash
cache is disabled; code and data placement must also satisfy ESP-IDF's
cache-safety requirements.

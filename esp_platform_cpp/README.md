# esp_platform_cpp

C++ resource wrappers for ESP-IDF applications. The component owns native driver
handles and exposes their configuration, operations, and error codes through C++
interfaces. It can be used directly from a FreeRTOS application.

The component currently provides `esp::platform::gptimer`, a wrapper for
ESP-IDF's general-purpose hardware timer driver. It supports counter access,
alarms, interrupt callbacks, and waiting for callbacks to finish before releasing
their state.

## Requirements

- ESP-IDF 6.0 or newer and a target with GPTimer support.
- A C++26 compiler supporting `-fcontracts`. The component enables this flag.
- The `esp_freertos_cpp` component.
- `CONFIG_GPTIMER_ISR_CACHE_SAFE` disabled. The current wrapper does not support
  this configuration.

## Add to an application

For a local checkout, add these dependencies to your application's
`main/idf_component.yml`, replacing the paths with your checkout location:

```yaml
dependencies:
  esp_platform_cpp:
    version: "*"
    override_path: /path/to/esp_tinymlsys/esp_platform_cpp
  esp_freertos_cpp:
    version: "*"
    override_path: /path/to/esp_tinymlsys/esp_freertos_cpp
```

Declare the dependency in `main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.cpp" REQUIRES esp_platform_cpp)
```

Include `esp_platform_cpp/gptimer.hpp` to use the timer. Build with
`idf.py build`; the component supplies its C++26 and contract compiler flags.
The application must define `handle_contract_violation` for contract failures.
See [native GPTimer tests](test_apps/catch2/main/test_gptimer.cpp) for wrapper
usage and the [test-app README](test_apps/catch2/README.md) for build and run
commands.

## Timer operations

Pass a `gptimer_config_t` to the constructor to select the clock source, counting
direction, resolution, interrupt priority, and native driver flags. Without a
configuration, the timer uses the default clock source and counts up at 1 MHz.

| Operation                                             | Purpose                                                                                |
| ----------------------------------------------------- | -------------------------------------------------------------------------------------- |
| `start()`, `stop()`                                   | Start or stop counting.                                                                |
| `count()`, `set_count(value)`                         | Read or set the counter.                                                               |
| `resolution_hz()`, `max_count()`                      | Query the tick rate and counter limit.                                                 |
| `ticks_for(duration)`                                 | Convert a nonnegative duration to ticks, rounding up. The result must fit the counter. |
| `set_alarm(config)`, `clear_alarm()`                  | Configure or disable an alarm.                                                         |
| `set_alarm_handler(handler)`, `clear_alarm_handler()` | Bind or unbind a borrowed callback.                                                    |
| `stop_and_join()`                                     | Stop counting and wait for pending or executing callbacks to finish.                   |

Driver operations return `std::expected` with an `esp_err_t` error. Check the
result before using its value or assuming the operation succeeded. Resource
creation and cleanup failures are checked through contracts and invoke the
application's contract-violation handler.

## Ownership and callback lifetime

The timer owns its driver handle and releases it on destruction. It cannot be
copied or moved because the interrupt callback retains the object's address.
Construct and destroy timers from task context.

The alarm handler is borrowed, not copied. It runs in interrupt context, must
be `noexcept`, and its `bool` return requests an IDF ISR yield. The handler and
everything it references must remain alive until callbacks have finished. Bind
or clear a handler only after construction or a successful `stop_and_join()`.

`stop()` alone does not establish that callbacks have finished. Before releasing
callback state, call `stop_and_join()` from task context and check its result.
Prevent other code from restarting the timer while it joins; callbacks may
update alarms but must not restart counting during that interval. The destructor
also joins callbacks. Declare the handler and its state before the timer so they
outlive it and its callbacks.

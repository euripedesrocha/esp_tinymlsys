# GPTimer tests

This Catch2 app exercises the `esp_platform_cpp` GPTimer wrapper.
[`main/test_gptimer.cpp`](main/test_gptimer.cpp) covers counter access, duration
conversion, counting direction, alarm errors, callback lifetime, counter reuse,
and joining executing or pending callbacks on another core. Multicore cases are
guarded on unicore targets. The app requires GPTimer support and does not build
with `CONFIG_GPTIMER_ISR_CACHE_SAFE` enabled.

Run from the repository root in the workspace IDF environment:

```sh
pytest esp_platform_cpp/test_apps/catch2/pytest_platform_console.py -v
pytest esp_platform_cpp/test_apps/catch2/pytest_platform_console.py --run-on target -v
```

The emulator defaults to ESP32-S3. Target mode auto-detects the connected port;
no external sensor or wiring is needed. These tests require an ESP target or
emulator and do not support host mode. Builds use `build_<target>_core`, with an
sdkconfig in that build directory and the component's contract handler supplied
by the test application.

For build-only validation, substitute `esp32p4` for `esp32c6` as needed:

```sh
idf.py -C esp_platform_cpp/test_apps/catch2 -B build_esp32c6_core \
  -DIDF_TARGET=esp32c6 -DSDKCONFIG=build_esp32c6_core/sdkconfig \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ci.esp32c6' build
```

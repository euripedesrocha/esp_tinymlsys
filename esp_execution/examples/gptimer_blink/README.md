# GPTimer interrupt-driven blink

This example uses a one-shot GPTimer alarm and hands completion from the timer ISR to a FreeRTOS worker task before toggling a WS2812 RGB LED. It repeats and logs each successful transition and the worker task name. See [gptimer_blink.cpp](main/gptimer_blink.cpp).

The default configuration targets the ESP32-S3-DevKitC-1 v1.1 RGB LED on GPIO38. For v1.0, set `ESP_EXECUTION_BLINK_GPIO` to 48 using menuconfig. `ESP_EXECUTION_BLINK_PERIOD_MS` sets the delay between toggles (default 500 ms). The target must support GPTimer, and `CONFIG_GPTIMER_ISR_CACHE_SAFE` must be disabled.

From the `esp_execution` directory:

```sh
idf.py -C examples/gptimer_blink set-target esp32s3
idf.py -C examples/gptimer_blink menuconfig
idf.py -C examples/gptimer_blink build flash monitor
```

The manifest uses the repository's local component directories and declares the LED strip dependency.

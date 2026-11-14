# Current-task blink example

This example repeatedly delays the current FreeRTOS task and toggles a WS2812 RGB LED using `just`, `then`, and `repeat`. It logs each successful on/off transition. See [blink.cpp](main/blink.cpp).

The default configuration targets the ESP32-S3-DevKitC-1 v1.1 RGB LED on GPIO38. For v1.0, set `ESP_EXECUTION_BLINK_GPIO` to 48 using menuconfig. `ESP_EXECUTION_BLINK_PERIOD_MS` sets the delay between toggles (default 500 ms) and must be at least one FreeRTOS tick.

From the `esp_execution` directory:

```sh
idf.py -C examples/blink set-target esp32s3
idf.py -C examples/blink menuconfig
idf.py -C examples/blink build flash monitor
```

The manifest uses the repository's local component directories and declares the LED strip dependency.

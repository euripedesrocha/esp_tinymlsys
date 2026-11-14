# GPTimer ping-pong between two tasks

This example alternates timer completions between two FreeRTOS task contexts. Every 500 ms it logs a ping or pong and the task name; each ping and its following pong share a round number. See [gptimer_ping_pong.cpp](main/gptimer_ping_pong.cpp).

Use a target with GPTimer support and disable `CONFIG_GPTIMER_ISR_CACHE_SAFE`. For example, from the `esp_execution` directory for an ESP32-S3:

```sh
idf.py -C examples/gptimer_ping_pong set-target esp32s3
idf.py -C examples/gptimer_ping_pong menuconfig
idf.py -C examples/gptimer_ping_pong build flash monitor
```

The example manifest uses the repository's local component directories.

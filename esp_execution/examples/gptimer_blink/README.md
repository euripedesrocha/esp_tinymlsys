# GPTimer interrupt-driven blink

Blink the ESP32-S3-DevKitC-1 RGB LED using a one-shot GPTimer alarm:

`GPTimer ISR → continues_on(blink-worker) → update LED → repeat`

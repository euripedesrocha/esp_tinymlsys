# Console Test App

This test app provides the basic Catch2 console setup for `esp_tensor`.

It is intentionally scaffold-only:

- `test`: Catch2 command registration for on-device unit tests

## Using the test app

Build and flash the app as usual. For example:

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

In the console, use `test` to interact with Catch2:

- `test -h`
- `test --list-tests`
- `test`

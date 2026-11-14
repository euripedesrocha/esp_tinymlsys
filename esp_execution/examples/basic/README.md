# Basic esp-execution example

This minimal ESP-IDF application runs `just(40) | then(...)` with an explicit receiver. On completion it logs `just(40) | then(...) completed with 42`. See [basic.cpp](main/basic.cpp).

From the `esp_execution` directory, select your chip, then build and run:

```sh
idf.py -C examples/basic set-target <target>
idf.py -C examples/basic build flash monitor
```

The example manifest uses the repository's local component directories.

# Basic esp-execution example

This minimal-build ESP-IDF application runs `just(40) | then(...)`, connects it
to a small explicit receiver, and starts the resulting operation state. The
receiver is intentionally visible so that all three completion channels remain
part of the example.

Build it with:

```sh
idf.py -C examples/basic build
```

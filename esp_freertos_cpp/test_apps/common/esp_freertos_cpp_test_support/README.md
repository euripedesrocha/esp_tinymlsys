# FreeRTOS C++ test support

This test-only component exposes `esp_freertos_cpp_test/support.hpp` in namespace
`esp_freertos_cpp_test`. It provides shared completion signaling, bounded waits,
and suspended-task cleanup for FreeRTOS tests, including execution integration
tests. It has no dependency on execution concepts or fixtures.

Timeouts abort because another task or ISR may still borrow test storage.
Cleanup waits for a task to suspend before deleting it, so completion signaling
cannot release borrowed storage while its producer is still using it.

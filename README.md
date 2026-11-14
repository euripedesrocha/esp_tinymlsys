# esp_tinymlsys

`esp_tinymlsys` is a research repository for tiny ML systems work on Espressif
targets. The goal is to build a small, understandable stack of ESP-IDF
components that can support embedded ML experiments without turning into a
general-purpose ML runtime too early.

## Repository Shape

```text
esp_tinymlsys/
├── esp_platform_cpp/ # Native ESP-IDF resources and services
├── esp_freertos_cpp/ # FreeRTOS ownership and synchronization
├── esp_execution/ # C++26 senders, receivers, and scheduling resources
├── esp_memory/    # Memory resources and allocation policy experiments
└── esp_tensor/    # Tensor views, tensor ownership, numeric domains, kernels
```

- [esp_platform_cpp](esp_platform_cpp/README.md) provides native resource ownership,
  currently for GPTimer.
- [esp_freertos_cpp](esp_freertos_cpp/README.md) provides FreeRTOS wrappers and chrono
  adaptations.
- [esp_execution](esp_execution/README.md) provides sender/receiver algorithms and
  FreeRTOS and ESP-IDF scheduling resources.
- [esp_memory](esp_memory/README.md) provides allocation and resource policy
  primitives.
- [esp_tensor](esp_tensor/README.md) provides tensor storage, views, domains,
  arithmetic, and kernels.

## Research Scope

The project is scoped around some questions:

1. How ML systems are structured once considering embedded targets?
2. Which data structures are necessary to construct such systems?
3. How a real-time system can be built to run such workloads?
4. What are the mathematical foundations for such systems, their theory, and
   practical implications of it.
5. How the programming language can support development of such systems to
   achieve both performance and correctness.

The project is not trying to implement all ONNX operators, duplicate TensorFlow
Lite Micro, or become a production component registry package at this stage.

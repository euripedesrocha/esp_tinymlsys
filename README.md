# esp_tinymlsys

`esp_tinymlsys` is a research repository for tiny ML systems work on Espressif
targets. The goal is to build a small, understandable stack of ESP-IDF
components that can support embedded ML experiments without turning into a
general-purpose ML runtime too early.

## Repository Shape

```text
esp_tinymlsys/
├── esp_execution/ # Portable C++26 sender/receiver teaching component
├── esp_memory/    # Memory resources and allocation policy experiments
└── esp_tensor/    # Tensor views, tensor ownership, numeric domains, kernels
```

- `esp_execution` provides a small, synchronous sender/receiver lifecycle core.
- `esp_memory` provides allocation and resource policy primitives.
- `esp_tensor` provides tensor storage, views, domains, arithmetic, and kernels.

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

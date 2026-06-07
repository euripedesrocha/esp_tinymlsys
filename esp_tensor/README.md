# esp_tensor

Experimental tensor and SIMD-oriented building blocks for Espressif targets.

## Development

## TODO

The list below is split into:

- core tensor primitives
- neural-network kernels
- higher-level ONNX operators

### Core Tensor Primitives

#### Elementwise Arithmetic

- [ ] `add`
  - [ ] Affine re-quantization planning: cache operation-specific plans that
        detect identity and power-of-two scale relationships, then dispatch
        eligible terms to cheaper copy or shift-only paths instead of always
        using the general quantized-multiplier re-quantization. Apply this first
        to `add`, but keep the design reusable for other affine elementwise
        operations whose intermediate values are mapped into an output
        quantization domain. Keep each optimized rounding contract explicit and
        covered by PBT before enabling it.
- [ ] `sub`
- [ ] `mul`
- [ ] `div`
- [ ] `mod`
- [ ] `pow`
- [ ] `min`
- [ ] `max`
- [ ] `clip`
- [ ] `neg`
- [ ] `abs`
- [ ] `sqrt`
- [ ] `exp`
- [ ] `log`

#### Comparison and Selection

- [ ] `equal`
- [ ] `not_equal`
- [ ] `greater`
- [ ] `greater_or_equal`
- [ ] `less`
- [ ] `less_or_equal`
- [ ] `where`
- [ ] `argmax`
- [ ] `argmin`

#### Shape, Layout, and Indexing

- [ ] `reshape`
- [ ] `flatten`
- [ ] `transpose`
- [ ] `squeeze`
- [ ] `unsqueeze`
- [ ] `concat`
- [ ] `split`
- [ ] `slice`
- [ ] `pad`
- [ ] `gather`
- [ ] `scatter_nd`
- [ ] `expand`
- [ ] `tile`
- [ ] `shape`
- [ ] `cast`
- [ ] `depth_to_space`
- [ ] `space_to_depth`

#### Reductions and Normalization Primitives

- [ ] `reduce_sum`
- [ ] `reduce_mean`
- [ ] `reduce_max`
- [ ] `reduce_min`
- [ ] `reduce_sum_square`
- [ ] `mean`
- [ ] `layer_normalization`

### Neural-Network Kernels

#### Activations

- [ ] `relu`
- [ ] `relu6`
- [ ] `sigmoid` / `logistic`
- [ ] `hard_sigmoid`
- [ ] `hard_swish`
- [ ] `leaky_relu`
- [ ] `elu`
- [ ] `prelu`

#### Pooling and Output Transforms

- [ ] `max_pool`
- [ ] `avg_pool`
- [ ] `global_average_pool`
- [ ] `softmax`
- [ ] `log_softmax`

#### Linear Algebra

- [ ] `matmul`
- [ ] `gemm`
- [ ] `fully_connected`
- [ ] `fully_connected_per_ch`

#### Convolution Family

- [ ] `conv`
- [ ] `depthwise_conv`
- [ ] `conv_transpose`
- [ ] `batch_normalization`

### Higher-Level ONNX Operators

#### Tensor/Graph Convenience

- [ ] `resize`

#### Recurrent and Sequence

- [ ] `gru`
- [ ] `lstm`

#### Transformer-Oriented Composite Support

- [ ] attention-enabling primitive coverage

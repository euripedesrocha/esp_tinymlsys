# Property Test App

This app is a dedicated target-side endpoint for host-driven property tests.

The transport is a framed byte-stream RPC layer over the target console serial
channel.

## Running Tests

```sh
pytest pbt_test
```

Use `--pbt-examples` to control how many Hypothesis examples are generated for
each tensor case. The default is `100`:

```sh
pytest pbt_test --pbt-examples 250
```

Run RPC transport validation explicitly when changing the framing layer or the
host RPC client:

```sh
pytest pbt_test --run-rpc-transport
```

When checking that the connected target is running the same binary that was just
built, ask pytest-embedded to confirm the target ELF hash:

```sh
pytest pbt_test --confirm-target-elf-sha256 yes
```

## Hypothesis Shrinking Workflow

Hypothesis is useful because it reduces failing randomized case to a smaller
counterexample. For this app, the full loop goes through the Python strategy,
the serial RPC client, the target-side tensor operation, and the Python
assertion.

Once a failure occurs, inspect shrinking running a failing property with verbose output:

```sh
pytest pbt_test/pytest_rpc_tensor_arith.py -vv --tb=short --hypothesis-show-statistics
```

For even more detail while debugging a specific property, temporarily add
verbose Hypothesis settings around the nested `run` function:

```python
from hypothesis import Verbosity, settings

@settings(deadline=None, max_examples=100, verbosity=Verbosity.verbose)
@given(strategy.binary_elementwise_case(tensor_rpc.AFFINE_S8))
def run(case):
    ...
```

A shrunk failure reports the final replayable inputs. Example:

```text
Falsifying example: run(
    case=BinaryElementwiseCase(
        lhs=TensorDescriptor(... shape=(1,), affine=AffineDomain(...)),
        rhs=TensorDescriptor(... shape=(1,), affine=AffineDomain(...)),
        out=TensorDescriptor(... shape=(1,), affine=AffineDomain(...)),
        lhs_data=[-128],
        rhs_data=[127],
    ),
)
```

Use that output as the concrete debug case:

```python
shape = (1,)
lhs = [-128]
rhs = [127]
```

Suggested debug loop:

1. Re-run the failing test with `-vv --tb=short --hypothesis-show-statistics`.
2. Copy the final `Falsifying example`.
3. Reproduce the case as a fixed smoke test or a temporary direct RPC call.
4. Compare the target response against the Python reference before inspecting lower layers.
5. If the response is wrong, add narrow target-side instrumentation around that operation.
6. Keep the shrunk case as a regression example only if it covers a distinct bug class.

When adding lower-level strategies that draw values manually, prefer labeled
draws because labels make the final counterexample directly usable:

```python
lhs = data.draw(strategy.vector(shape, dtype=strategy.S8), label="lhs")
rhs = data.draw(strategy.vector(shape, dtype=strategy.S8), label="rhs")
```

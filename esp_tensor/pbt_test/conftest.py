from __future__ import annotations

from collections.abc import Iterator

import pytest

from rpc import RpcClient


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--pbt-examples",
        action="store",
        type=int,
        default=100,
        help="number of Hypothesis examples per property case",
    )
    parser.addoption(
        "--run-rpc-transport",
        action="store_true",
        default=False,
        help="run RPC transport validation tests",
    )


def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    skip_rpc_transport = pytest.mark.skip(reason="need --run-rpc-transport option to run")
    for item in items:
        if "rpc_transport" in item.keywords and not config.getoption("--run-rpc-transport"):
            item.add_marker(skip_rpc_transport)


@pytest.fixture
def rpc_client(dut) -> Iterator[RpcClient]:
    with RpcClient(dut.serial) as client:
        yield client

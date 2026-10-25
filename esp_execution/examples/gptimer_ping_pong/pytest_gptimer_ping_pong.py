# SPDX-FileCopyrightText: 2026 Euripedes Rocha
# SPDX-License-Identifier: Apache-2.0

import re

import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.supported_targets
@pytest.mark.generic
@idf_parametrize("target", ["esp32s3"], indirect=["target"])
def test_gptimer_ping_pong(dut: Dut) -> None:
    for turn in range(1, 5):
        ping = dut.expect(re.compile(rb"Ping (\d+) on ping"), timeout=10)
        pong = dut.expect(re.compile(rb"Pong (\d+) on pong"), timeout=10)
        assert ping.group(1) == pong.group(1) == str(turn).encode()

# SPDX-FileCopyrightText: 2026 Euripedes Rocha
# SPDX-License-Identifier: Apache-2.0

import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.supported_targets
@pytest.mark.generic
@idf_parametrize("target", ["esp32s3"], indirect=["target"])
def test_gptimer_blink(dut: Dut) -> None:
    for _ in range(4):
        dut.expect_exact("RGB LED is on on blink-worker", timeout=10)
        dut.expect_exact("RGB LED is off on blink-worker", timeout=10)

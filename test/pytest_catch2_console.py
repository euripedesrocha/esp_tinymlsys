# SPDX-FileCopyrightText: 2026
# SPDX-License-Identifier: Unlicense OR CC0-1.0

import re

import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.supported_targets
@pytest.mark.generic
@idf_parametrize("target", ["esp32s3"], indirect=["target"])
def test_catch2_console_example(dut: Dut) -> None:
    dut.expect_exact("Type 'help' to get the list of commands.")
    dut.write("test -?\n")
    dut.expect_exact("For more detailed usage please see the project docs")
    dut.write("test\n")
    dut.expect_exact("All tests passed")
    dut.expect(re.compile(r"\d+ assertions in \d+ test case[s]?\)"))

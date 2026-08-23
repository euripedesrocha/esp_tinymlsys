# SPDX-FileCopyrightText: 2026 Euripedes Rocha
# SPDX-License-Identifier: Apache-2.0

import re

import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.host_test
@idf_parametrize("target", ["linux"], indirect=["target"])
def test_catch2_linux(dut: Dut) -> None:
    dut.expect_exact("All tests passed")
    dut.expect(re.compile(r"\d+ assertions in \d+ test case[s]?\)"))


@pytest.mark.supported_targets
@pytest.mark.generic
@idf_parametrize("target", ["esp32s3"], indirect=["target"])
def test_catch2_console(dut: Dut) -> None:
    dut.expect_exact("Type 'help' to get the list of commands.")
    dut.expect_exact("esp-execution>")

    dut.write("test -?\n")
    dut.expect_exact("For more detailed usage please see the project docs")
    dut.expect_exact("esp-execution>")

    dut.write("test\n")
    dut.expect_exact("All tests passed")
    dut.expect(re.compile(r"\d+ assertions in \d+ test case[s]?\)"))
    dut.expect_exact("esp-execution>")

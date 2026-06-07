import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.supported_targets
@pytest.mark.generic
@idf_parametrize("target", ["esp32s3"], indirect=["target"])
def test_catch2_console(dut: Dut) -> None:
    dut.expect_exact("Type 'help' to get the list of commands.")
    dut.expect_exact("esp-memory>")

    dut.write("test -?\n")
    dut.expect_exact("For more detailed usage please see the project docs")
    dut.expect_exact("esp-memory>")

    dut.write("test\n")
    dut.expect_exact("All tests passed")
    dut.expect("assertions? in")
    dut.expect_exact("esp-memory>")

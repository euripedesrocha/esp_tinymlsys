# SPDX-License-Identifier: Apache-2.0
import json
import logging
import os
import re
import subprocess
from pathlib import Path
import pytest

SUMMARY = re.compile(r"\((\d+) assertions? in (\d+) test cases?\)")


def test_platform_console(request, run_on, build_dir, target, tmp_path):
    selection = "test [gptimer]"
    if run_on == "target":
        dut = request.getfixturevalue("dut")
        dut.expect_exact("esp-platform>")
        dut.write(selection + "\n")
        dut.expect_exact("All tests passed", timeout=180)
        summary = dut.expect(SUMMARY)
    else:
        build = Path(build_dir)
        tools = Path(os.environ.get("IDF_TOOLS_PATH") or Path.home() / ".espressif")
        executable = request.config.getoption("espemu_prog_path") or str(tools / "tools/esp-emulator/0.51.0/esp-emu")
        description = json.loads((build / "project_description.json").read_text())
        command = [executable, "--chip", target, "--firmware", str(build / "merged-binary.bin"),
                   "--elf", str(build / description["app_elf"]), "--inject-on", "esp-platform>",
                   "--inject", selection + "\\n", "--exit-on",
                   "test cases)"]
        log_path = tmp_path / "esp-emu.log"
        try:
            with log_path.open("w") as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=180)
        except subprocess.TimeoutExpired:
            pytest.fail(f"Emulator timed out; {log_path}\n{log_path.read_text(errors='replace')[-6000:]}", pytrace=False)
        output = re.sub(r"\x1b\[[0-9;]*m", "", log_path.read_text(errors="replace"))
        assert result.returncode == 0, output[-6000:]
        assert "All tests passed" in output, output[-6000:]
        summary = SUMMARY.search(output)
        assert summary is not None, output[-6000:]
    assertions, cases = map(int, summary.groups())
    assert assertions > 0 and cases > 0
    logging.info("Platform Catch2 passed: %d cases, %d assertions (%s)", cases, assertions, run_on)

# SPDX-FileCopyrightText: 2026 Euripedes Rocha
# SPDX-License-Identifier: Apache-2.0

import json
import logging
import os
import re
import subprocess
from pathlib import Path

import pytest


SUMMARY = re.compile(r"\((\d+) assertions? in (\d+) test cases?\)")


def test_catch2_console(request, run_on, build_dir, target, tmp_path):
    if run_on == "emulator":
        tools_path = Path(
            os.environ.get("IDF_TOOLS_PATH") or Path.home() / ".espressif"
        ).expanduser()
        executable = request.config.getoption("espemu_prog_path") or str(
            tools_path / "tools" / "esp-emulator" / "0.51.0" / "esp-emu"
        )
        build = Path(build_dir)
        description = json.loads((build / "project_description.json").read_text())
        command = [
            executable,
            "--chip",
            target,
            "--firmware",
            str(build / "merged-binary.bin"),
            "--elf",
            str(build / description["app_elf"]),
            "--inject-on",
            "esp-execution>",
            "--inject",
            "test\\n",
            "--exit-on",
            "test case",
        ]
        log_path = tmp_path / "esp-emu.log"
        try:
            with log_path.open("w") as log:
                result = subprocess.run(
                    command, stdout=log, stderr=subprocess.STDOUT, timeout=180
                )
        except subprocess.TimeoutExpired:
            pytest.fail(
                f"Emulator timed out; log: {log_path}\n"
                f"{log_path.read_text(errors='replace')[-6000:]}",
                pytrace=False,
            )
        output = re.sub(r"\x1b\[[0-9;]*m", "", log_path.read_text(errors="replace"))
        assert result.returncode == 0, (
            f"Emulator exited with {result.returncode}; log: {log_path}\n{output[-6000:]}"
        )
        assert "All tests passed" in output, (
            f"Missing successful Catch2 result; log: {log_path}\n{output[-6000:]}"
        )
        summary = SUMMARY.search(output)
        assert summary is not None, (
            f"Missing Catch2 summary; log: {log_path}\n{output[-6000:]}"
        )
    else:
        dut = request.getfixturevalue("dut")
        if run_on == "target":
            dut.expect_exact("esp-execution>")
            dut.write("test\n")
        dut.expect_exact("All tests passed", timeout=180)
        summary = dut.expect(SUMMARY)
    assertions, cases = map(int, summary.groups())
    assert cases > 0
    assert assertions > 0
    logging.info("Catch2 passed: %d test cases, %d assertions (%s)", cases, assertions, run_on)

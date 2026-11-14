# SPDX-License-Identifier: Apache-2.0
import logging
import subprocess
from pathlib import Path

import pytest


collect_ignore = ["managed_components"]


def pytest_addoption(parser):
    if getattr(parser, "_esp_idf_options_registered", False):
        return
    group = parser.getgroup("esp-idf apps")
    group.addoption(
        "--run-on", choices=("emulator", "host", "target"), default="emulator",
        help="Execution mode; this app supports emulator and target",
    )
    parser._esp_idf_options_registered = True


def pytest_configure(config):
    if config.getoption("run_on") == "host" or config.getoption("target") == "linux":
        raise pytest.UsageError("Native GPTimer tests require an ESP target or emulator")


@pytest.fixture(scope="session")
def run_on(pytestconfig):
    return pytestconfig.getoption("run_on")


@pytest.fixture(scope="session")
def target(request):
    return request.param


def pytest_generate_tests(metafunc):
    if "target" in metafunc.fixturenames:
        chip = metafunc.config.getoption("target") or "esp32s3"
        metafunc.parametrize("target", [chip], indirect=True, scope="session")


@pytest.fixture(scope="session")
def app_path(pytestconfig):
    return str(Path(pytestconfig.getoption("app_path") or Path(__file__).parent).resolve())


@pytest.fixture(scope="session")
def build_dir(pytestconfig, app_path, target):
    app = Path(app_path)
    build = (app / (pytestconfig.getoption("build_dir") or f"build_{target}_core")).resolve()
    build.mkdir(parents=True, exist_ok=True)
    defaults = [app / "sdkconfig.defaults"]
    profile = app / f"sdkconfig.ci.{target}"
    if profile.is_file():
        defaults.append(profile)
    command = ["idf.py"]
    command.extend(
        [
            "-C",
            str(app),
            "-B",
            str(build),
            f"-DIDF_TARGET={target}",
            f"-DSDKCONFIG={build / 'sdkconfig'}",
            f"-DSDKCONFIG_DEFAULTS={';'.join(map(str, defaults))}",
        ]
    )
    command.append("build")
    command.extend(["merge-bin", "-o", str(build / "merged-binary.bin")])
    log_path = build / "pytest-build.log"
    logging.info("Building %s for %s; log: %s", app.name, target, log_path)
    with log_path.open("w") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        pytest.fail(
            f"Build failed; log: {log_path}\n{log_path.read_text(errors='replace')[-6000:]}",
            pytrace=False,
        )
    return str(build)


@pytest.fixture
def embedded_services(run_on):
    return "esp,idf" if run_on == "target" else "idf"

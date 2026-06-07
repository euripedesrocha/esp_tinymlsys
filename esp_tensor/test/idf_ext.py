import os
import re
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path


def action_extensions(base_actions, project_path):
    default_target = "esp32s3"
    root_dir = Path(project_path).resolve().parent
    project_dir = Path(project_path).resolve()
    clang_tidy_config = root_dir / ".clang-tidy"
    clang_build_dir = project_dir / "build_clang"

    def run_step(title, command, env=None, check=True):
        print(f"Running {title}...")
        result = subprocess.run(command, check=False, env=env, cwd=project_dir)
        if check and result.returncode != 0:
            raise SystemExit(result.returncode)
        return result

    def resolve_esp_clang_bin():
        clang_tidy = shutil.which("clang-tidy")
        if clang_tidy is not None:
            candidate = os.path.dirname(os.path.realpath(clang_tidy))
            if "esp-clang" in candidate and os.path.exists(os.path.join(candidate, "clang")):
                return candidate

        idf_tools_path = os.environ.get("IDF_TOOLS_PATH")
        if idf_tools_path is None:
            return None

        matches = sorted((Path(idf_tools_path) / "tools" / "esp-clang").glob("*/esp-clang/bin"))
        for candidate in reversed(matches):
            if (candidate / "clang").exists():
                return str(candidate)

        return None

    def configure_with_idf(build_dir, sdkconfig_defaults, env=None):
        run_step(
            f"configure {os.path.basename(build_dir)}",
            [
                sys.executable,
                os.path.join(os.environ["IDF_PATH"], "tools", "idf.py"),
                "-B",
                build_dir,
                f"-DIDF_TARGET={default_target}",
                f"-DSDKCONFIG_DEFAULTS={sdkconfig_defaults}",
                "reconfigure",
            ],
            env=env,
        )

    def verify_files():
        source_dirs = [root_dir, project_dir / "main"]
        files = []
        for source_dir in source_dirs:
            files.extend(sorted(source_dir.glob("*.cpp")))
        return [str(path) for path in files]

    def run_clang_tidy(esp_clang_bin, env):
        command = [
            os.path.join(esp_clang_bin, "clang-tidy"),
            "--quiet",
            "--warnings-as-errors=*",
            "-p",
            str(clang_build_dir),
            f"--config-file={clang_tidy_config}",
            *verify_files(),
        ]
        output_file = project_dir / "warnings.txt"
        diagnostics = []

        print("Running clang-tidy...")
        with output_file.open("w", encoding="utf-8") as output:
            process = subprocess.Popen(
                command,
                cwd=project_dir,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )

            assert process.stdout is not None
            for line in process.stdout:
                output.write(line)
                if ": error:" in line or ": warning:" in line:
                    diagnostics.append(line.rstrip())

            process.wait()

        if not diagnostics:
            print("clang-tidy: clean")
            if process.returncode != 0:
                raise SystemExit(process.returncode)
            return

        check_counts = Counter()
        for diagnostic in diagnostics:
            match = re.search(r"\[([^\]]+)\]$", diagnostic)
            if match:
                check_counts[match.group(1).replace(",-warnings-as-errors", "")] += 1

        print(f"clang-tidy: {len(diagnostics)} diagnostic(s) in {len(check_counts)} check(s)")
        print("-" * 60)
        for check, count in check_counts.most_common():
            print(f"{count:3d}  {check}")
        print("-" * 60)
        for diagnostic in diagnostics:
            print(diagnostic)
        print(f"Full clang-tidy output: {output_file}")

        raise SystemExit(process.returncode or 1)

    def verify(action, ctx, args):
        clang_env = os.environ.copy()
        clang_env["IDF_TOOLCHAIN"] = "clang"
        esp_clang_bin = resolve_esp_clang_bin()
        if esp_clang_bin is None:
            raise SystemExit("Unable to locate Espressif clang toolchain for verify action")
        clang_env["PATH"] = esp_clang_bin + os.pathsep + clang_env["PATH"]
        clang_env["CC"] = os.path.join(esp_clang_bin, "clang")
        clang_env["CXX"] = os.path.join(esp_clang_bin, "clang++")
        clang_warning_workaround = "-Wno-unused-command-line-argument"
        clang_env["CFLAGS"] = f"{clang_env.get('CFLAGS', '')} {clang_warning_workaround}".strip()
        clang_env["CXXFLAGS"] = f"{clang_env.get('CXXFLAGS', '')} {clang_warning_workaround}".strip()

        configure_with_idf("build_clang", "sdkconfig.defaults", env=clang_env)
        run_clang_tidy(esp_clang_bin, clang_env)

    return {
        "version": "1.0.0",
        "actions": {
            "verify": {
                "callback": verify,
                "help": "Run clang-tidy verification through the test app.",
            }
        },
    }

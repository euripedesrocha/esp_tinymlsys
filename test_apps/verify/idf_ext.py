import glob
import os
import re
import shutil
import subprocess
import sys


def action_extensions(base_actions, project_path):
    default_target = "esp32s3"
    root_dir = os.path.realpath(os.path.join(project_path, "..", ".."))
    include_dir = os.path.join(root_dir, "include")
    clang_tidy_config = os.path.join(root_dir, ".clang-tidy")
    idf_py = os.path.join(os.environ["IDF_PATH"], "tools", "idf.py")
    project_dir = os.path.realpath(project_path)
    clang_build_dir = os.path.join(project_dir, "build_clang")
    analyzer_build_dir = os.path.join(project_dir, "build_gcc_analyzer")
    clang_sdkconfig = os.path.join(project_dir, "sdkconfig.clang")
    analyzer_sdkconfig = os.path.join(project_dir, "sdkconfig.gcc_analyzer")
    excluded_dirs = {
        os.path.join(project_dir, "build"),
        clang_build_dir,
        analyzer_build_dir,
        os.path.join(root_dir, "test_apps", "catch2"),
        os.path.join(root_dir, "examples"),
    }

    def run_step(title, command, env=None):
        print(f"Running {title}...")
        subprocess.run(command, check=True, env=env)

    def build_command(build_dir, sdkconfig_path, sdkconfig_defaults=None):
        command = [
            sys.executable,
            idf_py,
            "-C",
            project_dir,
            "-B",
            build_dir,
            "-D",
            f"IDF_TARGET={default_target}",
            "-D",
            f"SDKCONFIG={sdkconfig_path}",
        ]
        if sdkconfig_defaults is not None:
            command.extend(["-D", f"SDKCONFIG_DEFAULTS={sdkconfig_defaults}"])
        return command

    def prepare_build_dir(build_dir, sdkconfig_path, sdkconfig_defaults=None, env=None):
        run_step(
            f"set target for {os.path.basename(build_dir)}",
            build_command(build_dir, sdkconfig_path, sdkconfig_defaults)
            + ["set-target", default_target],
            env=env,
        )

    def resolve_esp_clang_bin():
        clang_tidy = shutil.which("clang-tidy")
        if clang_tidy is not None:
            candidate = os.path.dirname(os.path.realpath(clang_tidy))
            if "esp-clang" in candidate and os.path.exists(os.path.join(candidate, "clang")):
                return candidate

        idf_tools_path = os.environ.get("IDF_TOOLS_PATH")
        if idf_tools_path is None:
            return None

        matches = sorted(
            glob.glob(
                os.path.join(
                    idf_tools_path,
                    "tools",
                    "esp-clang",
                    "*",
                    "esp-clang",
                    "bin",
                )
            )
        )
        for candidate in reversed(matches):
            if os.path.exists(os.path.join(candidate, "clang")):
                return candidate

        return None

    def discover_translation_units():
        patterns = ("*.c", "*.cc", "*.cpp", "*.cxx")

        try:
            tracked = subprocess.check_output(
                ["git", "-C", root_dir, "ls-files", *patterns],
                text=True,
            )
            untracked = subprocess.check_output(
                ["git", "-C", root_dir, "ls-files", "--others", "--exclude-standard", *patterns],
                text=True,
            )
            candidates = [
                os.path.join(root_dir, path)
                for path in (tracked.splitlines() + untracked.splitlines())
                if path
            ]
        except (subprocess.CalledProcessError, FileNotFoundError):
            candidates = []
            for current_root, dirs, files in os.walk(root_dir):
                current_root = os.path.realpath(current_root)
                dirs[:] = [
                    directory
                    for directory in dirs
                    if os.path.join(current_root, directory) not in excluded_dirs
                ]
                for name in files:
                    if name.endswith((".c", ".cc", ".cpp", ".cxx")):
                        candidates.append(os.path.join(current_root, name))

        filtered = []
        for path in candidates:
            real_path = os.path.realpath(path)
            if any(real_path == excluded or real_path.startswith(excluded + os.sep) for excluded in excluded_dirs):
                continue
            filtered.append(real_path)

        return sorted(set(filtered))

    def library_check_regex(translation_units):
        project_prefix = project_dir + os.sep
        library_units = [
            path for path in translation_units if not path.startswith(project_prefix)
        ]
        if not library_units:
            raise RuntimeError("No library translation units found for validation")

        escaped_paths = [re.escape(path) for path in library_units]
        return f"^({'|'.join(escaped_paths)})$"

    def verify(action, ctx, args):
        translation_units = discover_translation_units()
        check_regex = library_check_regex(translation_units)

        clang_env = os.environ.copy()
        clang_env["IDF_TOOLCHAIN"] = "clang"
        esp_clang_bin = resolve_esp_clang_bin()
        if esp_clang_bin is None:
            raise RuntimeError("Unable to locate Espressif clang toolchain for verify action")
        clang_env["PATH"] = esp_clang_bin + os.pathsep + clang_env["PATH"]
        clang_env["CC"] = os.path.join(esp_clang_bin, "clang")
        clang_env["CXX"] = os.path.join(esp_clang_bin, "clang++")
        prepare_build_dir(clang_build_dir, clang_sdkconfig, "sdkconfig.defaults", env=clang_env)
        run_step(
            "clang-tidy",
            build_command(clang_build_dir, clang_sdkconfig, "sdkconfig.defaults")
            + [
                "clang-check",
                "--exit-code",
                "--run-clang-tidy-options",
                f"-config-file={clang_tidy_config}",
                "--include-paths",
                root_dir,
                check_regex,
            ],
            env=clang_env,
        )

        analyzer_defaults = "sdkconfig.defaults;sdkconfig.analyzer.defaults"
        prepare_build_dir(analyzer_build_dir, analyzer_sdkconfig, analyzer_defaults)
        run_step(
            "gcc static analyzer",
            build_command(analyzer_build_dir, analyzer_sdkconfig, analyzer_defaults)
            + ["build"],
        )

    return {
        "version": "1.0.0",
        "actions": {
            "verify": {
                "callback": verify,
                "help": "Run clang-tidy and GCC static analyzer validation for this library.",
            }
        },
    }

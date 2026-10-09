#!/usr/bin/env python3
"""Exercise install/uninstall without compiling or modifying the user's home."""

import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="monologue-install-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.project = self.directory / "project with spaces"
        self.tools = self.directory / "tools"
        self.home = self.directory / "home with spaces"
        self.os_release = self.directory / "os-release"
        self.log = self.directory / "calls"
        self.qt_prefix = self.directory / "qt"
        self.qmake_arguments = self.directory / "qmake-arguments"
        (self.project / "bin").mkdir(parents=True)
        (self.project / "pkgbuild").mkdir()
        self.tools.mkdir()
        self.home.mkdir()
        shutil.copy2(str(ROOT / "bin/install"), str(self.project / "bin/install"))
        shutil.copy2(str(ROOT / "bin/uninstall"), str(self.project / "bin/uninstall"))
        shutil.copy2(str(ROOT / "bin/qt-env"), str(self.project / "bin/qt-env"))
        for filename in ("PKGBUILD", "monologue.desktop", "monologue.svg"):
            shutil.copy2(str(ROOT / "pkgbuild" / filename), str(self.project / "pkgbuild" / filename))
        shutil.copy2(str(ROOT / "LICENSE"), str(self.project / "LICENSE"))
        for tool in ("sh", "dirname", "mkdir", "chmod", "install", "sed", "awk", "mktemp", "rm"):
            executable = shutil.which(tool)
            self.assertIsNotNone(executable, "Required test tool: " + tool)
            (self.tools / tool).symlink_to(executable)
        for tool in ("g++", "make"):
            self.write_script(self.tools / tool, "exit 0\n")
        self.write_script(
            self.tools / "qmake6",
            'if [ "$1" = "-query" ]; then\n'
            '  printf "%s\\n" "$TEST_QT_PREFIX"\n'
            'else\n'
            '  for arg do printf "%s\\n" "$arg" >> "$TEST_QMAKE_ARGS"; done\n'
            'fi\n',
        )
        self.write_script(
            self.tools / "pkg-config", "exit \"${TEST_PKG_CONFIG_STATUS:-0}\"\n"
        )
        self.write_script(
            self.tools / "makepkg",
            'printf "makepkg\\n%s\\n" "$PWD" >> "$TEST_LOG"\n'
            'for arg do printf "<%s>\\n" "$arg" >> "$TEST_LOG"; done\n'
            'exit "${TEST_MAKEPKG_STATUS:-0}"\n',
        )
        self.write_script(self.tools / "id", 'printf "%s\\n" "${TEST_UID:-1000}"\n')
        self.write_script(
            self.tools / "pacman",
            'printf "pacman\\n" >> "$TEST_LOG"\n'
            'for arg do printf "<%s>\\n" "$arg" >> "$TEST_LOG"; done\n'
            'exit "${TEST_PACMAN_STATUS:-0}"\n',
        )
        self.write_script(
            self.tools / "sudo",
            'printf "sudo\\n" >> "$TEST_LOG"\nexec "$@"\n',
        )
        self.write_script(
            self.project / "bin/build",
            'printf "build\\n" >> "$TEST_LOG"\n'
            '[ "${TEST_BUILD_STATUS:-0}" -eq 0 ] || exit "$TEST_BUILD_STATUS"\n'
            'mkdir -p "$TEST_PROJECT/build"\n'
            'printf "%s\\n" "${PKG_CONFIG:-}" > "$TEST_PROJECT/build/pkg-config"\n'
            'printf "#!/bin/sh\\nexit 0\\n" > "$TEST_PROJECT/build/monologue"\n'
            'chmod 755 "$TEST_PROJECT/build/monologue"\n',
        )
        self.environment = os.environ.copy()
        for name in ("QMAKE", "PKG_CONFIG", "XDG_DATA_HOME", "TEST_BUILD_STATUS", "TEST_PKG_CONFIG_STATUS", "TEST_MAKEPKG_STATUS", "TEST_UID", "TEST_PACMAN_STATUS"):
            self.environment.pop(name, None)
        self.environment.update(
            HOME=str(self.home),
            PATH=str(self.tools),
            MONOLOGUE_OS_RELEASE=str(self.os_release),
            TEST_PROJECT=str(self.project),
            TEST_LOG=str(self.log),
            TEST_QT_PREFIX=str(self.qt_prefix),
            TEST_QMAKE_ARGS=str(self.qmake_arguments),
        )
        self.set_distribution("fedora")

    def write_script(self, path, body):
        path.write_text("#!/bin/sh\nset -eu\n" + body)
        path.chmod(0o755)

    def use_real_build(self):
        # Mock make instead so tool selection runs through the actual build script.
        shutil.copy2(str(self.project / "bin/build"), str(self.tools / "make"))
        shutil.copy2(str(ROOT / "bin/build"), str(self.project / "bin/build"))

    def use_custom_qmake(self):
        custom = self.directory / "custom qt/bin/qmake"
        custom.parent.mkdir(parents=True)
        shutil.copy2(str(self.tools / "qmake6"), str(custom))
        (self.tools / "qmake6").unlink()
        self.environment["QMAKE"] = str(custom)
        return custom

    def set_distribution(self, identifier, like=""):
        self.os_release.write_text('ID="{}"\nID_LIKE="{}"\n'.format(identifier, like))

    def run_install(self, *arguments):
        return self.run_script("install", *arguments)

    def run_script(self, script, *arguments):
        return subprocess.run(
            [str(self.project / "bin" / script)] + list(arguments),
            cwd=str(self.directory),
            env=self.environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
        )

    def assert_not_built(self):
        self.assertFalse(self.log.exists())
        self.assertFalse((self.home / ".local/bin/monologue").exists())

    def assert_installation(self, data_home):
        binary = self.home / ".local/bin/monologue"
        desktop = data_home / "applications/monologue.desktop"
        icon = data_home / "icons/hicolor/scalable/apps/monologue.svg"
        license_file = data_home / "licenses/monologue/LICENSE"
        self.assertEqual(binary.read_bytes(), (self.project / "build/monologue").read_bytes())
        self.assertEqual(stat.S_IMODE(binary.stat().st_mode), 0o755)
        for path in (desktop, icon, license_file):
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o644)
        self.assertEqual(icon.read_bytes(), (ROOT / "pkgbuild/monologue.svg").read_bytes())
        self.assertEqual(license_file.read_bytes(), (ROOT / "LICENSE").read_bytes())
        self.assertIn("Icon=monologue\n", desktop.read_text())
        self.assertIn("Categories=AudioVideo;Video;Recorder;\n", desktop.read_text())
        self.assertEqual(self.log.read_text(), "build\n")
        self.assertEqual(list((self.project / "build").glob("monologue.desktop.*")), [])
        validator = shutil.which("desktop-file-validate")
        if validator:
            subprocess.run([validator, str(desktop)], check=True)
        return desktop

    def test_fedora_user_install(self):
        # Fedora must not invoke makepkg, even if it happens to be installed.
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        desktop = self.assert_installation(self.home / ".local/share")
        self.assertIn('Exec="{}"\n'.format(self.home / ".local/bin/monologue"), desktop.read_text())

    def test_custom_xdg_data_home(self):
        data_home = self.directory / "custom data"
        self.environment["XDG_DATA_HOME"] = str(data_home)
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_installation(data_home)
        self.assertFalse((self.home / ".local/share").exists())

    def test_empty_xdg_data_home_uses_default(self):
        self.environment["XDG_DATA_HOME"] = ""
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_installation(self.home / ".local/share")

    def test_launcher_escapes_special_characters(self):
        self.home = self.directory / 'home $dollar `tick` "quote" \\slash %f'
        self.home.mkdir()
        self.environment["HOME"] = str(self.home)
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        desktop = self.assert_installation(self.home / ".local/share")
        escaped = str(self.home / ".local/bin/monologue")
        escaped = escaped.replace("\\", "\\\\\\\\").replace('"', '\\\\"')
        escaped = escaped.replace("`", "\\\\`").replace("$", "\\\\$").replace("%", "%%")
        self.assertIn('Exec="{}"\n'.format(escaped), desktop.read_text())

    def test_rerun_replaces_existing_binary(self):
        binary = self.home / ".local/bin/monologue"
        binary.parent.mkdir(parents=True)
        binary.write_text("old binary")
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_installation(self.home / ".local/share")

    def test_fedora_derivative(self):
        self.set_distribution("derivative", "rhel fedora")
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_installation(self.home / ".local/share")

    def test_fedora_rejects_makepkg_arguments(self):
        result = self.run_install("--noconfirm")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("takes no arguments", result.stderr)
        self.assert_not_built()

    def test_installer_delegates_dependency_checks_to_build(self):
        for tool in ("g++", "make", "pkg-config", "qmake6"):
            (self.tools / tool).unlink()
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_installation(self.home / ".local/share")

    def test_missing_build_tools_report_fedora_dependencies(self):
        self.use_real_build()
        for tool in ("make", "pkg-config", "qmake6"):
            with self.subTest(tool=tool):
                path = self.tools / tool
                backup = self.tools / (tool + ".disabled")
                path.rename(backup)
                try:
                    result = self.run_install()
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(
                        "sudo dnf --refresh --setopt=install_weak_deps=False install gcc-c++",
                        result.stderr,
                    )
                    self.assert_not_built()
                finally:
                    backup.rename(path)

    def test_qmake_fallback(self):
        self.use_real_build()
        (self.tools / "qmake6").rename(self.tools / "qmake")
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_installation(self.home / ".local/share")

    def test_qt_pkg_config_takes_precedence_over_path(self):
        self.use_real_build()
        native = self.qt_prefix / "bin/pkg-config"
        native.parent.mkdir(parents=True)
        self.write_script(native, "exit 0\n")
        self.write_script(self.tools / "pkg-config", "exit 1\n")
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.project / "build/pkg-config").read_text(), str(native) + "\n")
        self.assert_installation(self.home / ".local/share")

    def test_explicit_pkg_config_override(self):
        self.use_real_build()
        native = self.qt_prefix / "bin/pkg-config"
        native.parent.mkdir(parents=True)
        self.write_script(native, "exit 1\n")
        self.environment["PKG_CONFIG"] = str(self.tools / "pkg-config")
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            (self.project / "build/pkg-config").read_text(),
            str(self.tools / "pkg-config") + "\n",
        )
        self.assert_installation(self.home / ".local/share")

    def test_missing_pkg_config_override(self):
        self.use_real_build()
        self.environment["PKG_CONFIG"] = str(self.directory / "missing-pkg-config")
        result = self.run_install()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cannot find pkg-config", result.stderr)
        self.assertIn("pkgconf-pkg-config", result.stderr)
        self.assert_not_built()

    def test_build_passes_selected_pkg_config_to_qmake(self):
        self.use_real_build()
        native = self.qt_prefix / "bin/pkg-config"
        native.parent.mkdir(parents=True)
        self.write_script(native, "exit 0\n")
        self.write_script(self.tools / "pkg-config", "exit 1\n")
        result = self.run_script("build")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.qmake_arguments.read_text().splitlines(),
            ["QMAKE_PKG_CONFIG=" + str(native), str(self.project / "monologue.pro")],
        )
        self.assertFalse((self.home / ".local").exists())

    def test_custom_qmake_does_not_require_system_qt_pkg_config_metadata(self):
        self.use_real_build()
        self.use_custom_qmake()
        self.write_script(
            self.tools / "pkg-config", 'printf "pkg-config\\n" >> "$TEST_LOG"\nexit 1\n'
        )
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.qmake_arguments.read_text().splitlines(),
            ["QMAKE_PKG_CONFIG=pkg-config", str(self.project / "monologue.pro")],
        )
        self.assert_installation(self.home / ".local/share")

    def test_custom_qmake_failure_reports_build_diagnostic_and_fedora_hint(self):
        self.use_real_build()
        custom = self.use_custom_qmake()
        self.write_script(
            custom,
            'if [ "$1" = "-query" ]; then\n'
            '  printf "%s\\n" "$TEST_QT_PREFIX"\n'
            'else\n'
            '  echo "Project ERROR: Unknown module(s) in QT: multimedia" >&2\n'
            '  exit 3\n'
            'fi\n',
        )
        result = self.run_install()
        self.assertEqual(result.returncode, 3)
        self.assertIn("Project ERROR: Unknown module(s) in QT: multimedia", result.stderr)
        self.assertIn("pulseaudio-libs-devel", result.stderr)
        self.assertIn("--setopt=install_weak_deps=False", result.stderr)
        self.assert_not_built()

    def test_relative_data_home_is_rejected(self):
        self.environment["XDG_DATA_HOME"] = "relative/data"
        result = self.run_install()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("XDG_DATA_HOME must be an absolute path", result.stderr)
        self.assert_not_built()

    def test_invalid_home_is_rejected(self):
        for home in ("", "relative/home"):
            with self.subTest(home=home):
                self.environment["HOME"] = home
                result = self.run_install()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("HOME must be an absolute path", result.stderr)
                self.assert_not_built()

    def test_failed_build_preserves_existing_installation(self):
        binary = self.home / ".local/bin/monologue"
        binary.parent.mkdir(parents=True)
        binary.write_text("old binary")
        self.environment["TEST_BUILD_STATUS"] = "7"
        result = self.run_install()
        self.assertEqual(result.returncode, 7)
        self.assertIn("sudo dnf", result.stderr)
        self.assertEqual(binary.read_text(), "old binary")
        self.assertFalse((self.home / ".local/share/applications/monologue.desktop").exists())

    def test_arch_preserves_makepkg_arguments_and_directory(self):
        self.set_distribution("arch")
        (self.tools / "qmake6").unlink()
        result = self.run_install("--noconfirm", "--skippgpcheck")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.log.read_text(),
            "build\nmakepkg\n{}\n<-fsi>\n<--noconfirm>\n<--skippgpcheck>\n".format(self.project / "pkgbuild"),
        )
        self.assertFalse((self.home / ".local").exists())

    def test_arch_derivative(self):
        self.set_distribution("omarchy", "arch")
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("makepkg\n", self.log.read_text())

    def test_arch_requires_makepkg_before_building(self):
        self.set_distribution("arch")
        (self.tools / "makepkg").unlink()
        result = self.run_install()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("makepkg is required", result.stderr)
        self.assert_not_built()

    def test_arch_propagates_makepkg_failure(self):
        self.set_distribution("arch")
        self.environment["TEST_MAKEPKG_STATUS"] = "9"
        self.assertEqual(self.run_install().returncode, 9)

    def test_unsupported_distribution(self):
        self.set_distribution("debian")
        result = self.run_install()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unsupported distribution: debian", result.stderr)
        self.assertIn("./bin/build", result.stderr)
        self.assert_not_built()

    def test_missing_distribution_metadata(self):
        self.os_release.unlink()
        result = self.run_install()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cannot detect the distribution", result.stderr)
        self.assert_not_built()

    def test_fedora_uninstall_removes_only_installed_files(self):
        result = self.run_install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.log.unlink()
        data_home = self.home / ".local/share"
        installed = (
            self.home / ".local/bin/monologue",
            data_home / "applications/monologue.desktop",
            data_home / "icons/hicolor/scalable/apps/monologue.svg",
            data_home / "licenses/monologue/LICENSE",
        )
        preserved = (
            self.home / ".local/bin/another-app",
            data_home / "applications/another-app.desktop",
            data_home / "licenses/monologue/another-file",
            data_home / "omacom/monologue/recordings/take/original.mp4",
            self.home / ".config/omacom/monologue.conf",
            self.home / "Videos/saved.mp4",
        )
        for path in preserved:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("keep me")
        result = self.run_script("uninstall")
        self.assertEqual(result.returncode, 0, result.stderr)
        for path in installed:
            self.assertFalse(path.exists())
            self.assertTrue(path.parent.is_dir())
        for path in preserved:
            self.assertEqual(path.read_text(), "keep me")
        self.assertTrue((self.project / "build/monologue").exists())
        self.assertFalse(self.log.exists())

    def test_fedora_uninstall_custom_and_empty_data_home(self):
        for value in (str(self.directory / "custom data"), ""):
            with self.subTest(data_home=value):
                self.environment["XDG_DATA_HOME"] = value
                result = self.run_install()
                self.assertEqual(result.returncode, 0, result.stderr)
                data_home = Path(value) if value else self.home / ".local/share"
                result = self.run_script("uninstall")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertFalse((self.home / ".local/bin/monologue").exists())
                self.assertFalse((data_home / "applications/monologue.desktop").exists())
                self.assertFalse((data_home / "icons/hicolor/scalable/apps/monologue.svg").exists())
                self.assertFalse((data_home / "licenses/monologue/LICENSE").exists())

    def test_fedora_uninstall_is_idempotent_without_build_tools(self):
        for tool in ("g++", "make", "qmake6", "pkg-config", "makepkg", "pacman", "sudo"):
            (self.tools / tool).unlink()
        for _ in range(2):
            result = self.run_script("uninstall")
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_not_built()
        self.assertFalse((self.home / ".local").exists())

    def test_fedora_derivative_uninstall(self):
        self.set_distribution("derivative", "rhel fedora")
        result = self.run_script("uninstall")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_not_built()

    def test_fedora_uninstall_rejects_root_and_invalid_paths(self):
        binary = self.home / ".local/bin/monologue"
        binary.parent.mkdir(parents=True)
        binary.write_text("keep me")
        cases = (
            ("TEST_UID", "0", "without sudo"),
            ("HOME", "", "HOME must be an absolute path"),
            ("HOME", "relative/home", "HOME must be an absolute path"),
            ("XDG_DATA_HOME", "relative/data", "XDG_DATA_HOME must be an absolute path"),
        )
        for name, value, message in cases:
            with self.subTest(name=name, value=value):
                environment = self.environment.copy()
                self.environment[name] = value
                try:
                    result = self.run_script("uninstall")
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(message, result.stderr)
                    self.assertEqual(binary.read_text(), "keep me")
                    self.assertFalse(self.log.exists())
                finally:
                    self.environment = environment

    def test_arch_uninstall_uses_pacman_without_removing_dependencies(self):
        for identifier, like, uid, expected in (
            ("arch", "", "1000", "sudo\npacman\n<-R>\n<-->\n<monologue>\n"),
            ("omarchy", "arch", "0", "pacman\n<-R>\n<-->\n<monologue>\n"),
        ):
            with self.subTest(distribution=identifier, uid=uid):
                self.set_distribution(identifier, like)
                self.environment["TEST_UID"] = uid
                result = self.run_script("uninstall")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.log.read_text(), expected)
                self.log.unlink()
                self.assert_not_built()

    def test_arch_uninstall_requires_pacman_and_sudo(self):
        self.set_distribution("arch")
        for tool in ("pacman", "sudo"):
            with self.subTest(tool=tool):
                path = self.tools / tool
                backup = self.tools / (tool + ".disabled")
                path.rename(backup)
                try:
                    result = self.run_script("uninstall")
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(tool, result.stderr)
                    self.assert_not_built()
                finally:
                    backup.rename(path)

    def test_arch_uninstall_propagates_pacman_failure(self):
        self.set_distribution("arch")
        self.environment["TEST_PACMAN_STATUS"] = "9"
        self.assertEqual(self.run_script("uninstall").returncode, 9)

    def test_uninstall_rejects_arguments_before_removing_files(self):
        binary = self.home / ".local/bin/monologue"
        binary.parent.mkdir(parents=True)
        binary.write_text("keep me")
        result = self.run_script("uninstall", "--purge")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no arguments", result.stderr)
        self.assertEqual(binary.read_text(), "keep me")
        self.assertFalse(self.log.exists())

    def test_uninstall_rejects_unsupported_or_missing_distribution(self):
        binary = self.home / ".local/bin/monologue"
        binary.parent.mkdir(parents=True)
        binary.write_text("keep me")
        self.set_distribution("debian")
        result = self.run_script("uninstall")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unsupported distribution: debian", result.stderr)
        self.os_release.unlink()
        result = self.run_script("uninstall")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cannot detect the distribution", result.stderr)
        self.assertEqual(binary.read_text(), "keep me")
        self.assertFalse(self.log.exists())


if __name__ == "__main__":
    unittest.main()

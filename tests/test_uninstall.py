"""Exercise generated removers in temporary game folders, never in the checkout."""

import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
GPL_HASH = "3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986"
OWNED_FILES = (
    "OptiScaler.ini", "OptiScaler.log", "OptiScaler.dll", "OptiScaler.asi",
    "OptiScaler/amdvkffx64.dll", "OptiScaler/amdxcffx64.dll",
    "OptiScaler/Licenses/FidelityFX_v1_LICENSE.md",
    "OptiScaler/BC250Dx12/cache/test.pso", "OptiScaler/BC250Dx12/capture/test.dxil",
    "OptiScaler/tools/BC250-DX12.md",
    "OptiScaler/D3D12_OptiScaler/D3D12Core.dll", "OptiScaler/plugins/test.asi",
    "D3D12_OptiScaler/legacy.dll", "DlssOverrides/legacy.json",
    "docs/BC250.md", "docs/FSR4-providers.md", "profiles/BC250.ini",
    "Licenses/XeSS_LICENSE.txt", "Licenses/FidelityFX_v2_LICENSE.md",
    "Licenses/DirectX_LICENSE.txt", "Licenses/OptiScaler_LICENSE.txt",
    "fakenvapi.dll", "fakenvapi.ini", "fakenvapi.log",
    "dlssg_to_fsr3_amd_is_better.dll", "dlssg_to_fsr3.log",
    "setup_linux.sh", "setup_windows.bat", "!! EXTRACT ALL FILES TO GAME FOLDER !!",
    "remove_optiscaler.sh", "Remove_OptiScaler.bat", "Remove OptiScaler.bat",
)
USER_FILES = ("docs/game-notes.txt", "profiles/user.ini", "Licenses/game.txt")


def write_file(directory, name, content=b"fixture"):
    path = directory / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    return path


def snapshot(directory):
    return {str(p.relative_to(directory)): p.read_bytes()
            for p in directory.rglob("*") if p.is_file()}


class RemovalCases:
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="optiscaler-removal-")
        self.addCleanup(temporary.cleanup)
        self.game = Path(temporary.name) / "Game folder !"
        self.foreign = Path(temporary.name) / "Another game" / "bin"
        for directory in (self.game, self.foreign):
            for name in OWNED_FILES + USER_FILES:
                write_file(directory, name)
            write_file(directory, "LICENSE", b"A different game's license\n")
            write_file(directory, "dxgi.dll", b"An unrelated game proxy")
            write_file(directory, "version.dll", b"Injected OptiScaler fixture")
        self.foreign_before = snapshot(self.foreign)
        self.create_remover()

    def run_remover(self, confirm=True, relative=False):
        script = os.path.relpath(self.remover, self.foreign) if relative else str(self.remover)
        if os.name == "nt":
            command = ["cmd", "/d", "/c", script]
            response = "1\n\n" if confirm else "2\n\n"
        else:
            command = ["bash", script, "--remove=" + ("y" if confirm else "n")]
            response = ""
        result = subprocess.run(command, cwd=self.foreign, input=response, text=True,
                                capture_output=True, timeout=90,
                                env={**os.environ, "TERM": "dumb"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        if os.name == "nt":
            self.assertNotIn("the batch file cannot be found", (result.stdout + result.stderr).casefold())
        self.assertEqual(snapshot(self.foreign), self.foreign_before,
                         "Removal touched the caller's working directory")

    def assert_owned_files_removed(self):
        for name in OWNED_FILES + ("version.dll",):
            self.assertFalse((self.game / name).exists(), name)
        for name in ("OptiScaler", "D3D12_OptiScaler", "DlssOverrides"):
            self.assertFalse((self.game / name).exists(), name)
        self.assertFalse(self.remover.exists(), "Uninstaller should remove itself")

    def test_complete_cleanup_preserves_shared_game_files(self):
        self.run_remover()
        self.assert_owned_files_removed()
        for name in USER_FILES:
            self.assertEqual((self.game / name).read_bytes(), b"fixture")
        self.assertEqual((self.game / "LICENSE").read_bytes(), b"A different game's license\n")
        self.assertEqual((self.game / "dxgi.dll").read_bytes(), b"An unrelated game proxy")

    def test_empty_shared_directories_are_removed(self):
        for name in USER_FILES:
            (self.game / name).unlink()
        self.run_remover()
        self.assert_owned_files_removed()
        for name in ("docs", "profiles", "Licenses"):
            self.assertFalse((self.game / name).exists(), name)

    def test_cancel_preserves_installation(self):
        before = snapshot(self.game)
        self.run_remover(confirm=False)
        self.assertEqual(snapshot(self.game), before)

    def test_relative_script_path_cleans_up_and_removes_itself(self):
        self.run_remover(relative=True)
        self.assert_owned_files_removed()

    def test_legacy_package_license_is_removed(self):
        license_bytes = (ROOT / "LICENSE").read_bytes().replace(b"\r\n", b"\n")
        self.assertEqual(hashlib.sha256(license_bytes).hexdigest(), GPL_HASH)
        write_file(self.game, "LICENSE", license_bytes)
        self.run_remover()
        self.assertFalse((self.game / "LICENSE").exists())
        self.assert_owned_files_removed()

    def test_legacy_windows_line_endings_are_recognized(self):
        license_bytes = (ROOT / "LICENSE").read_bytes().replace(b"\r\n", b"\n")
        write_file(self.game, "LICENSE", license_bytes.replace(b"\n", b"\r\n"))
        self.run_remover()
        self.assertFalse((self.game / "LICENSE").exists())
        self.assert_owned_files_removed()


@unittest.skipIf(os.name == "nt", "Bash remover is exercised on Linux/macOS")
class LinuxRemovalTests(RemovalCases, unittest.TestCase):
    def create_remover(self):
        # Exercise the actual generated script, with the installer's filename substitution.
        source = (ROOT / "setup_linux.sh").read_text()
        payload = re.search(r'cat > "remove_optiscaler.sh" << \'EOF\'\n(.*?)\nEOF',
                            source, re.DOTALL).group(1)
        self.remover = self.game / "remove_optiscaler.sh"
        self.remover.write_text(payload.replace("SELECTED_FILENAME_PLACEHOLDER", "version.dll") + "\n")

    @unittest.skipUnless(sys.platform == "linux", "Full setup requires Linux sed/bash")
    def test_setup_generates_remover_in_its_own_directory(self):
        self.setup_and_remove(relative=False)

    @unittest.skipUnless(sys.platform == "linux", "Full setup requires Linux sed/bash")
    def test_setup_launched_by_relative_path_removes_itself(self):
        self.setup_and_remove(relative=True)

    def setup_and_remove(self, relative):
        # The full setup also runs from a foreign working directory, with no interactive prompts.
        self.remover.unlink()
        shutil.copyfile(ROOT / "setup_linux.sh", self.game / "setup_linux.sh")
        script = self.game / "setup_linux.sh"
        script = os.path.relpath(script, self.foreign) if relative else str(script)
        result = subprocess.run(
            ["bash", script, "--filename=version.dll",
             "--overwrite=y", "--using_nvidia=n", "--using_dlss=y"],
            cwd=self.foreign, text=True, capture_output=True, timeout=30,
            env={**os.environ, "TERM": "dumb"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(self.remover.exists())
        self.assertFalse((self.game / "setup_linux.sh").exists())
        self.assertEqual(snapshot(self.foreign), self.foreign_before)
        self.run_remover()
        self.assert_owned_files_removed()


@unittest.skipUnless(os.name == "nt", "Batch remover requires native Windows")
class WindowsRemovalTests(RemovalCases, unittest.TestCase):
    def create_remover(self):
        dll = Path(os.environ.get("OPTISCALER_TEST_DLL", ROOT / "x64/Release/a/OptiScaler.dll"))
        self.assertTrue(dll.is_file(), "Build the DLL before running Windows removal tests")
        shutil.copyfile(dll, self.game / "version.dll")
        # Run the actual batch generator so CMD escaping and DLL identification are exercised.
        source = (ROOT / "setup_windows.bat").read_text()
        generator_body = source.split("\n:create_uninstaller\n", 1)[1]
        generator = self.game / "generate-remover.bat"
        text = "@echo off\n" + generator_body + "\n:create_uninstaller_return\nexit /b 0\n"
        generator.write_bytes(text.replace("\n", "\r\n").encode())
        result = subprocess.run(["cmd", "/d", "/c", str(generator)], cwd=self.game,
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        generator.unlink()
        self.remover = self.game / "Remove_OptiScaler.bat"
        self.assertTrue(self.remover.is_file())


if __name__ == "__main__":
    unittest.main(verbosity=2)

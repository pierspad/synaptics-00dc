"""Installer boundary tests. Never write to the host system or open USB."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class InstallerTests(unittest.TestCase):
    def run_installer(self, *args, env=None):
        return subprocess.run(['bash', str(ROOT / 'install.sh'), *args],
                              capture_output=True, text=True, env=env)

    def test_help_needs_no_dependencies(self):
        result = self.run_installer('--help')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--apply', result.stdout)

    def test_unknown_argument_refused(self):
        result = self.run_installer('--surprise')
        self.assertNotEqual(result.returncode, 0)

    def test_no_build_is_staging_only(self):
        result = self.run_installer('--apply', '--no-build')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('only for --destdir', result.stderr)

    def test_missing_tod_stops_before_build_or_sudo(self):
        with tempfile.TemporaryDirectory() as tmp:
            pkg = Path(tmp) / 'pkg-config'
            pkg.write_text('#!/bin/sh\nexit 1\n')
            pkg.chmod(0o755)
            env = dict(os.environ, PATH=tmp + ':' + os.environ['PATH'])
            result = self.run_installer('--apply', env=env)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('libfprint-2-tod-1', result.stderr)

    def test_staging_install_has_no_permissive_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            build = base / 'build'
            build.mkdir()
            (build / 'libsyna_00dc_tod.so').write_bytes(b'offline test module')
            dest = base / 'dest'
            result = self.run_installer('--apply', '--destdir', str(dest),
                                        '--build-dir', str(build), '--no-build')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(list(dest.rglob('libsyna_00dc_tod.so')))
            self.assertFalse(list(dest.rglob('*polkit*')))
            self.assertFalse(list(dest.rglob('*.pem')))
            self.assertTrue((dest / 'var/lib/fprint/syna_00dc').is_dir())

    def test_symlinked_destination_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            (base / 'dest').mkdir()
            (base / 'outside').mkdir()
            (base / 'dest/var').symlink_to(base / 'outside', target_is_directory=True)
            result = self.run_installer('--apply', '--destdir', str(base / 'dest'), '--no-build')
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(list((base / 'outside').iterdir()), [])

if __name__ == '__main__':
    unittest.main()

"""Exercise service restoration in a pseudo terminal with fake commands only."""
import errno
import os
from pathlib import Path
import pty
import select
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]

@unittest.skipIf(os.geteuid() == 0, 'setup intentionally refuses root callers')
class SetupTests(unittest.TestCase):
    def make_fixture(self, root, build_dir=None, diagnostic='exit 99\n'):
        shutil.copy(ROOT / 'setup.sh', root / 'setup.sh')
        build = build_dir or root / 'build'
        build.mkdir(parents=True, exist_ok=True)
        (build / 'libsyna_00dc_tod.so').write_text('mock module')
        (build / 'test_fprint_open').write_text('#!/bin/bash\n' + diagnostic)
        (build / 'test_fprint_open').chmod(0o755)
        (root / 'bin').mkdir(exist_ok=True)
        def executable(path, text):
            path.write_text('#!/bin/bash\n' + text)
            path.chmod(0o755)
        executable(root / 'bin/sudo', '''
if [[ $1 == -v || $1 == bash ]]; then exit 0; fi
exec "$@"
''')
        executable(root / 'bin/systemctl', '''
echo "$*" >> "$SETUP_TEST_LOG"
if [[ $1 == is-active ]]; then exit "$SETUP_TEST_STATE"; fi
exit 0
''')
        executable(root / 'bin/timeout', '''
echo "timeout:$*" >> "$SETUP_TEST_LOG"
shift 3
if [[ $1 == env ]]; then
    shift
    while [[ $1 == *=* ]]; do export "$1"; shift; done
fi
exec "$@"
''')
        executable(root / 'bin/fprintd-enroll', 'exit 0\n')
        return build

    def run_pty(self, script, args=(), cwd=None, env=None, input_text='PAIR\n'):
        child_env = dict(os.environ, **(env or {}))
        pid, fd = pty.fork()
        if pid == 0:
            if cwd:
                os.chdir(cwd)
            os.execvpe('bash', ['bash', str(script), *args], child_env)
        output = b''
        sent = False
        deadline = time.monotonic() + 10
        try:
            while time.monotonic() < deadline:
                if select.select([fd], [], [], 0.1)[0]:
                    try:
                        chunk = os.read(fd, 4096)
                    except OSError as exc:
                        if exc.errno == errno.EIO: break
                        raise
                    if not chunk: break
                    output += chunk
                    if input_text and b'Type PAIR' in output and not sent:
                        os.write(fd, input_text.encode())
                        sent = True
            else:
                os.kill(pid, 9)
                self.fail('mocked setup timed out')
        finally:
            os.close(fd)
            _, status = os.waitpid(pid, 0)
        return os.waitstatus_to_exitcode(status), output.decode(errors='replace'), sent

    def test_custom_build_dir_with_spaces_drives_diagnostic_and_plugin_lookup(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'driver checkout'
            root.mkdir()
            work = Path(tmp) / 'invocation cwd'
            work.mkdir()
            build = work / 'build output with spaces'
            self.make_fixture(root, build, '''
printf '%s\\n' "$FP_TOD_DRIVERS_DIR" "$0" >> "$SETUP_TEST_LOG"
exit 99
''')
            log = root / 'commands'
            env = {'PATH': str(root / 'bin') + ':' + os.environ['PATH'],
                   'SETUP_TEST_LOG': str(log), 'SETUP_TEST_STATE': '3'}
            code, output, sent = self.run_pty(root / 'setup.sh',
                                               ['--build-dir', 'build output with spaces'],
                                               cwd=work, env=env)
            self.assertTrue(sent, output)
            self.assertNotEqual(code, 0)
            log_lines = log.read_text().splitlines()
            self.assertIn(str(build), log_lines)
            self.assertIn(str(build / 'test_fprint_open'), log_lines)

    def test_help_and_invalid_arguments_are_handled_without_a_tty(self):
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / 'setup.sh'
            shutil.copy(ROOT / 'setup.sh', script)
            help_result = subprocess.run(['bash', str(script), '--help'],
                                         stdin=subprocess.DEVNULL, text=True,
                                         capture_output=True, timeout=3)
            self.assertEqual(help_result.returncode, 0, help_result.stderr)
            self.assertIn('--build-dir DIR', help_result.stdout)
            for args in (['--bogus'], ['--build-dir']):
                result = subprocess.run(['bash', str(script), *args],
                                        stdin=subprocess.DEVNULL, text=True,
                                        capture_output=True, timeout=3)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertNotIn('interactive terminal', result.stderr)

    def test_module_is_required_before_any_privilege_command(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = self.make_fixture(root)
            (build / 'libsyna_00dc_tod.so').unlink()
            env = dict(os.environ, PATH=str(root / 'bin') + ':' + os.environ['PATH'],
                       SETUP_TEST_LOG=str(root / 'commands'), SETUP_TEST_STATE='3')
            code, output, _ = self.run_pty(root / 'setup.sh', env=env)
            self.assertNotEqual(code, 0)
            self.assertIn('TOD module missing', output)
            self.assertFalse((root / 'commands').exists())

    def check_failure_restores(self, initially_active):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_fixture(root)
            env = dict(os.environ, PATH=str(root / 'bin') + ':' + os.environ['PATH'],
                       SETUP_TEST_LOG=str(root / 'commands'),
                       SETUP_TEST_STATE='0' if initially_active else '3')
            code, output, sent = self.run_pty(root / 'setup.sh', env=env)
            self.assertTrue(sent, output)
            self.assertNotEqual(code, 0)
            actions = (root / 'commands').read_text().splitlines()
            self.assertEqual(actions[-1], 'start fprintd' if initially_active else 'stop fprintd')
            self.assertEqual(actions[1], 'stop fprintd')

    def test_failed_setup_restores_active_service(self):
        self.check_failure_restores(True)

    def test_failed_setup_restores_inactive_service(self):
        self.check_failure_restores(False)

    def test_successful_setup_restores_prior_service_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_fixture(root, diagnostic='exit 0\n')
            env = dict(os.environ, PATH=str(root / 'bin') + ':' + os.environ['PATH'],
                       SETUP_TEST_LOG=str(root / 'commands'), SETUP_TEST_STATE='0')
            code, output, sent = self.run_pty(root / 'setup.sh', env=env)
            self.assertTrue(sent, output)
            self.assertEqual(code, 0, output)
            actions = (root / 'commands').read_text().splitlines()
            self.assertEqual(actions[-1], 'start fprintd')
            self.assertIn('Enrollment complete', output)

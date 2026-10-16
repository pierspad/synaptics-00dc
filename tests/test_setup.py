"""Exercise service restoration in a pseudo terminal with fake commands only."""
import errno
import os
from pathlib import Path
import pty
import select
import shutil
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]

@unittest.skipIf(os.geteuid() == 0, 'setup intentionally refuses root callers')
class SetupTests(unittest.TestCase):
    def check_failure_restores(self, initially_active):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shutil.copy(ROOT / 'setup.sh', root / 'setup.sh')
            (root / 'build').mkdir()
            (root / 'bin').mkdir()
            def executable(path, text):
                path.write_text('#!/bin/bash\n' + text)
                path.chmod(0o755)
            executable(root / 'build/test_fprint_open', 'exit 99\n')
            executable(root / 'bin/sudo', '''
if [[ $1 == -v || $1 == bash ]]; then exit 0; fi
exec "$@"
''')
            executable(root / 'bin/systemctl', '''
echo "$*" >> "$SETUP_TEST_LOG"
if [[ $1 == is-active ]]; then exit "$SETUP_TEST_STATE"; fi
exit 0
''')
            executable(root / 'bin/timeout', 'exit 1\n')
            executable(root / 'bin/fprintd-enroll', 'exit 99\n')
            env = dict(os.environ, PATH=str(root / 'bin') + ':' + os.environ['PATH'],
                       SETUP_TEST_LOG=str(root / 'commands'),
                       SETUP_TEST_STATE='0' if initially_active else '3')
            pid, fd = pty.fork()
            if pid == 0:
                os.execvpe('bash', ['bash', str(root / 'setup.sh')], env)
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
                        if b'Type PAIR' in output and not sent:
                            os.write(fd, b'PAIR\n')
                            sent = True
                else:
                    os.kill(pid, 9)
                    self.fail('mocked setup timed out')
            finally:
                os.close(fd)
                _, status = os.waitpid(pid, 0)
            self.assertTrue(sent, output.decode())
            self.assertNotEqual(os.waitstatus_to_exitcode(status), 0)
            actions = (root / 'commands').read_text().splitlines()
            self.assertEqual(actions[-1], 'start fprintd' if initially_active else 'stop fprintd')
            self.assertEqual(actions[1], 'stop fprintd')

    def test_failed_setup_restores_active_service(self):
        self.check_failure_restores(True)

    def test_failed_setup_restores_inactive_service(self):
        self.check_failure_restores(False)

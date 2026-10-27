"""GDB/MI 的子进程边界回归：不需要设备、Docker 或裸装 GDB。"""
import importlib
import os
from pathlib import Path
import queue
import sys
import subprocess
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'pc/src'))


class GdbTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue((ROOT / 'pc/src/nxc_host/services/gdb.py').is_file(),
                        '统一后台的 GDB/MI 服务尚未实现')
        self.gdb = importlib.import_module('nxc_host.services.gdb')
        self.temp = tempfile.TemporaryDirectory(prefix='nxc gdb spaces ')
        self.addCleanup(self.temp.cleanup)
        self.binary = Path(self.temp.name) / 'fake gdb'
        helper = ROOT / 'pc/tests/helpers/fake_mi.py'
        self.binary.write_text('#!' + sys.executable + '\n' + helper.read_text().split('\n', 1)[1])
        self.binary.chmod(0o755)
        self.session = self.gdb.GdbSession('device', backend='native', executable=str(self.binary))
        self.addCleanup(self.session.close)

    def start(self, connect=False):
        self.session.start(connect=connect, timeout=3)

    def wait_state(self, state):
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            if self.session.status()['state'] == state:
                return
            time.sleep(.01)
        self.fail(str(self.session.status()))

    def test_matching_tokens_and_cli_encoding_with_space_path(self):
        self.start()
        output = self.session.run(['show version', '-list-features'], timeout=1)
        self.assertIn('-interpreter-exec console "show version"', output)
        self.assertIn('-list-features', output)
        self.assertEqual(self.session.status()['executable'], str(self.binary))
        self.assertFalse(self.session.status()['connected'])

    def test_gdb_error_and_failed_connect_never_mark_connected(self):
        self.start()
        with self.assertRaisesRegex(self.gdb.GdbError, '明确的 GDB 错误'):
            self.session.run('fail', timeout=1)
        failed = self.gdb.GdbSession('bad-host', backend='native', executable=str(self.binary))
        self.addCleanup(failed.close)
        with self.assertRaisesRegex(self.gdb.GdbError, '连接失败'):
            failed.start(timeout=1)
        self.assertFalse(failed.status()['connected'])

    def test_connect_supports_stub_without_selected_inferior(self):
        self.session.ip = 'attach-host'
        self.start(connect=True)
        self.assertTrue(self.session.status()['connected'])
        self.assertIsNone(self.session.status()['attached_pid'])

    def test_detach_releases_extended_connection_for_both_entry_points(self):
        for cli_route in (False, True):
            with self.subTest(cli_route=cli_route):
                self.session.close()
                self.start(connect=True)
                self.session.handle('attach', {'pid': 42, 'timeout': 1})
                if cli_route:
                    self.session.run('detach', timeout=1)
                else:
                    self.session.detach_only()
                self.assertIn('No connections.', self.session.run('info connections', timeout=1))
                self.assertFalse(self.session.status()['connected'])
                self.session.close()

    def test_failed_disconnect_retains_resource_ownership(self):
        self.session.ip = 'disconnect-error'
        self.start(connect=True)
        self.session.handle('attach', {'pid': 42, 'timeout': 1})
        with self.assertRaisesRegex(self.gdb.GdbError, '无法确认断开'):
            self.session.detach_only()
        self.assertTrue(self.session.status()['connected'])
        self.assertTrue(self.session.status()['uncertain'])
        self.assertIsNone(self.session.status()['attached_pid'])

    def test_manual_remote_detach_already_disconnected_is_success(self):
        self.start()
        self.session.run('target remote device:22225', timeout=1)
        self.session.handle('attach', {'pid': 42, 'timeout': 1})
        self.session.run('detach', timeout=1)
        self.assertFalse(self.session.status()['connected'])
        self.assertFalse(self.session.status()['uncertain'])

    def test_manual_remote_detach_recovers_prior_uncertainty(self):
        self.start()
        self.session.run('target remote device:22225', timeout=1)
        with self.assertRaisesRegex(self.gdb.GdbError, '超时'):
            self.session.run('timeout', timeout=.03)
        self.assertTrue(self.session.status()['uncertain'])
        self.session.detach_only()
        self.assertFalse(self.session.status()['connected'])
        self.assertFalse(self.session.status()['uncertain'])

    def test_continue_returns_running_then_async_stop(self):
        self.start(connect=True)
        self.assertIsNone(self.session.status()['attached_pid'])
        started = time.monotonic()
        result = self.session.run('continue', timeout=1)
        self.assertLess(time.monotonic() - started, .1)
        self.assertIn('^running', result)
        self.wait_state('stopped')
        self.assertTrue(self.session.status()['connected'])
        self.assertTrue(any('*stopped' in event for event in self.session.status()['events']))

    def test_timeout_is_real_and_no_replay_or_stale_result(self):
        self.start()
        started = time.monotonic()
        with self.assertRaisesRegex(self.gdb.GdbError, '超时'):
            self.session.run('timeout', timeout=.12)
        self.assertLess(time.monotonic() - started, .5)
        self.assertTrue(self.session.status()['uncertain'])
        self.assertIn('show version', self.session.run('show version', timeout=1))
        self.assertTrue(self.session.status()['uncertain'])

    def test_eof_reports_failure_and_reaps_process(self):
        self.start()
        process = self.session.proc
        with self.assertRaisesRegex(self.gdb.GdbError, '结束|退出'):
            self.session.run('eof', timeout=1)
        self.session.close()
        self.assertIsNotNone(process.poll())
        self.assertFalse(self.session.status()['running'])

    def test_attach_detach_close_cleanup(self):
        self.start(connect=True)
        self.session.handle('attach', {'pid': 42, 'timeout': 1})
        self.assertEqual(self.session.status()['attached_pid'], 42)
        self.session.detach_only()
        self.assertFalse(self.session.status()['connected'])
        self.assertIsNone(self.session.status()['attached_pid'])
        process = self.session.proc
        self.session.close()
        self.assertIsNotNone(process.poll())
        self.assertEqual(self.session.status()['state'], 'closed')

    def test_interrupt_bypasses_long_command_lock(self):
        self.start(connect=True)
        failures = []
        def long_command():
            try:
                self.session.run('timeout', timeout=.4)
            except self.gdb.GdbError as error:
                failures.append(error)
        worker = threading.Thread(target=long_command)
        worker.start()
        time.sleep(.04)
        started = time.monotonic()
        self.session.handle('interrupt', {'timeout': .15})
        self.assertLess(time.monotonic() - started, .2)
        worker.join(1)
        self.assertEqual(len(failures), 1)

    def test_late_response_is_not_consumed_by_following_command(self):
        self.start()
        with self.assertRaisesRegex(self.gdb.GdbError, '超时'):
            self.session.run('late-response', timeout=.05)
        time.sleep(.2)
        self.assertIn('show version', self.session.run('show version', timeout=1))

    def test_unresponsive_gdb_is_killed_and_reaped_on_close(self):
        self.start()
        process = self.session.proc
        with self.assertRaisesRegex(self.gdb.GdbError, '超时'):
            self.session.run('freeze', timeout=.05)
        started = time.monotonic()
        self.session.close()
        self.assertLess(time.monotonic() - started, 5)  # 未知状态也尽力 interrupt/detach。
        self.assertIsNotNone(process.poll())
        self.assertFalse(self.session._reader.is_alive())

    def test_pipe_backpressure_still_honors_request_timeout(self):
        self.start()
        with self.assertRaises(self.gdb.GdbError):
            self.session.run('freeze', timeout=.05)
        result = []
        def write_large_command():
            try:
                self.session.run('x' * (256 * 1024), timeout=.1)
            except self.gdb.GdbError as error:
                result.append(error)
        worker = threading.Thread(target=write_large_command, daemon=True)
        worker.start()
        worker.join(.4)
        blocked = worker.is_alive()
        # 失败实现的 stdin.write 会卡住；主动释放再断言，保证测试自身能结束。
        if blocked:
            self.session.proc.kill()
            self.session.proc.wait(timeout=2)
            worker.join(1)
        self.assertFalse(blocked, 'stdin 背压导致 timeout 未生效')
        self.assertEqual(len(result), 1)

    def test_manual_attach_and_target_commands_update_real_status(self):
        self.start()
        self.session.run('target remote device:22225', timeout=1)
        self.assertTrue(self.session.status()['connected'])
        self.session.run('attach 123', timeout=1)
        self.assertEqual(self.session.status()['attached_pid'], 123)
        self.session.run('detach', timeout=1)
        self.assertFalse(self.session.status()['connected'])

    def test_docker_command_never_pulls_or_reads_user_gdbinit(self):
        diagnostic = {'available': True, 'backend': 'docker',
                      'executable': str(self.binary), 'image': 'existing-image'}
        with patch.object(self.session, 'doctor', return_value=diagnostic), \
                patch.object(self.gdb.subprocess, 'Popen', wraps=subprocess.Popen) as spawn, \
                patch.object(self.gdb.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '', '')):
            self.start()
            command = spawn.call_args.args[0]
            self.assertIn('--pull=never', command)
            self.assertIn('-i', command)
            self.assertNotIn('-t', command)
            self.assertIn('--nx', command[-1])
            self.assertIn('--interpreter=mi2', command[-1])
            self.session.close()

    def test_attach_rejects_missing_or_invalid_pid(self):
        self.start()
        for pid in (None, 0, -1, True, '1\nquit'):
            with self.subTest(pid=pid), self.assertRaises(self.gdb.GdbError):
                self.session.handle('attach', {'pid': pid})
        self.assertIsNone(self.session.status()['attached_pid'])

    def test_late_connect_success_stays_uncertain_until_confirmed_detach(self):
        self.session.ip = 'slow-host'
        self.start()
        with self.assertRaisesRegex(self.gdb.GdbError, '超时'):
            self.session.connect(timeout=.03)
        self.assertTrue(self.session.status().get('uncertain'), self.session.status())
        time.sleep(.2)
        self.assertTrue(self.session.status().get('uncertain'))
        self.assertTrue(self.session.status()['connected'])
        self.assertEqual(self.session.status()['state'], 'stopped')
        self.assertIn('show version', self.session.run('show version', timeout=1))
        self.session.handle('interrupt', {'timeout': 1})
        self.assertTrue(self.session.status()['uncertain'])
        self.session.detach_only()
        self.assertFalse(self.session.status()['uncertain'])
        self.assertFalse(self.session.status()['connected'])

    def test_late_attach_success_stays_uncertain_until_close(self):
        self.start()
        with self.assertRaisesRegex(self.gdb.GdbError, '超时'):
            self.session.handle('attach', {'pid': 999, 'timeout': .03})
        self.assertTrue(self.session.status().get('uncertain'), self.session.status())
        time.sleep(.2)
        self.assertTrue(self.session.status()['uncertain'])
        self.session.close()
        self.assertFalse(self.session.status()['uncertain'])

    def test_queued_but_never_sent_command_does_not_mark_uncertain(self):
        self.start()
        with patch.object(self.session._outgoing, 'put_nowait'), \
                self.assertRaisesRegex(self.gdb.GdbError, '超时'):
            self.session.run('attach 999', timeout=.03)
        self.assertFalse(self.session.status().get('uncertain', True))

    def test_close_with_dead_writer_and_full_queue_is_bounded_and_notifies(self):
        self.start()
        self.session._outgoing.put_nowait(None)
        self.session._writer.join(1)
        self.assertFalse(self.session._writer.is_alive())
        self.session.proc.kill()
        self.session.proc.wait(timeout=2)
        self.session._reader.join(1)
        for token in range(self.session._outgoing.maxsize):
            self.session._outgoing.put_nowait((1000 + token, '-list-features'))
        awaiting = {'queue': queue.Queue(), 'output': [], 'sent': False}
        self.session._pending[9000] = awaiting
        worker = threading.Thread(target=self.session.close, daemon=True)
        worker.start()
        worker.join(.5)
        blocked = worker.is_alive()
        if blocked:
            # 释放旧实现的阻塞 put，避免失败测试自身遗留线程。
            self.session._outgoing.get_nowait()
            worker.join(2)
        self.assertFalse(blocked, 'close 被已无消费者的满队列阻塞')
        notification = awaiting['queue'].get_nowait()
        self.assertIsInstance(notification, self.gdb.GdbError)
        self.assertFalse(self.session.status()['running'])

    def test_stream_close_oserror_does_not_skip_remaining_cleanup(self):
        self.start()
        process = self.session.proc
        process.kill()
        process.wait(timeout=2)
        self.session._reader.join(1)
        self.session._uncertain = True
        class FailingClose:
            def __init__(self, stream):
                self.stream = stream
                self.called = False
            def close(self):
                self.called = True
                self.stream.close()
                raise BrokenPipeError('注入的管道 flush 失败')
        stdin, stdout = FailingClose(process.stdin), FailingClose(process.stdout)
        with patch.object(process, 'stdin', stdin), patch.object(process, 'stdout', stdout):
            result = self.session.close()
        self.assertTrue(stdin.called)
        self.assertTrue(stdout.called)
        self.assertIn('管道关闭', result)
        self.assertIsNone(self.session.proc)
        self.assertFalse(self.session.status()['uncertain'])
        self.assertEqual(self.session.status()['state'], 'closed')
        self.assertIsNotNone(process.poll())
        self.assertFalse(self.session._reader.is_alive())
        self.assertFalse(self.session._writer.is_alive())

    def test_missing_backend_diagnostic_does_not_install(self):
        other = self.gdb.GdbSession('device', backend='auto')
        with patch.dict(os.environ, {'NXC_GDB_PATH': ''}), patch.object(self.gdb.shutil, 'which', return_value=None), patch.object(self.gdb, '_bundled_candidates', return_value=[]):
            diagnostic = other.doctor()
            self.assertFalse(diagnostic['available'])
            with self.assertRaisesRegex(self.gdb.GdbError, 'GDB|gdb'):
                other.start(connect=False)
        self.assertFalse(other.status()['running'])


if __name__ == '__main__':
    unittest.main()

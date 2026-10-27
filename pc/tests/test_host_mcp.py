"""stdio 代理保留通知语义和共享后台的生命周期。"""
import io
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'src'))


class Client:
    def __init__(self):
        self.requests = []

    def request(self, request, autostart=True):
        self.requests.append((request, autostart))
        return {'jsonrpc': '2.0', 'id': request.get('id'), 'result': {'ok': True}}


class StdioTests(unittest.TestCase):
    def run_bridge(self, text, client=None):
        from nxc_host.mcp import run_stdio
        client = client or Client()
        out, err = io.StringIO(), io.StringIO()
        status = run_stdio(client, stdin=io.StringIO(text), stdout=out, stderr=err)
        return status, client, [json.loads(line) for line in out.getvalue().splitlines()], err

    def test_initialize_list_and_notification_forwarded_eof_keeps_host(self):
        messages = [{'jsonrpc': '2.0', 'id': 1, 'method': 'initialize'},
                    {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
                    {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'}]
        status, client, replies, err = self.run_bridge('\n'.join(map(json.dumps, messages)))
        self.assertEqual(status, 0)
        self.assertEqual([r['id'] for r in replies], [1, 2])
        self.assertEqual([r[0] for r in client.requests], messages)
        self.assertTrue(all(r[1] for r in client.requests))
        self.assertEqual(err.getvalue(), '')

    def test_parse_error_and_invalid_request_do_not_break_next_frame(self):
        status, client, replies, err = self.run_bridge(
            'broken\n[]\n{"jsonrpc":"2.0","id":3,"method":"ping"}\n')
        self.assertEqual([r.get('error', {}).get('code') for r in replies],
                         [-32700, -32600, None])
        self.assertEqual(replies[-1]['id'], 3)
        self.assertEqual(len(client.requests), 1)

    def test_nonstandard_json_is_parse_error(self):
        _, client, replies, _ = self.run_bridge(
            '{"jsonrpc":"2.0","id":NaN,"method":"ping"}\n')
        self.assertEqual(replies[0]['error']['code'], -32700)
        self.assertEqual(client.requests, [])

    def test_client_stdout_is_redirected_to_stderr(self):
        client = Client()
        def noisy(request, autostart=True):
            print('后台诊断')
            return {'jsonrpc': '2.0', 'id': request['id'], 'result': {}}
        client.request = noisy
        _, _, replies, err = self.run_bridge(
            '{"jsonrpc":"2.0","id":1,"method":"ping"}\n', client)
        self.assertEqual(replies[0]['result'], {})
        self.assertIn('后台诊断', err.getvalue())

    def test_invalid_id_uses_null_error_id(self):
        _, _, replies, _ = self.run_bridge(
            '{"jsonrpc":"2.0","id":{},"method":"ping"}\n')
        self.assertIsNone(replies[0]['id'])
        self.assertEqual(replies[0]['error']['code'], -32600)

    def test_missing_reply_and_serialization_exception_return_errors(self):
        client = Client()
        for result in (None, {'jsonrpc': '2.0', 'id': 1, 'result': {1, 2}}):
            client.request = lambda *a, **k: result
            _, _, replies, _ = self.run_bridge(
                '{"jsonrpc":"2.0","id":1,"method":"ping"}\n', client)
            self.assertEqual(replies[0]['error']['code'], -32603)

    def test_exception_has_rpc_error_and_logs_only_stderr(self):
        client = Client()
        def broken(*args, **kwargs):
            raise RuntimeError('后台失败')
        client.request = broken
        status, client, replies, err = self.run_bridge(
            '{"jsonrpc":"2.0","id":4,"method":"ping"}\n'
            '{"jsonrpc":"2.0","method":"notifications/initialized"}\n', client)
        self.assertEqual(len(replies), 1)
        self.assertEqual(replies[0]['error']['code'], -32603)
        self.assertIn('后台失败', err.getvalue())


if __name__ == '__main__':
    unittest.main()

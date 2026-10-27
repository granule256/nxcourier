"""JSON-RPC stdio 代理；EOF 不关闭共享后台或 GDB。"""
from __future__ import annotations

import contextlib
import json
import math
import sys


def _error(request_id, code, message):
    return {'jsonrpc': '2.0', 'id': request_id,
            'error': {'code': code, 'message': message}}


def _valid_id(value):
    return (value is None or isinstance(value, str)
            or (isinstance(value, (int, float)) and not isinstance(value, bool)
                and (not isinstance(value, float) or math.isfinite(value))))


def _valid_request(request):
    return (isinstance(request, dict) and request.get('jsonrpc') == '2.0'
            and isinstance(request.get('method'), str)
            and isinstance(request.get('params', {}), (dict, list))
            and _valid_id(request.get('id')))


def _invalid_constant(value):
    raise ValueError(f'JSON 非法数值：{value}')


def run_stdio(client, *, stdin=None, stdout=None, stderr=None):
    stdin = sys.stdin if stdin is None else stdin
    stdout = sys.stdout if stdout is None else stdout
    stderr = sys.stderr if stderr is None else stderr
    for line in stdin:
        if not line.strip():
            continue
        request_id = None
        try:
            request = json.loads(line, parse_constant=_invalid_constant)
        except (ValueError, TypeError):
            reply = _error(None, -32700, 'JSON 解析失败')
        else:
            if not _valid_request(request):
                request_id = request.get('id') if isinstance(request, dict) else None
                if not _valid_id(request_id):
                    request_id = None
                reply = _error(request_id, -32600, '无效的 JSON-RPC 请求')
            else:
                request_id = request.get('id')
                try:
                    with contextlib.redirect_stdout(stderr):
                        reply = client.request(request, autostart=True)
                except Exception as exc:
                    print(f'MCP 后台请求失败：{exc}', file=stderr, flush=True)
                    reply = _error(request.get('id'), -32603, f'后台请求失败：{exc}')
                if 'id' not in request:
                    reply = None
                elif reply is None:
                    reply = _error(request_id, -32603, '后台未返回请求响应')
        if reply is not None:
            try:
                encoded = json.dumps(reply, ensure_ascii=False, allow_nan=False)
            except (TypeError, ValueError) as exc:
                print(f'MCP 后台响应格式错误：{exc}', file=stderr, flush=True)
                encoded = json.dumps(_error(request_id, -32603, '后台响应无法序列化'),
                                     ensure_ascii=False)
            stdout.write(encoded + '\n')
            stdout.flush()
    return 0

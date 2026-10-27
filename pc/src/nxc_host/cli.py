"""一个 nxc 入口；CLI、MCP 和 Web 共用本机后台。"""
from __future__ import annotations

import argparse
import base64
import json
import math
from pathlib import Path
import sys
import time
import uuid
import webbrowser

from . import __version__


class ChineseParser(argparse.ArgumentParser):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._positionals.title = '位置参数'
        self._optionals.title = '选项'
        for action in self._actions:
            if action.dest == 'help':
                action.help = '显示帮助并退出'

    def format_usage(self):
        return super().format_usage().replace('usage: ', '用法：', 1)

    def format_help(self):
        return super().format_help().replace('usage: ', '用法：', 1)

    def error(self, message):
        self.print_usage(sys.stderr)
        self.exit(2, f'参数错误：{message}\n')


def _add_global_options(parser):
    parser.add_argument('--ip', dest='device_ip', help='设备 IP 地址')
    parser.add_argument('--port', dest='device_port', type=int, help='设备端口')
    parser.add_argument('--timeout', type=float, help='设备通信超时秒数')
    parser.add_argument('--host-port', type=int, help='本机后台端口')
    parser.add_argument('--gdb-backend', choices=('auto', 'native', 'docker'), help='GDB 后端')
    parser.add_argument('--gdb-path', dest='gdb_executable', help='本机 GDB 可执行文件')
    parser.add_argument('--ftp-port', type=int, help='设备 FTPD 端口，默认5000')
    parser.add_argument('--ftp-user', help='设备 FTP 用户名，默认anonymous')
    parser.add_argument('--json', action='store_true', help='输出结构化 JSON')
    parser.add_argument('--version', action='store_true', help='显示版本')
    return parser


def _global_parser():
    return _add_global_options(ChineseParser(add_help=False, allow_abbrev=False))


def _parser():
    parser = ChineseParser(prog='nxc', description='NXC 统一后台、设备控制和调试入口',
                           allow_abbrev=False)
    _add_global_options(parser)
    sub = parser.add_subparsers(dest='command', metavar='命令', required=True)
    start = sub.add_parser('start', help='启动共享后台')
    start.add_argument('--foreground', action='store_true', help='在当前终端运行后台')
    sub.add_parser('_serve', help='内部后台启动入口')
    for name, help_text in [('stop', '停止共享后台'), ('status', '查看后台状态（不自动启动）'),
                            ('doctor', '诊断设备和 GDB 环境'), ('web', '启动后台并打开 Web 界面'),
                            ('mcp', '通过标准输入输出提供 MCP'), ('caps', '发现设备能力')]:
        sub.add_parser(name, help=help_text)
    call = sub.add_parser('call', help='调用设备方法，参数使用自然值 key=value')
    call.add_argument('method', nargs='?', help='设备方法，例如 fs.ls')
    call.add_argument('values', nargs='*', metavar='key=value')
    call.add_argument('--raw', help='直接发送完整协议文本，不再转义')
    gdb = sub.add_parser('gdb', help='操作共享 GDB 会话')
    gdb.add_argument('action', choices=('open', 'run', 'attach', 'interrupt', 'detach', 'close', 'status'))
    gdb.add_argument('commands', nargs='*', help='GDB 命令（含空格时加引号）')
    gdb.add_argument('--pid', type=int, help='附加进程 PID，省略则选择当前应用')
    gdb.add_argument('--no-connect', action='store_true', help='open 时只启动 GDB，不连接设备')
    put = sub.add_parser('put', help='上传并校验文件')
    put.add_argument('local', metavar='LOCAL')
    put.add_argument('remote', metavar='REMOTE')
    fetch = sub.add_parser('fetch', help='下载设备文件')
    fetch.add_argument('remote', metavar='REMOTE')
    fetch.add_argument('local', nargs='?', metavar='LOCAL')
    deploy = sub.add_parser('deploy', help='发布并校验设备模块文件（不重启）')
    deploy.add_argument('local', metavar='LOCAL')
    for transfer in (put,fetch,deploy):
        transfer.add_argument('--transport',choices=('auto','fs','ftp'),help='文件传输通道，默认auto优先FTP')
    ftp=sub.add_parser('ftp',help='探测设备FTPD（PC内置客户端）')
    ftp.add_argument('action',choices=('status',),default='status',nargs='?')
    see = sub.add_parser('see', help='查看画面并保存截图')
    see.add_argument('--output', metavar='PATH', help='本地截图文件路径')
    step = sub.add_parser('step', help='输入动作并保存动作后的画面')
    step.add_argument('steps', metavar='STEPS')
    step.add_argument('--wait', choices=('stable', 'change', 'none'), default='stable')
    step.add_argument('--output', metavar='PATH', help='本地截图文件路径')
    sha = sub.add_parser('sha', help='计算远端文件 SHA-256')
    sha.add_argument('remote', metavar='REMOTE')
    boot = sub.add_parser('boot', help='查询或等待设备开机号变化（不触发重启）')
    boot.add_argument('--once', action='store_true', help='只查询一次')
    boot.add_argument('--expect-boot', type=int, help='等待开机号至少达到此值')
    boot.add_argument('--max-wait', type=float, default=900.0, help='最长等待秒数，默认 900')
    return parser


def _downloads_dir():
    from .config import config_dir
    return config_dir() / 'downloads'


def _request(client, method, params=None):
    return client.request({'jsonrpc': '2.0', 'id': 1, 'method': method,
                           'params': params if params is not None else {}})


def _tool(client, name, arguments=None):
    return _request(client, 'tools/call', {'name': name, 'arguments': arguments or {}})


def _content_value(item):
    if item.get('type') == 'text':
        text = item.get('text', '')
        try:
            return json.loads(text)
        except (ValueError, TypeError):
            return text
    return item


def _present(response, *, output=None):
    """解包 MCP 内容并保存图片；任何输出模式均不打印图片 base64。"""
    if response is None:
        return None, False
    if not isinstance(response, dict):
        raise RuntimeError('后台响应格式错误')
    if 'error' in response:
        return response['error'], True
    result = response.get('result', response)
    failed = isinstance(result, dict) and bool(result.get('isError'))
    if not isinstance(result, dict) or 'content' not in result:
        return result, failed
    values = []
    image_index = 0
    for item in result['content']:
        if not isinstance(item, dict):
            raise RuntimeError('后台内容格式错误')
        if item.get('type') != 'image':
            value = _content_value(item)
            if isinstance(value, dict) and 'error' in value:
                failed = True
            values.append(value)
            continue
        mime = item.get('mimeType', 'application/octet-stream')
        extension = {'image/png': '.png', 'image/jpeg': '.jpg', 'image/webp': '.webp',
                     'image/gif': '.gif'}.get(mime, '.bin')
        if output:
            target = Path(output).expanduser().resolve()
            if image_index:
                target = target.with_name(f'{target.stem}-{image_index + 1}{target.suffix}')
        else:
            target = _downloads_dir() / f'{time.strftime("%Y%m%d-%H%M%S")}-{uuid.uuid4().hex[:8]}{extension}'
        data = base64.b64decode(item.get('data', ''), validate=True)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        values.append({'image': str(target.resolve()), 'mimeType': mime, 'bytes': len(data)})
        image_index += 1
    return values[0] if len(values) == 1 else values, failed


def _emit(value, *, failed=False, as_json=False):
    stream = sys.stderr if failed else sys.stdout
    if value is None:
        return
    if isinstance(value, str) and not as_json:
        print(value, file=stream)
    else:
        print(json.dumps(value, ensure_ascii=False, indent=2), file=stream)


def _boot_number(result):
    if not isinstance(result, dict):
        return None
    value = result.get('boot')
    if value is None:
        value = next((row['boot'] for row in result.get('records', [])
                      if isinstance(row, dict) and 'boot' in row), None)
    try:
        return int(value) if value is not None else None
    except (TypeError, ValueError):
        return None


def _boot(client, args):
    deadline = time.monotonic() + args.max_wait
    initial = None
    expected = args.expect_boot
    last = None
    while True:
        reply = _request(client, 'host.boot')
        result, failed = _present(reply)
        boot = _boot_number(result)
        ready = not failed and isinstance(result, dict) and bool(result.get('ready'))
        if args.once:
            return result, failed or not ready or (expected is not None and
                                                    (boot is None or boot < expected))
        if last is None:
            initial = boot
            if expected is None and boot is not None:
                expected = boot + 1
        if ready and ((expected is not None and boot is not None and boot >= expected)
                      or (expected is None and boot is not None)
                      or (args.expect_boot is None and initial is not None
                          and boot is not None and boot < initial)):
            return result, False
        last = result
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return {'error': '等待开机号超时', 'expected_boot': expected, 'last': last}, True
        time.sleep(min(1.0, remaining))


def _run(client, args, parser):
    command = args.command
    if command == 'start':
        return client.start(), False
    if command == 'status':
        return client.status(), False
    if command == 'stop':
        return client.stop(), False
    if command == 'web':
        status = client.start()
        url = f'http://127.0.0.1:{client.config.host_port}/'
        webbrowser.open(url)
        return {'message': '已打开本机 Web 界面', 'url': url, 'service': status}, False
    if command == 'doctor':
        response = _request(client, 'host.doctor')
    elif command == 'ftp':
        response = _request(client, 'host.ftp.status')
    elif command == 'caps':
        response = _tool(client, 'nxc_caps')
    elif command == 'call':
        if args.raw is not None:
            if args.values:
                parser.error('--raw 不能同时指定 key=value')
            arguments = {'raw': args.raw}
        else:
            if not args.method:
                parser.error('call 需要 METHOD 或 --raw TEXT')
            params = {}
            for value in args.values:
                key, separator, data = value.partition('=')
                if not separator or not key:
                    parser.error('方法参数必须使用 key=value')
                params[key] = data
            arguments = {'method': args.method, 'params': params}
        response = _tool(client, 'nxc_call', arguments)
    elif command == 'gdb':
        arguments = {'action': args.action}
        if args.pid is not None:
            if args.action != 'attach' or args.pid <= 0:
                parser.error('--pid 只适用于 attach，且必须为正整数')
            arguments['pid'] = args.pid
        if args.no_connect:
            if args.action != 'open':
                parser.error('--no-connect 只适用于 open')
            arguments['connect'] = False
        if args.commands:
            if args.action != 'run':
                parser.error('只有 gdb run 接受 GDB 命令')
            arguments['commands'] = args.commands
        elif args.action == 'run':
            parser.error('gdb run 需要至少一条命令')
        response = _tool(client, 'nxc_gdb', arguments)
    elif command in ('put', 'fetch', 'deploy'):
        arguments = {}
        if args.transport:
            arguments['transport']=args.transport
        if getattr(args, 'local', None):
            arguments['local_path'] = str(Path(args.local).expanduser().resolve())
        if getattr(args, 'remote', None):
            arguments['remote_path'] = args.remote
        if command == 'fetch' and not args.local:
            arguments['local_path'] = str(Path(Path(args.remote).name).resolve())
        response = _tool(client, f'nxc_{command}', arguments)
    elif command in ('see', 'step'):
        arguments = {'steps': args.steps, 'wait': args.wait} if command == 'step' else {}
        response = _tool(client, f'nxc_{command}', arguments)
    elif command == 'sha':
        response = _request(client, 'host.sha', {'remote_path': args.remote})
    elif command == 'boot':
        if not math.isfinite(args.max_wait) or args.max_wait < 0:
            parser.error('--max-wait 必须为有限的非负秒数')
        return _boot(client, args)
    else:
        raise RuntimeError(f'未知命令：{command}')
    return _present(response, output=getattr(args, 'output', None))


def main(argv=None, *, client_factory=None, config_loader=None):
    """依赖注入仅用于无设备回归；正常调用使用正式 HostClient/Config。"""
    argv = list(sys.argv[1:] if argv is None else argv)
    # 先提取全局选项，允许位于任一子命令后；余下的位置参数交给子命令解析。
    global_args, remaining = _global_parser().parse_known_args(argv)
    if global_args.version:
        print(f'nxc {__version__}')
        return 0
    parser = _parser()
    args = parser.parse_args(remaining)
    overrides = {key: value for key, value in vars(global_args).items()
                 if value is not None and key not in ('json', 'version')}
    try:
        if config_loader is None:
            from .config import load_config
            config_loader = load_config
        config = config_loader(overrides)
        if args.command == '_serve' or (args.command == 'start' and args.foreground):
            from .daemon import serve
            serve(config)
            return 0
        if client_factory is None:
            from .client import HostClient
            client_factory = HostClient
        client = client_factory(config)
        if args.command == 'mcp':
            from .mcp import run_stdio
            return run_stdio(client)
        value, failed = _run(client, args, parser)
        _emit(value, failed=failed, as_json=global_args.json)
        return 1 if failed else 0
    except KeyboardInterrupt:
        print('已取消；共享后台保持运行。', file=sys.stderr)
        return 130
    except Exception as exc:
        _emit({'error': f'执行失败：{exc}'}, failed=True, as_json=global_args.json)
        return 1

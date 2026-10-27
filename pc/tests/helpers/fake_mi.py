#!/usr/bin/env python3
"""无需设备或 Docker 的真实子进程 MI 协议测试替身。"""
import json
import re
import sys
import threading
import time


def emit(line):
    print(line, flush=True)


def stop_later():
    time.sleep(.12)
    emit('*stopped,reason="breakpoint-hit",thread-id="1"')


emit('~"假 GDB 已启动\\n"')
emit('(gdb)')
connected = False
protocol = None
disconnect_fails = False
for line in sys.stdin:
    match = re.match(r'(\d+)(.*)', line.strip())
    if not match:
        continue
    token, command = match.groups()
    if 'freeze' in command:
        time.sleep(30)
        continue
    if 'late-response' in command:
        def late(saved_token=token):
            time.sleep(.18)
            emit(saved_token + '^error,msg="迟到的错误"')
        threading.Thread(target=late, daemon=True).start()
        continue
    if command == '-gdb-exit':
        emit(token + '^exit')
        break
    if 'timeout' in command:
        continue
    if 'eof' in command:
        sys.exit(0)
    if 'fail' in command:
        emit(token + '^error,msg="明确的 GDB 错误"')
    elif command.startswith('-target-select'):
        # GDB 的 target-select 将地址作为原始文本传递，双引号不会去除。
        if '"' in command.split(' ', 2)[2]:
            emit(token + '^error,msg="地址包含字面量引号"')
            continue
        if command.startswith('-target-select remote ') and 'attach-host' in command:
            emit(token + '^error,msg="The target is not running (try extended-remote?)"')
            continue
        if 'slow-host' in command:
            time.sleep(.15)
        if 'bad-host' in command:
            emit(token + '^error,msg="连接失败"')
        else:
            connected = True
            protocol = command.split(' ', 2)[1]
            disconnect_fails = 'disconnect-error' in command
            emit(token + '^connected')
            emit('*stopped,reason="signal-received"')
    elif command == '-target-detach':
        if protocol == 'remote':
            connected = False
            emit('=connection-removed,id="1"')
        emit(token + '^done')
    elif command == '-target-disconnect':
        if not connected or disconnect_fails:
            emit(token + '^error,msg="无法确认断开"')
        else:
            connected = False
            emit('=connection-removed,id="1"')
            emit(token + '^done')
    elif 'info connections' in command:
        emit('~' + json.dumps('extended-remote\n' if connected else 'No connections.\n'))
        emit(token + '^done')
    elif command.startswith('-target-attach'):
        if command.endswith(' 999'):
            time.sleep(.15)
        emit(token + '^done')
    elif 'continue' in command or command == '-exec-continue':
        emit(token + '^running')
        emit('*running,thread-id="all"')
        threading.Thread(target=stop_later, daemon=True).start()
    elif command.startswith('-exec-interrupt'):
        emit(token + '^done')
        emit('*stopped,reason="signal-received"')
    else:
        # 额外 token 不可被误认作当前命令响应。
        emit('999999^done')
        emit('~' + json.dumps(command + '\n'))
        emit(token + '^done')

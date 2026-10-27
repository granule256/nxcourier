"""仅用于测试的本地被动 FTP 协议服务器，使用真实控制与数据 socket。"""
from __future__ import annotations

import posixpath
import socket
import socketserver
import threading


class FtpServer:
    def __init__(self, *, mlsd=True, login=True):
        self.files = {}
        self.dirs = {'/'}
        self.commands = []
        self.mlsd = mlsd
        self.login = login
        self.login_error = '530 Login incorrect'
        self.permission_message = '550 Permission denied'
        self.retr_limit = {}
        self.permission_denied = set()
        self.fail_retr = set()
        self.corrupt_upload_read = False
        self.fail_commit = False
        self.fail_backup = False
        self.corrupt_backup_read = False
        self.fail_restore = False
        self.drop_stor = False
        self.connections = 0
        outer = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                outer.connections += 1
                self.cwd = '/'
                self.passive = None
                self.rename = None
                self.reply('220 NXC local test FTP')
                try:
                    while line := self.rfile.readline():
                        line = line.decode('utf-8').rstrip('\r\n')
                        command, _, argument = line.partition(' ')
                        outer.commands.append((command, argument))
                        if command == 'USER':
                            self.reply('331 Password required')
                        elif command == 'PASS':
                            self.reply('230 Logged in' if outer.login else outer.login_error)
                        elif command in ('TYPE', 'OPTS', 'NOOP'):
                            self.reply('200 OK')
                        elif command == 'PWD':
                            self.reply('257 "' + self.cwd + '"')
                        elif command == 'CWD':
                            path = self.path(argument)
                            if path in outer.dirs:
                                self.cwd = path
                                self.reply('250 Directory changed')
                            else:
                                self.reply('550 No such file or directory')
                        elif command == 'PASV':
                            if self.passive:
                                self.passive.close()
                            self.passive = socket.socket()
                            self.passive.bind(('127.0.0.1', 0))
                            self.passive.listen(1)
                            self.passive.settimeout(2)
                            port = self.passive.getsockname()[1]
                            self.reply(f'227 Entering Passive Mode (127,0,0,1,{port // 256},{port % 256})')
                        elif command == 'SIZE':
                            path = self.path(argument)
                            if path in outer.permission_denied:
                                self.reply(outer.permission_message)
                            elif path in outer.files:
                                self.reply(f'213 {len(outer.files[path])}')
                            elif path in outer.dirs:
                                self.reply('550 Not a regular file')
                            else:
                                self.reply('550 No such file or directory')
                        elif command in ('MLSD', 'NLST', 'RETR', 'STOR'):
                            path = self.path(argument)
                            if command == 'MLSD' and not outer.mlsd:
                                self.reply('502 MLSD not implemented')
                                continue
                            if path in outer.permission_denied:
                                self.reply(outer.permission_message)
                                continue
                            if command == 'RETR' and path not in outer.files:
                                self.reply('550 No such file or directory')
                                continue
                            if command in ('MLSD', 'NLST') and path not in outer.dirs:
                                self.reply('550 No such file or directory')
                                continue
                            if command == 'STOR' and outer.drop_stor:
                                return
                            self.reply('150 Opening binary connection')
                            data, _ = self.passive.accept()
                            with data:
                                if command == 'STOR':
                                    chunks = []
                                    while chunk := data.recv(65536):
                                        chunks.append(chunk)
                                    outer.files[path] = b''.join(chunks)
                                elif command == 'RETR':
                                    payload = outer.files[path]
                                    if outer.corrupt_upload_read and '/.nxc-upload-' in path:
                                        payload += b'corrupted'
                                    if outer.corrupt_backup_read and '/.nxc-backup-' in path:
                                        payload += b'corrupted'
                                    if path in outer.retr_limit:
                                        payload = payload[:outer.retr_limit[path]]
                                    data.sendall(payload)
                                else:
                                    children = sorted(p for p in outer.files.keys() | outer.dirs
                                                      if p != path and posixpath.dirname(p) == path)
                                    lines = []
                                    for child in children:
                                        name = posixpath.basename(child)
                                        if command == 'MLSD':
                                            kind = 'dir' if child in outer.dirs else 'file'
                                            size = len(outer.files.get(child, b''))
                                            name = f'type={kind};size={size}; {name}'
                                        else:
                                            name = child
                                        lines.append(name)
                                    data.sendall(('\r\n'.join(lines) + ('\r\n' if lines else '')).encode('utf-8'))
                            self.passive.close()
                            self.passive = None
                            if command == 'RETR' and path in outer.fail_retr:
                                self.reply('426 Transfer aborted')
                            else:
                                self.reply('226 Transfer complete')
                        elif command == 'RNFR':
                            path = self.path(argument)
                            if path in outer.files:
                                self.rename = path
                                self.reply('350 Rename destination required')
                            else:
                                self.reply('550 No such file or directory')
                        elif command == 'RNTO':
                            path = self.path(argument)
                            fail = (outer.fail_backup and path.startswith('/.nxc-backup-')) or (outer.fail_commit and '/.nxc-upload-' in (self.rename or '')) or (outer.fail_restore and '/.nxc-backup-' in (self.rename or ''))
                            if fail:
                                self.reply('550 Rename permission denied')
                            elif self.rename:
                                outer.files[path] = outer.files.pop(self.rename)
                                self.reply('250 Renamed')
                            else:
                                self.reply('503 RNFR required')
                            self.rename = None
                        elif command == 'DELE':
                            outer.files.pop(self.path(argument), None)
                            self.reply('250 Deleted')
                        elif command == 'QUIT':
                            self.reply('221 Goodbye')
                            return
                        else:
                            self.reply('502 Command not implemented')
                except (OSError, UnicodeError):
                    pass
                finally:
                    if self.passive:
                        self.passive.close()

            def path(self, value):
                return posixpath.normpath(posixpath.join(self.cwd, value))

            def reply(self, line):
                self.wfile.write((line + '\r\n').encode('utf-8'))
                self.wfile.flush()

        class Server(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = True

        self.server = Server(('127.0.0.1', 0), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)

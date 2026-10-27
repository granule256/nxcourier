import { initFtpSetup } from '/assets/ftp-setup.js';
/* NxCourier 本机控制台。标准浏览器 API，无第三方运行时。 */
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const state = { requestId: 0, host: null, binding: null, targetStale: false, caps: null, gdbEvents: [], imageUrl: null, directory: '/', offset: 0, nextOffset: -1, history: [], busy: new Set() };
  let ftpSetupUI = null;
  const tabNames = { overview: '概览', screen: '画面与控制', files: '文件', debug: '调试', caps: '能力', logs: '日志' };
  const pretty = value => typeof value === 'string' ? value : JSON.stringify(value, null, 2);
  const errorMessage = error => typeof error === 'string' ? error : error?.message || pretty(error) || '未知错误';
  function setText(id, value) { $(id).textContent = value; }
  function notice(message, kind = '') {
    const el = $('notice'); el.textContent = message; el.className = `notice ${kind}`; el.hidden = false;
  }
  function showTab(name) {
    if (!tabNames[name]) name = 'overview';
    document.querySelectorAll('.tab-panel').forEach(panel => { panel.hidden = panel.id !== `panel-${name}`; });
    document.querySelectorAll('[data-tab]').forEach(button => {
      const active = button.dataset.tab === name;
      button.classList.toggle('active', active); button.setAttribute('aria-current', active ? 'page' : 'false');
    });
    setText('breadcrumb-name', tabNames[name]);
    history.replaceState(null, '', `#${name}`);
  }
  document.querySelectorAll('[data-tab], [data-go]').forEach(button => button.addEventListener('click', () => showTab(button.dataset.tab || button.dataset.go)));
  window.addEventListener('hashchange', () => showTab(location.hash.slice(1)));
  showTab(location.hash.slice(1));
  setText('host-address', location.host || '127.0.0.1:47801');

  function clearDeviceCache() {
    ftpSetupUI?.reset();
    if (state.imageUrl) URL.revokeObjectURL(state.imageUrl);
    state.imageUrl = null;
    $('screen-image').removeAttribute('src'); $('dialog-image').removeAttribute('src');
    $('screen-image').hidden = true; $('screen-empty').hidden = false; $('open-image').disabled = true;
    if ($('image-dialog').open) $('image-dialog').close();
    setText('capture-time', '新目标尚未采集'); setText('screen-detail', '目标已切换，请明确采集新目标画面后操作');
    setText('screen-output', ''); $('screen-output').hidden = true;
    state.caps = null; $('caps-search').value = '';
    setText('caps-identity', '目标已切换，尚未请求新目标的能力清单。'); setText('caps-count', '未加载');
    setText('doc-title', '方法文档'); setText('caps-doc', '新目标尚未读取，请重新发现设备能力后选择方法。');
    const capsEmpty = document.createElement('p'); capsEmpty.className = 'muted'; capsEmpty.textContent = '新目标尚未读取，请点击「发现设备能力」。';
    $('caps-list').replaceChildren(capsEmpty);
    state.directory = '/'; state.offset = 0; state.nextOffset = -1; state.history = [];
    $('remote-directory').value = '/'; $('download-path').value = ''; $('upload-path').value = ''; $('upload-file').value = '';
    $('file-rows').replaceChildren();
    const row = $('file-rows').insertRow(); const cell = row.insertCell(); cell.colSpan = 4; cell.className = 'table-empty'; cell.textContent = '新目标尚未读取，请重新读取设备目录。';
    setText('files-status', '新目标尚未读取'); setText('files-page', '每页最多 50 项'); updatePagination();
    setText('transfer-output', '新目标尚无文件操作。'); setText('ftp-status', '新目标尚未检测 FTP。'); $('target-ftp-password').value = ''; $('target-ftp-clear').checked = false; setText('device-logs', '新目标设备日志尚未读取。');
    state.gdbEvents = []; setText('gdb-output', '尚无调试输出。');
    $('gdb-pid').value = ''; $('gdb-commands').value = ''; setText('current-title-info', '新目标尚未读取当前应用，请重新读取 PID 后明确附加。');
    gdbState({});
  }

  async function rpc(method, params = {}, confirmTarget = false) {
    if (state.targetStale && !['host.status', 'host.doctor', 'host.logs'].includes(method)) {
      throw new Error('后台实例或目标配置已变化，当前操作未执行。请点击右上角「刷新后台状态」，在概览确认新目标后重新操作。');
    }
    const id = ++state.requestId;
    const requestBinding = state.binding;
    // 在发送时固定用户已确认的后台与目标，不根据失败响应更新或重发操作。
    const headers = { 'Content-Type': 'application/json' };
    if (state.binding) {
      headers['X-NXC-Instance'] = state.binding.instance;
      headers['X-NXC-Config'] = state.binding.config;
    }
    const controller = new AbortController();
    // 超时仅终止浏览器等待；禁止自动重放可能已在设备上执行的操作。
    const timer = setTimeout(() => controller.abort(), 120000);
    try {
      const response = await fetch('/rpc', { method: 'POST', headers, body: JSON.stringify({ jsonrpc: '2.0', id, method, params }), signal: controller.signal });
      if (!response.ok) throw new Error(`本机后台返回 HTTP ${response.status}，请检查后台状态后重试。`);
      let packet;
      try { packet = await response.json(); } catch { throw new Error('后台返回了无法解析的响应。请检查本机日志。'); }
      if (packet.error) {
        if (packet.error.code === -32009) {
          state.targetStale = true;
          throw new Error(`${errorMessage(packet.error)} 请点击右上角「刷新后台状态」，在概览确认新目标后重新操作。`);
        }
        throw new Error(errorMessage(packet.error));
      }
      if (packet.id !== id || !Object.hasOwn(packet, 'result')) throw new Error('后台响应格式不正确，未收到对应的操作结果。');
      if (['host.status', 'host.configure'].includes(method)) {
        if (state.targetStale && method === 'host.status' && !confirmTarget) {
          throw new Error('目标配置已变化，请显式刷新后台状态并在概览确认新目标。');
        }
        const status = packet.result;
        if (status?.service !== 'nxc-host' || typeof status.instance_id !== 'string' || !status.instance_id || typeof status.config_fingerprint !== 'string' || !/^[a-f0-9]{64}$/i.test(status.config_fingerprint)) {
          throw new Error('后台未返回有效的实例与目标配置标识，请更新后台后刷新概览。');
        }
        const changed = state.binding && (state.binding.instance !== status.instance_id || state.binding.config !== status.config_fingerprint);
        if (changed && method === 'host.status' && !confirmTarget) {
          state.targetStale = true;
          throw new Error('后台实例或目标配置已变化，请显式刷新后台状态并在概览确认新目标。');
        }
        if (changed) clearDeviceCache();
        state.binding = { instance: status.instance_id, config: status.config_fingerprint };
        state.targetStale = false;
      } else if (requestBinding && state.binding && (requestBinding.instance !== state.binding.instance || requestBinding.config !== state.binding.config)) {
        throw new Error('目标已切换，旧目标的操作响应已忽略。请确认概览中的新目标后再操作。');
      }
      return packet.result;
    } catch (error) {
      if (error.name === 'AbortError') throw new Error('操作等待超时。设备可能已执行，请先检查实际状态，再决定是否重试。');
      if (error instanceof TypeError) throw new Error('无法连接本机后台。请确认 nxc 后台已启动，再点击刷新重试。');
      throw error;
    } finally { clearTimeout(timer); }
  }
  function unpack(result) {
    if (!result || !Array.isArray(result.content)) throw new Error('工具响应缺少 content，无法确认操作结果。');
    const values = [];
    for (const block of result.content) {
      if (block.type !== 'text') continue;
      let value = block.text;
      try { value = JSON.parse(value); } catch { /* 普通文本输出按原样展示。 */ }
      if (value && typeof value === 'object' && value.error) throw new Error(errorMessage(value.error));
      values.push(value);
    }
    if (result.isError) throw new Error(values.map(pretty).join('\n') || '工具执行失败，未返回详细信息。');
    return { values, text: values.map(pretty).join('\n'), content: result.content };
  }
  async function tool(name, args = {}) { return unpack(await rpc('tools/call', { name, arguments: args })); }
  async function call(method, params = {}) { return tool('nxc_call', { method, params }); }
  function payload(response) {
    return response.values.flatMap(value => value && typeof value === 'object' && Array.isArray(value.payload) ? value.payload : [value]);
  }
  async function action(key, buttons, description, task, success = '操作完成') {
    if (state.busy.has(key)) return;
    state.busy.add(key);
    const controls = [...buttons]; const previous = controls.map(button => button.disabled);
    controls.forEach(button => { button.disabled = true; button.setAttribute('aria-busy', 'true'); });
    notice(description, 'loading');
    try {
      await task(); notice(success);
    } catch (error) { notice(errorMessage(error), 'error'); }
    finally {
      controls.forEach((button, i) => { button.disabled = previous[i]; button.removeAttribute('aria-busy'); });
      state.busy.delete(key);
    }
  }
  const gdbButtons = () => document.querySelectorAll('[data-gdb], #gdb-command-form button, #gdb-refresh, #current-title');
  function gdbState(gdb = {}) {
    const attached = gdb.attached_pid !== undefined && gdb.attached_pid !== null && gdb.attached_pid !== '';
    const target = { stopped: '已暂停', running: '运行中', exited: '已退出', idle: '未附加', disconnected: '未连接', unknown: '未知' }[gdb.state] || gdb.state || '尚未报告';
    setText('gdb-value', attached ? `已附加 · PID ${gdb.attached_pid}` : gdb.connected ? '已连接 · 未附加' : gdb.running ? 'GDB 已启动' : '未启动');
    setText('gdb-detail', attached ? `目标${target} · ${gdb.backend || '后端未报告'}` : gdb.connected ? '已连接调试目标，等待附加进程' : '打开会话后再选择目标进程');
    $('gdb-dot').classList.toggle('good', !!gdb.connected);
    setText('debug-state', gdb.running ? 'GDB 已启动' : '未启动');
    setText('debug-connection', gdb.connected ? '目标已连接' : '未连接');
    setText('debug-pid', attached ? String(gdb.attached_pid) : '未附加');
    setText('debug-target', attached ? target : '未附加');
    $('debug-uncertain').hidden = !gdb.uncertain;
    if (gdb.uncertain) {
      setText('gdb-value', '结果未知 · 资源仍占用');
      setText('gdb-detail', '请先分离或关闭 GDB，再使用内存能力');
      setText('debug-state', gdb.running ? 'GDB 已启动 · 结果未知' : '会话结果未知');
      setText('debug-connection', '资源仍占用');
      setText('debug-target', '状态不确定');
      $('gdb-dot').classList.remove('good');
    }
  }
  function applyHostStatus(status) {
      if (status.service !== 'nxc-host') throw new Error('此地址未返回 NxCourier 后台标识。');
      state.host = status;
      const device = status.device || {};
      $('host-badge').className = 'badge good'; $('host-badge').replaceChildren();
      const dot = document.createElement('i'); $('host-badge').append(dot, '本机后台已就绪');
      setText('service-value', '运行中');
      setText('service-detail', `PID ${status.pid ?? '未报告'} · ${location.host}`);
      if (status.started_at) $('service-detail').title = `启动于 ${status.started_at}`;
      $('service-dot').classList.add('good');
      setText('device-value', device.connected ? '已连接' : '未连接');
      setText('device-detail', device.ip ? `${device.ip}:${device.port ?? 47800}` : '目标地址未报告');
      $('device-dot').classList.toggle('good', !!device.connected); gdbState(status.gdb);
      const config = status.config;
      if (config) {
        if (!$('target-form').dataset.dirty) {
          $('target-ip').value = config.device_ip ?? '';
          $('target-port').value = config.device_port ?? '';
          $('target-timeout').value = config.timeout ?? '';
          $('target-ftp-port').value = config.ftp_port ?? 5000;
          $('target-ftp-user').value = config.ftp_user ?? 'anonymous';
        }
        $('save-target').disabled = false;
        setText('target-current', `当前目标 ${config.device_ip}:${config.device_port} · 超时 ${config.timeout} 秒 · ${device.connected ? '已连接' : '未连接'}`);
      } else {
        $('save-target').disabled = true;
        setText('target-current', '后台未返回目标配置，请更新后台后刷新。');
      }
  }
  async function refreshStatus(report = false) {
    if (state.targetStale && !report) return;
    try {
      applyHostStatus(await rpc('host.status', {}, report || !state.host));
      if (report) notice('本机状态已更新；未向设备发送操作。');
    } catch (error) {
      if (state.targetStale) { notice(errorMessage(error), 'error'); return; }
      $('host-badge').className = 'badge bad'; setText('host-badge', '本机后台不可用');
      setText('service-value', '连接失败'); setText('service-detail', '请启动后台后点击右上角刷新');
      $('service-dot').classList.remove('good');
      setText('device-value', '状态未知'); setText('device-detail', '无法读取后台，先前状态不可用');
      $('device-dot').classList.remove('good'); setText('gdb-value', '状态未知');
      setText('gdb-detail', '无法读取后台'); $('gdb-dot').classList.remove('good');
      $('save-target').disabled = true;
      setText('target-current', '无法读取当前目标配置，请恢复后台连接后刷新。');
      ['debug-state', 'debug-connection', 'debug-pid', 'debug-target'].forEach(id => setText(id, '状态未知'));
      if (report || !state.host) notice(errorMessage(error), 'error');
    }
  }
  $('refresh-status').addEventListener('click', () => refreshStatus(true));
  $('diagnose').addEventListener('click', () => action('doctor', [$('diagnose')], '正在检查本机环境…', async () => { setText('doctor-output', pretty(await rpc('host.doctor'))); }, '本机诊断完成，请查看实际检测结果。'));
  $('target-form').addEventListener('input', () => { $('target-form').dataset.dirty = 'true'; });
  $('target-form').addEventListener('submit', event => {
    event.preventDefault();
    action('configure', $('target-form').querySelectorAll('input, button'), '正在保存共享后台目标配置…', async () => {
      const device_ip = $('target-ip').value.trim();
      const device_port = Number($('target-port').value);
      const timeout = Number($('target-timeout').value);
      if (!device_ip || /\s/.test(device_ip)) throw new Error('设备地址不能为空或含空白。');
      if (!Number.isInteger(device_port) || device_port < 1 || device_port > 65535) throw new Error('设备端口需为 1 至 65535 的整数。');
      if (!Number.isFinite(timeout) || timeout <= 0 || timeout > 3600) throw new Error('请求超时需大于 0 且不超过 3600 秒。');
      const ftp_port = Number($('target-ftp-port').value);
      if (!Number.isInteger(ftp_port) || ftp_port < 1 || ftp_port > 65535) throw new Error('FTP 端口需为 1 到 65535。');
      const settings = { device_ip, device_port, timeout, ftp_port, ftp_user: $('target-ftp-user').value };
      if ($('target-ftp-clear').checked) settings.ftp_password = '';
      else if ($('target-ftp-password').value) settings.ftp_password = $('target-ftp-password').value;
      const status = await rpc('host.configure', settings);
      $('target-ftp-password').value = ''; $('target-ftp-clear').checked = false;
      delete $('target-form').dataset.dirty;
      applyHostStatus(status);
    }, '目标配置已保存。当前连接状态已更新；设备将在下一次明确操作时连接。');
  });

  function showCapture(response) {
    const image = response.content.find(block => block.type === 'image');
    if (!image || !image.data) throw new Error(response.text || '操作未返回图片，无法展示设备画面。');
    if (!['image/jpeg', 'image/png', 'image/webp'].includes(image.mimeType)) throw new Error(`不支持的图片格式：${image.mimeType}`);
    const bytes = Uint8Array.from(atob(image.data), character => character.charCodeAt(0));
    if (state.imageUrl) URL.revokeObjectURL(state.imageUrl);
    state.imageUrl = URL.createObjectURL(new Blob([bytes], { type: image.mimeType }));
    $('screen-image').src = state.imageUrl; $('dialog-image').src = state.imageUrl;
    $('screen-empty').hidden = true; $('screen-image').hidden = false; $('open-image').disabled = false;
    const time = new Date().toLocaleTimeString('zh-CN', { hour12: false });
    setText('capture-time', `采集于 ${time}`); setText('screen-detail', '展示最近一次响应 · 点击原图查看返回分辨率');
    $('screen-output').hidden = !response.text; setText('screen-output', response.text);
  }
  const screenButtons = () => document.querySelectorAll('#capture, [data-key]');
  $('capture').addEventListener('click', () => action('screen', screenButtons(), '正在采集设备画面…', async () => { showCapture(await tool('nxc_see', { max_width: 1280 })); await refreshStatus(); }, '设备画面已更新。'));
  document.querySelectorAll('[data-key]').forEach(button => button.addEventListener('click', () => action('screen', screenButtons(), `正在发送 ${button.textContent.trim()}，等待动作后画面…`, async () => {
    const response = await tool('nxc_step', { steps: `${button.dataset.key}:120`, wait: $('step-wait').value, max_width: 1280 });
    showCapture(response); await refreshStatus();
  }, `按键 ${button.textContent.trim()} 操作完成，画面已更新。`)));
  $('open-image').addEventListener('click', () => $('image-dialog').showModal());
  $('close-image').addEventListener('click', () => $('image-dialog').close());

  function absolutePath(value) {
    const path = value.trim();
    if (!path.startsWith('/') || path.includes('\0')) throw new Error('设备路径需为以 / 开头的绝对路径。');
    return path;
  }
  function formatBytes(value) {
    const size = Number(value);
    if (!Number.isFinite(size) || size < 0) return '—';
    if (size < 1024) return `${size} B`;
    return `${(size / (size < 1048576 ? 1024 : 1048576)).toFixed(1)} ${size < 1048576 ? 'KiB' : 'MiB'}`;
  }
  const fileTransport = () => $('file-transport').value;
  $('file-transport').addEventListener('change', () => {
    state.offset = 0; state.nextOffset = -1; state.history = []; updatePagination();
    $('file-rows').replaceChildren(); setText('files-status', '传输方式已切换，请重新读取目录。');
  });
  $('probe-ftp').addEventListener('click', () => action('file-io', fileControls(), '正在检测设备 FTPD…', async () => {
    const status = await rpc('host.ftp.status');
    setText('ftp-status', status.available ? `FTP 可用 · ${state.host?.config?.device_ip}:${state.host?.config?.ftp_port || 5000}` : (status.message || 'FTP 不可用，请在设备上运行 FTPD。'));
  }, 'FTP 检测完成。'));
  const fileControls = () => document.querySelectorAll('#files-form button, #files-prev, #files-next, #file-rows button, #file-transport, #probe-ftp, #upload-form button, #download-form button, #ftp-setup-card button, #ftp-setup-file');
  ftpSetupUI = initFtpSetup({ $, rpc, action, state, fileControls, refreshStatus, tool });
  function updatePagination() {
    $('files-prev').disabled = state.history.length === 0 || state.busy.has('file-io');
    $('files-next').disabled = state.nextOffset < 0 || state.busy.has('file-io');
  }
  async function listFiles(directory, offset = 0, nextHistory = []) {
    directory = absolutePath(directory);
    const meta = await rpc('host.files.list', { path: directory, offset, count: 50, transport: fileTransport() });
    const entries = meta.entries || [];
    if (!meta) throw new Error('设备目录响应缺少分页信息，请先更新设备能力或查看 fs.ls 文档。');
    state.directory = directory; state.offset = offset; state.history = nextHistory; state.nextOffset = Number(meta.next_offset);
    $('remote-directory').value = directory;
    const body = $('file-rows'); body.replaceChildren();
    if (!entries.length) {
      const row = body.insertRow(); const cell = row.insertCell(); cell.colSpan = 4; cell.className = 'table-empty'; cell.textContent = '此目录没有更多项目。';
    }
    for (const entry of entries) {
      const row = body.insertRow(); const name = row.insertCell();
      const fullPath = `${directory.replace(/\/$/, '')}/${entry.name}`;
      const link = document.createElement('button'); link.className = 'text-button'; link.textContent = `${entry.type === 'dir' ? '▱ ' : '▫ '}${entry.name}`;
      link.addEventListener('click', () => {
        if (entry.type === 'dir') navigateFiles(fullPath);
        else { $('download-path').value = fullPath; fileStat(); }
      }); name.append(link);
      row.insertCell().textContent = entry.type === 'dir' ? '目录' : entry.type === 'file' ? '文件' : '未知';
      row.insertCell().textContent = entry.type === 'dir' ? '—' : formatBytes(entry.size);
      const operation = row.insertCell(); const button = document.createElement('button'); button.className = 'button'; button.textContent = entry.type === 'dir' ? '打开 →' : '下载 ↓';
      button.addEventListener('click', () => entry.type === 'dir' ? navigateFiles(fullPath) : downloadFile(fullPath)); operation.append(button);
    }
    setText('files-status', `${directory} · ${meta.transport} · 本页 ${entries.length} 项${meta.legacy && meta.truncated ? ' · 旧模块列表被截断，请使用 FTP 获取完整目录' : ''}`);
    setText('files-page', `偏移 ${offset} · ${state.nextOffset >= 0 ? '后续还有内容' : '已到末页'}`);
    updatePagination(); await refreshStatus();
  }
  async function navigateFiles(directory, offset = 0, historyEntries = []) {
    await action('file-io', fileControls(), '正在读取设备目录…', () => listFiles(directory, offset, historyEntries), '目录已读取。'); updatePagination();
  }
  $('files-form').addEventListener('submit', event => { event.preventDefault(); navigateFiles($('remote-directory').value); });
  $('parent-directory').addEventListener('click', () => navigateFiles($('remote-directory').value.replace(/\/+$/, '').replace(/\/[^/]*$/, '') || '/'));
  $('files-prev').addEventListener('click', () => {
    const previous = state.history.slice(); const offset = previous.pop(); if (offset !== undefined) navigateFiles(state.directory, offset, previous);
  });
  $('files-next').addEventListener('click', () => { if (state.nextOffset >= 0) navigateFiles(state.directory, state.nextOffset, [...state.history, state.offset]); });
  async function fileStat() {
    await action('file-io', fileControls(), '正在读取文件属性…', async () => {
      setText('transfer-output', pretty(await rpc('host.files.stat', { path: absolutePath($('download-path').value), transport: fileTransport() }))); await refreshStatus();
    }, '文件属性已读取。');
  }
  $('file-stat').addEventListener('click', fileStat);
  async function downloadFile(path) {
    await action('file-io', fileControls(), '正在下载设备文件…', async () => {
      path = absolutePath(path); $('download-path').value = path;
      const result = await rpc('host.download', { remote_path: path, transport: fileTransport() });
      if (typeof result.data !== 'string' || result.bytes > 8 * 1024 * 1024) throw new Error('下载响应无效或超过 8 MiB 限制。');
      const bytes = Uint8Array.from(atob(result.data), char => char.charCodeAt(0));
      if (bytes.length !== Number(result.bytes)) throw new Error('下载字节数与后台声明不一致，未保存文件。');
      if (result.sha256 && crypto.subtle) {
        const digest = await crypto.subtle.digest('SHA-256', bytes);
        const hash = [...new Uint8Array(digest)].map(byte => byte.toString(16).padStart(2, '0')).join('');
        if (hash.toLowerCase() !== result.sha256.toLowerCase()) throw new Error('下载文件 SHA-256 校验不一致，未保存文件。');
      }
      const url = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
      const anchor = document.createElement('a'); anchor.href = url; anchor.download = result.name || path.split('/').pop() || 'download';
      document.body.append(anchor); anchor.click(); anchor.remove(); setTimeout(() => URL.revokeObjectURL(url), 60000);
      const { data, ...details } = result; setText('transfer-output', `下载已交给浏览器保存。\n${pretty(details)}`); await refreshStatus();
    }, '文件已下载并交给浏览器保存。');
  }
  $('download-form').addEventListener('submit', event => { event.preventDefault(); downloadFile($('download-path').value); });
  $('upload-file').addEventListener('change', () => {
    const file = $('upload-file').files[0];
    if (file && !$('upload-path').value) $('upload-path').value = `${state.directory.replace(/\/$/, '')}/${file.name}`;
  });
  $('upload-form').addEventListener('submit', event => {
    event.preventDefault(); action('file-io', fileControls(), '正在上传文件并校验…', async () => {
      const file = $('upload-file').files[0];
      if (!file) throw new Error('请先选择本机文件。');
      if (file.size > 8 * 1024 * 1024) throw new Error('文件超过 8 MiB，请使用 CLI 分块传输。');
      const remotePath = absolutePath($('upload-path').value);
      const data = await new Promise((resolve, reject) => {
        const reader = new FileReader(); reader.onload = () => resolve(String(reader.result).split(',')[1]); reader.onerror = () => reject(new Error('无法读取所选本机文件。')); reader.readAsDataURL(file);
      });
      const result = await rpc('host.upload', { remote_path: remotePath, data, transport: fileTransport() });
      const receipt = [
        `传输方式：${result.transport || '后台未报告'}`,
        `目标路径：${result.remote || remotePath}`,
        `文件大小：${result.bytes !== undefined ? `${result.bytes} 字节（${formatBytes(result.bytes)}）` : '后台未报告'}`,
        `SHA-256：${result.sha256 || '后台未报告'}`,
        `回读校验：${result.verified === true ? '通过' : result.verified === false ? '未通过' : '后台未确认'}`,
      ];
      if (result.backup) receipt.push(`备份路径：${result.backup}`);
      if (result.rollback) receipt.push(`回滚路径：${result.rollback}`);
      setText('transfer-output', receipt.join('\n')); await refreshStatus();
    }, '文件上传完成，请按需重新读取目录。');
  });

  function updateGdbResponse(response) {
    const messages = [];
    for (const value of response.values) {
      if (value && typeof value === 'object') {
        const status = value.status && typeof value.status === 'object' ? value.status : value;
        if (Object.hasOwn(status, 'running') || Object.hasOwn(status, 'connected')) gdbState(status);
        if (typeof value.output === 'string' && value.output.trim()) messages.push(value.output.trim());
        if (Array.isArray(status.events)) {
          const events = status.events.map(String);
          // 事件为有界尾部快照：只展示与上次快照不重合的新事件。
          let overlap = Math.min(state.gdbEvents.length, events.length);
          while (overlap > 0 && !state.gdbEvents.slice(-overlap).every((event, i) => event === events[i])) overlap--;
          const fresh = events.slice(overlap).filter(event => /^[*=]|^GDB reader:/.test(event)).slice(-12);
          if (fresh.length) messages.push(`事件\n${fresh.join('\n')}`);
          state.gdbEvents = events;
        }
        if (status.uncertain) messages.push('结果未知 / 调试资源仍占用。请先分离或关闭 GDB，再使用内存能力。');
      } else if (typeof value === 'string' && value.trim()) {
        messages.push(value.trim());
      }
    }
    if (!messages.length) return;
    const output = $('gdb-output');
    const prefix = output.textContent === '尚无调试输出。' ? '' : `${output.textContent}\n\n`;
    output.textContent = `${prefix}[${new Date().toLocaleTimeString('zh-CN', { hour12: false })}]\n${messages.join('\n\n')}`.slice(-150000);
    output.scrollTop = output.scrollHeight;
  }
  async function gdbAction(operation, args = {}) {
    await action('gdb', gdbButtons(), `正在执行 GDB ${operation}…`, async () => {
      if (operation === 'attach') {
        const pid = $('gdb-pid').value.trim();
        if (!/^(?:[1-9]\d*|0x[\da-f]+)$/i.test(pid)) throw new Error('请输入有效的正整数 PID（也支持 0x 十六进制）。');
        // 后台 attach 合同使用十进制 PID；保留字符串避免大整数丢失精度。
        args.pid = BigInt(pid).toString(10);
      }
      if (operation === 'open') args.connect = true;
      updateGdbResponse(await tool('nxc_gdb', { action: operation, ...args }));
      await refreshStatus();
    }, `GDB ${operation} 已返回，请查看会话状态与输出。`);
  }
  document.querySelectorAll('[data-gdb]').forEach(button => button.addEventListener('click', () => gdbAction(button.dataset.gdb)));
  $('gdb-refresh').addEventListener('click', () => gdbAction('status'));
  $('gdb-command-form').addEventListener('submit', event => {
    event.preventDefault(); const commands = $('gdb-commands').value.split('\n').map(line => line.trim()).filter(Boolean);
    if (commands.length) gdbAction('run', { commands });
  });
  $('current-title').addEventListener('click', () => action('gdb', gdbButtons(), '正在读取当前应用 PID…', async () => {
    const response = await call('title.current'); const current = payload(response).find(item => item && typeof item === 'object' && item.pid !== undefined);
    setText('current-title-info', response.text);
    if (!current) throw new Error('设备未报告正在运行的应用 PID。');
    $('gdb-pid').value = String(current.pid); await refreshStatus();
  }, '当前应用 PID 已填入，尚未附加进程。'));

  function renderCaps() {
    if (!state.caps) return;
    const filter = $('caps-search').value.trim().toLowerCase(); const list = $('caps-list'); list.replaceChildren();
    let matches = 0;
    for (const group of state.caps.tools) {
      const methods = (group.methods || []).filter(method => `${method.call} ${method.help}`.toLowerCase().includes(filter));
      if (!methods.length) continue;
      const heading = document.createElement('div'); heading.className = 'caps-group'; heading.textContent = `${group.name} · ${methods.length}`; list.append(heading);
      for (const method of methods) {
        matches++;
        const button = document.createElement('button'); button.className = 'method-button';
        const risk = document.createElement('span'); risk.className = 'risk'; risk.textContent = method.risk || '未标注';
        const title = document.createElement('strong'); title.textContent = method.call;
        const help = document.createElement('small'); help.textContent = method.help || '设备未提供简介'; button.append(risk, title, help);
        button.addEventListener('click', () => loadDoc(method, button)); list.append(button);
      }
    }
    if (!matches) { const empty = document.createElement('p'); empty.className = 'muted'; empty.textContent = '没有匹配的方法。'; list.append(empty); }
  }
  async function loadDoc(method, button) {
    await action('doc', [button], `正在读取 ${method.call} 文档…`, async () => {
      setText('doc-title', method.call);
      const response = await call('tools.doc', { name: method.call });
      setText('caps-doc', `设备方法元数据\n${pretty(method)}\n\n设备文档\n${response.text}`);
    }, '方法文档已读取。');
  }
  $('load-caps').addEventListener('click', () => action('caps', [$('load-caps')], '正在从设备发现能力…', async () => {
    const response = await tool('nxc_caps');
    const caps = response.values.find(value => value && typeof value === 'object' && Array.isArray(value.tools));
    if (!caps) throw new Error('设备未返回有效的动态能力清单。');
    state.caps = caps; setText('caps-count', `${caps.toolCount ?? caps.tools.length} 个工具 · ${caps.methodCount ?? caps.tools.reduce((sum, group) => sum + (group.methods || []).length, 0)} 个方法`);
    setText('caps-identity', `设备身份 · ${pretty(caps.identity || '未报告')}`); renderCaps(); await refreshStatus();
  }, '设备能力清单已更新。'));
  $('caps-search').addEventListener('input', renderCaps);
  $('load-mcp-tools').addEventListener('click', () => action('mcp', [$('load-mcp-tools')], '正在读取后台 MCP 工具清单…', async () => {
    const response = await rpc('tools/list');
    if (!Array.isArray(response.tools)) throw new Error('后台未返回工具列表。');
    const container = $('mcp-tools'); container.replaceChildren();
    for (const item of response.tools) {
      const section = document.createElement('div'); section.className = 'mcp-tool';
      const title = document.createElement('strong'); title.textContent = item.name;
      const description = document.createElement('p'); description.textContent = item.description;
      const details = document.createElement('details'); const summary = document.createElement('summary'); summary.textContent = '查看参数定义';
      const schema = document.createElement('pre'); schema.className = 'output light-output'; schema.textContent = pretty(item.inputSchema); details.append(summary, schema);
      section.append(title, description, details); container.append(section);
    }
    $('mcp-card').hidden = false;
  }, '后台 MCP 工具清单已读取。'));
  $('load-host-logs').addEventListener('click', () => action('host-logs', [$('load-host-logs')], '正在读取本机后台日志…', async () => {
    const response = await rpc('host.logs', { lines: 80 });
    if (!Array.isArray(response.lines)) throw new Error('后台未返回有效的日志行。');
    setText('host-logs', response.lines.length ? response.lines.join('\n') : '后台暂无日志。');
  }, '本机日志已更新。'));
  $('load-device-logs').addEventListener('click', () => action('device-logs', [$('load-device-logs')], '正在读取设备环形日志…', async () => {
    const response = await call('log.dump', { lines: 40 }); setText('device-logs', response.text || '设备未返回日志文本。'); await refreshStatus();
  }, '设备日志已更新。'));
  // 首次仅读取本机状态；切换页面不会自动访问设备、采集画面或启动调试。
  refreshStatus();
})();

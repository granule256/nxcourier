// 后台FTP向导：准备包与设备安装分开，运行状态以实际检测为准。
export function initFtpSetup({ $, rpc, action, state, fileControls, refreshStatus, tool }) {
  let detected = null;
  let prepared = null;
  let generation = 0;
  const labels = { ready: '已就绪', missing: '尚未安装', auth_required: '需要配置登录',
    pending_activation: '待加载或恢复', conflict: '已有其他模块', unreachable: '设备不可达', error: '检测失败' };
  const text = (id, value) => { $(id).textContent = String(value ?? '未确认'); };
  const binding = () => `${state.binding?.instance || ''}/${state.binding?.config || ''}`;

  function buttons() {
    if (state.busy.has('file-io')) return;
    const installed = detected?.module?.installed === true;
    const blocked = ['conflict','unreachable'].includes(detected?.state);
    $('ftp-install-official').disabled = installed || blocked;
    $('ftp-install-local').disabled = !prepared || installed || blocked;
    $('ftp-enable-anonymous').disabled = !detected?.can_configure || (detected?.configuration?.anonymous === true && detected?.module?.boot_enabled === true);
    $('ftp-use-device-port').hidden = detected?.configuration?.port_matches !== false;
    $('ftp-recheck-reboot').hidden = detected?.state !== 'pending_activation';
    $('ftp-restart').hidden = detected?.state !== 'pending_activation';
  }

  function showStatus(result) {
    detected = { ...detected, ...result };
    const module = detected.module || {};
    const config = detected.configuration || {};
    text('ftp-setup-state', labels[detected.state] || '待检测');
    text('ftp-setup-summary', detected.message || '请核对下方检测结果。');
    text('ftp-module-check', module.installed === true ? '已安装' : module.installed === false ? '未安装' : '未知');
    text('ftp-version-check', module.known_version || (module.installed ? '现有版本未识别' : '—'));
    text('ftp-boot-check', module.boot_enabled === true ? '已启用' : module.boot_enabled === false ? '未启用' : '未知');
    text('ftp-login-check', detected.ftp?.available ? '登录及目录验证通过' : detected.ftp?.message || '尚不可用');
    text('ftp-config-check', config.exists === true ? `${config.anonymous ? '匿名登录' : '账号登录'} · 端口 ${config.port ?? '未知'}${config.port_matches === false ? '（与PC设置不同）' : ''}` : config.exists === false ? '配置缺失' : '未知');
    if (result.backup || result.receipt) text('ftp-setup-output', JSON.stringify(result.receipt || { backup: result.backup }, null, 2));
    buttons();
  }

  function operationResult(result) {
    showStatus(result);
    if (['error','unreachable','conflict'].includes(result.state)) {
      throw new Error(`${result.message || '操作未完成'}\n${result.receipt ? JSON.stringify(result.receipt,null,2) : ''}`);
    }
  }

  function showPackage(result) {
    prepared = result;
    text('ftp-package-detail', `版本：${result.version}\n来源：${result.source === 'official' ? '官方固定版本' : '本地安装包'}\n安装包 SHA-256：${result.sha256}\n${JSON.stringify(result.files || [], null, 2)}`);
    buttons();
  }

  function progress(result) {
    text('ftp-setup-progress-text', result.message || '正在处理…');
    const completed = Number(result.completed);
    const total = Number(result.total);
    const bar = $('ftp-setup-progress');
    if (Number.isFinite(total) && total > 0 && Number.isFinite(completed)) {
      bar.max = total; bar.value = Math.min(total, Math.max(0, completed));
    } else bar.removeAttribute('value');
  }

  async function run(description, task, success) {
    const target = binding();
    const revision = generation;
    const ensure = () => {
      if (target !== binding() || revision !== generation) throw new Error('目标已变化，安装操作已停止。请重新检测并准备安装包。');
    };
    await action('file-io', fileControls(), description, async () => {
      ensure();
      $('ftp-setup-progress-area').hidden = false;
      text('ftp-setup-progress-text', description);
      $('ftp-setup-progress').removeAttribute('value');
      let polling = false;
      let finished = false;
      const timer = setInterval(async () => {
        if (finished || polling || target !== binding() || revision !== generation) return;
        polling = true;
        try { const result = await rpc('host.ftp.setup.progress'); ensure(); if (!finished) progress(result); }
        catch { /* 主请求负责展示失败；进度失败不重放设备操作。 */ }
        finally { polling = false; }
      }, 1000);
      try {
        await task(ensure);
        ensure();
        text('ftp-setup-progress-text', success);
        $('ftp-setup-progress').max = 1; $('ftp-setup-progress').value = 1;
      } catch (error) {
        if (target === binding() && revision === generation) {
          text('ftp-setup-progress-text', '操作未完成，请查看原因及恢复路径。');
          text('ftp-setup-output', error.message || error);
        }
        throw error;
      } finally { finished = true; clearInterval(timer); }
    }, success);
    buttons();
  }

  async function inspect() {
    await run('正在检测后台 FTP 模块和登录状态…', async ensure => {
      const result = await rpc('host.ftp.setup.status'); ensure(); showStatus(result);
    }, '后台 FTP 检测完成。');
  }

  $('ftp-setup-detect').addEventListener('click', inspect);
  $('ftp-recheck-reboot').addEventListener('click', inspect);
  $('ftp-install-official').addEventListener('click', () => run('正在下载并校验官方固定版本…', async ensure => {
    const manifest = await rpc('host.ftp.setup.prepare', { source: 'official' }); ensure(); showPackage(manifest);
    text('ftp-setup-progress-text', '安装包已校验，正在安装并配置…');
    ensure(); const result = await rpc('host.ftp.setup.install', { package_id: manifest.package_id, anonymous: true });
    ensure(); operationResult(result);
  }, '安装步骤完成，请以检测结果确认是否需要重启。'));

  $('ftp-setup-file').addEventListener('change', () => run('正在校验本地安装包…', async ensure => {
    const file = $('ftp-setup-file').files[0];
    if (!file) throw new Error('请先选择安装包。');
    if (file.size > 4 * 1024 * 1024) throw new Error('后台 FTP 安装包最多 4 MiB。');
    const data = await new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => resolve(String(reader.result).split(',')[1]);
      reader.onerror = () => reject(new Error('无法读取安装包。'));
      reader.readAsDataURL(file);
    });
    ensure(); const manifest = await rpc('host.ftp.setup.prepare', { source: 'local', data });
    ensure(); showPackage(manifest);
  }, '本地安装包已校验；确认版本后可安装。'));

  $('ftp-install-local').addEventListener('click', () => run('正在安装已校验的本地包…', async ensure => {
    if (!prepared) throw new Error('请先导入并校验安装包。');
    ensure(); const result = await rpc('host.ftp.setup.install', { package_id: prepared.package_id, anonymous: true });
    ensure(); operationResult(result);
  }, '安装步骤完成，请以检测结果确认是否需要重启。'));

  $('ftp-enable-anonymous').addEventListener('click', () => run('正在备份配置并启用匿名登录…', async ensure => {
    const result = await rpc('host.ftp.setup.configure'); ensure(); operationResult(result);
  }, '匿名配置操作完成，请核对登录状态。'));

  $('ftp-use-device-port').addEventListener('click', async () => {
    const port = detected?.configuration?.port;
    if (!Number.isInteger(port) || port < 1 || port > 65535) return;
    await action('file-io', fileControls(), '正在将PC连接端口与设备配置对齐…', async () => {
      await rpc('host.configure', { ftp_port: port });
      await refreshStatus();
      showStatus(await rpc('host.ftp.setup.status'));
    }, '连接端口已对齐，后台服务已重新检测。');
    buttons();
  });

  $('ftp-restart').addEventListener('click', () => $('ftp-restart-dialog').showModal());
  $('ftp-restart-cancel').addEventListener('click', () => $('ftp-restart-dialog').close());
  $('ftp-restart-confirm').addEventListener('click', () => {
    $('ftp-restart-dialog').close();
    run('正在请求设备重启…', async ensure => {
      if (state.host?.gdb?.running || state.host?.gdb?.uncertain) throw new Error('请先关闭 GDB 会话再重启。');
      const before = await rpc('host.boot'); ensure();
      if (!Number.isInteger(before.boot)) throw new Error('无法确认开机号，未发送重启。');
      try { await tool('nxc_call', { method: 'power.reboot', params: { confirm: 'I-KNOW-THIS-REBOOTS' } }); }
      catch (error) { if (!/未知|连接|Connection|响应/.test(error.message || '')) throw error; }
      const deadline = Date.now() + 90000;
      while (Date.now() < deadline) {
        ensure();
        text('ftp-setup-progress-text', '等待设备重新连接…');
        try {
          const current = await rpc('host.boot'); ensure();
          if (Number.isInteger(current.boot) && current.boot !== before.boot) {
            const result = await rpc('host.ftp.setup.status'); ensure(); showStatus(result); await refreshStatus(); return;
          }
        } catch (error) { if (targetChanged(error)) throw error; }
        await new Promise(resolve => setTimeout(resolve, 2000));
      }
      throw new Error('尚未确认重启完成。请检查设备，之后点击“我已重启，重新检测”；不会重复发送重启。');
    }, '设备已重新连接，后台 FTP 已重新检测。');
  });
  const targetChanged = error => /目标已变化|配置已变化|实例已变化/.test(error.message || '');

  return { reset() {
    generation += 1; detected = null; prepared = null;
    text('ftp-setup-state', '待检测'); text('ftp-setup-summary', '目标已切换，请重新检测后台服务。');
    for (const id of ['ftp-module-check','ftp-version-check','ftp-boot-check','ftp-config-check','ftp-login-check']) text(id, '未知');
    text('ftp-package-detail', '尚未准备安装包。'); text('ftp-setup-output', '尚无安装操作。');
    $('ftp-setup-file').value = ''; $('ftp-setup-progress-area').hidden = true;
    if ($('ftp-restart-dialog').open) $('ftp-restart-dialog').close();
    buttons();
  } };
}

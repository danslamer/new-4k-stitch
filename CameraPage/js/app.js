/**
 * 江苏攸洋智控管理平台 - 前端交互逻辑
 */

// ========== 页面导航 ==========
const PAGE_TITLES = {
  home: '图像输出',
  network: '网络配置',
  algorithm: '算法管理',
  devices: '设备管理'
};

const PAGE_STORAGE_KEY = 'camera-platform-active-page';
const PAGE_TRANSITION_MS = 260;
let pageSwitchTimer = null;

function setBreadcrumbText(text, instant = false) {
  const el = document.getElementById('breadcrumb-current');
  if (!el || el.textContent === text) return;
  if (instant) {
    el.textContent = text;
    return;
  }
  el.classList.add('text-switching');
  setTimeout(() => {
    el.textContent = text;
    el.classList.remove('text-switching');
    el.classList.add('text-switched');
    requestAnimationFrame(() => el.classList.remove('text-switched'));
  }, 140);
}

/** 切换左侧菜单页面，并记住当前页以便刷新后恢复 */
function switchPage(page, instant = false) {
  if (!PAGE_TITLES[page]) return;
  const nextPanel = document.getElementById(`page-${page}`);
  const currentPanel = document.querySelector('.page-panel.active');
  if (currentPanel === nextPanel) return;

  if (pageSwitchTimer) {
    clearTimeout(pageSwitchTimer);
    pageSwitchTimer = null;
  }

  document.querySelectorAll('.nav-item').forEach(n => {
    n.classList.toggle('active', n.dataset.page === page);
  });
  setBreadcrumbText(PAGE_TITLES[page], instant);

  const activatePanel = () => {
    document.querySelectorAll('.page-panel').forEach(p => {
      p.classList.remove('active', 'page-leaving', 'page-entering');
    });
    nextPanel.classList.add('active');
    if (!instant) {
      nextPanel.classList.add('page-entering');
      requestAnimationFrame(() => {
        requestAnimationFrame(() => nextPanel.classList.remove('page-entering'));
      });
    }
    try {
      sessionStorage.setItem(PAGE_STORAGE_KEY, page);
    } catch (_) { /* 隐私模式等场景忽略 */ }
  };

  if (instant || !currentPanel) {
    activatePanel();
    return;
  }

  currentPanel.classList.add('page-leaving');
  pageSwitchTimer = setTimeout(() => {
    pageSwitchTimer = null;
    activatePanel();
  }, PAGE_TRANSITION_MS);
}

/** 从 sessionStorage 恢复上次停留的页面 */
function restoreActivePage() {
  let page = 'home';
  try {
    const saved = sessionStorage.getItem(PAGE_STORAGE_KEY);
    if (saved && PAGE_TITLES[saved]) page = saved;
  } catch (_) {}
  switchPage(page, true);
  if (page === 'network' && typeof drawBandwidthChart === 'function') {
    requestAnimationFrame(drawBandwidthChart);
  }
}

document.querySelectorAll('.nav-item').forEach(item => {
  item.addEventListener('click', () => {
    const page = item.dataset.page;
    switchPage(page);
    if (page === 'network' && typeof drawBandwidthChart === 'function') {
      requestAnimationFrame(drawBandwidthChart);
    }
  });
});

// ========== Toast 提示 ==========
function showToast(message, type = 'success') {
  const toast = document.getElementById('toast');
  toast.classList.remove('show');
  void toast.offsetWidth;
  toast.textContent = message;
  toast.className = `toast show ${type}`;
  clearTimeout(toast._hideTimer);
  toast._hideTimer = setTimeout(() => toast.classList.remove('show'), 2800);
}

// ========== 弹窗控制 ==========
function openModal(id) {
  const overlay = document.getElementById(id);
  overlay.classList.add('show');
  requestAnimationFrame(() => overlay.classList.add('show-animate'));
}

function closeModal(id) {
  if (id === 'modal-algo-preview') stopLocatePreviewAnim();
  const overlay = document.getElementById(id);
  overlay.classList.remove('show-animate');
  setTimeout(() => overlay.classList.remove('show'), 240);
}

document.querySelectorAll('[data-close]').forEach(btn => {
  btn.addEventListener('click', () => closeModal(btn.dataset.close));
});

document.querySelectorAll('.modal-overlay').forEach(overlay => {
  overlay.addEventListener('click', e => {
    if (e.target === overlay) closeModal(overlay.id);
  });
});

// ========== 复制功能 ==========
document.querySelectorAll('.copy-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    const el = document.getElementById(btn.dataset.copy);
    navigator.clipboard.writeText(el.textContent).then(() => {
      showToast('已复制到剪贴板');
    });
  });
});

// ========== 1. 首页 - 图像输出 ==========

const streamConfigs = {
  main: { encoding: 'H.265', resolution: '1920x1080', fps: '25', bitrateType: 'CBR', bitrate: 4096, iframe: 50, quality: 'medium', smart: true },
  sub: { encoding: 'H.264', resolution: '1280x720', fps: '15', bitrateType: 'CBR', bitrate: 1024, iframe: 50, quality: 'medium', smart: false },
  third: { encoding: 'H.264', resolution: '704x576', fps: '10', bitrateType: 'VBR', bitrate: 512, iframe: 50, quality: 'low', smart: false }
};

let currentStream = 'main';

document.querySelectorAll('.stream-tab').forEach(tab => {
  tab.addEventListener('click', () => {
    document.querySelectorAll('.stream-tab').forEach(t => t.classList.remove('active'));
    tab.classList.add('active');
    currentStream = tab.dataset.stream;
    const form = document.getElementById('stream-config-form');
    if (form) form.classList.add('ui-switching');
    loadStreamConfig(currentStream);
    updateLiveParams();
    requestAnimationFrame(() => {
      if (!form) return;
      form.classList.remove('ui-switching');
      form.classList.add('ui-switched');
      setTimeout(() => form.classList.remove('ui-switched'), 360);
    });
  });
});

function loadStreamConfig(stream) {
  const cfg = streamConfigs[stream];
  document.getElementById('cfg-encoding').value = cfg.encoding;
  document.getElementById('cfg-resolution').value = cfg.resolution;
  document.getElementById('cfg-fps').value = cfg.fps;
  document.getElementById('cfg-bitrate-type').value = cfg.bitrateType;
  document.getElementById('cfg-bitrate').value = cfg.bitrate;
  document.getElementById('cfg-iframe').value = cfg.iframe;
  document.getElementById('cfg-quality').value = cfg.quality;
  document.getElementById('cfg-smart').checked = cfg.smart;
}

function saveStreamConfig() {
  streamConfigs[currentStream] = {
    encoding: document.getElementById('cfg-encoding').value,
    resolution: document.getElementById('cfg-resolution').value,
    fps: document.getElementById('cfg-fps').value,
    bitrateType: document.getElementById('cfg-bitrate-type').value,
    bitrate: parseInt(document.getElementById('cfg-bitrate').value),
    iframe: parseInt(document.getElementById('cfg-iframe').value),
    quality: document.getElementById('cfg-quality').value,
    smart: document.getElementById('cfg-smart').checked
  };
}

const STREAM_LABELS = { main: '主码流', sub: '子码流', third: '第三码流' };

function updateLiveParams() {
  const cfg = streamConfigs[currentStream];
  const res = cfg.resolution.replace('x', '×');
  document.getElementById('live-stream-type').textContent = STREAM_LABELS[currentStream];
  document.getElementById('live-resolution').textContent = res;
  document.getElementById('live-fps').textContent = `${cfg.fps} fps`;
  document.getElementById('live-encoding').textContent = cfg.encoding;
  document.getElementById('live-bitrate-type').textContent = cfg.bitrateType;
  document.getElementById('live-iframe').textContent = cfg.iframe;

  const variance = Math.floor(Math.random() * 200) - 100;
  const liveBitrate = Math.max(256, cfg.bitrate + variance);
  document.getElementById('live-bitrate').textContent = `${liveBitrate} Kbps`;

  updatePreviewPlaceholder();
}

document.getElementById('btn-save-stream').addEventListener('click', () => {
  saveStreamConfig();
  updateLiveParams();
  showToast('码流配置已保存');
});

document.getElementById('btn-reset-stream').addEventListener('click', () => {
  const defaults = {
    main: { encoding: 'H.265', resolution: '1920x1080', fps: '25', bitrateType: 'CBR', bitrate: 4096, iframe: 50, quality: 'medium', smart: true },
    sub: { encoding: 'H.264', resolution: '1280x720', fps: '15', bitrateType: 'CBR', bitrate: 1024, iframe: 50, quality: 'medium', smart: false },
    third: { encoding: 'H.264', resolution: '704x576', fps: '10', bitrateType: 'VBR', bitrate: 512, iframe: 50, quality: 'low', smart: false }
  };
  streamConfigs[currentStream] = { ...defaults[currentStream] };
  loadStreamConfig(currentStream);
  updateLiveParams();
  showToast('已恢复默认配置');
});

document.getElementById('btn-snapshot').addEventListener('click', () => showToast('抓图成功，已保存至本地'));
document.getElementById('btn-fullscreen').addEventListener('click', () => {
  const el = document.getElementById('video-preview');
  if (el.requestFullscreen) el.requestFullscreen();
});

function updateClock() {
  const now = new Date();
  document.getElementById('overlay-time').textContent = now.toLocaleString('zh-CN', { hour12: false });
}
setInterval(updateClock, 1000);
updateClock();
setInterval(updateLiveParams, 3000);

// ========== 阶段 2: MJPEG panorama 实时预览接管 ==========
// 浏览器原生 <img src="/api/stream"> 加载 multipart/x-mixed-replace.
// 流建立 -> onload -> 加 .stream-active -> 隐藏 placeholder + 显示图.
// 流断开 -> onerror -> 移除 .stream-active + 加 .preview-offline -> 回退到 placeholder.
function onStreamLoad(img) {
  if (!img) return;
  const wrap = img.parentElement;
  if (!wrap) return;
  wrap.classList.add('stream-active');
  wrap.classList.remove('preview-offline');
}

function onStreamError(img) {
  if (!img) return;
  const wrap = img.parentElement;
  if (!wrap) return;
  wrap.classList.remove('stream-active');
  wrap.classList.add('preview-offline');
}

// 启动时主动连 (部分浏览器延迟到 layout 后才请求 src, 显式 kick 一下)
const mjpegImg = document.getElementById('mjpeg-stream');
if (mjpegImg && !mjpegImg.src) {
  mjpegImg.src = '/api/stream';
}

// 切码流 / 切 RTSP 设备时, 重新触发 onload/onerror (避免 img cache 旧流).
function reloadMjpegStream() {
  const img = document.getElementById('mjpeg-stream');
  if (!img) return;
  const current = img.src;
  img.src = '';
  setTimeout(() => { img.src = current || '/api/stream'; }, 80);
}

// ========== 算法数据（最多 6 个） ==========

const MAX_ALGORITHMS = 6;

let algorithms = [
  { id: 'detect', name: '目标检测', desc: '基于深度学习的目标检测算法，支持人、车、物等多类别识别。', version: 'v2.3.1', enabled: true, running: true,
    recognizeResult: 'person×2, car×1 (置信度 92%)', locateResult: '—',
    params: { confidence: 0.6, maxTargets: 32, roi: '全画面' },
    preview: { results: [{ label: '检测目标', value: '3 个 (人×2, 车×1)' }, { label: '平均置信度', value: '92.3%' }, { label: '处理耗时', value: '18 ms' }] } },
  { id: 'locate', name: '目标定位', desc: '对检测目标精确定位，输出坐标框及中心点，支持多目标跟踪。', version: 'v1.8.0', enabled: true, running: true,
    recognizeResult: '跟踪目标 2 个', locateResult: 'ID#1024 (640,360), ID#1025 (820,410)',
    params: { trackMode: '多目标', smoothFactor: 0.8, minSize: 32 },
    preview: { results: [{ label: '跟踪 ID', value: '#1024, #1025' }, { label: '中心坐标', value: '(640, 360)' }, { label: '移动速度', value: '1.2 m/s' }] } },
  { id: 'face', name: '人脸识别', desc: '人脸检测与识别，支持人脸库比对、陌生人告警。', version: 'v3.1.0', enabled: true, running: false,
    recognizeResult: '张三 (员工, 相似度 89%)', locateResult: '人脸框 (512, 180, 128×160)',
    params: { threshold: 0.75, library: '默认人脸库', liveness: true },
    preview: { results: [{ label: '识别结果', value: '张三 (员工)' }, { label: '相似度', value: '89.2%' }, { label: '活体检测', value: '通过' }] } },
  { id: 'plate', name: '车牌识别', desc: '支持蓝牌、黄牌、新能源车牌识别。', version: 'v2.0.5', enabled: false, running: false,
    recognizeResult: '—', locateResult: '—',
    params: { province: '自动', nightMode: true },
    preview: { results: [{ label: '车牌号码', value: '京A·D1234' }, { label: '车牌颜色', value: '蓝色' }, { label: '识别置信度', value: '96.1%' }] } },
  { id: 'behavior', name: '行为分析', desc: '检测区域入侵、越界、徘徊等异常行为。', version: 'v1.5.2', enabled: false, running: false,
    recognizeResult: '—', locateResult: '—',
    params: { sensitivity: '中', duration: 3, linkAlarm: true },
    preview: { results: [{ label: '告警类型', value: '区域入侵' }, { label: '触发区域', value: '禁区 A' }, { label: '持续时间', value: '3.2 s' }] } },
  { id: 'helmet', name: '安全帽识别', desc: '检测作业人员是否佩戴安全帽，支持未佩戴实时告警。', version: 'v1.2.0', enabled: false, running: false,
    recognizeResult: '—', locateResult: '—',
    params: { sensitivity: '中', alarmMode: true, minHeadSize: 24 },
    preview: { results: [{ label: '佩戴人数', value: '1 人' }, { label: '未佩戴人数', value: '1 人' }, { label: '告警状态', value: '已触发' }] } }
];

let currentAlgoId = null;

function getEnabledCount() {
  return algorithms.filter(a => a.enabled).length;
}

function canEnableMore() {
  return getEnabledCount() < MAX_ALGORITHMS;
}

// ========== 首页文字结果输出 ==========

function renderHomeAlgoResults() {
  const tbody = document.getElementById('home-algo-result-body');
  const empty = document.getElementById('home-algo-empty');
  const table = document.querySelector('.algo-result-table');
  const enabled = algorithms.filter(a => a.enabled);

  document.getElementById('algo-output-count').textContent = `已加载 ${enabled.length}/${MAX_ALGORITHMS}`;

  if (enabled.length === 0) {
    tbody.innerHTML = '';
    table.classList.add('hidden');
    empty.classList.add('show');
    return;
  }

  table.classList.remove('hidden');
  empty.classList.remove('show');

  tbody.innerHTML = enabled.map(algo => `
    <tr>
      <td class="col-name">
        <span class="algo-status"></span>${algo.name}
      </td>
      <td class="col-result ${algo.recognizeResult === '—' ? 'muted' : ''}">${algo.recognizeResult}</td>
      <td class="col-result ${algo.locateResult === '—' ? 'muted' : ''}">${algo.locateResult}</td>
    </tr>
  `).join('');
}

function simulateAlgoResults() {
  algorithms.filter(a => a.enabled).forEach(algo => {
    if (algo.id === 'detect') {
      const p = Math.floor(Math.random() * 3) + 1;
      const c = Math.floor(Math.random() * 2);
      algo.recognizeResult = `person×${p}, car×${c} (置信度 ${(85 + Math.random() * 10).toFixed(0)}%)`;
    } else if (algo.id === 'locate') {
      const x1 = Math.floor(400 + Math.random() * 200);
      const y1 = Math.floor(300 + Math.random() * 100);
      algo.locateResult = `ID#1024 (${x1},${y1}), ID#1025 (${x1 + 180},${y1 + 50})`;
    } else if (algo.id === 'face') {
      algo.recognizeResult = Math.random() > 0.3 ? '张三 (员工, 相似度 89%)' : '陌生人 (未匹配)';
    } else if (algo.id === 'plate') {
      algo.recognizeResult = `京A·${Math.floor(Math.random() * 90000 + 10000)} (置信度 96%)`;
      algo.locateResult = `车牌区域 (580, 420, 120×36)`;
    } else if (algo.id === 'behavior') {
      algo.recognizeResult = Math.random() > 0.7 ? '区域入侵告警' : '正常';
      algo.locateResult = algo.recognizeResult.includes('告警') ? '禁区 A (320, 200, 400×300)' : '—';
    } else if (algo.id === 'helmet') {
      algo.recognizeResult = Math.random() > 0.4 ? '已佩戴×1, 未佩戴×1' : '已佩戴×2';
      algo.locateResult = algo.recognizeResult.includes('未佩戴')
        ? '未佩戴 (480, 290, 48×52)'
        : '—';
    } else if (algo.id.startsWith('custom_')) {
      algo.recognizeResult = `目标检测 (置信度 ${(80 + Math.random() * 15).toFixed(0)}%)`;
      algo.locateResult = Math.random() > 0.5 ? `区域 (${Math.floor(Math.random()*800)}, ${Math.floor(Math.random()*600)})` : '—';
    }
  });
  renderHomeAlgoResults();
}

document.getElementById('btn-goto-algo').addEventListener('click', () => {
  switchPage('algorithm');
});

setInterval(simulateAlgoResults, 4000);

// ========== 2. 网络配置 ==========

const netFields = ['net-ip', 'net-mask', 'net-gateway', 'net-dns1', 'net-dns2', 'net-mtu', 'net-http-port', 'net-rtsp-port'];

document.getElementById('net-dhcp').addEventListener('change', e => {
  const disabled = e.target.checked;
  netFields.forEach(id => {
    const el = document.getElementById(id);
    if (el && id !== 'net-mtu') el.disabled = disabled;
  });
});

document.getElementById('btn-save-network').addEventListener('click', () => {
  const name = document.getElementById('net-device-name').value.trim();
  const ip = document.getElementById('net-ip').value.trim();
  const rtspPort = document.getElementById('net-rtsp-port').value;
  if (!name) {
    showToast('请填写设备名称', 'error');
    return;
  }
  if (!isValidDeviceIp(ip)) {
    showToast('IP 须在 192.168.1.1 ~ 192.168.1.100 范围内', 'error');
    return;
  }
  if (devices.some(d => !d.isLocal && d.ip === ip)) {
    showToast('该 IP 已被其他设备使用', 'error');
    return;
  }
  const local = getLocalDevice();
  if (local) {
    local.name = name;
    local.ip = ip;
    local.rtsp = buildRtspUrl(ip, rtspPort);
  }
  syncLocalDeviceUI();
  if (getPreviewDevice()?.isLocal) applyPreviewDeviceUI({ animated: false });
  renderDevices(document.getElementById('dev-search')?.value || '');
  updateNetworkOutput();
  showToast('网络配置已保存');
});

document.getElementById('btn-ping').addEventListener('click', () => {
  const target = document.getElementById('ping-target').value;
  const result = document.getElementById('ping-result');
  result.textContent = `正在 Ping ${target} ...`;
  result.style.color = 'var(--text-secondary)';

  setTimeout(() => {
    result.innerHTML = `正在 Ping ${target} 具有 32 字节的数据:<br>` +
      `来自 ${target} 的回复: 字节=32 时间=1ms TTL=64<br>` +
      `来自 ${target} 的回复: 字节=32 时间=1ms TTL=64<br>` +
      `来自 ${target} 的回复: 字节=32 时间=2ms TTL=64<br>` +
      `来自 ${target} 的回复: 字节=32 时间=1ms TTL=64<br><br>` +
      `${target} 的 Ping 统计信息:<br>` +
      `    数据包: 已发送 = 4，已接收 = 4，丢失 = 0 (0% 丢失)`;
    result.style.color = 'var(--success)';
  }, 800);
});

// ========== 网络输出监测 ==========

const bandwidthHistory = { up: [], down: [] };
const MAX_CHART_POINTS = 30;

function setGauge(id, percent) {
  const el = document.getElementById(id);
  if (!el) return;
  const offset = 264 - (264 * Math.min(percent, 100) / 100);
  el.style.strokeDashoffset = offset;
}

function updateNetworkOutput() {
  const upload = (3 + Math.random() * 2).toFixed(1);
  const download = (10 + Math.random() * 6).toFixed(1);
  const streamBw = (3.8 + Math.random() * 0.8).toFixed(1);
  const latency = Math.floor(1 + Math.random() * 4);
  const util = Math.floor(30 + Math.random() * 20);

  document.getElementById('net-upload').textContent = upload;
  document.getElementById('net-download').textContent = download;
  document.getElementById('net-stream-bw').textContent = `${streamBw} Mbps`;
  document.getElementById('net-latency').textContent = `${latency} ms`;
  document.getElementById('net-util').textContent = `${util}%`;

  setGauge('gauge-upload', parseFloat(upload) / 10 * 100);
  setGauge('gauge-download', parseFloat(download) / 20 * 100);

  bandwidthHistory.up.push(parseFloat(upload));
  bandwidthHistory.down.push(parseFloat(download));
  if (bandwidthHistory.up.length > MAX_CHART_POINTS) bandwidthHistory.up.shift();
  if (bandwidthHistory.down.length > MAX_CHART_POINTS) bandwidthHistory.down.shift();

  drawBandwidthChart();
}

function drawBandwidthChart() {
  const canvas = document.getElementById('bandwidth-chart');
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const dpr = window.devicePixelRatio || 1;
  const rect = canvas.getBoundingClientRect();
  canvas.width = rect.width * dpr;
  canvas.height = 120 * dpr;
  ctx.scale(dpr, dpr);
  const w = rect.width;
  const h = 120;

  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = '#fafbfc';
  ctx.fillRect(0, 0, w, h);

  const maxVal = 20;
  const pad = { t: 10, b: 20, l: 4, r: 4 };
  const chartW = w - pad.l - pad.r;
  const chartH = h - pad.t - pad.b;

  // 网格线
  ctx.strokeStyle = '#f0f0f0';
  ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = pad.t + (chartH / 4) * i;
    ctx.beginPath();
    ctx.moveTo(pad.l, y);
    ctx.lineTo(w - pad.r, y);
    ctx.stroke();
  }

  function drawLine(data, color) {
    if (data.length < 2) return;
    ctx.beginPath();
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    data.forEach((v, i) => {
      const x = pad.l + (chartW / (MAX_CHART_POINTS - 1)) * i;
      const y = pad.t + chartH - (v / maxVal) * chartH;
      i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    });
    ctx.stroke();
  }

  drawLine(bandwidthHistory.up, '#1890ff');
  drawLine(bandwidthHistory.down, '#52c41a');

  // 图例
  ctx.font = '11px Microsoft YaHei';
  ctx.fillStyle = '#1890ff';
  ctx.fillText('● 上行', pad.l + 4, h - 4);
  ctx.fillStyle = '#52c41a';
  ctx.fillText('● 下行', pad.l + 60, h - 4);
}

// 初始化网络数据
for (let i = 0; i < 15; i++) {
  bandwidthHistory.up.push(3 + Math.random() * 2);
  bandwidthHistory.down.push(10 + Math.random() * 5);
}
updateNetworkOutput();
setInterval(updateNetworkOutput, 2000);

window.addEventListener('resize', drawBandwidthChart);

// ========== 3. 算法管理 ==========

function renderAlgorithms() {
  const grid = document.getElementById('algo-grid');
  const empty = document.getElementById('algo-list-empty');
  const addBtn = document.getElementById('btn-add-algo');

  document.getElementById('algo-list-count').textContent = `已添加 ${algorithms.length}/${MAX_ALGORITHMS} 个`;
  addBtn.disabled = algorithms.length >= MAX_ALGORITHMS;
  addBtn.title = algorithms.length >= MAX_ALGORITHMS ? '已达上限，请先删除已有算法' : '';

  if (algorithms.length === 0) {
    grid.innerHTML = '';
    grid.classList.add('hidden');
    empty.classList.add('show');
    updateAlgoStats();
    renderHomeAlgoResults();
    return;
  }

  grid.classList.remove('hidden');
  empty.classList.remove('show');

  grid.innerHTML = algorithms.map(algo => `
    <div class="algo-card ${algo.enabled ? 'enabled' : ''}" data-id="${algo.id}" data-algo-preview="${algo.id}">
      <div class="algo-card-body">
        <div class="algo-card-header">
          <div class="algo-card-title">${algo.name}</div>
          <label class="switch" onclick="event.stopPropagation()">
            <input type="checkbox" ${algo.enabled ? 'checked' : ''} data-algo-toggle="${algo.id}">
            <span class="switch-slider"></span>
          </label>
        </div>
        <div class="algo-card-desc">${algo.desc}</div>
        <div class="algo-card-meta">
          <span>版本 ${algo.version}</span>
          <span>${algo.enabled ? (algo.running ? '<span style="color:var(--success)">● 运行中</span>' : '<span style="color:var(--warning)">● 已启用</span>') : '<span style="color:var(--text-muted)">○ 未启用</span>'}</span>
        </div>
        <div class="algo-card-actions" onclick="event.stopPropagation()">
          <button class="btn btn-default btn-sm" data-algo-config="${algo.id}" ${!algo.enabled ? 'disabled' : ''}>参数配置</button>
          <button class="btn btn-primary btn-sm" data-algo-effect="${algo.id}">查看图像效果</button>
          <button class="btn btn-danger btn-sm btn-delete" data-algo-delete="${algo.id}">删除</button>
        </div>
        <div class="algo-card-hint">点击卡片查看图像识别效果</div>
      </div>
    </div>
  `).join('');

  grid.querySelectorAll('[data-algo-toggle]').forEach(input => {
    input.addEventListener('change', e => {
      e.stopPropagation();
      const algo = algorithms.find(a => a.id === e.target.dataset.algoToggle);
      if (e.target.checked && !canEnableMore()) {
        e.target.checked = false;
        showToast(`最多同时启用 ${MAX_ALGORITHMS} 个算法`, 'error');
        return;
      }
      algo.enabled = e.target.checked;
      algo.running = e.target.checked;
      if (algo.enabled && algo.recognizeResult === '—') {
        activateAlgoResults(algo);
      } else if (!algo.enabled) {
        algo.recognizeResult = '—';
        algo.locateResult = '—';
      }
      renderAlgorithms();
      updateAlgoStats();
      renderHomeAlgoResults();
      showToast(algo.enabled ? `已启用「${algo.name}」` : `已停用「${algo.name}」`);
    });
  });

  grid.querySelectorAll('[data-algo-preview]').forEach(card => {
    card.addEventListener('click', () => openAlgoPreview(card.dataset.algoPreview));
  });

  grid.querySelectorAll('[data-algo-config]').forEach(btn => {
    btn.addEventListener('click', e => {
      e.stopPropagation();
      openAlgoConfig(btn.dataset.algoConfig);
    });
  });

  grid.querySelectorAll('[data-algo-effect]').forEach(btn => {
    btn.addEventListener('click', e => {
      e.stopPropagation();
      openAlgoPreview(btn.dataset.algoEffect);
    });
  });

  grid.querySelectorAll('[data-algo-delete]').forEach(btn => {
    btn.addEventListener('click', e => {
      e.stopPropagation();
      deleteAlgorithm(btn.dataset.algoDelete);
    });
  });
}

function deleteAlgorithm(id) {
  const algo = algorithms.find(a => a.id === id);
  if (!algo) return;
  if (!confirm(`确定删除算法「${algo.name}」？\n删除后首页将不再输出该算法结果。`)) return;

  algorithms = algorithms.filter(a => a.id !== id);
  if (currentAlgoId === id) currentAlgoId = null;
  renderAlgorithms();
  updateAlgoStats();
  renderHomeAlgoResults();
  showToast(`已删除「${algo.name}」`);
}

function activateAlgoResults(algo) {
  const defaults = {
    detect: { recognizeResult: 'person×2, car×1 (置信度 92%)', locateResult: '—' },
    locate: { recognizeResult: '跟踪目标 2 个', locateResult: 'ID#1024 (640,360), ID#1025 (820,410)' },
    face: { recognizeResult: '张三 (员工, 相似度 89%)', locateResult: '人脸框 (512, 180, 128×160)' },
    plate: { recognizeResult: '京A·D1234 (置信度 96%)', locateResult: '车牌区域 (580, 420, 120×36)' },
    behavior: { recognizeResult: '正常', locateResult: '—' },
    helmet: { recognizeResult: '已佩戴×1, 未佩戴×1', locateResult: '未佩戴 (480, 290, 48×52)' }
  };
  const d = defaults[algo.id] || {
    recognizeResult: '检测中...',
    locateResult: '—'
  };
  algo.recognizeResult = d.recognizeResult;
  algo.locateResult = d.locateResult;
}

function updateAlgoStats() {
  const enabled = getEnabledCount();
  document.getElementById('algo-enabled-count').textContent = `${enabled}/${MAX_ALGORITHMS}`;
  document.getElementById('algo-running-count').textContent = algorithms.filter(a => a.running).length;
}

function openAlgoConfig(id) {
  currentAlgoId = id;
  const algo = algorithms.find(a => a.id === id);
  document.getElementById('modal-algo-title').textContent = `${algo.name} - 参数配置`;

  const labels = {
    confidence: '置信度阈值', maxTargets: '最大目标数', roi: '检测区域',
    trackMode: '跟踪模式', smoothFactor: '平滑系数', minSize: '最小目标尺寸(px)',
    threshold: '识别阈值', library: '人脸库', liveness: '活体检测',
    province: '省份识别', nightMode: '夜间模式',
    sensitivity: '灵敏度', duration: '持续时间(s)', linkAlarm: '联动告警',
    alarmMode: '未佩戴告警', minHeadSize: '最小头部尺寸(px)'
  };

  const paramHtml = Object.entries(algo.params).map(([key, val]) => {
    const label = labels[key] || key;
    if (typeof val === 'boolean') {
      return `<div class="form-item"><label class="form-label">${label}</label><label class="switch"><input type="checkbox" data-param="${key}" ${val ? 'checked' : ''}><span class="switch-slider"></span></label></div>`;
    }
    if (typeof val === 'number') {
      return `<div class="form-item"><label class="form-label">${label}</label><input type="number" class="form-control" data-param="${key}" value="${val}" step="0.1"></div>`;
    }
    return `<div class="form-item"><label class="form-label">${label}</label><input type="text" class="form-control" data-param="${key}" value="${val}"></div>`;
  }).join('');

  document.getElementById('modal-algo-body').innerHTML = `<div class="form-grid cols-1">${paramHtml}</div>`;
  openModal('modal-algo-config');
}

document.getElementById('btn-save-algo-config').addEventListener('click', () => {
  const algo = algorithms.find(a => a.id === currentAlgoId);
  document.querySelectorAll('#modal-algo-body [data-param]').forEach(el => {
    const key = el.dataset.param;
    if (el.type === 'checkbox') algo.params[key] = el.checked;
    else if (el.type === 'number') algo.params[key] = parseFloat(el.value);
    else algo.params[key] = el.value;
  });
  closeModal('modal-algo-config');
  showToast(`「${algo.name}」参数已保存`);
});

document.getElementById('btn-add-algo').addEventListener('click', () => {
  if (algorithms.length >= MAX_ALGORITHMS) {
    showToast(`最多添加 ${MAX_ALGORITHMS} 个算法，请先删除已有算法`, 'error');
    return;
  }
  document.getElementById('add-algo-name').value = '';
  document.getElementById('add-algo-desc').value = '';
  document.getElementById('add-algo-version').value = 'v1.0.0';
  document.getElementById('add-algo-file').value = '';
  openModal('modal-add-algo');
});

document.getElementById('btn-confirm-add-algo').addEventListener('click', () => {
  const name = document.getElementById('add-algo-name').value.trim();
  if (!name) {
    showToast('请填写算法名称', 'error');
    return;
  }
  if (algorithms.length >= MAX_ALGORITHMS) {
    showToast(`最多添加 ${MAX_ALGORITHMS} 个算法`, 'error');
    return;
  }
  if (algorithms.some(a => a.name === name)) {
    showToast('算法名称已存在', 'error');
    return;
  }

  const desc = document.getElementById('add-algo-desc').value.trim() || '自定义上传算法';
  const version = document.getElementById('add-algo-version').value.trim() || 'v1.0.0';

  algorithms.push({
    id: `custom_${Date.now()}`,
    name,
    desc,
    version,
    enabled: false,
    running: false,
    recognizeResult: '—',
    locateResult: '—',
    params: { confidence: 0.6, roi: '全画面' },
    preview: {
      results: [
        { label: '算法类型', value: '自定义' },
        { label: '状态', value: '待启用' }
      ]
    }
  });

  closeModal('modal-add-algo');
  renderAlgorithms();
  showToast(`算法「${name}」添加成功`);
});

// ========== 算法效果预览 ==========

const PREVIEW_SCALE = 1; // 与首页图像输出同尺寸，超出视口时自动缩小

/** 计算首页实时预览区域的尺寸（与图像输出页一致） */
function getHomePreviewSize() {
  const homePanel = document.getElementById('page-home');
  const homeVideo = document.getElementById('video-preview');
  if (!homePanel || !homeVideo) {
    return fallbackHomePreviewSize();
  }

  const wasActive = homePanel.classList.contains('active');
  let w = 0;
  let h = 0;

  if (wasActive) {
    const rect = homeVideo.getBoundingClientRect();
    w = rect.width;
    h = rect.height;
  } else {
    // 首页隐藏时临时测量布局尺寸
    const prevDisplay = homePanel.style.display;
    homePanel.style.display = 'block';
    homePanel.style.visibility = 'hidden';
    homePanel.style.position = 'absolute';
    homePanel.style.pointerEvents = 'none';
    const rect = homeVideo.getBoundingClientRect();
    w = rect.width;
    h = rect.height;
    homePanel.style.display = prevDisplay;
    homePanel.style.visibility = '';
    homePanel.style.position = '';
    homePanel.style.pointerEvents = '';
  }

  if (w > 100 && h > 50) return { w, h };
  return fallbackHomePreviewSize();
}

function fallbackHomePreviewSize() {
  const content = document.querySelector('.content-wrapper');
  const contentW = content ? content.clientWidth : window.innerWidth - 220 - 56;
  const homeMainW = contentW - 340 - 20 - 28;
  const w = Math.max(560, homeMainW);
  return { w, h: Math.round(w * 9 / 16) };
}

function sizeModalPreview() {
  const { w, h } = getHomePreviewSize();
  const infoW = 320;
  let pw = Math.round(w * PREVIEW_SCALE);
  let ph = Math.round(h * PREVIEW_SCALE);

  // 尽量占满视口，预留标题栏和底部按钮
  const maxBodyH = window.innerHeight * 0.82;
  if (ph > maxBodyH) {
    ph = Math.round(maxBodyH);
    pw = Math.round(ph * 16 / 9);
  }
  const maxTotalW = window.innerWidth * 0.98;
  let totalW = pw + infoW + 16 + 32;
  if (totalW > maxTotalW) {
    pw = Math.max(520, maxTotalW - infoW - 16 - 32);
    ph = Math.round(pw * 9 / 16);
    if (ph > maxBodyH) {
      ph = Math.round(maxBodyH);
      pw = Math.round(ph * 16 / 9);
    }
    totalW = pw + infoW + 16 + 32;
  }

  const root = document.documentElement;
  root.style.setProperty('--modal-preview-w', `${pw}px`);
  root.style.setProperty('--modal-preview-h', `${ph}px`);
  root.style.setProperty('--modal-info-w', `${infoW}px`);

  const modal = document.querySelector('#modal-algo-preview .modal-algo-preview-dialog');
  if (modal) {
    modal.style.width = `${totalW}px`;
  }
}

// 与首页图像输出一致：16:9 (1920×1080)
const PREVIEW_ASPECT = 16 / 9;
const PREVIEW_WIDTH = 1920;
const PREVIEW_HEIGHT = 1080;
/** 六路算法预览统一背景色（与首页预览区、弹窗容器完全一致） */
const PREVIEW_BG = '#1a2332';

let previewBaseCache = { w: 0, h: 0, canvas: null };

/** 绘制/复用纯色底图，保证六路算法背景像素级一致 */
function blitUnifiedBackground(ctx, w, h) {
  if (!previewBaseCache.canvas || previewBaseCache.w !== w || previewBaseCache.h !== h) {
    const base = document.createElement('canvas');
    base.width = w;
    base.height = h;
    const bctx = base.getContext('2d');
    bctx.fillStyle = PREVIEW_BG;
    bctx.fillRect(0, 0, w, h);
    previewBaseCache = { w, h, canvas: base };
  }
  ctx.drawImage(previewBaseCache.canvas, 0, 0);
}

function roundRect(ctx, x, y, width, height, radius) {
  const r = Math.min(radius, width / 2, height / 2);
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.lineTo(x + width - r, y);
  ctx.quadraticCurveTo(x + width, y, x + width, y + r);
  ctx.lineTo(x + width, y + height - r);
  ctx.quadraticCurveTo(x + width, y + height, x + width - r, y + height);
  ctx.lineTo(x + r, y + height);
  ctx.quadraticCurveTo(x, y + height, x, y + height - r);
  ctx.lineTo(x, y + r);
  ctx.quadraticCurveTo(x, y, x + r, y);
  ctx.closePath();
}

function drawCornerBrackets(ctx, x, y, bw, bh, color, len, lineWidth) {
  ctx.strokeStyle = color;
  ctx.lineWidth = lineWidth;
  ctx.beginPath();
  // 左上
  ctx.moveTo(x, y + len); ctx.lineTo(x, y); ctx.lineTo(x + len, y);
  // 右上
  ctx.moveTo(x + bw - len, y); ctx.lineTo(x + bw, y); ctx.lineTo(x + bw, y + len);
  // 右下
  ctx.moveTo(x + bw, y + bh - len); ctx.lineTo(x + bw, y + bh); ctx.lineTo(x + bw - len, y + bh);
  // 左下
  ctx.moveTo(x + len, y + bh); ctx.lineTo(x, y + bh); ctx.lineTo(x, y + bh - len);
  ctx.stroke();
}

/** 检测框：仅描边 + 标签，不填充框内区域，避免破坏统一背景 */
function drawDetectionBox(ctx, x, y, bw, bh, color, label, isMini) {
  const lw = isMini ? 2 : 3;
  const corner = isMini ? 10 : 18;
  const fontSize = isMini ? 11 : 16;
  const labelH = isMini ? 16 : 24;
  const pad = isMini ? 5 : 8;

  ctx.strokeStyle = color;
  ctx.lineWidth = lw;
  ctx.strokeRect(x, y, bw, bh);
  drawCornerBrackets(ctx, x, y, bw, bh, color, corner, lw + 1);

  if (label) {
    ctx.font = `600 ${fontSize}px Consolas, "Microsoft YaHei", sans-serif`;
    const tw = ctx.measureText(label).width;
    const lx = x;
    const ly = Math.max(4, y - labelH - 3);
    ctx.fillStyle = color;
    roundRect(ctx, lx, ly, tw + pad * 2, labelH, 3);
    ctx.fill();
    ctx.fillStyle = '#ffffff';
    ctx.textBaseline = 'middle';
    ctx.fillText(label, lx + pad, ly + labelH / 2);
    ctx.textBaseline = 'alphabetic';
  }
}

/** 虚线区域：仅描边，不铺色 */
function drawZoneOutline(ctx, x, y, zw, zh, color, isMini) {
  ctx.strokeStyle = color;
  ctx.lineWidth = isMini ? 2 : 3;
  ctx.setLineDash([10, 6]);
  ctx.strokeRect(x, y, zw, zh);
  ctx.setLineDash([]);
}

function drawTrackCross(ctx, x, y, color, isMini) {
  const s = isMini ? 5 : 10;
  ctx.strokeStyle = color;
  ctx.lineWidth = isMini ? 1.5 : 2.5;
  ctx.beginPath();
  ctx.moveTo(x - s, y); ctx.lineTo(x + s, y);
  ctx.moveTo(x, y - s); ctx.lineTo(x, y + s);
  ctx.stroke();
  ctx.fillStyle = color;
  ctx.beginPath();
  ctx.arc(x, y, isMini ? 2 : 4, 0, Math.PI * 2);
  ctx.fill();
}

/** 目标定位预览：右上角二维坐标小地图（闪烁） */
function drawLocateCoordMiniMap(ctx, w, h, targets, tick = 0) {
  const boxW = Math.round(w * 0.20);
  const boxH = Math.round(h * 0.22);
  const bx = w - boxW - 16;
  const by = 16;
  const pad = 12;
  const plotX = bx + pad;
  const plotY = by + 28;
  const plotW = boxW - pad * 2;
  const plotH = boxH - 38;

  ctx.save();
  ctx.fillStyle = 'rgba(0, 0, 0, 0.62)';
  ctx.strokeStyle = 'rgba(54, 207, 201, 0.85)';
  ctx.lineWidth = 1.5;
  roundRect(ctx, bx, by, boxW, boxH, 4);
  ctx.fill();
  ctx.stroke();

  ctx.fillStyle = 'rgba(255, 255, 255, 0.75)';
  ctx.font = '600 12px "Microsoft YaHei", sans-serif';
  ctx.fillText('二维坐标', bx + pad, by + 18);

  // 坐标系网格
  ctx.strokeStyle = 'rgba(255, 255, 255, 0.08)';
  ctx.lineWidth = 1;
  for (let i = 1; i <= 3; i++) {
    const gx = plotX + (plotW * i) / 4;
    const gy = plotY + (plotH * i) / 4;
    ctx.beginPath();
    ctx.moveTo(gx, plotY); ctx.lineTo(gx, plotY + plotH);
    ctx.moveTo(plotX, gy); ctx.lineTo(plotX + plotW, gy);
    ctx.stroke();
  }

  targets.forEach(t => {
    const px = plotX + t.nx * plotW;
    const py = plotY + t.ny * plotH;
    const blink = 0.45 + 0.55 * (0.5 + 0.5 * Math.sin(tick * 0.14 + t.phase));
    const r = 5;

    // 外圈脉冲
    ctx.globalAlpha = blink * 0.35;
    ctx.fillStyle = t.color;
    ctx.beginPath();
    ctx.arc(px, py, r + 6, 0, Math.PI * 2);
    ctx.fill();

    // 目标点
    ctx.globalAlpha = blink;
    ctx.beginPath();
    ctx.arc(px, py, r, 0, Math.PI * 2);
    ctx.fill();

    ctx.globalAlpha = 1;
    ctx.fillStyle = '#fff';
    ctx.font = '11px Consolas, monospace';
    ctx.fillText(t.id, px + 7, py - 6);
  });

  ctx.restore();
}

let locatePreviewAnimId = null;

function stopLocatePreviewAnim() {
  if (locatePreviewAnimId) {
    cancelAnimationFrame(locatePreviewAnimId);
    locatePreviewAnimId = null;
  }
}

function startLocatePreviewAnim(canvas) {
  stopLocatePreviewAnim();
  let tick = 0;
  const loop = () => {
    const modal = document.getElementById('modal-algo-preview');
    if (!modal?.classList.contains('show') || currentAlgoId !== 'locate') {
      stopLocatePreviewAnim();
      return;
    }
    drawAlgoPreviewScene(canvas, 'locate', false, tick);
    tick += 1;
    locatePreviewAnimId = requestAnimationFrame(loop);
  };
  locatePreviewAnimId = requestAnimationFrame(loop);
}

function drawAlgoPreviewScene(canvas, algoId, isMini = false, animTick = 0) {
  const ctx = canvas.getContext('2d');
  const w = canvas.width = isMini ? 400 : PREVIEW_WIDTH;
  const h = canvas.height = isMini ? Math.round(400 / PREVIEW_ASPECT) : PREVIEW_HEIGHT;

  blitUnifiedBackground(ctx, w, h);

  const C = {
    person: '#40a9ff',
    car: '#ffc53d',
    track: '#36cfc9',
    face: '#b37feb',
    plate: '#95de64',
    alert: '#ff7875',
    count: '#69c0ff',
    helmetOk: '#95de64',
    helmetBad: '#ff7875'
  };

  // 共用目标位置
  const P1 = { x: w * 0.13, y: h * 0.26, bw: w * 0.11, bh: h * 0.42 };
  const P2 = { x: w * 0.31, y: h * 0.30, bw: w * 0.10, bh: h * 0.38 };
  const CAR = { x: w * 0.57, y: h * 0.46, bw: w * 0.30, bh: h * 0.22 };
  const FACE = { x: w * 0.155, y: h * 0.28, bw: w * 0.06, bh: h * 0.12 };
  const PLATE = { x: w * 0.62, y: h * 0.58, bw: w * 0.18, bh: h * 0.06 };

  const scenes = {
    detect: () => {
      drawDetectionBox(ctx, P1.x, P1.y, P1.bw, P1.bh, C.person, isMini ? 'person' : 'person 0.94', isMini);
      drawDetectionBox(ctx, P2.x, P2.y, P2.bw, P2.bh, C.person, isMini ? 'person' : 'person 0.87', isMini);
      drawDetectionBox(ctx, CAR.x, CAR.y, CAR.bw, CAR.bh, C.car, isMini ? 'car' : 'car 0.91', isMini);
    },
    locate: () => {
      drawDetectionBox(ctx, P1.x, P1.y, P1.bw, P1.bh, C.track, isMini ? 'ID:1024' : 'ID:1024  track', isMini);
      drawTrackCross(ctx, P1.x + P1.bw / 2, P1.y + P1.bh * 0.45, C.track, isMini);
      drawDetectionBox(ctx, P2.x, P2.y, P2.bw, P2.bh, C.track, isMini ? 'ID:1025' : 'ID:1025  track', isMini);
      drawTrackCross(ctx, P2.x + P2.bw / 2, P2.y + P2.bh * 0.5, C.track, isMini);
      if (!isMini) {
        drawLocateCoordMiniMap(ctx, w, h, [
          {
            id: '#1024',
            nx: (P1.x + P1.bw / 2) / w,
            ny: (P1.y + P1.bh * 0.45) / h,
            color: C.track,
            phase: 0
          },
          {
            id: '#1025',
            nx: (P2.x + P2.bw / 2) / w,
            ny: (P2.y + P2.bh * 0.5) / h,
            color: C.track,
            phase: Math.PI * 0.85
          }
        ], animTick);
      }
    },
    face: () => {
      drawDetectionBox(ctx, P1.x, P1.y, P1.bw, P1.bh, C.person, isMini ? 'person' : 'person 0.96', isMini);
      drawDetectionBox(ctx, FACE.x, FACE.y, FACE.bw, FACE.bh, C.face, isMini ? 'face' : '张三  89.2%', isMini);
      if (!isMini) {
        // 人脸关键点
        ctx.fillStyle = C.face;
        [[0.35, 0.35], [0.65, 0.35], [0.5, 0.55], [0.38, 0.72], [0.62, 0.72]].forEach(([px, py]) => {
          ctx.beginPath();
          ctx.arc(FACE.x + FACE.bw * px, FACE.y + FACE.bh * py, 3, 0, Math.PI * 2);
          ctx.fill();
        });
      }
    },
    plate: () => {
      drawDetectionBox(ctx, CAR.x, CAR.y, CAR.bw, CAR.bh, C.car, isMini ? 'vehicle' : 'vehicle 0.93', isMini);
      drawDetectionBox(ctx, PLATE.x, PLATE.y, PLATE.bw, PLATE.bh, C.plate, isMini ? 'plate' : '京A·D1234  96.1%', isMini);
    },
    behavior: () => {
      const zx = w * 0.08, zy = h * 0.18, zw = w * 0.38, zh = h * 0.62;
      drawZoneOutline(ctx, zx, zy, zw, zh, C.alert, isMini);
      if (!isMini) {
        ctx.fillStyle = C.alert;
        ctx.font = '600 17px "Microsoft YaHei", sans-serif';
        ctx.fillText('禁区 A', zx + 8, zy - 10);
        ctx.font = '600 15px "Microsoft YaHei", sans-serif';
        ctx.fillText('区域入侵告警', zx + 8, zy + zh + 24);
      }
      drawDetectionBox(ctx, P1.x, P1.y, P1.bw, P1.bh, C.alert, isMini ? 'intrusion' : 'intrusion  0.88', isMini);
    },
    helmet: () => {
      const HEAD1 = { x: P1.x + P1.bw * 0.28, y: P1.y + P1.bh * 0.02, bw: P1.bw * 0.44, bh: P1.bh * 0.16 };
      const HEAD2 = { x: P2.x + P2.bw * 0.26, y: P2.y + P2.bh * 0.03, bw: P2.bw * 0.48, bh: P2.bh * 0.15 };
      drawDetectionBox(ctx, P1.x, P1.y, P1.bw, P1.bh, C.person, isMini ? 'person' : 'person 0.95', isMini);
      drawDetectionBox(ctx, HEAD1.x, HEAD1.y, HEAD1.bw, HEAD1.bh, C.helmetOk, isMini ? 'ok' : 'helmet  96.2%', isMini);
      drawDetectionBox(ctx, P2.x, P2.y, P2.bw, P2.bh, C.person, isMini ? 'person' : 'person 0.91', isMini);
      drawDetectionBox(ctx, HEAD2.x, HEAD2.y, HEAD2.bw, HEAD2.bh, C.helmetBad, isMini ? '!' : 'no helmet  88.5%', isMini);
      if (!isMini) {
        ctx.fillStyle = C.helmetBad;
        ctx.font = '600 15px "Microsoft YaHei", sans-serif';
        ctx.fillText('未佩戴安全帽告警', P2.x, P2.y + P2.bh + 22);
      }
    }
  };

  const sceneKey = scenes[algoId] ? algoId : 'detect';
  scenes[sceneKey]();

  // 右下角分辨率水印
  if (!isMini) {
    ctx.fillStyle = 'rgba(255, 255, 255, 0.35)';
    ctx.font = '13px Consolas, monospace';
    ctx.fillText('1920×1080', w - 90, h - 14);
  }
}

function openAlgoPreview(id) {
  currentAlgoId = id;
  const algo = algorithms.find(a => a.id === id);
  document.getElementById('modal-preview-title').textContent = `${algo.name} - 图像效果预览`;

  const info = document.getElementById('algo-preview-info');
  info.innerHTML = `
    <div class="preview-info-block">
      <div class="preview-info-title">文字结果</div>
      <div class="preview-result-item">
        <div class="preview-result-label">识别结果</div>
        <div class="preview-result-value highlight">${algo.recognizeResult}</div>
      </div>
      <div class="preview-result-item">
        <div class="preview-result-label">定位结果</div>
        <div class="preview-result-value">${algo.locateResult}</div>
      </div>
    </div>
    <div class="preview-info-block">
      <div class="preview-info-title">详细数据</div>
      ${algo.preview.results.map(r => `
        <div class="preview-result-item">
          <div class="preview-result-label">${r.label}</div>
          <div class="preview-result-value">${r.value}</div>
        </div>
      `).join('')}
      <div class="preview-result-item">
        <div class="preview-result-label">算法版本</div>
        <div class="preview-result-value">${algo.version}</div>
      </div>
      <div class="preview-result-item">
        <div class="preview-result-label">输出分辨率</div>
        <div class="preview-result-value">1920×1080 (16:9)</div>
      </div>
    </div>
  `;

  openModal('modal-algo-preview');
  sizeModalPreview();
  requestAnimationFrame(() => {
    sizeModalPreview();
    const canvas = document.getElementById('algo-preview-canvas');
    if (!canvas) return;
    stopLocatePreviewAnim();
    drawAlgoPreviewScene(canvas, id, false);
    if (id === 'locate') startLocatePreviewAnim(canvas);
  });
}

window.addEventListener('resize', () => {
  if (document.getElementById('modal-algo-preview').classList.contains('show')) {
    sizeModalPreview();
  }
});

document.getElementById('btn-preview-config').addEventListener('click', () => {
  closeModal('modal-algo-preview');
  if (currentAlgoId) openAlgoConfig(currentAlgoId);
});

// ========== 4. 设备管理 ==========

/** 设备 IP 合法范围：192.168.1.1 ~ 192.168.1.100 */
function isValidDeviceIp(ip) {
  const m = ip.match(/^192\.168\.1\.(\d+)$/);
  if (!m) return false;
  const n = parseInt(m[1], 10);
  return n >= 1 && n <= 100;
}

function buildRtspUrl(ip, port = 554, channel = '101') {
  return `rtsp://${ip}:${port}/Streaming/Channels/${channel}`;
}

function getLocalDevice() {
  return devices.find(d => d.isLocal);
}

const PREVIEW_DEVICE_STORAGE_KEY = 'camera-platform-preview-device';
let currentPreviewDeviceId = 0;

function getPreviewDevice() {
  return devices.find(d => d.id === currentPreviewDeviceId) || getLocalDevice();
}

/** 同步本机设备到网络配置表单（不覆盖当前预览源 UI） */
function syncLocalDeviceUI() {
  const dev = getLocalDevice();
  if (!dev) return;
  const nameInput = document.getElementById('net-device-name');
  const ipInput = document.getElementById('net-ip');
  if (nameInput) nameInput.value = dev.name;
  if (ipInput) ipInput.value = dev.ip;
}

function updatePreviewPlaceholder() {
  const dev = getPreviewDevice();
  const placeholder = document.querySelector('.preview-placeholder span');
  const icon = document.querySelector('.preview-icon');
  if (!placeholder || !dev) return;
  if (!dev.online) {
    placeholder.textContent = `设备离线 · ${dev.name} (${dev.ip})`;
    if (icon) icon.textContent = '✕';
    return;
  }
  if (icon) icon.textContent = '▶';
  const cfg = streamConfigs[currentStream];
  const res = cfg.resolution.replace('x', '×');
  placeholder.textContent = `${dev.name} · ${STREAM_LABELS[currentStream]} · ${res} @ ${cfg.fps}fps`;
}

/** 将当前预览设备信息同步到首页、顶栏与 RTSP 面板 */
function applyPreviewDeviceUI(options = {}) {
  const animated = options.animated !== false;
  const dev = getPreviewDevice();
  if (!dev) return;

  const update = () => {
    const port = document.getElementById('net-rtsp-port')?.value || '554';
    const previewName = document.getElementById('preview-device-name');
    const headerName = document.getElementById('header-device-name');
    const backBtn = document.getElementById('btn-back-local');
    const videoArea = document.getElementById('video-preview');
    const rtspTitle = document.getElementById('rtsp-panel-title');
    const rtspMainLabel = document.getElementById('rtsp-main-label');
    const rtspSubRow = document.getElementById('rtsp-sub-row');
    const rtspMain = document.getElementById('preview-rtsp-main');
    const rtspSub = document.getElementById('preview-rtsp-sub');

    if (previewName) {
      previewName.textContent = dev.name;
      previewName.classList.toggle('external', !dev.isLocal);
    }
    if (headerName) headerName.textContent = `${dev.name} · ${dev.ip}`;
    if (backBtn) backBtn.classList.toggle('hidden', !!dev.isLocal);
    if (videoArea) {
      videoArea.classList.toggle('preview-external', !dev.isLocal);
      videoArea.classList.toggle('preview-offline', !dev.online);
    }
    if (rtspTitle) rtspTitle.textContent = dev.isLocal ? '本机 RTSP 输出' : '当前预览 RTSP';
    if (rtspMainLabel) rtspMainLabel.textContent = dev.isLocal ? '主码流' : '视频流';
    if (rtspSubRow) rtspSubRow.classList.toggle('rtsp-sub-hidden', !dev.isLocal);
    if (rtspMain) rtspMain.textContent = dev.isLocal ? buildRtspUrl(dev.ip, port, '101') : dev.rtsp;
    if (rtspSub && dev.isLocal) rtspSub.textContent = buildRtspUrl(dev.ip, port, '102');

    updatePreviewPlaceholder();
    renderDevices(document.getElementById('dev-search')?.value || '');
  };

  if (!animated) {
    update();
    return;
  }

  const fadeTargets = [
    document.getElementById('video-preview'),
    document.getElementById('preview-device-name'),
    document.getElementById('header-device-name'),
    document.querySelector('.home-sidebar')
  ].filter(Boolean);

  fadeTargets.forEach(el => el.classList.add('ui-switching'));
  setTimeout(() => {
    update();
    fadeTargets.forEach(el => {
      el.classList.remove('ui-switching');
      el.classList.add('ui-switched');
    });
    setTimeout(() => fadeTargets.forEach(el => el.classList.remove('ui-switched')), 360);
  }, 200);
}

function switchPreviewDevice(id, options = {}) {
  const { showToastMsg = true, animated = true } = options;
  const dev = devices.find(d => d.id === id);
  if (!dev) return false;
  if (!dev.online) {
    showToast(`设备「${dev.name}」离线，无法预览`, 'error');
    return false;
  }
  currentPreviewDeviceId = id;
  try {
    sessionStorage.setItem(PREVIEW_DEVICE_STORAGE_KEY, String(id));
  } catch (_) {}
  applyPreviewDeviceUI({ animated });
  if (showToastMsg) showToast(`已切换至「${dev.name}」预览`);
  return true;
}

function restorePreviewDevice() {
  try {
    const saved = sessionStorage.getItem(PREVIEW_DEVICE_STORAGE_KEY);
    if (saved !== null) {
      const id = parseInt(saved, 10);
      const dev = devices.find(d => d.id === id);
      if (dev?.online) {
        currentPreviewDeviceId = id;
        return;
      }
    }
  } catch (_) {}
  currentPreviewDeviceId = getLocalDevice()?.id ?? 0;
}

let devices = [
  { id: 0, name: '入口枪机-本机', type: 'camera', typeLabel: '网络摄像机', ip: '192.168.1.10', protocol: 'RTSP', rtsp: 'rtsp://192.168.1.10:554/Streaming/Channels/101', online: true, isLocal: true },
  { id: 1, name: '大厅枪机-01', type: 'camera', typeLabel: '网络摄像机', ip: '192.168.1.11', protocol: 'RTSP', rtsp: 'rtsp://192.168.1.11:554/Streaming/Channels/101', online: true },
  { id: 2, name: '停车场球机-02', type: 'camera', typeLabel: '网络摄像机', ip: '192.168.1.12', protocol: 'ONVIF', rtsp: 'rtsp://192.168.1.12:554/onvif1', online: true },
  { id: 3, name: 'NVR-主控', type: 'nvr', typeLabel: 'NVR', ip: '192.168.1.20', protocol: 'RTSP', rtsp: 'rtsp://192.168.1.20:554/Streaming/Channels/101', online: true },
  { id: 4, name: '后门IPC-03', type: 'ipc', typeLabel: 'IPC', ip: '192.168.1.13', protocol: 'RTSP', rtsp: 'rtsp://192.168.1.13:554/Streaming/Channels/101', online: false }
];

function renderDevices(filter = '') {
  const typeFilter = document.getElementById('dev-filter-type').value;
  const tbody = document.getElementById('device-table-body');
  let list = devices;

  if (filter) {
    const kw = filter.toLowerCase();
    list = list.filter(d => d.name.toLowerCase().includes(kw) || d.ip.includes(kw));
  }
  if (typeFilter) list = list.filter(d => d.type === typeFilter);

  if (list.length === 0) {
    tbody.innerHTML = `<tr><td colspan="8"><div class="empty-state"><div class="empty-icon">📡</div><p>暂无设备，点击「添加设备」接入外部相机</p></div></td></tr>`;
    return;
  }

  tbody.innerHTML = list.map(d => `
    <tr class="${d.id === currentPreviewDeviceId ? 'row-preview-active' : ''}">
      <td><input type="checkbox" data-dev-id="${d.id}" ${d.isLocal ? 'disabled' : ''}></td>
      <td>${d.name}${d.isLocal ? ' <span class="tag tag-primary">本机</span>' : ''}${d.id === currentPreviewDeviceId ? ' <span class="tag tag-warning">预览中</span>' : ''}</td>
      <td>${d.typeLabel}</td>
      <td>${d.ip}</td>
      <td><span class="tag tag-default">${d.protocol}</span></td>
      <td>
        <div class="rtsp-url">
          <code title="${d.rtsp}">${d.rtsp}</code>
          <button class="copy-btn" onclick="copyText('${d.rtsp}')">复制</button>
        </div>
      </td>
      <td>${d.online ? '<span class="tag tag-success">在线</span>' : '<span class="tag tag-error">离线</span>'}</td>
      <td class="actions">
        <button class="link-btn" onclick="previewDevice(${d.id})">预览</button>
        <button class="link-btn" onclick="editDevice(${d.id})">编辑</button>
        ${d.isLocal ? '' : `<button class="link-btn danger" onclick="deleteDevice(${d.id})">删除</button>`}
      </td>
    </tr>
  `).join('');

  updateDeviceStats();
}

function updateDeviceStats() {
  document.getElementById('dev-total').textContent = devices.length;
  document.getElementById('dev-online').textContent = devices.filter(d => d.online).length;
  document.getElementById('dev-offline').textContent = devices.filter(d => !d.online).length;
  document.getElementById('dev-streams').textContent = devices.filter(d => d.online).length;
}

window.copyText = function (text) {
  navigator.clipboard.writeText(text).then(() => showToast('RTSP 地址已复制'));
};

window.previewDevice = function (id) {
  const dev = devices.find(d => d.id === id);
  if (!dev) return;
  if (!dev.online) {
    showToast(`设备「${dev.name}」离线，无法预览`, 'error');
    return;
  }

  const isHome = document.getElementById('page-home')?.classList.contains('active');
  const applyPreview = () => switchPreviewDevice(id, { showToastMsg: true, animated: true });

  if (!isHome) {
    switchPage('home');
    setTimeout(applyPreview, PAGE_TRANSITION_MS + 60);
  } else {
    applyPreview();
  }
};

window.editDevice = function (id) {
  const dev = devices.find(d => d.id === id);
  document.getElementById('add-dev-name').value = dev.name;
  document.getElementById('add-dev-type').value = dev.type === 'ipc' ? 'camera' : dev.type;
  document.getElementById('add-dev-ip').value = dev.ip;
  document.getElementById('add-dev-protocol').value = dev.protocol;
  document.getElementById('add-dev-rtsp').value = dev.rtsp;
  openModal('modal-add-device');
  document.getElementById('btn-confirm-add-device').dataset.editId = id;
};

window.deleteDevice = function (id) {
  const dev = devices.find(d => d.id === id);
  if (dev?.isLocal) {
    showToast('本机设备不可删除', 'error');
    return;
  }
  if (confirm(`确定删除设备「${dev.name}」？`)) {
    devices = devices.filter(d => d.id !== id);
    if (currentPreviewDeviceId === id) {
      currentPreviewDeviceId = getLocalDevice()?.id ?? 0;
      try { sessionStorage.setItem(PREVIEW_DEVICE_STORAGE_KEY, String(currentPreviewDeviceId)); } catch (_) {}
      applyPreviewDeviceUI({ animated: false });
    }
    renderDevices();
    showToast('设备已删除');
  }
};

document.getElementById('btn-add-device').addEventListener('click', () => {
  document.getElementById('add-dev-name').value = '';
  document.getElementById('add-dev-ip').value = '';
  document.getElementById('add-dev-rtsp').value = '';
  delete document.getElementById('btn-confirm-add-device').dataset.editId;
  openModal('modal-add-device');
});

document.getElementById('btn-discover-device').addEventListener('click', () => {
  showToast('正在扫描局域网设备...');
  setTimeout(() => showToast('发现 2 台新设备（模拟）'), 1500);
});

document.getElementById('btn-confirm-add-device').addEventListener('click', () => {
  const name = document.getElementById('add-dev-name').value.trim();
  const ip = document.getElementById('add-dev-ip').value.trim();
  if (!name || !ip) {
    showToast('请填写设备名称和 IP 地址', 'error');
    return;
  }
  if (!isValidDeviceIp(ip)) {
    showToast('IP 须在 192.168.1.1 ~ 192.168.1.100 范围内', 'error');
    return;
  }

  const type = document.getElementById('add-dev-type').value;
  const protocol = document.getElementById('add-dev-protocol').value;
  const port = document.getElementById('add-dev-port').value || '554';
  let rtsp = document.getElementById('add-dev-rtsp').value.trim();
  if (!rtsp) rtsp = buildRtspUrl(ip, port);

  const typeLabels = { camera: '网络摄像机', nvr: 'NVR', other: '其他视频源' };
  const editId = document.getElementById('btn-confirm-add-device').dataset.editId;

  if (editId) {
    const dev = devices.find(d => d.id === parseInt(editId));
    Object.assign(dev, { name, type, typeLabel: typeLabels[type], ip, protocol, rtsp });
    if (dev.isLocal) syncLocalDeviceUI();
    if (dev.id === currentPreviewDeviceId) applyPreviewDeviceUI({ animated: true });
    showToast(dev.isLocal ? '本机设备信息已更新' : '设备信息已更新');
  } else {
    if (devices.some(d => d.ip === ip)) {
      showToast('该 IP 已被其他设备使用', 'error');
      return;
    }
    devices.push({ id: Date.now(), name, type, typeLabel: typeLabels[type], ip, protocol, rtsp, online: true });
    showToast('设备添加成功');
  }

  closeModal('modal-add-device');
  renderDevices();
});

document.getElementById('dev-search').addEventListener('input', e => renderDevices(e.target.value));
document.getElementById('dev-filter-type').addEventListener('change', () => renderDevices(document.getElementById('dev-search').value));

document.getElementById('dev-check-all').addEventListener('change', e => {
  document.querySelectorAll('#device-table-body input[type=checkbox]').forEach(cb => { cb.checked = e.target.checked; });
});

document.getElementById('add-dev-ip').addEventListener('blur', () => {
  const rtspInput = document.getElementById('add-dev-rtsp');
  if (!rtspInput.value.trim()) {
    const ipVal = document.getElementById('add-dev-ip').value.trim();
    const port = document.getElementById('add-dev-port').value || '554';
    if (ipVal) rtspInput.placeholder = `rtsp://${ipVal}:${port}/Streaming/Channels/101`;
  }
});

document.getElementById('btn-back-local').addEventListener('click', () => {
  const local = getLocalDevice();
  if (local) switchPreviewDevice(local.id, { showToastMsg: false, animated: true });
});

// ========== 初始化 ==========
loadStreamConfig('main');
updateLiveParams();
renderHomeAlgoResults();
renderAlgorithms();
updateAlgoStats();
syncLocalDeviceUI();
restorePreviewDevice();
applyPreviewDeviceUI({ animated: false });
restoreActivePage();

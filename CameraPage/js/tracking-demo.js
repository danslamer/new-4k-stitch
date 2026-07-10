/**
 * 轨迹跟踪 - 文字版轨迹结果与轨迹回放
 */

const TrackingDemo = (() => {
  let manifest = null;
  let manifestReady = false;
  let pageActive = false;
  let trackingEnabled = false;
  let replayTimer = null;
  let replayIndex = 0;

  function $(id) { return document.getElementById(id); }

  async function loadManifest() {
    try {
      const res = await fetch('demo/demo-manifest.json');
      if (!res.ok) throw new Error('manifest not found');
      manifest = await res.json();
      manifestReady = true;
      if (trackingEnabled) {
        renderTrackResults();
        renderReplayClips();
      }
      return true;
    } catch (e) {
      console.warn('[TrackingDemo] 配置加载失败', e);
      return false;
    }
  }

  function getPersonClips() {
    return (manifest?.clips || []).filter(c => c.personDetected !== false);
  }

  function renderTrackResults() {
    const summaryEl = $('track-result-summary');
    const detailEl = $('track-result-detail');
    const replayDesc = $('track-replay-desc');
    if (!manifest || !summaryEl) return;

    const result = manifest.trackResult || manifest.locateResult;
    const clips = getPersonClips();

    if (result) {
      summaryEl.innerHTML = `<span class="track-result-count">${result.summary || `共 ${result.personCount || 0} 条轨迹`}</span>`;
      if (detailEl && result.details?.length) {
        detailEl.innerHTML = result.details.map(d => `
          <div class="track-result-item">
            <div class="track-result-item-head">
              <span class="track-result-target">目标 #${d.targetId}</span>
              ${result.crossCamera ? '<span class="tag tag-primary">跨相机轨迹</span>' : ''}
            </div>
            <ul class="track-result-cam-list">
              ${(d.cameras || []).map(cam => `
                <li>
                  <span class="cam-name">${cam.deviceName}</span>
                  <span class="cam-range">${cam.range}</span>
                  ${cam.lastCoord ? `<span class="cam-coord">轨迹终点 ${cam.lastCoord}</span>` : ''}
                </li>
              `).join('')}
            </ul>
          </div>
        `).join('');
      }
    } else {
      const ids = new Set();
      clips.forEach(c => (c.tracks || []).forEach(t => ids.add(t.id)));
      const n = ids.size || clips.length;
      summaryEl.innerHTML = `<span class="track-result-count">共 ${n} 条轨迹</span>`;
      if (detailEl) {
        detailEl.innerHTML = clips.map(c => `
          <div class="track-result-item">
            <div class="track-result-item-head">
              <span class="track-result-target">${c.deviceName}</span>
              <span class="tag tag-success">有人</span>
            </div>
            <p class="track-result-range">${c.originalRange}</p>
          </div>
        `).join('');
      }
    }

    if (replayDesc && clips.length >= 2) {
      replayDesc.textContent = `${clips[0].deviceName}（${clips[0].originalRange}）→ ${clips[1].deviceName}（${clips[1].originalRange}）· 按时间顺序文字回放`;
    }
  }

  function renderReplayClips() {
    const box = $('track-replay-clips');
    if (!box || !manifest) return;
    const clips = getPersonClips();
    box.innerHTML = clips.map((c, i) => `
      <div class="track-replay-clip-row" data-clip-id="${c.id}">
        <span class="track-replay-clip-idx">片段 ${i + 1}</span>
        <span class="track-replay-clip-name">${c.deviceName}</span>
        <span class="track-replay-clip-range">${c.originalRange}</span>
        <span class="track-replay-clip-dur">${c.durationSec} 秒</span>
        <span class="track-replay-clip-path" title="${c.savedPath}">${c.savedPath}</span>
      </div>
    `).join('');
  }

  function setReplayRowActive(clipId) {
    document.querySelectorAll('.track-replay-clip-row').forEach(row => {
      row.classList.toggle('active', row.dataset.clipId === clipId);
    });
  }

  function clearReplayHighlight() {
    document.querySelectorAll('.track-replay-clip-row').forEach(row => {
      row.classList.remove('active');
    });
  }

  function stopReplay() {
    if (replayTimer) {
      clearTimeout(replayTimer);
      replayTimer = null;
    }
    replayIndex = 0;
    const btn = $('btn-track-replay');
    if (btn) {
      btn.classList.remove('playing');
      btn.disabled = false;
      btn.querySelector('.track-replay-btn-title').textContent = '开始轨迹回放';
    }
    const status = $('track-replay-status');
    if (status) status.classList.add('hidden');
    clearReplayHighlight();
  }

  function playNextClip(clips) {
    if (replayIndex >= clips.length) {
      const status = $('track-replay-status');
      if (status) {
        status.textContent = '轨迹回放完成';
        status.classList.remove('hidden');
      }
      stopReplay();
      return;
    }

    const clip = clips[replayIndex];
    const status = $('track-replay-status');
    if (status) {
      status.classList.remove('hidden');
      status.textContent = `正在回放：${clip.deviceName} · ${clip.originalRange}（${clip.durationSec} 秒）`;
    }
    setReplayRowActive(clip.id);

    if (typeof updateTrackTargets === 'function' && clip.tracks?.length) {
      const last = clip.tracks[0].keyframes?.slice(-1)[0];
      if (last) {
        updateTrackTargets([{
          id: clip.tracks[0].id,
          cx: last.cx,
          cy: last.cy,
          status: '回放中'
        }]);
      }
    }

    replayTimer = setTimeout(() => {
      replayIndex += 1;
      playNextClip(clips);
    }, Math.max(800, clip.durationSec * 200));
  }

  function startTrajectoryReplay() {
    const clips = getPersonClips();
    if (!clips.length) {
      showToast('暂无轨迹片段', 'error');
      return;
    }

    stopReplay();
    const btn = $('btn-track-replay');
    if (btn) {
      btn.classList.add('playing');
      btn.disabled = true;
      btn.querySelector('.track-replay-btn-title').textContent = '回放中…';
    }
    playNextClip(clips);
  }

  function updateTrackingStartUI() {
    const btn = $('btn-track-start');
    const panel = $('track-result-panel');
    const title = $('track-start-title');
    const desc = $('track-start-desc');
    const icon = $('track-start-icon');
    if (!btn) return;

    btn.classList.toggle('active', trackingEnabled);
    panel?.classList.toggle('hidden', !trackingEnabled);
    if (title) title.textContent = trackingEnabled ? '轨迹跟踪运行中' : '启用轨迹跟踪';
    if (desc) {
      if (trackingEnabled) {
        desc.textContent = '正在采集各主板轨迹片段，点击下方可停止';
        desc.classList.remove('hidden');
      } else {
        desc.textContent = '';
        desc.classList.add('hidden');
      }
    }
    if (icon) icon.textContent = trackingEnabled ? '■' : '▶';
  }

  /** 格式化 "HH:MM:SS" (本机时区), 给 UI 显示用. */
  function formatTime(d) {
    if (!(d instanceof Date)) d = new Date();
    const pad = (n) => String(n).padStart(2, '0');
    return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
  }

  /** 把可读的文件大小渲染成 "1.2 MB" / "834.5 KB" / "123 B". */
  function formatSize(bytes) {
    if (!bytes || bytes < 0) return '0 B';
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
    return `${(bytes / 1024 / 1024).toFixed(2)} MB`;
  }

  /**
   * 把刚保存的视频片段追加到「目标轨迹 - 已保存视频片段」列表.
   * - 新条目置顶, 老的往下推.
   * - 自动切走空状态, 显示 #track-clip-list.
   * - 累计数 (badge) 自增, 但不重复追加同名条目.
   *
   * v3.x: 这是给 track-recorder.js 通过 window.onTrackClipSaved(name, size, mime, when) 调用的钩子.
   *   改视频格式 / 改保存间隔不用动这里, 录制器只负责 call, UI 只负责渲染.
   */
  function appendClipItem(name, size, when) {
    const list = $('track-clip-list');
    const empty = $('track-clip-empty');
    const counter = $('track-clip-count');
    if (!list || !empty) return;

    // 防重复: 同一文件名 (同一段重复触发 onstop 时偶发) 只追加一次
    if (list.querySelector(`[data-clip-name="${CSS.escape(name)}"]`)) {
      return;
    }

    const sizeText = formatSize(size);
    const timeText = when ? formatTime(when) : formatTime(new Date());

    const row = document.createElement('div');
    row.className = 'track-clip-item';
    row.dataset.clipName = name;
    row.innerHTML = `
      <span class="track-clip-item-icon" aria-hidden="true">🎞️</span>
      <span class="track-clip-item-name" title="${name}">${name}</span>
      <span class="track-clip-item-meta">
        <span>${timeText}</span>
        <span>·</span>
        <span>${sizeText}</span>
      </span>
    `;

    // 新片段放最前, 保持「最新在上」的视觉习惯.
    list.insertBefore(row, list.firstChild);

    // 空状态收起来, 列表亮起来.
    empty.classList.add('hidden');
    list.classList.remove('hidden');

    // 累计计数 + 滚动到顶部, 让用户立刻看到新条目.
    if (counter) {
      const n = list.querySelectorAll('.track-clip-item').length;
      counter.textContent = String(n);
    }
    list.scrollTop = 0;
  }

  /** 重置已保存视频片段区 (用户停录后, 不清空; 给个开关随时手动 reset). */
  function clearClipList() {
    const list = $('track-clip-list');
    const empty = $('track-clip-empty');
    const counter = $('track-clip-count');
    if (list) {
      list.innerHTML = '';
      list.classList.add('hidden');
    }
    if (empty) empty.classList.remove('hidden');
    if (counter) counter.textContent = '0';
  }

  /**
   * 全局钩子: track-recorder.js 每成功上传一段就调一次.
   * 暴露到 window 而非 IIFE 内部, 因为 track-recorder.js 不依赖 TrackingDemo,
   * 反向依赖避免循环 (recorder 不依赖 demo, demo 也不直接引 recorder).
   */
  window.onTrackClipSaved = function (name, size, mime, when) {
    if (!name) return;
    appendClipItem(name, size, when);
  };

  function setTrackingEnabled(enabled) {
    trackingEnabled = enabled;
    updateTrackingStartUI();

    if (enabled) {
      // 启动浏览器端 MediaRecorder: 每 5 秒的 MJPEG 全景 → WebM → POST 到主板.
      if (typeof TrackRecorder !== 'undefined') {
        TrackRecorder.start();
      } else {
        console.warn('[TrackingDemo] TrackRecorder 未加载, 跳过视频保存');
      }
      // 浏览器端帧差叠加层: 把 absdiff 连通域画到 home 页的 #motion-overlay 上.
      if (typeof FrameDiffOverlay !== 'undefined') {
        FrameDiffOverlay.start();
      } else {
        console.warn('[TrackingDemo] FrameDiffOverlay 未加载, 跳过运动框');
      }
      if (!manifestReady) loadManifest();
      else {
        renderTrackResults();
        renderReplayClips();
      }
      showToast('轨迹跟踪已启动');
    } else {
      // 停录: 当前段跑完这一段再退出 (保证最后一段也落到 clips/).
      if (typeof TrackRecorder !== 'undefined') {
        TrackRecorder.stop();
      }
      // 帧差 overlay 关闭, 清空画布并隐藏 canvas.
      if (typeof FrameDiffOverlay !== 'undefined') {
        FrameDiffOverlay.stop();
      }
      stopReplay();
      showToast('轨迹跟踪已停止');
    }
  }

  function toggleTracking() {
    setTrackingEnabled(!trackingEnabled);
  }

  async function onPageEnter() {
    pageActive = true;
    if (!manifestReady) await loadManifest();
    if (!pageActive || !manifestReady) return;
    stopReplay();
    updateTrackingStartUI();
    if (trackingEnabled) {
      renderTrackResults();
      renderReplayClips();
    }
  }

  function onPageLeave() {
    pageActive = false;
    stopReplay();
  }

  function bindEvents() {
    $('btn-track-start')?.addEventListener('click', () => {
      toggleTracking();
    });

    $('btn-track-replay')?.addEventListener('click', () => {
      if (!trackingEnabled) {
        showToast('请先启用轨迹跟踪', 'error');
        return;
      }
      if (!manifestReady) {
        showToast('配置未加载', 'error');
        return;
      }
      startTrajectoryReplay();
    });
  }

  async function init() {
    bindEvents();
    await loadManifest();
    if ($('page-tracking')?.classList.contains('active')) {
      onPageEnter();
    }
  }

  return { init, onPageEnter, onPageLeave, loadManifest };
})();

document.addEventListener('DOMContentLoaded', () => {
  TrackingDemo.init();
});

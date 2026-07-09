/**
 * 轨迹跟踪视频录制 - MediaRecorder + canvas.captureStream
 *
 * 数据流: 浏览器轮询 GET /api/snapshot (主板 mjpeg_streamer 缓存的最新 JPEG)
 *   → createImageBitmap 解码 → drawImage 到 offscreen canvas
 *   → canvas.captureStream(10) → MediaRecorder 切片 5 秒一段
 *   → POST /api/clips/upload, 服务端按魔数定扩展名落 CameraPage/demo/clips/.
 *
 * 触发: TrackingDemo.setTrackingEnabled(true/false) → TrackRecorder.start/stop.
 *
 * v3.x: 早期版本用 <img id="mjpeg-stream"> 的 naturalWidth 镜像, 但 MJPEG
 *   流媒体没有 naturalWidth, drawImage 永远被跳过, canvas 始终空, 编码器吐
 *   header-only 的 WebM (ffprobe 报 "0x00 at pos 36 invalid EBML number").
 *   改为轮询 /api/snapshot, 不依赖 <img>, 命中率 100%.
 */

const TrackRecorder = (() => {
  let active = false;
  let segmentIdx = 0;
  let recorder = null;
  let offscreenCanvas = null;
  let offscreenCtx = null;
  let canvasStream = null;
  let pollingPromise = null;

  function pad(n) { return String(n).padStart(2, '0'); }

  function timestampString(d) {
    return `${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}` +
           `_${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`;
  }

  function pickMimeType() {
    // MP4 优先: Chrome 桌面版支持 video/mp4 + AVC1, 输出 .mp4 后 WMP / Windows
    // "电影和电视" / VLC / 手机 都能直接播. 不支持再退回 WebM (VP9 -> VP8).
    const candidates = [
      'video/mp4;codecs=avc1.42E01E',
      'video/mp4;codecs=avc1',
      'video/mp4',
      'video/webm;codecs=vp9',
      'video/webm;codecs=vp8',
      'video/webm'
    ];
    for (const t of candidates) {
      if (typeof MediaRecorder !== 'undefined' && MediaRecorder.isTypeSupported(t)) {
        return t;
      }
    }
    return '';
  }

  function extForMime(mimeType) {
    return mimeType.indexOf('mp4') >= 0 ? 'mp4' : 'webm';
  }

  function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }

  async function uploadClip(blob, mimeType) {
    if (blob.size === 0) return;
    const ts = timestampString(new Date());
    const ext = extForMime(mimeType);
    const filename = `track_${ts}_${String(segmentIdx).padStart(3, '0')}.${ext}`;
    const fd = new FormData();
    fd.append('clip', blob, filename);
    try {
      const res = await fetch('/api/clips/upload', { method: 'POST', body: fd });
      const json = await res.json().catch(() => ({}));
      if (res.ok && json.ok !== false) {
        const kb = (blob.size / 1024).toFixed(1);
        console.log(`[TrackRecorder] saved ${json.name || filename} (${kb} KB, ${mimeType})`);
        if (typeof showToast === 'function') {
          showToast(`已保存片段 ${json.name || filename} (${mimeType.split(';')[0]})`);
        }
      } else {
        const msg = (json && json.error) || `HTTP ${res.status}`;
        console.warn('[TrackRecorder] upload failed:', msg);
      }
    } catch (e) {
      console.warn('[TrackRecorder] upload error:', e);
    }
  }

  /**
   * 轮询 /api/snapshot, 把每帧画到 canvas.
   * - 首帧到达时锁尺寸 (captureStream 创建后, 中途改尺寸会断流).
   * - 10 fps polling; 主板 stitcher 一般 25~30 fps, 所以前后两次拉大概率不同帧.
   */
  async function pollSnapshotsLoop(ctx, canvas) {
    let firstFrame = true;
    while (active) {
      try {
        const res = await fetch(`/api/snapshot?cb=${Date.now()}`, {
          cache: 'no-store',
          headers: { 'Accept': 'image/jpeg' }
        });
        if (res.ok) {
          const blob = await res.blob();
          const bmp = await createImageBitmap(blob);
          if (firstFrame) {
            canvas.width = bmp.width;
            canvas.height = bmp.height;
            firstFrame = false;
            console.log(`[TrackRecorder] first frame ${bmp.width}x${bmp.height}`);
          }
          ctx.drawImage(bmp, 0, 0);
          if (bmp.close) bmp.close();
        } else {
          // 503/404 = 主板还没出帧, 等一拍再拉
        }
      } catch (e) {
        // 网络层失败 (主板 web server 临时不可用?), 不要中断循环
        console.warn('[TrackRecorder] poll error', e && e.message ? e.message : e);
      }
      await sleep(100);  // 10 fps
    }
  }

  function recordOneSegment(stream) {
    const mimeType = pickMimeType();
    if (!mimeType) {
      console.warn('[TrackRecorder] MediaRecorder unsupported in this browser');
      return Promise.resolve();
    }
    return new Promise((resolve) => {
      const r = new MediaRecorder(stream, { mimeType, videoBitsPerSecond: 1_500_000 });
      recorder = r;
      const chunks = [];
      r.ondataavailable = (e) => { if (e.data && e.data.size > 0) chunks.push(e.data); };
      r.onstop = async () => {
        recorder = null;
        const blob = new Blob(chunks, { type: mimeType });
        console.log(`[TrackRecorder] segment #${segmentIdx} done: ${blob.size} bytes, ${chunks.length} chunks`);
        if (blob.size > 0) await uploadClip(blob, mimeType);
        resolve();
      };
      r.onerror = (e) => {
        console.warn('[TrackRecorder] recorder error', e && e.error ? e.error : e);
        recorder = null;
        resolve();
      };
      try {
        r.start();
      } catch (e) {
        console.warn('[TrackRecorder] start() failed:', e && e.message ? e.message : e);
        resolve();
        return;
      }
      setTimeout(() => {
        if (r.state !== 'inactive') {
          try { r.stop(); } catch (_) { /* ignore */ }
        }
      }, 5000);
    });
  }

  async function loop() {
    offscreenCanvas = document.createElement('canvas');
    offscreenCtx = offscreenCanvas.getContext('2d');

    // 启动轮询, 持续往 canvas 画最新帧 (异步, 不阻塞主循环)
    pollingPromise = pollSnapshotsLoop(offscreenCtx, offscreenCanvas).catch((e) => {
      console.warn('[TrackRecorder] polling crashed', e);
    });

    // 等第一帧到位, 再 captureStream, 否则流是空流
    // (轮询内部 console.log "first frame" 那行提示首帧已到)
    // 不强制 sleep: captureStream 会在 canvas 内容变化时自动出帧

    let stream;
    try {
      stream = offscreenCanvas.captureStream(10);  // 10 fps
    } catch (e) {
      console.warn('[TrackRecorder] captureStream failed', e);
      active = false;
      return;
    }
    canvasStream = stream;

    while (active) {
      await recordOneSegment(stream);
    }

    // 收尾: 关流, 等最后几个 snapshot poll 退出
    if (stream.getTracks) {
      stream.getTracks().forEach((t) => { try { t.stop(); } catch (_) {} });
    }
    canvasStream = null;
    if (pollingPromise) {
      try { await pollingPromise; } catch (_) {}
      pollingPromise = null;
    }
    offscreenCanvas = null;
    offscreenCtx = null;
  }

  async function start() {
    if (active) return;
    active = true;
    segmentIdx = 0;
    loop().catch((e) => console.warn('[TrackRecorder] loop error', e));
  }

  function stop() {
    if (!active) return;
    active = false;
    // 当前段仍在跑的话, 让它跑完这一段 (保证最后一段也上传).
    if (recorder && recorder.state !== 'inactive') {
      try { recorder.stop(); } catch (_) { /* ignore */ }
    }
  }

  function isRecording() { return active; }

  return { start, stop, isRecording };
})();

window.TrackRecorder = TrackRecorder;

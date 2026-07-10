/**
 * Frame-Diff 运动框叠加层 (Frame-Difference Overlay)
 *
 * 数据流: 浏览器轮询 GET /api/snapshot (主板 mjpeg_streamer 缓存的最新 JPEG)
 *   → createImageBitmap 解码 → 缩放到 work 画布 (480×408)
 *   → 取上一帧灰度的 absdiff + 阈值
 *   → 4 连通 flood-fill 求每个连通块最小包围矩形 (≥ MIN_AREA 才保留)
 *   → 把矩形从 work 坐标系缩放到 overlay 画布 (display) 大小, 描边 + 标签
 *
 * 触发: TrackingDemo.setTrackingEnabled(true/false) → FrameDiffOverlay.start/stop.
 *  仅在「轨迹跟踪」开关 ON 时跑, 关闭就清空 overlay, 回到普通 MJPEG 预览.
 *
 * v3.x: 早期方案用服务器算 frame-diff 再画到 MJPEG, 但切换 / 关停信号要绕 RTSP, 不灵活.
 *   改为前端算, 仅依赖既有的 /api/snapshot (http_server.cc 已有), 不打乱服务器流水线.
 */

const FrameDiffOverlay = (() => {
  // 工作画布尺寸 - 缩到这么小再做 diff, 性能/灵敏度平衡.
  const WORK_W = 480;
  const WORK_H = 408;

  // absdiff 阈值 (灰度 0~255). 30 适配正常室内光照, 室外强光可放宽到 50.
  const THRESHOLD = 30;

  // 连通块最小像素数 (在 work 画布上的像素数). ≤ 60 px 视为噪声剔除.
  const MIN_AREA = 60;

  // 最大同时显示的运动框数量, 防止抖动场景刷屏.
  const MAX_BOXES = 24;

  // 轮询间隔 (ms) - 5 fps 足够人眼识别运动, 不会过载 /api/snapshot.
  const POLL_MS = 200;

  // 颜色 (与项目主色保持一致).
  const COLOR_STROKE = '#40a9ff';
  const COLOR_LABEL_BG = 'rgba(64, 169, 255, 0.92)';

  let active = false;
  let pollTimer = null;
  let prevGray = null;         // Uint8Array(WORK_W*WORK_H) 上一帧灰度
  let workCanvas = null;
  let workCtx = null;
  let overlayCanvas = null;
  let overlayCtx = null;
  let overlayAttached = false;
  let attachedPreview = null;  // 当前 attach 的 .video-preview-area (home 或 tracking)
  let segmentCount = 0;        // 仅用于日志, 不影响 UI

  /** 把 RGBA 像素数组 → 灰度 (BT.601). 输入长度 = w*h*4. */
  function toGray(rgba, w, h) {
    const out = new Uint8Array(w * h);
    for (let i = 0, j = 0; i < rgba.length; i += 4, j++) {
      // 0.299 R + 0.587 G + 0.114 B, 整数加权 (<< 6 是 *64) 避免浮点
      const r = rgba[i], g = rgba[i + 1], b = rgba[i + 2];
      out[j] = (r * 19 + g * 38 + b * 7) >> 6;  // 19+38+7=64, 总和>>6 ≈ BT.601
    }
    return out;
  }

  /**
   * 二值 mask 上跑 4 连通 flood-fill, 把所有 ≥ minArea 的连通块合并成唯一一个外接矩形.
   *   - mask[i] 0/255
   *   - 内部栈用 stack-based BFS, 不递归 (避免深度爆炸).
   *   - 单纯扫描 + 标记, 480×408 上耗时 < 5ms (有运动时多一些, 也 < 30ms).
   *
   * v3.x (聚合): 跟踪场景默认画面中只有一个主体在动, 多框显得零碎.
   *   把所有连通块的最小/最大 x/y 取并集, 总面积累加, 算成一个 outer box.
   *   - 没有候选块 → 返回空数组.
   *   - 只有 1 个块 → 直接返回它.
   *   - 多个块 → 把它们"打包"成一个 {x, y, w, h, area} 元素.
   *   MAX_BOXES 限制语义变成"只画一个外接框", 所以下面省掉 slice.
   */
  function findBoundingBoxes(mask, w, h, minArea) {
    const visited = new Uint8Array(mask.length);
    const blobs = [];
    const stack = [];

    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const seedIdx = y * w + x;
        if (mask[seedIdx] === 0 || visited[seedIdx]) continue;

        // BFS 这一片区域
        let minX = x, maxX = x, minY = y, maxY = y;
        let area = 0;
        stack.length = 0;
        stack.push(seedIdx);

        while (stack.length > 0) {
          const i = stack.pop();
          if (visited[i]) continue;
          visited[i] = 1;
          if (mask[i] === 0) continue;

          area++;
          const cx = i % w;
          const cy = (i / w) | 0;
          if (cx < minX) minX = cx;
          if (cx > maxX) maxX = cx;
          if (cy < minY) minY = cy;
          if (cy > maxY) maxY = cy;

          if (cx > 0)     stack.push(i - 1);
          if (cx < w - 1) stack.push(i + 1);
          if (cy > 0)     stack.push(i - w);
          if (cy < h - 1) stack.push(i + w);
        }

        if (area >= minArea) {
          blobs.push({ minX, maxX, minY, maxY, area });
        }
      }
    }

    if (blobs.length === 0) return [];
    if (blobs.length === 1) {
      const b = blobs[0];
      return [{
        x: b.minX,
        y: b.minY,
        w: b.maxX - b.minX + 1,
        h: b.maxY - b.minY + 1,
        area: b.area
      }];
    }

    // 多块聚合: 一个 outer box 覆盖全部运动像素, 总面积 = 各块之和.
    let outMinX = blobs[0].minX, outMaxX = blobs[0].maxX;
    let outMinY = blobs[0].minY, outMaxY = blobs[0].maxY;
    let totalArea = 0;
    for (const b of blobs) {
      if (b.minX < outMinX) outMinX = b.minX;
      if (b.maxX > outMaxX) outMaxX = b.maxX;
      if (b.minY < outMinY) outMinY = b.minY;
      if (b.maxY > outMaxY) outMaxY = b.maxY;
      totalArea += b.area;
    }
    return [{
      x: outMinX,
      y: outMinY,
      w: outMaxX - outMinX + 1,
      h: outMaxY - outMinY + 1,
      area: totalArea,
      blobs: blobs.length   // 携带原始块数, 给标签用 "运动 ≈{area}px · {n}块合并"
    }];
  }

  /** 把 work 坐标 boxes 画到 overlay 画布, 按 overlay.display size 缩放. */
  function paintBoxes(canvas, ctx, boxes) {
    const dispW = canvas.width;
    const dispH = canvas.height;
    if (dispW <= 0 || dispH <= 0) return;

    const scaleX = dispW / WORK_W;
    const scaleY = dispH / WORK_H;

    ctx.clearRect(0, 0, dispW, dispH);

    if (boxes.length === 0) {
      // 在右上角画个 "无运动" 小角标, 提示 overlay 是开着的
      ctx.fillStyle = 'rgba(255, 255, 255, 0.65)';
      ctx.font = '12px "Microsoft YaHei", sans-serif';
      ctx.textBaseline = 'top';
      ctx.fillText('跟踪中 · 无运动', 12, 12);
      return;
    }

    ctx.lineWidth = Math.max(2, dispW / 320);  // 屏越宽, 线越粗
    ctx.font = `${Math.max(11, dispW / 60)}px "Microsoft YaHei", sans-serif`;
    ctx.textBaseline = 'top';

    for (const b of boxes) {
      const x = b.x * scaleX;
      const y = b.y * scaleY;
      const w = b.w * scaleX;
      const h = b.h * scaleY;

      // 描边 + 半透明内填充
      ctx.fillStyle = 'rgba(64, 169, 255, 0.18)';
      ctx.fillRect(x, y, w, h);
      ctx.strokeStyle = COLOR_STROKE;
      ctx.strokeRect(x, y, w, h);

      // 标签: 聚合后仍有 b.blobs (原始连通块数); 单块时省略 "· M 块合并"
      const label = b.blobs && b.blobs > 1
        ? `运动 ${b.area}px · ${b.blobs} 块合并`
        : `运动 ${b.area}px`;
      const padX = 6;
      const padY = 3;
      const tw = ctx.measureText(label).width;
      const lh = parseInt(ctx.font, 10) + padY * 2;
      const lx = x;
      const ly = Math.max(0, y - lh - 2);

      ctx.fillStyle = COLOR_LABEL_BG;
      ctx.fillRect(lx, ly, tw + padX * 2, lh);
      ctx.fillStyle = '#fff';
      ctx.fillText(label, lx + padX, ly + padY);
    }

    // 顶部小角标: 跟踪开启提示 (boxes 已是聚合后的 0/1, 用 b.blobs 表达原始块数).
    const tipText = boxes.length === 0
      ? '跟踪中 · 无运动'
      : (boxes[0].blobs && boxes[0].blobs > 1
          ? `跟踪中 · 运动 1 (聚合自 ${boxes[0].blobs} 块)`
          : '跟踪中 · 运动 1');
    ctx.fillStyle = 'rgba(0, 0, 0, 0.55)';
    ctx.font = '12px "Microsoft YaHei", sans-serif';
    const tipW = Math.ceil(ctx.measureText(tipText).width) + 16;
    const tipH = 22;
    ctx.fillRect(dispW - tipW - 12, 12, tipW, tipH);
    ctx.fillStyle = '#fff';
    ctx.fillText(tipText, dispW - tipW - 4, 16);
  }

  /** 让 overlay canvas 跟着 #video-preview 的实际显示尺寸, 同步 resize. */
  function syncOverlaySize() {
    if (!overlayCanvas) return;
    const preview = document.getElementById('video-preview');
    if (!preview) return;
    const rect = preview.getBoundingClientRect();
    if (rect.width <= 0 || rect.height <= 0) return;
    if (overlayCanvas.width !== Math.round(rect.width) ||
        overlayCanvas.height !== Math.round(rect.height)) {
      overlayCanvas.width = Math.round(rect.width);
      overlayCanvas.height = Math.round(rect.height);
    }
  }

  let resizeObserver = null;

  /** 让 overlay canvas 跟着当前 attach 的 preview 容器的实际显示尺寸同步 resize. */
  function syncOverlaySize() {
    if (!overlayCanvas || !attachedPreview) return;
    const rect = attachedPreview.getBoundingClientRect();
    if (rect.width <= 0 || rect.height <= 0) return;
    if (overlayCanvas.width !== Math.round(rect.width) ||
        overlayCanvas.height !== Math.round(rect.height)) {
      overlayCanvas.width = Math.round(rect.width);
      overlayCanvas.height = Math.round(rect.height);
    }
  }

  /**
   * 找到当前激活的预览容器, 优先选可见的那个, 否则用 fallback.
   *   跟踪页有 #tracking-video-preview 配 #tracking-motion-overlay,
   *   首页有 #video-preview 配 #motion-overlay. 用户在哪页开启就跟哪页.
   *
   * 如果选不到 active 的, 也 fallback 到第一个非空的 motion-overlay.
   */
  function findActivePreview() {
    const candidates = [
      { previewId: 'tracking-video-preview', overlayId: 'tracking-motion-overlay' },
      { previewId: 'video-preview',          overlayId: 'motion-overlay' }
    ];
    for (const c of candidates) {
      const p = document.getElementById(c.previewId);
      if (!p) continue;
      // offsetParent !== null 说明在可见的 page-panel 内 (display:none 的 panel 子树整体不可见)
      if (p.offsetParent !== null || p.getClientRects().length > 0) {
        return { preview: p, overlay: document.getElementById(c.overlayId) };
      }
    }
    // 都不可见 (用户切到别的页), 用第一个还存在的 overlay 兜底, 这样切换回去不会丢连线
    for (const c of candidates) {
      const o = document.getElementById(c.overlayId);
      if (o) {
        return { preview: document.getElementById(c.previewId), overlay: o };
      }
    }
    return null;
  }

  function attachOverlay() {
    if (overlayAttached) return;
    const found = findActivePreview();
    if (!found || !found.preview || !found.overlay) {
      // 页面还没渲染, 推迟一帧
      requestAnimationFrame(attachOverlay);
      return;
    }
    const { preview, overlay } = found;
    overlayCanvas = overlay;
    overlayCtx = overlayCanvas.getContext('2d');
    overlayAttached = true;
    attachedPreview = preview;

    syncOverlaySize();

    if (typeof ResizeObserver !== 'undefined') {
      resizeObserver = new ResizeObserver(syncOverlaySize);
      resizeObserver.observe(preview);
    } else {
      window.addEventListener('resize', syncOverlaySize);
    }
  }

  function detachOverlay() {
    if (!overlayAttached) return;
    overlayAttached = false;
    attachedPreview = null;
    if (resizeObserver) {
      resizeObserver.disconnect();
      resizeObserver = null;
    }
    if (overlayCanvas) {
      overlayCanvas.classList.add('hidden');
    }
  }

  async function tick() {
    if (!active) return;
    try {
      const res = await fetch(`/api/snapshot?cb=${Date.now()}`, {
        cache: 'no-store',
        headers: { 'Accept': 'image/jpeg' }
      });
      if (!res.ok) return;
      const blob = await res.blob();
      const bmp = await createImageBitmap(blob);

      // 1. 缩放到 work 画布
      workCtx.clearRect(0, 0, WORK_W, WORK_H);
      workCtx.drawImage(bmp, 0, 0, WORK_W, WORK_H);
      if (bmp.close) bmp.close();

      // 2. 取灰度
      const imgData = workCtx.getImageData(0, 0, WORK_W, WORK_H);
      const currGray = toGray(imgData.data, WORK_W, WORK_H);

      // 3. 首帧只缓存, 不画框
      if (!prevGray || prevGray.length !== currGray.length) {
        prevGray = currGray;
        return;
      }

      // 4. absdiff + 阈值
      const mask = new Uint8Array(WORK_W * WORK_H);
      let motionPx = 0;
      for (let i = 0; i < currGray.length; i++) {
        const d = currGray[i] - prevGray[i];
        const absD = d < 0 ? -d : d;
        if (absD > THRESHOLD) {
          mask[i] = 255;
          motionPx++;
        }
      }
      prevGray = currGray;

      // 5. 没有任何运动像素, 描一下空状态 (cool anim) 就退出
      const boxes = motionPx === 0 ? [] : findBoundingBoxes(mask, WORK_W, WORK_H, MIN_AREA);

      // 6. 画到 overlay (尺寸不变, 只改像素)
      if (overlayCtx) {
        // overlay 跟着 home 页切换 / 窗口 resize 变尺寸
        syncOverlaySize();
        paintBoxes(overlayCanvas, overlayCtx, boxes);
      }

      // 7. 调试计数 (DevTools 看)
      segmentCount = (segmentCount + 1) % 100000;
    } catch (e) {
      // 网络层失败 (board 临时不可用), 保持 overlay 上次结果, 别抖
    }
  }

  async function start() {
    if (active) return;
    active = true;
    prevGray = null;
    segmentCount = 0;

    workCanvas = document.createElement('canvas');
    workCanvas.width = WORK_W;
    workCanvas.height = WORK_H;
    workCtx = workCanvas.getContext('2d', { willReadFrequently: true });

    attachOverlay();

    if (overlayCanvas) {
      overlayCanvas.classList.remove('hidden');
      syncOverlaySize();
    }

    // 立即跑一次 (init prev), 之后每 POLL_MS 跑一次
    tick().catch(() => {});
    pollTimer = setInterval(() => { tick().catch(() => {}); }, POLL_MS);
    console.log(`[FrameDiffOverlay] started (work ${WORK_W}x${WORK_H}, threshold=${THRESHOLD})`);
  }

  function stop() {
    if (!active) return;
    active = false;
    if (pollTimer) {
      clearInterval(pollTimer);
      pollTimer = null;
    }
    prevGray = null;
    workCanvas = null;
    workCtx = null;
    if (overlayCtx) {
      overlayCtx.clearRect(0, 0, overlayCanvas.width, overlayCanvas.height);
    }
    if (overlayCanvas) {
      overlayCanvas.classList.add('hidden');
    }
    detachOverlay();
    console.log('[FrameDiffOverlay] stopped');
  }

  function isActive() { return active; }

  return { start, stop, isActive };
})();

window.FrameDiffOverlay = FrameDiffOverlay;

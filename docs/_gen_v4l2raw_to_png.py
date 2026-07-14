"""将 4 路 v4l2 抓的 raw BA10 (10-bit packed bayer GRGR/BGBG) 转成可视 PNG.

BA10 packing (MIPI RAW10):
  4 pixels (4*10=40 bit) 打包到 5 bytes:
    pixel[0] low 8  = byte[0]
    pixel[1] low 8  = byte[1]
    pixel[2] low 8  = byte[2]
    pixel[3] low 8  = byte[3]
    byte[4] = (pixel[3] high 2 << 6) | (pixel[2] high 2 << 4)
             | (pixel[1] high 2 << 2) | (pixel[0] high 2 << 0)
"""
import os
import sys
import numpy as np
import cv2

WIDTH = 2560
HEIGHT = 1440
STRIDE = 3328            # 3328 = 2560*10/8 + 128 padding
RAW_SIZE = STRIDE * HEIGHT

BAYER = cv2.COLOR_BayerRGGB2BGR  # GC4683 BA10 = "Bayer GRGR/BGBG" = RGGB pattern
INPUT_DIR = "/home/rocktech/Projects/new-4k-stitch/results/test"


def unpack_raw10(buf: np.ndarray) -> np.ndarray:
    """buf shape=(HEIGHT, STRIDE) uint8 → (HEIGHT, WIDTH) uint16."""
    # 只取前 3200 字节有效区 (2560 * 10 / 8), 跳过末尾 128 padding
    payload = buf[:, :3200].copy()                    # (H, 3200)
    # 5 bytes -> 4 pixels
    H = payload.shape[0]
    payload = payload.reshape(H, 800, 5).astype(np.uint16)
    low = payload[:, :, 0:4]                          # (H, 800, 4)
    high_byte = payload[:, :, 4]                      # (H, 800)
    high = np.empty((H, 800, 4), dtype=np.uint16)
    for i in range(4):
        high[:, :, i] = (high_byte >> (i * 2)) & 0x3
    img16 = (low << 2) | high                        # (H, 800, 4) 0..1023
    img16 = img16.reshape(H, 3200)[:, :WIDTH]         # trim to 2560
    return img16


def raw_to_png(raw_path: str, png_path: str):
    with open(raw_path, "rb") as f:
        data = np.frombuffer(f.read(), dtype=np.uint8)
    assert data.size == RAW_SIZE, f"size mismatch: {data.size} != {RAW_SIZE}"
    img16 = unpack_raw10(data.reshape(HEIGHT, STRIDE))
    # 10bit -> 8bit: simple top-8 truncation; 也可以用 ((v << 6) | (v >> 4)) 拉伸
    img8 = (img16 >> 2).astype(np.uint8)
    # OpenCV demosaic RGGB -> BGR
    bgr = cv2.dvtColor(img8, BAYER) if hasattr(cv2, "dvtColor") else cv2.cvtColor(img8, BAYER)
    # 自动白平衡 (gray-world 近似)
    bgr = cv2.xphoto.createSimpleWB().balanceWhite(bgr) if hasattr(cv2, "xphoto") else _simple_wb(bgr)
    # 直方图均衡 (提高暗部可视度)
    bgr = cv2.convertScaleAbs(bgr, alpha=1.4, beta=15)
    cv2.imwrite(png_path, bgr)
    return img16, bgr


def _simple_wb(bgr):
    """Gray-world 白平衡."""
    b, g, r = cv2.split(bgr.astype(np.float32))
    mb, mg, mr = b.mean() + 1e-6, g.mean() + 1e-6, r.mean() + 1e-6
    m_gray = (mb + mg + mr) / 3
    b = np.clip(b * m_gray / mb, 0, 255)
    g = np.clip(g * m_gray / mg, 0, 255)
    r = np.clip(r * m_gray / mr, 0, 255)
    return cv2.merge([b, g, r]).astype(np.uint8)


def main():
    for f in sorted(os.listdir(INPUT_DIR)):
        if not f.endswith(".raw"):
            continue
        raw_p = os.path.join(INPUT_DIR, f)
        png_p = os.path.join(INPUT_DIR, f.replace(".raw", ".png"))
        img16, bgr = raw_to_png(raw_p, png_p)
        print(f"{f}: 10bit mean={img16.mean():.1f} std={img16.std():.1f}  "
              f"-> {os.path.basename(png_p)} ({bgr.shape})")


if __name__ == "__main__":
    main()

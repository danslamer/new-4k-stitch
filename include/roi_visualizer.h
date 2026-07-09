#ifndef ROI_VISUALIZER_H
#define ROI_VISUALIZER_H

#include <SDL2/SDL.h>
#include <cstdint>
#include <string>
#include <vector>
#include "roi_config.h"

extern bool g_show_roi_markers;

enum VisAction {
    kVisNone = 0,
    kVisStepUp,        // 方向键: y -= step
    kVisStepDown,      // y += step
    kVisStepLeft,      // x -= step
    kVisStepRight,     // x += step
    // v3.x (2026-07-09): shift+方向键 调整 ROI 尺寸
    kVisResizeUp,      // height -= step
    kVisResizeDown,    // height += step
    kVisResizeLeft,    // width -= step
    kVisResizeRight,   // width += step
    kVisNextCam,
    kVisPrevCam,
    kVisSaveConfig,
    kVisRefreshFrames,  // v3.x (2026-07-09): 在 debug 模式里重新抓最新帧作为快照, 看动态场景
    kVisEnterDebug,
    kVisExitDebug,
    kVisStepSize1,
    kVisStepSize5,
    kVisStepSize10,
    kVisStepSize50,
    kVisToggleMarkers,
    kVisFeatherToggle,
    kVisFeatherWidthUp,
    kVisFeatherWidthDown,
    kVisSaveToggle,
    kVisSaveIntervalUp,
    kVisSaveIntervalDown,
    kVisNeedRestitch,
    kVisNeedRebuild
};

struct StitchTask;

class RoiVisualizer {
public:
    // v3.x (2026-07-09): 加 num_cams 参数 — 4 路 (2x2) 和 6 路 (2x3) 共用 visualizer,
    //   用 num_cams 决定画 ROI marker 的数量 + Tab 切换的范围. 默认 6 兼容现状.
    static bool Init(int panorama_width, int panorama_height, int num_cams = 6);
    static void Shutdown();

    static VisAction ShowStreamingFrame(const uint8_t* bgr_data,
                                         int width, int height,
                                         int stride, double fps);
    static VisAction ShowDebugFrame(const uint8_t* bgr_data,
                                    int width, int height,
                                    int stride, double fps,
                                    const std::vector<StitchTask>& tasks);

private:
    static SDL_Window* window_;
    static SDL_Renderer* renderer_;
    static SDL_Texture* texture_;
    static int display_w_;
    static int display_h_;
    static int panorama_w_;
    static int panorama_h_;
    static int num_cams_;  // 4 (2x2) 或 6 (2x3), 由 Init 注入
    static bool initialized_;
    
    static void FillRect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);
    static void DrawRectOutline(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, int thickness);
    static void DrawText(int x, int y, const std::string& text, uint8_t r, uint8_t g, uint8_t b);
    static void DrawChar(int x, int y, char c, uint8_t r, uint8_t g, uint8_t b);
    
    static void DrawROIMarkers(const std::vector<StitchTask>& tasks);
    static void DrawFpsOverlay(double fps);
    static void DrawControlInfo();
    static void DrawFeatherSaveInfo();
    
    static VisAction PollEvents(bool debug_mode);
    // v3.x (2026-07-09): shift 标志传入, 让方向键既能挪 (x,y) 也能改 (w,h)
    static VisAction ParseKey(SDL_Keycode key, bool shift);
    static VisAction HandleDebugAction(VisAction action);
    
    static int NormalizeEvenFloor(int value);
    
    static int ScaleX(int x);
    static int ScaleY(int y);
    static int ScaleW(int w);
    static int ScaleH(int h);
};

#endif
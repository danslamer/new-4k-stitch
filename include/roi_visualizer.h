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
    kVisStepUp,
    kVisStepDown,
    kVisStepLeft,
    kVisStepRight,
    kVisNextCam,
    kVisPrevCam,
    kVisSaveConfig,
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
    static bool Init(int panorama_width, int panorama_height);
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
    static VisAction ParseKey(SDL_Keycode key);
    static VisAction HandleDebugAction(VisAction action);
    
    static int NormalizeEvenFloor(int value);
    
    static int ScaleX(int x);
    static int ScaleY(int y);
    static int ScaleW(int w);
    static int ScaleH(int h);
};

#endif
#include "roi_visualizer.h"
#include "logger.h"
#include "image_stitcher.h"

#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>

extern StitchGlobalConfig g_config;
extern bool g_show_roi_markers;

namespace {

constexpr int FONT_W = 5;
constexpr int FONT_H = 7;
constexpr int FONT_SCALE = 2;
constexpr int CHAR_SPACING = 1;

constexpr int DISPLAY_DEFAULT_W = 1280;
constexpr int DISPLAY_DEFAULT_H = 720;

constexpr uint8_t CAM_COLORS_BGR[6][3] = {
    {0, 255, 0},     // cam0 - green
    {0, 0, 255},     // cam1 - red
    {255, 0, 0},     // cam2 - blue
    {0, 255, 255},   // cam3 - yellow
    {255, 0, 255},   // cam4 - magenta
    {255, 255, 0}    // cam5 - cyan
};

// v3.x (2026-07-09): 扩到 6 项 (2x3 布局):
//   cam0 LT 左上, cam1 RT 右上, cam2 LM 左中, cam3 RM 右中, cam4 LB 左下, cam5 RB 右下.
// 4 路 (2x2) 走 cam0..cam3 复用前 4 项.
const char* CAM_LABELS[6] = {"Cam0", "Cam1", "Cam2", "Cam3", "Cam4", "Cam5"};
const char* CAM_POSITIONS[6] = {
    "LT(C0)", "RT(C1)", "LM(C2)", "RM(C3)", "LB(C4)", "RB(C5)"
};

static const uint8_t FONT_5x7[][7][5] = {
    {{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0}}, // space
    {{0,1,1,1,0},{1,0,0,0,1},{1,0,0,1,1},{1,0,1,0,1},{1,1,0,0,1},{1,0,0,0,1},{0,1,1,1,0}}, // 0
    {{0,0,1,0,0},{0,1,1,0,0},{1,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0},{1,1,1,1,1}}, // 1
    {{0,1,1,1,0},{1,0,0,0,1},{0,0,0,0,1},{0,0,1,1,0},{0,1,0,0,0},{1,0,0,0,0},{1,1,1,1,1}}, // 2
    {{0,1,1,1,0},{1,0,0,0,1},{0,0,0,0,1},{0,0,1,1,0},{0,0,0,0,1},{1,0,0,0,1},{0,1,1,1,0}}, // 3
    {{0,0,0,1,0},{0,0,1,1,0},{0,1,0,1,0},{1,0,0,1,0},{1,1,1,1,1},{0,0,0,1,0},{0,0,0,1,0}}, // 4
    {{1,1,1,1,1},{1,0,0,0,0},{1,1,1,1,0},{0,0,0,0,1},{0,0,0,0,1},{1,0,0,0,1},{0,1,1,1,0}}, // 5
    {{0,1,1,1,0},{1,0,0,0,0},{1,0,0,0,0},{1,1,1,1,0},{1,0,0,0,1},{1,0,0,0,1},{0,1,1,1,0}}, // 6
    {{1,1,1,1,1},{0,0,0,0,1},{0,0,0,1,0},{0,0,1,0,0},{0,1,0,0,0},{0,1,0,0,0},{0,1,0,0,0}}, // 7
    {{0,1,1,1,0},{1,0,0,0,1},{1,0,0,0,1},{0,1,1,1,0},{1,0,0,0,1},{1,0,0,0,1},{0,1,1,1,0}}, // 8
    {{0,1,1,1,0},{1,0,0,0,1},{1,0,0,0,1},{0,1,1,1,1},{0,0,0,0,1},{0,0,0,0,1},{0,1,1,1,0}}, // 9
    {{0,1,1,1,0},{1,0,0,0,1},{1,0,0,0,1},{1,1,1,1,1},{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1}}, // A
    {{1,1,1,1,0},{1,0,0,0,1},{1,0,0,0,1},{1,1,1,1,0},{1,0,0,0,1},{1,0,0,0,1},{1,1,1,1,0}}, // B
    {{0,1,1,1,1},{1,0,0,0,0},{1,0,0,0,0},{1,0,0,0,0},{1,0,0,0,0},{1,0,0,0,0},{0,1,1,1,1}}, // C
    {{1,1,1,1,0},{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1},{1,1,1,1,0}}, // D
    {{1,1,1,1,1},{1,0,0,0,0},{1,0,0,0,0},{1,1,1,1,0},{1,0,0,0,0},{1,0,0,0,0},{1,1,1,1,1}}, // E
    {{1,1,1,1,1},{1,0,0,0,0},{1,0,0,0,0},{1,1,1,1,0},{1,0,0,0,0},{1,0,0,0,0},{1,0,0,0,0}}, // F
    {{0,1,1,1,1},{1,0,0,0,0},{1,0,0,0,0},{1,0,1,1,1},{1,0,0,0,1},{1,0,0,0,1},{0,1,1,1,0}}, // G
    {{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1},{1,1,1,1,1},{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1}}, // H
    {{0,1,1,1,0},{0,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0},{0,1,1,1,0}}, // I
    {{0,0,1,1,1},{0,0,0,1,0},{0,0,0,1,0},{0,0,0,1,0},{1,0,0,1,0},{1,0,0,1,0},{0,1,1,0,0}}, // J
    {{1,0,0,0,1},{1,0,0,1,0},{1,0,1,0,0},{1,1,0,0,0},{1,0,1,0,0},{1,0,0,1,0},{1,0,0,0,1}}, // K
    {{1,0,0,0,0},{1,0,0,0,0},{1,0,0,0,0},{1,0,0,0,0},{1,0,0,0,0},{1,0,0,0,0},{1,1,1,1,1}}, // L
    {{1,0,0,0,1},{1,1,0,1,1},{1,0,1,0,1},{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1}}, // M
    {{1,0,0,0,1},{1,1,0,0,1},{1,0,1,0,1},{1,0,0,1,1},{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1}}, // N
    {{0,1,1,1,0},{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1},{0,1,1,1,0}}, // O
    {{1,1,1,1,0},{1,0,0,0,1},{1,0,0,0,1},{1,1,1,1,0},{1,0,0,0,0},{1,0,0,0,0},{1,0,0,0,0}}, // P
    {{0,1,1,1,0},{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1},{1,0,1,0,1},{1,0,0,1,0},{0,1,1,0,1}}, // Q
    {{1,1,1,1,0},{1,0,0,0,1},{1,0,0,0,1},{1,1,1,1,0},{1,0,1,0,0},{1,0,0,1,0},{1,0,0,0,1}}, // R
    {{0,1,1,1,1},{1,0,0,0,0},{1,0,0,0,0},{0,1,1,1,0},{0,0,0,0,1},{0,0,0,0,1},{1,1,1,1,0}}, // S
    {{1,1,1,1,1},{0,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0}}, // T
    {{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1},{0,1,1,1,0}}, // U
    {{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1},{0,1,0,1,0},{0,1,0,1,0},{0,0,1,0,0}}, // V
    {{1,0,0,0,1},{1,0,0,0,1},{1,0,0,0,1},{1,0,1,0,1},{1,0,1,0,1},{1,1,0,1,1},{1,0,0,0,1}}, // W
    {{1,0,0,0,1},{0,1,0,1,0},{0,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0},{0,1,0,1,0},{1,0,0,0,1}}, // X
    {{1,0,0,0,1},{0,1,0,1,0},{0,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0},{0,0,1,0,0}}, // Y
    {{1,1,1,1,1},{0,0,0,0,1},{0,0,0,1,0},{0,0,1,0,0},{0,1,0,0,0},{1,0,0,0,0},{1,1,1,1,1}}, // Z
    {{0,0,0,0,0},{0,0,1,0,0},{0,0,1,0,0},{0,0,0,0,0},{0,0,1,0,0},{0,0,1,0,0},{0,0,0,0,0}}, // :
    {{0,0,0,0,0},{0,0,1,0,0},{0,0,1,0,0},{1,1,1,1,1},{0,0,1,0,0},{0,0,1,0,0},{0,0,0,0,0}}, // +
    {{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},{1,1,1,1,1},{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0}}, // -
    {{0,0,0,0,0},{0,0,0,0,0},{1,1,1,1,1},{0,0,0,0,0},{1,1,1,1,1},{0,0,0,0,0},{0,0,0,0,0}}, // =
    {{0,1,1,1,0},{0,1,0,0,0},{0,1,0,0,0},{0,1,0,0,0},{0,1,0,0,0},{0,1,0,0,0},{0,1,1,1,0}}, // [
    {{0,1,1,1,0},{0,0,0,1,0},{0,0,0,1,0},{0,0,0,1,0},{0,0,0,1,0},{0,0,0,1,0},{0,1,1,1,0}}, // ]
    {{0,0,1,1,0},{0,1,0,0,0},{1,0,0,0,0},{1,0,0,0,0},{1,0,0,0,0},{0,1,0,0,0},{0,0,1,1,0}}, // (
    {{0,1,1,0,0},{0,0,0,1,0},{0,0,0,0,1},{0,0,0,0,1},{0,0,0,0,1},{0,0,0,1,0},{0,1,1,0,0}}, // )
    {{0,0,0,0,1},{0,0,0,1,0},{0,0,0,1,0},{0,0,1,0,0},{0,1,0,0,0},{0,1,0,0,0},{1,0,0,0,0}}, // /
    {{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},{0,0,1,0,0},{0,0,1,0,0}}, // .
    {{0,1,1,1,0},{1,0,0,0,1},{0,0,1,0,0},{0,1,0,0,0},{0,0,1,0,0},{1,0,0,0,1},{0,1,1,1,0}}, // %
};

static const char FONT_CHARS[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:+-=[]()/.%";

int CharToFontIndex(char c) {
    for (size_t i = 0; i < sizeof(FONT_CHARS) - 1; ++i) {
        if (FONT_CHARS[i] == c) return static_cast<int>(i);
    }
    return -1;
}

const uint8_t* GetFontBits(int idx) {
    if (idx < 0 || idx >= static_cast<int>(sizeof(FONT_5x7) / sizeof(FONT_5x7[0]))) {
        return nullptr;
    }
    return &FONT_5x7[idx][0][0];
}

}

SDL_Window* RoiVisualizer::window_ = nullptr;
SDL_Renderer* RoiVisualizer::renderer_ = nullptr;
SDL_Texture* RoiVisualizer::texture_ = nullptr;
int RoiVisualizer::display_w_ = 0;
int RoiVisualizer::display_h_ = 0;
int RoiVisualizer::panorama_w_ = 0;
int RoiVisualizer::panorama_h_ = 0;
int RoiVisualizer::num_cams_ = 6;  // 默认 6, Init() 重新注入
bool RoiVisualizer::initialized_ = false;

int RoiVisualizer::NormalizeEvenFloor(int value) {
    return std::max(0, value & ~1);
}

int RoiVisualizer::ScaleX(int x) {
    if (panorama_w_ <= 0) return x;
    return x * display_w_ / panorama_w_;
}

int RoiVisualizer::ScaleY(int y) {
    if (panorama_h_ <= 0) return y;
    return y * display_h_ / panorama_h_;
}

int RoiVisualizer::ScaleW(int w) {
    if (panorama_w_ <= 0) return w;
    return w * display_w_ / panorama_w_;
}

int RoiVisualizer::ScaleH(int h) {
    if (panorama_h_ <= 0) return h;
    return h * display_h_ / panorama_h_;
}

bool RoiVisualizer::Init(int panorama_width, int panorama_height, int num_cams) {
    panorama_w_ = panorama_width;
    panorama_h_ = panorama_height;
    num_cams_ = num_cams > 0 ? num_cams : 6;  // 兜底 6
    
    const char* display = getenv("DISPLAY");
    Logger::GetInstance().Log("[RoiVisualizer] DISPLAY env: " + 
                               (display ? std::string(display) : "(not set)"));
    
    if (display == nullptr || strlen(display) == 0 || strstr(display, "192.168") != nullptr) {
        setenv("DISPLAY", ":0", 1);
        Logger::GetInstance().Log("[RoiVisualizer] DISPLAY set to :0");
    }
    
    const char* env_driver = getenv("SDL_VIDEODRIVER");
    Logger::GetInstance().Log("[RoiVisualizer] SDL_VIDEODRIVER env: " + 
                               (env_driver ? std::string(env_driver) : "(not set)"));
    
    const char* drivers[] = {"x11", "wayland", "kmsdrm", "dummy", nullptr};
    
    for (int i = 0; drivers[i] != nullptr; ++i) {
        const char* driver = drivers[i];
        Logger::GetInstance().Log("[RoiVisualizer] Trying video driver: " + std::string(driver));
        
        // SDL 2.0.22+ 重命名为 SDL_HINT_VIDEO_DRIVER, 老常量在某些发行版头文件已删除。
        // 用 env var 走 SDL_VIDEODRIVER 兜底, 跨版本都生效 (SDL_Init 时会读取)。
        setenv("SDL_VIDEODRIVER", driver, 1);
        
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            Logger::GetInstance().Log("[RoiVisualizer] SDL_Init with " + std::string(driver) + " failed: " + std::string(SDL_GetError()));
            continue;
        }
        
        if (strcmp(driver, "dummy") == 0) {
            Logger::GetInstance().Log("[RoiVisualizer] Using dummy driver - no display output");
            initialized_ = true;
            return true;
        }
        
        window_ = SDL_CreateWindow("2K Stitch ROI Tuner",
                                    SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                    DISPLAY_DEFAULT_W, DISPLAY_DEFAULT_H,
                                    SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!window_) {
            Logger::GetInstance().Log("[RoiVisualizer] SDL_CreateWindow with " + std::string(driver) + " failed: " + std::string(SDL_GetError()));
            SDL_Quit();
            continue;
        }
        
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
        if (!renderer_) {
            Logger::GetInstance().Log("[RoiVisualizer] SDL_CreateRenderer failed, trying software: " + std::string(SDL_GetError()));
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!renderer_) {
            Logger::GetInstance().Log("[RoiVisualizer] SDL_CreateRenderer failed completely: " + std::string(SDL_GetError()));
            SDL_DestroyWindow(window_);
            SDL_Quit();
            window_ = nullptr;
            continue;
        }
        
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_BGR24,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      panorama_width, panorama_height);
        if (!texture_) {
            Logger::GetInstance().Log("[RoiVisualizer] SDL_CreateTexture failed: " + std::string(SDL_GetError()));
            SDL_DestroyRenderer(renderer_);
            SDL_DestroyWindow(window_);
            SDL_Quit();
            renderer_ = nullptr;
            window_ = nullptr;
            continue;
        }
        
        SDL_GetWindowSize(window_, &display_w_, &display_h_);
        
        const char* actual_driver = SDL_GetCurrentVideoDriver();
        Logger::GetInstance().Log("[RoiVisualizer] SDL2 window created with driver: " + 
                                   (actual_driver ? std::string(actual_driver) : "unknown") +
                                   " size: " + std::to_string(display_w_) + "x" + std::to_string(display_h_));
        
        initialized_ = true;
        return true;
    }
    
    Logger::GetInstance().Log("[RoiVisualizer] All video drivers failed, cannot create window");
    return false;
}

void RoiVisualizer::Shutdown() {
    if (!initialized_) return;
    
    if (texture_) SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
    
    texture_ = nullptr;
    renderer_ = nullptr;
    window_ = nullptr;
    initialized_ = false;
}

void RoiVisualizer::FillRect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    SDL_SetRenderDrawColor(renderer_, r, g, b, 255);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(renderer_, &rect);
}

void RoiVisualizer::DrawRectOutline(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, int thickness) {
    SDL_SetRenderDrawColor(renderer_, r, g, b, 255);
    for (int t = 0; t < thickness; ++t) {
        SDL_Rect top = {x + t, y + t, w - 2*t, 1};
        SDL_Rect bottom = {x + t, y + h - t - 1, w - 2*t, 1};
        SDL_Rect left = {x + t, y + t, 1, h - 2*t};
        SDL_Rect right = {x + w - t - 1, y + t, 1, h - 2*t};
        SDL_RenderFillRect(renderer_, &top);
        SDL_RenderFillRect(renderer_, &bottom);
        SDL_RenderFillRect(renderer_, &left);
        SDL_RenderFillRect(renderer_, &right);
    }
}

void RoiVisualizer::DrawChar(int x, int y, char c, uint8_t r, uint8_t g, uint8_t b) {
    int idx = CharToFontIndex(c);
    const uint8_t* bits = GetFontBits(idx);
    if (!bits) return;
    
    for (int row = 0; row < FONT_H; ++row) {
        for (int col = 0; col < FONT_W; ++col) {
            if (bits[row * FONT_W + col]) {
                FillRect(x + col * FONT_SCALE, y + row * FONT_SCALE,
                         FONT_SCALE, FONT_SCALE, r, g, b);
            }
        }
    }
}

void RoiVisualizer::DrawText(int x, int y, const std::string& text, uint8_t r, uint8_t g, uint8_t b) {
    int cursor_x = x;
    int char_w = (FONT_W + CHAR_SPACING) * FONT_SCALE;
    
    for (char c : text) {
        DrawChar(cursor_x, y, c, r, g, b);
        cursor_x += char_w;
    }
}

VisAction RoiVisualizer::ShowStreamingFrame(const uint8_t* bgr_data,
                                             int width, int height,
                                             int stride, double fps) {
    if (!initialized_ || !bgr_data) return kVisNone;
    
    if (!renderer_ || !texture_) {
        return PollEvents(false);
    }
    
    int tex_stride = 0;
    uint8_t* tex_pixels = nullptr;
    SDL_LockTexture(texture_, nullptr, (void**)&tex_pixels, &tex_stride);
    
    const uint8_t* src = bgr_data;
    for (int row = 0; row < height; ++row) {
        memcpy(tex_pixels + row * tex_stride, src + row * stride, width * 3);
    }
    SDL_UnlockTexture(texture_);
    
    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
    
    DrawFpsOverlay(fps);
    
    SDL_RenderPresent(renderer_);
    
    return PollEvents(false);
}

VisAction RoiVisualizer::ShowDebugFrame(const uint8_t* bgr_data,
                                        int width, int height,
                                        int stride, double fps,
                                        const std::vector<StitchTask>& tasks) {
    if (!initialized_ || !bgr_data) return kVisNone;
    
    if (!renderer_ || !texture_) {
        return PollEvents(true);
    }
    
    int tex_stride = 0;
    uint8_t* tex_pixels = nullptr;
    SDL_LockTexture(texture_, nullptr, (void**)&tex_pixels, &tex_stride);
    
    const uint8_t* src = bgr_data;
    for (int row = 0; row < height; ++row) {
        memcpy(tex_pixels + row * tex_stride, src + row * stride, width * 3);
    }
    SDL_UnlockTexture(texture_);
    
    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
    
    if (g_show_roi_markers) DrawROIMarkers(tasks);
    DrawFpsOverlay(fps);
    DrawControlInfo();
    DrawFeatherSaveInfo();
    
    SDL_RenderPresent(renderer_);
    
    return PollEvents(true);
}

void RoiVisualizer::DrawROIMarkers(const std::vector<StitchTask>& tasks) {
    // v3.x (2026-07-09): 用 num_cams_ 决定画多少个 cam 框, 兼容 2x2 (4) 和 2x3 (6).
    //   边界用 std::min 三重兜底: 不超过 num_cams_, 不超过 tasks.size(), 不超过颜色表.
    const int n = std::min({num_cams_,
                            static_cast<int>(tasks.size()),
                            static_cast<int>(sizeof(CAM_COLORS_BGR) / sizeof(CAM_COLORS_BGR[0]))});
    if (n <= 0) return;

    for (int i = 0; i < n; ++i) {
        if (!tasks[i].enabled) continue;

        bool rotated = (tasks[i].rotation_deg == 90 || tasks[i].rotation_deg == 270);
        int w = rotated ? tasks[i].src_h : tasks[i].src_w;
        int h = rotated ? tasks[i].src_w : tasks[i].src_h;

        int sx = ScaleX(tasks[i].dst_x);
        int sy = ScaleY(tasks[i].dst_y);
        int sw = ScaleW(w);
        int sh = ScaleH(h);

        uint8_t r = CAM_COLORS_BGR[i][0];
        uint8_t g = CAM_COLORS_BGR[i][1];
        uint8_t b = CAM_COLORS_BGR[i][2];

        int thickness = (i == g_config.selected_cam) ? 3 : 2;
        DrawRectOutline(sx, sy, sw, sh, r, g, b, thickness);

        DrawText(sx + 5, sy + 5, CAM_LABELS[i], r, g, b);

        if (i == g_config.selected_cam) {
            DrawText(sx + 5, sy + 5 + (FONT_H + 1) * FONT_SCALE, "[SEL]", r, g, b);
        }
    }
}

void RoiVisualizer::DrawFpsOverlay(double fps) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << "FPS:" << fps;
    DrawText(10, 10, oss.str(), 255, 255, 255);
}

void RoiVisualizer::DrawControlInfo() {
    int cam = g_config.selected_cam;
    const CameraRoiRect& r = g_config.camera_rois[cam];

    std::ostringstream oss;
    // v3.x (2026-07-09): 显示完整 ROI 坐标 (x, y, w, h) — yaml 里就是这四个字段.
    oss << CAM_POSITIONS[cam]
        << " x:" << r.x << " y:" << r.y
        << " w:" << r.width << " h:" << r.height
        << " St:" << g_config.step_size;

    int text_h = (FONT_H + 2) * FONT_SCALE;
    int bar_h = (text_h + 4) * 2;
    int y_pos = display_h_ - bar_h - (FONT_H + 2) * FONT_SCALE - 5;

    FillRect(0, y_pos, display_w_, bar_h, 40, 40, 40);
    DrawText(10, y_pos + 4, oss.str(), 255, 255, 255);
    // v3.x (2026-07-09): 简短按键提示. 方向键挪位置, shift+方向键改尺寸;
    //   R 重新抓当前帧 (debug 里画面"暂停"是 by design, 调参需要稳定参考).
    DrawText(10, y_pos + 4 + text_h + 2,
             "Arrows:move  Shift+Arrows:resize  Tab:cam  R:refresh  E:save  Q:quit",
             180, 180, 180);
}

void RoiVisualizer::DrawFeatherSaveInfo() {
    std::ostringstream oss;
    oss << "F:" << (g_config.feather_enabled ? "ON" : "OFF")
        << " W:" << g_config.feather_width
        << " S:" << (g_config.save_enabled ? "ON" : "OFF")
        << " I:" << g_config.save_interval;
    
    int text_h = (FONT_H + 2) * FONT_SCALE;
    int bar_h = text_h + 10;
    int y_pos = display_h_ - bar_h;
    
    FillRect(0, y_pos, display_w_, bar_h, 40, 40, 40);
    DrawText(10, y_pos + 5, oss.str(), 255, 255, 255);
}

VisAction RoiVisualizer::PollEvents(bool debug_mode) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            return kVisExitDebug;
        }
        if (event.type == SDL_KEYDOWN) {
            // v3.x (2026-07-09): shift 状态透传到 ParseKey, 让方向键 + shift 切到 resize 模式.
            const bool shift = (event.key.keysym.mod & KMOD_SHIFT) != 0;
            VisAction action = ParseKey(event.key.keysym.sym, shift);

            if (!debug_mode) {
                if (action == kVisEnterDebug) return kVisEnterDebug;
                return kVisNone;
            }

            if (action != kVisNone) {
                return HandleDebugAction(action);
            }
        }
        if (event.type == SDL_WINDOWEVENT) {
            if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                display_w_ = event.window.data1;
                display_h_ = event.window.data2;
            }
        }
    }
    return kVisNone;
}

VisAction RoiVisualizer::ParseKey(SDL_Keycode key, bool shift) {
    switch (key) {
        // v3.x (2026-07-09): shift + 方向键改 w/h, 否则挪 x/y
        case SDLK_UP:    return shift ? kVisResizeUp    : kVisStepUp;
        case SDLK_DOWN:  return shift ? kVisResizeDown  : kVisStepDown;
        case SDLK_LEFT:  return shift ? kVisResizeLeft  : kVisStepLeft;
        case SDLK_RIGHT: return shift ? kVisResizeRight : kVisStepRight;
        case SDLK_w:     return shift ? kVisResizeUp    : kVisStepUp;
        case SDLK_s:     return shift ? kVisResizeDown  : kVisStepDown;
        case SDLK_a:     return shift ? kVisResizeLeft  : kVisStepLeft;
        case SDLK_d:     return shift ? kVisResizeRight : kVisStepRight;
        case SDLK_TAB:   return kVisNextCam;
        case SDLK_1:     return kVisStepSize1;
        case SDLK_5:     return kVisStepSize5;
        case SDLK_0:     return kVisStepSize10;
        case SDLK_p:     return kVisStepSize50;
        case SDLK_m:     return kVisToggleMarkers;
        case SDLK_q:     return kVisExitDebug;
        case SDLK_ESCAPE:return kVisExitDebug;
        case SDLK_e:     return kVisSaveConfig;
        case SDLK_r:     return kVisRefreshFrames;  // v3.x: debug 模式下重新抓帧
        case SDLK_f:     return kVisFeatherToggle;
        case SDLK_PLUS:  
        case SDLK_EQUALS:return kVisFeatherWidthUp;
        case SDLK_MINUS: return kVisFeatherWidthDown;
        case SDLK_b:     return kVisSaveToggle;
        case SDLK_l:     return kVisSaveIntervalUp;
        case SDLK_k:     return kVisSaveIntervalDown;
        case SDLK_RETURN:return kVisEnterDebug;
        default:         return kVisNone;
    }
}

VisAction RoiVisualizer::HandleDebugAction(VisAction action) {
    int cam = g_config.selected_cam;

    switch (action) {
        case kVisNextCam:
            // v3.x (2026-07-09): 用 num_cams_ (4 for 2x2, 6 for 2x3) 而不是硬编码 4.
            //   同时兜底 selected_cam 防止 yaml / 视觉器外部把 cam 写到 5 但模式是 2x2 的情况.
            g_config.selected_cam = (g_config.selected_cam + 1) % num_cams_;
            return kVisNone;
        case kVisToggleMarkers:
            g_show_roi_markers = !g_show_roi_markers;
            return kVisNone;
        case kVisStepSize1:
            g_config.step_size = 1;
            return kVisNone;
        case kVisStepSize5:
            g_config.step_size = 5;
            return kVisNone;
        case kVisStepSize10:
            g_config.step_size = 10;
            return kVisNone;
        case kVisStepSize50:
            g_config.step_size = 50;
            return kVisNone;
        case kVisStepUp:
            // v3.x (2026-07-09): 直接改 g_config.camera_rois[i] 的 (x, y), BuildCameraRois
            //   下次调用就读到新值. 偶数对齐交给 NormalizeEvenFloor.
            g_config.camera_rois[cam].y -= g_config.step_size;
            return kVisNeedRestitch;
        case kVisStepDown:
            g_config.camera_rois[cam].y += g_config.step_size;
            return kVisNeedRestitch;
        case kVisStepLeft:
            g_config.camera_rois[cam].x -= g_config.step_size;
            return kVisNeedRestitch;
        case kVisStepRight:
            g_config.camera_rois[cam].x += g_config.step_size;
            return kVisNeedRestitch;
        case kVisResizeUp:
            // shift+方向键 — 缩 w/h. 最小 2 像素保底, 不然 BuildCameraRois 抛 runtime_error.
            g_config.camera_rois[cam].height =
                std::max(2, g_config.camera_rois[cam].height - g_config.step_size);
            return kVisNeedRestitch;
        case kVisResizeDown:
            g_config.camera_rois[cam].height += g_config.step_size;
            return kVisNeedRestitch;
        case kVisResizeLeft:
            g_config.camera_rois[cam].width =
                std::max(2, g_config.camera_rois[cam].width - g_config.step_size);
            return kVisNeedRestitch;
        case kVisResizeRight:
            g_config.camera_rois[cam].width += g_config.step_size;
            return kVisNeedRestitch;
        case kVisFeatherToggle:
            g_config.feather_enabled = !g_config.feather_enabled;
            return kVisNeedRebuild;
        case kVisFeatherWidthUp:
            g_config.feather_width = NormalizeEvenFloor(
                std::max(20, g_config.feather_width + 10));
            return kVisNeedRebuild;
        case kVisFeatherWidthDown:
            g_config.feather_width = NormalizeEvenFloor(
                std::max(20, g_config.feather_width - 10));
            return kVisNeedRebuild;
        case kVisRefreshFrames:
            // v3.x (2026-07-09): 透传, 由 caller (App) 拉新帧 + SaveCurrentFrames.
            //   这里不直接动数据是因为 get_image_vector 涉及 6 路解码器, 必须 caller 触发.
            return kVisRefreshFrames;
        case kVisSaveToggle:
            g_config.save_enabled = !g_config.save_enabled;
            return kVisNone;
        case kVisSaveIntervalUp:
            g_config.save_interval = std::max(1, g_config.save_interval + 10);
            return kVisNone;
        case kVisSaveIntervalDown:
            g_config.save_interval = std::max(1, g_config.save_interval - 10);
            return kVisNone;
        default:
            return action;
    }
}
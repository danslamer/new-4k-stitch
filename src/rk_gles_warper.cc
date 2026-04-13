#include "rk_gles_warper.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>
#include <sstream>
#include <string>
#include <vector>

#if __has_include(<drm/drm_fourcc.h>)
#include <drm/drm_fourcc.h>
#elif __has_include(<libdrm/drm_fourcc.h>)
#include <libdrm/drm_fourcc.h>
#else
#error "drm_fourcc.h not found. Please install libdrm development headers."
#endif

#include "logger.h"

namespace {

static int g_debug_level = -1;

constexpr const char* kRequiredEglExtensions[] = {
    "EGL_KHR_image_base",
    "EGL_KHR_gl_texture_2D_image",
    "EGL_EXT_image_dma_buf_import",
};

constexpr const char* kImportantClientExtensions[] = {
    "EGL_EXT_platform_base",
    "EGL_KHR_platform_gbm",
    "EGL_EXT_platform_x11",
    "EGL_KHR_platform_surfaceless",
    "EGL_MESA_platform_surfaceless",
};

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

using GetPlatformDisplayExtProc = EGLDisplay (*)(EGLenum, void*, const EGLint*);

int GetDebugLevel() {
    if (g_debug_level >= 0) {
        return g_debug_level;
    }

    const char* env = std::getenv("STITCH_DEBUG_LEVEL");
    if (env == nullptr || *env == '\0') {
        env = std::getenv("RK_GLES_WARPER_DEBUG_LEVEL");
    }
    g_debug_level = env == nullptr ? 0 : std::max(0, std::atoi(env));
    return g_debug_level;
}

bool IsDebugEnabled(int level) {
    return GetDebugLevel() >= level;
}

std::string GetEnvValue(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return "<unset>";
    }
    return value;
}

std::string GetEglErrorString(EGLint error) {
    switch (error) {
        case EGL_SUCCESS: return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
        case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
        case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
        case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
        default: break;
    }
    return "0x" + cv::format("%x", static_cast<unsigned int>(error));
}

std::string GetGlErrorString(GLenum error) {
    switch (error) {
        case GL_NO_ERROR: return "GL_NO_ERROR";
        case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
        default: break;
    }
    return "0x" + cv::format("%x", static_cast<unsigned int>(error));
}

void LogEnvironmentContext(const char* stage) {
    if (!IsDebugEnabled(1)) {
        return;
    }

    Logger::GetInstance().Log(
        std::string("[RkGlesWarper] ") + stage +
        " env DISPLAY=" + GetEnvValue("DISPLAY") +
        " WAYLAND_DISPLAY=" + GetEnvValue("WAYLAND_DISPLAY") +
        " XDG_RUNTIME_DIR=" + GetEnvValue("XDG_RUNTIME_DIR") +
        " EGL_PLATFORM=" + GetEnvValue("EGL_PLATFORM"));
}

bool HasEglExtension(const char* extensions, const char* target) {
    if (extensions == nullptr || target == nullptr) {
        return false;
    }
    const std::string haystack(extensions);
    const std::string needle(target);
    size_t pos = 0;
    while (true) {
        pos = haystack.find(needle, pos);
        if (pos == std::string::npos) {
            return false;
        }
        const bool left_ok = (pos == 0) || haystack[pos - 1] == ' ';
        const size_t right = pos + needle.size();
        const bool right_ok = (right >= haystack.size()) || haystack[right] == ' ';
        if (left_ok && right_ok) {
            return true;
        }
        pos = right;
    }
}

void LogExtensionSummary(const char* title, const char* extensions) {
    std::ostringstream stream;
    stream << "[RkGlesWarper] " << title << "=" << (extensions == nullptr ? "<null>" : extensions);
    Logger::GetInstance().Log(stream.str());

    if (extensions == nullptr) {
        return;
    }

    for (const char* required : kRequiredEglExtensions) {
        Logger::GetInstance().Log(
            std::string("[RkGlesWarper] ") + title + " support " + required +
            "=" + (HasEglExtension(extensions, required) ? "yes" : "no"));
    }
}

void LogClientExtensionSummary(const char* extensions) {
    std::ostringstream stream;
    stream << "[RkGlesWarper] EGL_CLIENT_EXTENSIONS="
           << (extensions == nullptr ? "<null>" : extensions);
    Logger::GetInstance().Log(stream.str());

    if (extensions == nullptr) {
        return;
    }

    for (const char* required : kImportantClientExtensions) {
        Logger::GetInstance().Log(
            std::string("[RkGlesWarper] EGL_CLIENT_EXTENSIONS support ") + required +
            "=" + (HasEglExtension(extensions, required) ? "yes" : "no"));
    }
}

void LogDisplayStrings(EGLDisplay display, const char* stage) {
    const char* vendor = eglQueryString(display, EGL_VENDOR);
    const char* version = eglQueryString(display, EGL_VERSION);
    const char* client_apis = eglQueryString(display, EGL_CLIENT_APIS);
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);

    Logger::GetInstance().Log(
        std::string("[RkGlesWarper] ") + stage + " EGL_VENDOR=" +
        (vendor == nullptr ? "<null>" : vendor));
    Logger::GetInstance().Log(
        std::string("[RkGlesWarper] ") + stage + " EGL_VERSION=" +
        (version == nullptr ? "<null>" : version));
    Logger::GetInstance().Log(
        std::string("[RkGlesWarper] ") + stage + " EGL_CLIENT_APIS=" +
        (client_apis == nullptr ? "<null>" : client_apis));

    LogExtensionSummary("EGL_EXTENSIONS", extensions);
}

bool OpenFirstAvailableRenderNode(int* fd_out) {
    if (fd_out == nullptr) {
        return false;
    }

    const char* candidates[] = {
        "/dev/dri/renderD128",
        "/dev/dri/renderD129",
        "/dev/dri/card0",
        "/dev/dri/card1",
    };

    for (const char* path : candidates) {
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            *fd_out = fd;
            Logger::GetInstance().Log(
                std::string("[RkGlesWarper] opened DRM device node: ") + path);
            return true;
        }
    }

    return false;
}

void LogGlStrings(const char* stage) {
    const GLubyte* vendor = glGetString(GL_VENDOR);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* version = glGetString(GL_VERSION);
    const GLubyte* gl_extensions = glGetString(GL_EXTENSIONS);

    Logger::GetInstance().Log(
        std::string("[RkGlesWarper] ") + stage + " GL_VENDOR=" +
        (vendor == nullptr ? "<null>" : reinterpret_cast<const char*>(vendor)));
    Logger::GetInstance().Log(
        std::string("[RkGlesWarper] ") + stage + " GL_RENDERER=" +
        (renderer == nullptr ? "<null>" : reinterpret_cast<const char*>(renderer)));
    Logger::GetInstance().Log(
        std::string("[RkGlesWarper] ") + stage + " GL_VERSION=" +
        (version == nullptr ? "<null>" : reinterpret_cast<const char*>(version)));

    if (IsDebugEnabled(2)) {
        Logger::GetInstance().Log(
            std::string("[RkGlesWarper] ") + stage + " GL_EXTENSIONS=" +
            (gl_extensions == nullptr ? "<null>" : reinterpret_cast<const char*>(gl_extensions)));
    }
}

void LogEglFailure(const char* stage) {
    const EGLint error = eglGetError();
    Logger::GetInstance().LogError(
        std::string("[RkGlesWarper] ") + stage + " failed, eglError=" +
        GetEglErrorString(error));
}

void LogGlFailure(const char* stage) {
    const GLenum error = glGetError();
    Logger::GetInstance().LogError(
        std::string("[RkGlesWarper] ") + stage + " failed, glError=" +
        GetGlErrorString(error));
}

EGLDisplay TryAcquireDisplay(bool* used_surfaceless_platform,
                             bool* used_gbm_platform,
                             gbm_device** gbm_device_out,
                             int* gbm_fd_out) {
    if (used_surfaceless_platform != nullptr) {
        *used_surfaceless_platform = false;
    }
    if (used_gbm_platform != nullptr) {
        *used_gbm_platform = false;
    }
    if (gbm_device_out != nullptr) {
        *gbm_device_out = nullptr;
    }
    if (gbm_fd_out != nullptr) {
        *gbm_fd_out = -1;
    }

    const char* client_extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    LogClientExtensionSummary(client_extensions);
    const auto get_platform_display = reinterpret_cast<GetPlatformDisplayExtProc>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (get_platform_display != nullptr &&
        HasEglExtension(client_extensions, "EGL_MESA_platform_surfaceless")) {
        EGLDisplay display = get_platform_display(
            EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (display != EGL_NO_DISPLAY) {
            if (used_surfaceless_platform != nullptr) {
                *used_surfaceless_platform = true;
            }
            Logger::GetInstance().Log(
                "[RkGlesWarper] using EGL_PLATFORM_SURFACELESS_MESA display path.");
            return display;
        }
        Logger::GetInstance().LogError(
            "[RkGlesWarper] EGL_PLATFORM_SURFACELESS_MESA display path unavailable, fallback to EGL_DEFAULT_DISPLAY.");
    }

    if (get_platform_display != nullptr &&
        HasEglExtension(client_extensions, "EGL_KHR_platform_gbm")) {
        int gbm_fd = -1;
        if (OpenFirstAvailableRenderNode(&gbm_fd)) {
            gbm_device* device = gbm_create_device(gbm_fd);
            if (device != nullptr) {
                EGLDisplay display = get_platform_display(
                    EGL_PLATFORM_GBM_KHR, device, nullptr);
                if (display != EGL_NO_DISPLAY) {
                    if (used_gbm_platform != nullptr) {
                        *used_gbm_platform = true;
                    }
                    if (gbm_device_out != nullptr) {
                        *gbm_device_out = device;
                    }
                    if (gbm_fd_out != nullptr) {
                        *gbm_fd_out = gbm_fd;
                    }
                    Logger::GetInstance().Log(
                        "[RkGlesWarper] using EGL_PLATFORM_GBM_KHR display path.");
                    return display;
                }
                gbm_device_destroy(device);
            }
            if (gbm_fd >= 0) {
                close(gbm_fd);
            }
            Logger::GetInstance().LogError(
                "[RkGlesWarper] EGL_PLATFORM_GBM_KHR display path unavailable, fallback to EGL_DEFAULT_DISPLAY.");
        } else {
            Logger::GetInstance().LogError(
                "[RkGlesWarper] no DRM render node available for GBM display initialization.");
        }
    }

    return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

bool ChooseConfig(EGLDisplay display, EGLConfig* config) {
    if (config == nullptr) {
        return false;
    }

    const EGLint renderable_bits = EGL_OPENGL_ES3_BIT;
    const EGLint config_candidates[][13] = {
        {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, renderable_bits,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE
        },
        {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, renderable_bits,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 0,
            EGL_NONE
        }
    };

    for (const auto& candidate : config_candidates) {
        EGLint num_configs = 0;
        EGLConfig chosen = nullptr;
        if (eglChooseConfig(display, candidate, &chosen, 1, &num_configs) == EGL_TRUE &&
            num_configs > 0 && chosen != nullptr) {
            *config = chosen;
            return true;
        }
    }

    return false;
}

void LogConfigDetails(EGLDisplay display, EGLConfig config) {
    EGLint red = 0;
    EGLint green = 0;
    EGLint blue = 0;
    EGLint alpha = 0;
    EGLint surface_type = 0;
    EGLint renderable_type = 0;
    eglGetConfigAttrib(display, config, EGL_RED_SIZE, &red);
    eglGetConfigAttrib(display, config, EGL_GREEN_SIZE, &green);
    eglGetConfigAttrib(display, config, EGL_BLUE_SIZE, &blue);
    eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE, &alpha);
    eglGetConfigAttrib(display, config, EGL_SURFACE_TYPE, &surface_type);
    eglGetConfigAttrib(display, config, EGL_RENDERABLE_TYPE, &renderable_type);

    std::ostringstream stream;
    stream << "[RkGlesWarper] EGL config RGBA=" << red << "," << green << "," << blue << "," << alpha
           << " surface_type=0x" << std::hex << surface_type
           << " renderable_type=0x" << std::hex << renderable_type;
    Logger::GetInstance().Log(stream.str());
}

constexpr char kVertexShaderSource[] = R"GLSL(#version 300 es
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
)GLSL";

constexpr char kFragmentShaderSource[] = R"GLSL(#version 300 es
precision highp float;
in vec2 vTexCoord;
uniform sampler2D uSourceTexture;
uniform sampler2D uMapTexture;
uniform vec2 uSourceSize;
uniform int uUvPlane;
layout(location = 0) out vec4 outColor;
void main() {
    vec2 srcCoord = texture(uMapTexture, vTexCoord).rg;
    if (srcCoord.x < 0.0 || srcCoord.y < 0.0 ||
        srcCoord.x >= uSourceSize.x || srcCoord.y >= uSourceSize.y) {
        outColor = vec4(0.0);
        return;
    }

    vec2 sampleCoord = (srcCoord + vec2(0.5, 0.5)) / uSourceSize;
    vec4 sampleValue = texture(uSourceTexture, sampleCoord);
    if (uUvPlane != 0) {
        outColor = vec4(sampleValue.rg, 0.0, 1.0);
    } else {
        outColor = vec4(sampleValue.r, 0.0, 0.0, 1.0);
    }
}
)GLSL";

cv::Mat BuildPackedMap(const cv::Mat& map_x, const cv::Mat& map_y) {
    cv::Mat packed(map_x.size(), CV_32FC2, cv::Scalar(-1.0f, -1.0f));
    for (int y = 0; y < map_x.rows; ++y) {
        const float* src_x = map_x.ptr<float>(y);
        const float* src_y = map_y.ptr<float>(y);
        cv::Vec2f* dst = packed.ptr<cv::Vec2f>(y);
        for (int x = 0; x < map_x.cols; ++x) {
            dst[x][0] = src_x[x];
            dst[x][1] = src_y[x];
        }
    }
    return packed;
}

cv::Mat BuildUvPackedMap(const cv::Mat& map_x, const cv::Mat& map_y) {
    const int uv_rows = std::max(1, map_x.rows / 2);
    const int uv_cols = std::max(1, map_x.cols / 2);
    cv::Mat packed(uv_rows, uv_cols, CV_32FC2, cv::Scalar(-1.0f, -1.0f));
    for (int y = 0; y < uv_rows; ++y) {
        cv::Vec2f* dst = packed.ptr<cv::Vec2f>(y);
        for (int x = 0; x < uv_cols; ++x) {
            const int src_x_index = std::min(map_x.cols - 1, x * 2);
            const int src_y_index = std::min(map_x.rows - 1, y * 2);
            const float source_x = map_x.at<float>(src_y_index, src_x_index);
            const float source_y = map_y.at<float>(src_y_index, src_x_index);
            if (source_x < 0.0f || source_y < 0.0f) {
                dst[x] = cv::Vec2f(-1.0f, -1.0f);
            } else {
                dst[x] = cv::Vec2f(source_x * 0.5f, source_y * 0.5f);
            }
        }
    }
    return packed;
}

std::string GlErrorMessage(const char* stage, GLenum error) {
    return std::string("[RkGlesWarper] ") + stage + " GL error=0x" +
           cv::format("%x", static_cast<unsigned int>(error));
}

}  // namespace

RkGlesWarper::RkGlesWarper() = default;

RkGlesWarper::~RkGlesWarper() {
    Shutdown();
}

bool RkGlesWarper::Initialize(const std::vector<WarpMapEntry>& warp_entries,
                              int input_width,
                              int input_height) {
    Shutdown();
    if (warp_entries.empty() || input_width <= 0 || input_height <= 0) {
        return false;
    }
    if (!InitializeEgl() || !InitializeShaders()) {
        Shutdown();
        return false;
    }
    if (!InitializeWarpPrograms(warp_entries, input_width, input_height)) {
        Shutdown();
        return false;
    }
    return true;
}

void RkGlesWarper::Shutdown() {
    for (size_t i = 0; i < warp_programs_.size(); ++i) {
        if (warp_programs_[i].y_map_texture != 0) {
            glDeleteTextures(1, &warp_programs_[i].y_map_texture);
        }
        if (warp_programs_[i].uv_map_texture != 0) {
            glDeleteTextures(1, &warp_programs_[i].uv_map_texture);
        }
    }
    warp_programs_.clear();

    for (auto& item : input_cache_) {
        ReleaseImportedBuffer(&item.second);
    }
    input_cache_.clear();
    for (auto& item : output_cache_) {
        ReleaseImportedBuffer(&item.second);
    }
    output_cache_.clear();

    if (quad_vbo_ != 0) {
        glDeleteBuffers(1, &quad_vbo_);
        quad_vbo_ = 0;
    }
    if (quad_vao_ != 0) {
        glDeleteVertexArrays(1, &quad_vao_);
        quad_vao_ = 0;
    }
    if (shader_program_ != 0) {
        glDeleteProgram(shader_program_);
        shader_program_ = 0;
    }

    if (egl_display_ != EGL_NO_DISPLAY) {
        if (egl_context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(egl_display_, egl_context_);
        }
        if (egl_surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(egl_display_, egl_surface_);
        }
        eglTerminate(egl_display_);
    }

    if (gbm_device_ != nullptr) {
        gbm_device_destroy(gbm_device_);
        gbm_device_ = nullptr;
    }
    if (gbm_fd_ >= 0) {
        close(gbm_fd_);
        gbm_fd_ = -1;
    }

    egl_display_ = EGL_NO_DISPLAY;
    egl_context_ = EGL_NO_CONTEXT;
    egl_surface_ = EGL_NO_SURFACE;
}

bool RkGlesWarper::IsReady() const {
    return egl_display_ != EGL_NO_DISPLAY &&
           egl_context_ != EGL_NO_CONTEXT &&
           shader_program_ != 0 &&
           !warp_programs_.empty();
}

bool RkGlesWarper::InitializeEgl() {
    LogEnvironmentContext("InitializeEgl");

    bool used_surfaceless_display = false;
    bool used_gbm_display = false;
    egl_display_ = TryAcquireDisplay(&used_surfaceless_display,
                                     &used_gbm_display,
                                     &gbm_device_,
                                     &gbm_fd_);
    if (egl_display_ == EGL_NO_DISPLAY) {
        LogEglFailure("eglGetDisplay/eglGetPlatformDisplayEXT");
        return false;
    }
    if (!eglInitialize(egl_display_, nullptr, nullptr)) {
        LogEglFailure("eglInitialize");
        return false;
    }

    LogDisplayStrings(egl_display_, used_surfaceless_display ? "surfaceless" : "default");
    if (IsDebugEnabled(2)) {
        Logger::GetInstance().Log("[RkGlesWarper] eglInitialize succeeded.");
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        LogEglFailure("eglBindAPI(EGL_OPENGL_ES_API)");
        return false;
    }

    EGLConfig config = nullptr;
    if (!ChooseConfig(egl_display_, &config)) {
        LogEglFailure("eglChooseConfig");
        return false;
    }
    LogConfigDetails(egl_display_, config);

    const bool use_surfaceless_context = HasEglExtension(
        eglQueryString(egl_display_, EGL_EXTENSIONS), "EGL_KHR_surfaceless_context");
    if (!use_surfaceless_context) {
        const EGLint pbuffer_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        egl_surface_ = eglCreatePbufferSurface(egl_display_, config, pbuffer_attribs);
        if (egl_surface_ == EGL_NO_SURFACE) {
            LogEglFailure("eglCreatePbufferSurface");
            return false;
        }
    } else {
        egl_surface_ = EGL_NO_SURFACE;
        Logger::GetInstance().Log("[RkGlesWarper] using EGL_KHR_surfaceless_context, no pbuffer surface created.");
    }

    const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    egl_context_ = eglCreateContext(egl_display_, config, EGL_NO_CONTEXT, context_attribs);
    if (egl_context_ == EGL_NO_CONTEXT) {
        LogEglFailure("eglCreateContext");
        return false;
    }

    if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_)) {
        LogEglFailure("eglMakeCurrent");
        return false;
    }

    if (IsDebugEnabled(1)) {
        LogGlStrings("InitializeEgl");
        Logger::GetInstance().Log(
            std::string("[RkGlesWarper] required extension check EGL_EXT_image_dma_buf_import=") +
            (HasEglExtension(eglQueryString(egl_display_, EGL_EXTENSIONS),
                             "EGL_EXT_image_dma_buf_import") ? "yes" : "no"));
        Logger::GetInstance().Log(
            std::string("[RkGlesWarper] display path used: surfaceless=") +
            (used_surfaceless_display ? "yes" : "no") +
            " gbm=" + (used_gbm_display ? "yes" : "no") +
            " default=" + ((used_surfaceless_display || used_gbm_display) ? "no" : "yes"));
        Logger::GetInstance().Log(
            std::string("[RkGlesWarper] context ready with EGL_KHR_surfaceless_context=") +
            (HasEglExtension(eglQueryString(egl_display_, EGL_EXTENSIONS),
                             "EGL_KHR_surfaceless_context") ? "yes" : "no") +
            " EGL_KHR_gl_texture_2D_image=" +
            (HasEglExtension(eglQueryString(egl_display_, EGL_EXTENSIONS),
                             "EGL_KHR_gl_texture_2D_image") ? "yes" : "no") +
            " EGL_EXT_image_dma_buf_import=" +
            (HasEglExtension(eglQueryString(egl_display_, EGL_EXTENSIONS),
                             "EGL_EXT_image_dma_buf_import") ? "yes" : "no"));
    }

    egl_create_image_khr_ =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    egl_destroy_image_khr_ =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    gl_egl_image_target_texture_2d_oes_ =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    if (egl_create_image_khr_ == nullptr ||
        egl_destroy_image_khr_ == nullptr ||
        gl_egl_image_target_texture_2d_oes_ == nullptr) {
        Logger::GetInstance().LogError(
            "[RkGlesWarper] required EGLImage/OES entry points are unavailable.");
        Logger::GetInstance().LogError(
            std::string("[RkGlesWarper] proc eglCreateImageKHR=") +
            (egl_create_image_khr_ == nullptr ? "null" : "ok") +
            " eglDestroyImageKHR=" + (egl_destroy_image_khr_ == nullptr ? "null" : "ok") +
            " glEGLImageTargetTexture2DOES=" +
            (gl_egl_image_target_texture_2d_oes_ == nullptr ? "null" : "ok"));
        return false;
    }

    if (IsDebugEnabled(1)) {
        Logger::GetInstance().Log(
            std::string("[RkGlesWarper] EGLImage entry points ready, surfaceless_context=") +
            (use_surfaceless_context ? "yes" : "no") +
            " surfaceless_display=" + (used_surfaceless_display ? "yes" : "no"));
    }

    return true;
}

GLuint RkGlesWarper::CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) {
        return shader;
    }

    GLint log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    std::string log(static_cast<size_t>(std::max(0, log_length)), '\0');
    if (log_length > 0) {
        glGetShaderInfoLog(shader, log_length, nullptr, &log[0]);
    }
    Logger::GetInstance().LogError(
        "[RkGlesWarper] shader compile failed: " + log);
    LogGlFailure("glCompileShader");
    glDeleteShader(shader);
    return 0;
}

bool RkGlesWarper::InitializeShaders() {
    const GLuint vertex_shader = CompileShader(GL_VERTEX_SHADER, kVertexShaderSource);
    const GLuint fragment_shader = CompileShader(GL_FRAGMENT_SHADER, kFragmentShaderSource);
    if (vertex_shader == 0 || fragment_shader == 0) {
        if (vertex_shader != 0) {
            glDeleteShader(vertex_shader);
        }
        if (fragment_shader != 0) {
            glDeleteShader(fragment_shader);
        }
        return false;
    }

    shader_program_ = glCreateProgram();
    glAttachShader(shader_program_, vertex_shader);
    glAttachShader(shader_program_, fragment_shader);
    glLinkProgram(shader_program_);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    GLint linked = GL_FALSE;
    glGetProgramiv(shader_program_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint log_length = 0;
        glGetProgramiv(shader_program_, GL_INFO_LOG_LENGTH, &log_length);
        std::string log(static_cast<size_t>(std::max(0, log_length)), '\0');
        if (log_length > 0) {
            glGetProgramInfoLog(shader_program_, log_length, nullptr, &log[0]);
        }
        Logger::GetInstance().LogError("[RkGlesWarper] program link failed: " + log);
        LogGlFailure("glLinkProgram");
        glDeleteProgram(shader_program_);
        shader_program_ = 0;
        return false;
    }

    source_texture_location_ = glGetUniformLocation(shader_program_, "uSourceTexture");
    map_texture_location_ = glGetUniformLocation(shader_program_, "uMapTexture");
    source_size_location_ = glGetUniformLocation(shader_program_, "uSourceSize");
    uv_plane_location_ = glGetUniformLocation(shader_program_, "uUvPlane");

    const GLfloat quad_vertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    glGenVertexArrays(1, &quad_vao_);
    glGenBuffers(1, &quad_vbo_);
    glBindVertexArray(quad_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    return true;
}

bool RkGlesWarper::CreateMapTexture(const cv::Mat& map_x,
                                    const cv::Mat& map_y,
                                    GLuint* texture,
                                    bool uv_plane) {
    if (texture == nullptr || map_x.empty() || map_y.empty()) {
        return false;
    }

    const cv::Mat packed = uv_plane ? BuildUvPackedMap(map_x, map_y)
                                    : BuildPackedMap(map_x, map_y);
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RG32F,
                 packed.cols,
                 packed.rows,
                 0,
                 GL_RG,
                 GL_FLOAT,
                 packed.ptr<float>());
    glBindTexture(GL_TEXTURE_2D, 0);
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        Logger::GetInstance().LogError(
            std::string("[RkGlesWarper] create map texture failed, uv_plane=") +
            (uv_plane ? "true" : "false") + " glError=" + GetGlErrorString(error));
        return false;
    }
    if (IsDebugEnabled(2)) {
        Logger::GetInstance().Log(
            std::string("[RkGlesWarper] map texture created, uv_plane=") +
            (uv_plane ? "true" : "false") +
            " size=" + std::to_string(packed.cols) + "x" + std::to_string(packed.rows));
    }
    return true;
}

bool RkGlesWarper::InitializeWarpPrograms(const std::vector<WarpMapEntry>& warp_entries,
                                          int,
                                          int) {
    warp_programs_.resize(warp_entries.size());
    for (size_t i = 0; i < warp_entries.size(); ++i) {
        if (!warp_entries[i].valid()) {
            continue;
        }
        WarpProgram& program = warp_programs_[i];
        program.output_width = warp_entries[i].xmap.cols;
        program.output_height = warp_entries[i].xmap.rows;
        program.uv_width = std::max(1, program.output_width / 2);
        program.uv_height = std::max(1, program.output_height / 2);
        if (!CreateMapTexture(warp_entries[i].xmap, warp_entries[i].ymap,
                              &program.y_map_texture, false) ||
            !CreateMapTexture(warp_entries[i].xmap, warp_entries[i].ymap,
                              &program.uv_map_texture, true)) {
            Logger::GetInstance().LogError("[RkGlesWarper] failed to create warp map textures.");
            return false;
        }
    }
    return true;
}

bool RkGlesWarper::ImportBuffer(const NV12Frame& frame, ImportedBuffer* imported) {
    if (imported == nullptr || frame.empty() || frame.fd < 0) {
        return false;
    }
    imported->fd = frame.fd;
    imported->width = frame.width;
    imported->height = frame.height;
    imported->stride = frame.stride_w > 0 ? frame.stride_w : frame.width;

    const EGLint y_attrs[] = {
        EGL_WIDTH, imported->width,
        EGL_HEIGHT, imported->height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT, imported->fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, imported->stride,
        EGL_NONE
    };
    const EGLint uv_attrs[] = {
        EGL_WIDTH, std::max(1, imported->width / 2),
        EGL_HEIGHT, std::max(1, imported->height / 2),
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_GR88,
        EGL_DMA_BUF_PLANE0_FD_EXT, imported->fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, imported->stride * imported->height,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, imported->stride,
        EGL_NONE
    };

    imported->y_plane.image = egl_create_image_khr_(
        egl_display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, y_attrs);
    imported->uv_plane.image = egl_create_image_khr_(
        egl_display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, uv_attrs);
    if (imported->y_plane.image == EGL_NO_IMAGE_KHR ||
        imported->uv_plane.image == EGL_NO_IMAGE_KHR) {
        ReleaseImportedBuffer(imported);
        return false;
    }

    glGenTextures(1, &imported->y_plane.texture);
    glBindTexture(GL_TEXTURE_2D, imported->y_plane.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl_egl_image_target_texture_2d_oes_(GL_TEXTURE_2D, imported->y_plane.image);

    glGenTextures(1, &imported->uv_plane.texture);
    glBindTexture(GL_TEXTURE_2D, imported->uv_plane.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl_egl_image_target_texture_2d_oes_(GL_TEXTURE_2D, imported->uv_plane.image);
    glBindTexture(GL_TEXTURE_2D, 0);

    return glGetError() == GL_NO_ERROR;
}

bool RkGlesWarper::ImportBuffer(const DrmBuffer& buffer, ImportedBuffer* imported) {
    NV12Frame frame;
    frame.fd = buffer.fd;
    frame.width = buffer.width;
    frame.height = buffer.height;
    frame.stride_w = static_cast<int>(buffer.pitch);
    frame.stride_h = buffer.height;
    return ImportBuffer(frame, imported);
}

void RkGlesWarper::ReleaseImportedBuffer(ImportedBuffer* imported) {
    if (imported == nullptr) {
        return;
    }
    if (imported->y_plane.texture != 0) {
        glDeleteTextures(1, &imported->y_plane.texture);
    }
    if (imported->uv_plane.texture != 0) {
        glDeleteTextures(1, &imported->uv_plane.texture);
    }
    if (imported->y_plane.image != EGL_NO_IMAGE_KHR && egl_destroy_image_khr_ != nullptr) {
        egl_destroy_image_khr_(egl_display_, imported->y_plane.image);
    }
    if (imported->uv_plane.image != EGL_NO_IMAGE_KHR && egl_destroy_image_khr_ != nullptr) {
        egl_destroy_image_khr_(egl_display_, imported->uv_plane.image);
    }
    *imported = ImportedBuffer{};
}

RkGlesWarper::ImportedBuffer* RkGlesWarper::GetOrCreateInputBuffer(const NV12Frame& frame) {
    auto it = input_cache_.find(frame.fd);
    if (it != input_cache_.end()) {
        const int stride = frame.stride_w > 0 ? frame.stride_w : frame.width;
        if (it->second.width == frame.width &&
            it->second.height == frame.height &&
            it->second.stride == stride) {
            return &it->second;
        }
        ReleaseImportedBuffer(&it->second);
        input_cache_.erase(it);
    }
    ImportedBuffer imported;
    if (!ImportBuffer(frame, &imported)) {
        Logger::GetInstance().LogError("[RkGlesWarper] failed to import input dma-buf.");
        return nullptr;
    }
    auto inserted = input_cache_.insert(std::make_pair(frame.fd, imported));
    return inserted.second ? &inserted.first->second : nullptr;
}

RkGlesWarper::ImportedBuffer* RkGlesWarper::GetOrCreateOutputBuffer(const DrmBuffer& buffer) {
    auto it = output_cache_.find(buffer.fd);
    if (it != output_cache_.end()) {
        const int stride = static_cast<int>(buffer.pitch);
        if (it->second.width == buffer.width &&
            it->second.height == buffer.height &&
            it->second.stride == stride) {
            return &it->second;
        }
        ReleaseImportedBuffer(&it->second);
        output_cache_.erase(it);
    }
    ImportedBuffer imported;
    if (!ImportBuffer(buffer, &imported)) {
        Logger::GetInstance().LogError("[RkGlesWarper] failed to import output dma-buf.");
        return nullptr;
    }
    auto inserted = output_cache_.insert(std::make_pair(buffer.fd, imported));
    return inserted.second ? &inserted.first->second : nullptr;
}

bool RkGlesWarper::RenderPlane(GLuint framebuffer,
                               GLuint source_texture,
                               GLuint map_texture,
                               int source_width,
                               int source_height,
                               int viewport_width,
                               int viewport_height,
                               bool uv_plane) {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, viewport_width, viewport_height);
    glUseProgram(shader_program_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source_texture);
    glUniform1i(source_texture_location_, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, map_texture);
    glUniform1i(map_texture_location_, 1);

    glUniform2f(source_size_location_,
                static_cast<float>(std::max(1, source_width)),
                static_cast<float>(std::max(1, source_height)));
    glUniform1i(uv_plane_location_, uv_plane ? 1 : 0);

    glBindVertexArray(quad_vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    return glGetError() == GL_NO_ERROR;
}

bool RkGlesWarper::WarpFrame(int camera_index, const NV12Frame& input, const DrmBuffer& output) {
    if (!IsReady() ||
        camera_index < 0 ||
        camera_index >= static_cast<int>(warp_programs_.size())) {
        return false;
    }
    WarpProgram& program = warp_programs_[camera_index];
    if (program.y_map_texture == 0 || program.uv_map_texture == 0) {
        return false;
    }

    ImportedBuffer* source = GetOrCreateInputBuffer(input);
    ImportedBuffer* target = GetOrCreateOutputBuffer(output);
    if (source == nullptr || target == nullptr) {
        return false;
    }

    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER,
                           GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,
                           target->y_plane.texture,
                           0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &framebuffer);
        return false;
    }
    if (!RenderPlane(framebuffer,
                     source->y_plane.texture,
                     program.y_map_texture,
                     source->width,
                     source->height,
                     program.output_width,
                     program.output_height,
                     false)) {
        glDeleteFramebuffers(1, &framebuffer);
        return false;
    }

    glFramebufferTexture2D(GL_FRAMEBUFFER,
                           GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,
                           target->uv_plane.texture,
                           0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &framebuffer);
        return false;
    }
    if (!RenderPlane(framebuffer,
                     source->uv_plane.texture,
                     program.uv_map_texture,
                     std::max(1, source->width / 2),
                     std::max(1, source->height / 2),
                     program.uv_width,
                     program.uv_height,
                     true)) {
        glDeleteFramebuffers(1, &framebuffer);
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    glFinish();
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        Logger::GetInstance().LogError(
            std::string("[RkGlesWarper] WarpFrame gl error=") + GetGlErrorString(error) +
            " camera_index=" + std::to_string(camera_index));
        return false;
    }
    if (IsDebugEnabled(3)) {
        Logger::GetInstance().Log(
            std::string("[RkGlesWarper] WarpFrame success camera_index=") +
            std::to_string(camera_index) +
            " input_fd=" + std::to_string(input.fd) +
            " output_fd=" + std::to_string(output.fd));
    }
    return true;
}

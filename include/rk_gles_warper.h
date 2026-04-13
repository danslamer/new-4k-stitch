#ifndef RK_GLES_WARPER_H
#define RK_GLES_WARPER_H

#include <unordered_map>
#include <vector>

#include <opencv2/opencv.hpp>

#include "drm_allocator.h"
#include "nv12_frame.h"
#include "stitching_param_generater.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#ifndef PFNGLEGLIMAGETARGETTEXTURE2DOESPROC
typedef void (GL_APIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target,
                                                                 GLeglImageOES image);
#endif

class RkGlesWarper {
 public:
    RkGlesWarper();
    ~RkGlesWarper();

    bool Initialize(const std::vector<WarpMapEntry>& warp_entries,
                    int input_width,
                    int input_height);
    void Shutdown();

    bool IsReady() const;
    bool WarpFrame(int camera_index, const NV12Frame& input, const DrmBuffer& output);

 private:
    struct PlaneImage {
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint texture = 0;
    };

    struct ImportedBuffer {
        int fd = -1;
        int width = 0;
        int height = 0;
        int stride = 0;
        PlaneImage y_plane;
        PlaneImage uv_plane;
    };

    struct WarpProgram {
        GLuint y_map_texture = 0;
        GLuint uv_map_texture = 0;
        int output_width = 0;
        int output_height = 0;
        int uv_width = 0;
        int uv_height = 0;
    };

    bool InitializeEgl();
    bool InitializeShaders();
    bool InitializeWarpPrograms(const std::vector<WarpMapEntry>& warp_entries,
                                int input_width,
                                int input_height);
    GLuint CompileShader(GLenum type, const char* source);
    bool CreateMapTexture(const cv::Mat& map_x,
                          const cv::Mat& map_y,
                          GLuint* texture,
                          bool uv_plane);
    bool ImportBuffer(const NV12Frame& frame, ImportedBuffer* imported);
    bool ImportBuffer(const DrmBuffer& buffer, ImportedBuffer* imported);
    void ReleaseImportedBuffer(ImportedBuffer* imported);
    ImportedBuffer* GetOrCreateInputBuffer(const NV12Frame& frame);
    ImportedBuffer* GetOrCreateOutputBuffer(const DrmBuffer& buffer);
    bool RenderPlane(GLuint framebuffer,
                     GLuint source_texture,
                     GLuint map_texture,
                     int source_width,
                     int source_height,
                     int viewport_width,
                     int viewport_height,
                     bool uv_plane);

    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
    EGLSurface egl_surface_ = EGL_NO_SURFACE;
    struct gbm_device* gbm_device_ = nullptr;
    int gbm_fd_ = -1;
    GLuint shader_program_ = 0;
    GLuint quad_vbo_ = 0;
    GLuint quad_vao_ = 0;
    GLint position_location_ = -1;
    GLint texcoord_location_ = -1;
    GLint source_texture_location_ = -1;
    GLint map_texture_location_ = -1;
    GLint source_size_location_ = -1;
    GLint uv_plane_location_ = -1;
    PFNEGLCREATEIMAGEKHRPROC egl_create_image_khr_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image_khr_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_egl_image_target_texture_2d_oes_ = nullptr;
    std::vector<WarpProgram> warp_programs_;
    std::unordered_map<int, ImportedBuffer> input_cache_;
    std::unordered_map<int, ImportedBuffer> output_cache_;
};

#endif

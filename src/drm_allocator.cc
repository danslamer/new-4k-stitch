#include "drm_allocator.h"

/**
 * @file drm_allocator.cc
 * @brief DRM DMA-BUF内存分配器实现
 * 负责RockChip平台上DRM设备缓冲区的创建、映射、释放等操作
 * 用于零拷贝硬件加速处理
 */

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>

#if __has_include(<drm/drm.h>)
#include <drm/drm.h>
#elif __has_include(<libdrm/drm.h>)
#include <libdrm/drm.h>
#elif __has_include(<drm.h>)
#include <drm.h>
#else
#error "drm.h not found. Please install libdrm development headers."
#endif
#include <xf86drm.h>

/**
 * @brief 分配NV12格式的DRM DMA-BUF缓冲区
 * 
 * 分配过程：
 * 1. 打开DRM设备(/dev/dri/card0)
 * 2. 创建虚拟帧缓冲对象(dumb buffer)，尺寸为width * height * 1.5（NV12格式）
 * 3. 导出为DMA-BUF文件描述符，用于零拷贝共享
 * 
 * @param width 缓冲区宽度(像素)
 * @param height 缓冲区高度(像素)
 * @param buf 输出的DrmBuffer结构体，包含fd、handle、pitch等信息
 * @return 成功返回0，失败返回-1
 */
int drm_alloc_nv12(int width, int height, DrmBuffer& buf) {
    drm_free(buf);

    int drm_fd = open("/dev/dri/card0", O_RDWR);
    if (drm_fd < 0) return -1;

    struct drm_mode_create_dumb creq = {};
    creq.width = width;
    creq.height = height * 3 / 2;
    creq.bpp = 8;

    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        close(drm_fd);
        return -1;
    }

    buf.drm_fd = drm_fd;
    buf.handle = creq.handle;
    buf.pitch = creq.pitch;
    buf.size = creq.size;
    buf.width = width;
    buf.height = height;

    struct drm_prime_handle prime = {};
    prime.handle = buf.handle;
    prime.flags = DRM_CLOEXEC | DRM_RDWR;

    if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
        drm_free(buf);
        return -1;
    }

    buf.fd = prime.fd;
    return 0;
}

/**
 * @brief 内存映射DRM缓冲区到用户空间
 * 
 * 通过mmap()将DRM缓冲区映射到进程的虚拟地址空间
 * 支持CPU直接访问缓冲区数据
 * 
 * @param buf DrmBuffer结构体，包含drm_fd和handle等信息
 * @return 映射成功返回虚拟地址指针；失败返回MAP_FAILED
 */
void* drm_map(const DrmBuffer& buf) {
    if (buf.drm_fd < 0 || buf.handle == 0 || buf.size == 0) {
        return MAP_FAILED;
    }

    struct drm_mode_map_dumb mreq = {};
    mreq.handle = buf.handle;
    if (ioctl(buf.drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        return MAP_FAILED;
    }

    return mmap(nullptr,
                static_cast<size_t>(buf.size),
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                buf.drm_fd,
                mreq.offset);
}

/**
 * @brief 取消内存映射DRM缓冲区
 * 
 * 解除之前通过drm_map()建立的内存映射
 * 在不再需要访问缓冲区数据时调用
 * 
 * @param buf DrmBuffer结构体，提供缓冲区大小信息
 * @param addr 由drm_map()返回的虚拟地址指针
 */
void drm_unmap(const DrmBuffer& buf, void* addr) {
    if (addr != nullptr && addr != MAP_FAILED && buf.size > 0) {
        munmap(addr, static_cast<size_t>(buf.size));
    }
}

/**
 * @brief 释放DRM缓冲区
 * 
 * 完全释放DRM缓冲区及其关联的所有资源：
 * 1. 关闭DMA-BUF文件描述符
 * 2. 销毁DRM虚拟帧缓冲对象
 * 3. 关闭DRM设备文件描述符
 * 4. 重置DrmBuffer结构体为默认值
 * 
 * 安全析构：
 * - 检查每个资源的有效性后再进行释放
 * - 即使多次调用该函数也是安全的（幂等性）
 * - 被drm_alloc_nv12()直接调用以清理失败状态
 * 
 * @param buf 被释放的DrmBuffer结构体，操作后会被重置
 */
void drm_free(DrmBuffer& buf) {
    if (buf.fd >= 0) {
        close(buf.fd);
        buf.fd = -1;
    }

    if (buf.drm_fd >= 0 && buf.handle != 0) {
        struct drm_mode_destroy_dumb dreq = {};
        dreq.handle = buf.handle;
        ioctl(buf.drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }

    if (buf.drm_fd >= 0) {
        close(buf.drm_fd);
    }

    buf = DrmBuffer{};
}

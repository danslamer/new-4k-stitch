#include "drm_allocator.h"

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

void drm_unmap(const DrmBuffer& buf, void* addr) {
    if (addr != nullptr && addr != MAP_FAILED && buf.size > 0) {
        munmap(addr, static_cast<size_t>(buf.size));
    }
}

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

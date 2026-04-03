#ifndef DRM_ALLOCATOR_H
#define DRM_ALLOCATOR_H

#include <stdint.h>

struct DrmBuffer {
    int fd = -1;
    int drm_fd = -1;
    uint32_t handle = 0;
    uint32_t pitch = 0;
    uint64_t size = 0;
    int width = 0;
    int height = 0;
};

int drm_alloc_nv12(int width, int height, DrmBuffer& buf);
void* drm_map(const DrmBuffer& buf);
void drm_unmap(const DrmBuffer& buf, void* addr);
void drm_free(DrmBuffer& buf);

#endif

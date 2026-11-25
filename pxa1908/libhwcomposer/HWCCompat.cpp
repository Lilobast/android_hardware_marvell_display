/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Pie compatibility for the Lollipop libHWComposerGC blob.
 *
 * libHWComposerGC does `new GraphicBuffer(w, h, format, usage)`: operator new(128) with the
 * Lollipop sizeof, then the constructor (provided by libui_shim, placement-constructing a Pie
 * GraphicBuffer). The Pie object is larger, so 128-byte allocations made by that library are
 * padded to the Pie size. Other 128-byte allocations of the library just get spare bytes.
 */
#define LOG_TAG "HWCCompat"

#include <new>
#include <dlfcn.h>
#include <stddef.h>
#include <pthread.h>
#include <unistd.h>
#include <stdarg.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <cutils/properties.h>
#include <utils/Timers.h>
#include <cutils/log.h>
#include <ui/GraphicBuffer.h>

#include "GotHook.h"
#include "HWCCompat.h"

#define GC_LIB "libHWComposerGC.so"
#define LOLLIPOP_GRAPHICBUFFER_SIZE 128

static_assert(sizeof(android::GraphicBuffer) >= LOLLIPOP_GRAPHICBUFFER_SIZE,
              "Pie GraphicBuffer smaller than Lollipop's?");

static void* padded_new(size_t size)
{
    if (size == LOLLIPOP_GRAPHICBUFFER_SIZE)
        size = sizeof(android::GraphicBuffer);
    return ::operator new(size);
}

/*
 * Flip stall: the blob queues its 2D composition (gco2D_*) and then flips the framebuffer. The
 * Vivante 2D engine runs asynchronously, so a flip could show a frame whose upper layers (the
 * notification shade over a video) were not drawn yet: the top of the screen flickered.
 * Before any framebuffer flip request from the blob or gralloc, commit the queues and wait for
 * them (gcoHAL_Commit(NULL, stall)). gcoHAL_Commit only acts on the thread's CURRENT hardware
 * type, which is 3D in SurfaceFlinger's thread at flip time, so the 2D engine (used by
 * libHWComposerGC) kept writing into the buffer already on the (video-mode) panel: black bands
 * and squares over video. Both engines are stalled now. persist.hwc.flip_stall: 0 = off,
 * 1 = current engine only, 2 = 3D and 2D (default);
 * debug.hwc.ioctl.log=N logs ioctl requests (N per second, until set back to 0).
 */
typedef int (*ioctl_fn)(int, int, ...);
typedef int (*gc_commit_fn)(void*, int);
static ioctl_fn sRealIoctlGc, sRealIoctlGralloc;
static gc_commit_fn sGcCommit;
typedef int (*gc_hwtype_set_fn)(void*, int);
typedef int (*gc_hwtype_get_fn)(void*, int*);
static gc_hwtype_set_fn sGcSetType;
static gc_hwtype_get_fn sGcGetType;
/* gceHARDWARE_TYPE: 1 = 3D, 2 = 2D. persist.hwc.flip_stall: 0 = off, 1 = current engine only
 * (old behaviour), 2 = both the 3D and the 2D engine (default). */
static void stall_engines(int mode)
{
    if (mode < 2 || !sGcSetType || !sGcGetType) {
        sGcCommit(NULL, 1);
        return;
    }
    int cur = 0;
    sGcGetType(NULL, &cur);
    static int sLogged = 0;
    if (sLogged < 3) { sLogged++; ALOGI("flip stall: current hardware type %d", cur); }
    sGcSetType(NULL, 1); sGcCommit(NULL, 1);
    sGcSetType(NULL, 2); sGcCommit(NULL, 1);
    sGcSetType(NULL, cur);
}
static int sStall = -1;
static int sLastFlipFence = -1;
static int sPrevFlipFence = -1;
static pthread_mutex_t sFenceLock = PTHREAD_MUTEX_INITIALIZER;

/* Userspace copy of the kernel's struct mmp_surface (include/video/mmp_disp.h). */
struct mmp_surface_user {
    unsigned short win_u16[10];
    int pix_fmt;
    unsigned int pitch[3];
    unsigned int phys[6];
    unsigned int hdr_addr[3];
    unsigned int hdr_size[3];
    unsigned int flag;
    int fence_fd;
    int fd;
};
static_assert(offsetof(struct mmp_surface_user, fence_fd) == 88, "mmp_surface layout");

static bool is_fb_flip(int req)
{
    switch ((unsigned)req) {
    case FBIOPAN_DISPLAY:
    case FBIOPUT_VSCREENINFO:
    case _IO('m', 8):   /* FB_IOCTL_FLIP_USR_BUF */
    case _IO('m', 26):  /* FB_IOCTL_FLIP_VSYNC */
    case _IO('m', 27):  /* FB_IOCTL_FLIP_COMMIT */
        return true;
    }
    return false;
}

static int stalled_ioctl(ioctl_fn real, const char* who, int fd, int req, void* arg)
{
    static int logLeft = 0;
    static int64_t lastCheck = 0;
    int64_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    if (logLeft == 0 && now - lastCheck > 1000000000LL) {   /* re-read at most once a second */
        lastCheck = now;
        logLeft = property_get_int32("debug.hwc.ioctl.log", 0);
    }
    if (logLeft > 0) {
        logLeft--;
        ALOGD("%s ioctl fd %d req %#x%s", who, fd, (unsigned)req, is_fb_flip(req) ? " (flip)" : "");
    }
    static int64_t sStallCheck = 0;
    if (sStall < 0 || now - sStallCheck > 1000000000LL) {
        sStallCheck = now;
        sStall = property_get_int32("persist.hwc.flip_stall", 2);
    }
    if (sStall && sGcCommit && is_fb_flip(req))
        stall_engines(sStall);
    int ret = real(fd, req, arg);
    if (ret == 0 && (unsigned)req == _IO('m', 8) && arg != NULL) {
        /* FB_IOCTL_FLIP_USR_BUF returns the release fence of the buffer it puts on screen: it
         * signals when the NEXT flip has replaced that buffer on the panel. HWC-only frames
         * alternate between two buffers, so the buffer about to be redrawn is the one flipped
         * two flips ago: keep the last two fences, hwc_set waits on the older one. */
        int ffd = ((const struct mmp_surface_user*)arg)->fence_fd;
        int dupfd = ffd >= 0 ? dup(ffd) : -1;
        pthread_mutex_lock(&sFenceLock);
        if (sPrevFlipFence >= 0) close(sPrevFlipFence);
        sPrevFlipFence = sLastFlipFence;
        sLastFlipFence = dupfd;
        pthread_mutex_unlock(&sFenceLock);
    }
    return ret;
}

int hwc_take_flip_fence()
{
    pthread_mutex_lock(&sFenceLock);
    int fd = sPrevFlipFence;
    sPrevFlipFence = -1;
    pthread_mutex_unlock(&sFenceLock);
    return fd;
}

static int gc_ioctl(int fd, int req, ...)
{
    va_list ap; va_start(ap, req); void* arg = va_arg(ap, void*); va_end(ap);
    return stalled_ioctl(sRealIoctlGc, "hwcgc", fd, req, arg);
}

static int gralloc_ioctl(int fd, int req, ...)
{
    va_list ap; va_start(ap, req); void* arg = va_arg(ap, void*); va_end(ap);
    return stalled_ioctl(sRealIoctlGralloc, "gralloc", fd, req, arg);
}

static void install_flip_stall()
{
    sGcCommit = (gc_commit_fn)dlsym(RTLD_DEFAULT, "gcoHAL_Commit");
    sGcSetType = (gc_hwtype_set_fn)dlsym(RTLD_DEFAULT, "gcoHAL_SetHardwareType");
    sGcGetType = (gc_hwtype_get_fn)dlsym(RTLD_DEFAULT, "gcoHAL_GetHardwareType");
    if (!sGcCommit) {
        ALOGE("gcoHAL_Commit not found: no flip stall");
        return;
    }
    void* orig = NULL;
    if (got_hook(GC_LIB, "ioctl", (void*)gc_ioctl, &orig) == 0) sRealIoctlGc = (ioctl_fn)orig;
    else ALOGW("ioctl hook in %s failed", GC_LIB);
    orig = NULL;
    if (got_hook("gralloc.mrvl.so", "ioctl", (void*)gralloc_ioctl, &orig) == 0) sRealIoctlGralloc = (ioctl_fn)orig;
    else ALOGW("ioctl hook in gralloc.mrvl.so failed");
    ALOGI("flip stall hooks: %s %s", sRealIoctlGc ? "hwcgc" : "-", sRealIoctlGralloc ? "gralloc" : "-");
}

void hwc_compat_install()
{
    install_flip_stall();
    void* orig = NULL;
    if (got_hook(GC_LIB, "_Znwj", (void*)padded_new, &orig) != 0)
        ALOGE("operator new hook failed: GraphicBuffer allocations of %s stay %d bytes",
              GC_LIB, LOLLIPOP_GRAPHICBUFFER_SIZE);
    else
        ALOGI("%s: 128-byte allocations padded to %zu (GraphicBuffer)", GC_LIB,
              sizeof(android::GraphicBuffer));
}

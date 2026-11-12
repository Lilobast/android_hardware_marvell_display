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
 * Before any framebuffer flip request from the blob or gralloc, commit the 2D queue and wait
 * for it (gcoHAL_Commit(NULL, stall)). persist.hwc.flip_stall=0 disables it;
 * debug.hwc.ioctl.log=N logs ioctl requests (N per second, until set back to 0).
 */
typedef int (*ioctl_fn)(int, int, ...);
typedef int (*gc_commit_fn)(void*, int);
static ioctl_fn sRealIoctlGc, sRealIoctlGralloc;
static gc_commit_fn sGcCommit;
static int sStall = -1;

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
    if (sStall < 0) sStall = property_get_bool("persist.hwc.flip_stall", true) ? 1 : 0;
    if (sStall && sGcCommit && is_fb_flip(req))
        sGcCommit(NULL, 1);
    return real(fd, req, arg);
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

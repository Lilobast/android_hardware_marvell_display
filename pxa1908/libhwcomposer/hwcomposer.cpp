#include "HWCCompat.h"
#include <sync/sync.h>
#include <time.h>
#include <sys/ioctl.h>
/*
 * Copyright (C) 2016 The CyanogenMod Project
 *               2017 The LineageOS Project
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

#include <hardware/hardware.h>

#include <fcntl.h>
#include <errno.h>

#include <cutils/log.h>
#include <cutils/atomic.h>

#include <hardware/hwcomposer.h>
#include <cutils/properties.h>
#include <utils/String8.h>
#include <utils/Mutex.h>

#include <EGL/egl.h>

#ifdef ENABLE_OVERLAY
#include "HWOverlayComposer.h"
#endif
#ifdef ENABLE_WFD_OPTIMIZATION
#include "GcuEngine.h"
#include "HWVirtualComposer.h"
#endif
#include "HWCFenceManager.h"

#include "HWBaselayComposer.h"
#include "HWCDisplayEventMonitor.h"

#include "hwcomposer_defs_mrvl.h"

#include <gralloc_priv.h>

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <utils/Trace.h>

using namespace android;
/*****************************************************************************/

#define HWC_1_1 1
#if HWC_1_1
#define HWC_VERSION HWC_DEVICE_API_VERSION_1_2
#else
#define HWC_VERSION HWC_DEVICE_API_VERSION_1_0
#endif

struct hwc_context_t {
    hwc_composer_device_1_t device;
    /* our private state goes below here */
#ifdef ENABLE_OVERLAY
    HWOverlayComposer *overlayComposer;
#endif
#ifdef ENABLE_WFD_OPTIMIZATION
    HWVirtualComposer *virtualComposer;
#endif
#ifdef ENABLE_HWC_GC_PATH
    HWBaselayComposer *baseComposer;
#endif

    HWCFenceManager *pFenceManager;

    hwc_procs_t *procs;
    bool skip;

    // total display device must keep same as MAX_DISPLAYS define in surfaceflinger/DisplayHardware/HWComposer.h
    bool disp_actived[HWC_NUM_DISPLAY_TYPES + 3];
    framebuffer_device_t *fbdev[HWC_NUM_DISPLAY_TYPES + 3];

    sp<HWCDisplayEventMonitor> monitor;
};

static int hwc_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device);

static struct hw_module_methods_t hwc_module_methods = {
    open: hwc_device_open
};

hwc_module_t HAL_MODULE_INFO_SYM
__attribute__ ((visibility ("default"))) =
{
    common: {
        tag: HARDWARE_MODULE_TAG,
        module_api_version: (uint16_t)HWC_DEVICE_API_VERSION_1_0,
        hal_api_version: (uint16_t)HARDWARE_HAL_API_VERSION,
        id: HWC_HARDWARE_MODULE_ID,
        name: "Marvell Hardware Composer HAL",
        author: "Nemirtingas (Maxime P)",
        methods: &hwc_module_methods,
        dso : NULL,
        reserved : {0,0,0,0,0,0,0,0,0,0,
                  0,0,0,0,0,0,0,0,0,0,
                  0,0,0,0,0},
    }
};

/*****************************************************************************/
static void dump_layer(hwc_layer_1_t const* l) {
    ALOGD("\ttype=%d, flags=%08x, handle=%p, tr=%02x, blend=%04x, {%d,%d,%d,%d}, {%d,%d,%d,%d}",
            l->compositionType, l->flags, l->handle, l->transform, l->blending,
            l->sourceCrop.left,
            l->sourceCrop.top,
            l->sourceCrop.right,
            l->sourceCrop.bottom,
            l->displayFrame.left,
            l->displayFrame.top,
            l->displayFrame.right,
            l->displayFrame.bottom);
}

// Pie's HWC2-on-HWC1 adapter only fills the framebuffer-target handle in set(); Nougat's
// SurfaceFlinger also passed the last one to prepare(). libHWComposerGC needs it there to
// learn the display surface, otherwise it composes nothing (every layer stays FRAMEBUFFER).
// The framebuffer-target buffers are SurfaceFlinger's fixed framebuffer slots, so the last
// handle stays valid.
static buffer_handle_t sLastFbTarget[HWC_NUM_PHYSICAL_DISPLAY_TYPES];

// Frames without client composition: SurfaceFlinger keeps handing over the same framebuffer
// target (the one on screen), so libHWComposerGC blits straight into the scanned-out buffer
// and the panel shows half-composed frames (tearing). Instead compose into another of
// SurfaceFlinger's framebuffer slots, never the one on screen nor the one SurfaceFlinger
// currently holds; the GC posts the handle it composed into. persist.hwc.fb_rotate=0: off.
#define MAX_FB_SLOTS 3
static buffer_handle_t sFbSlots[MAX_FB_SLOTS];
static int sNumFbSlots = 0;
static buffer_handle_t sShownFb = NULL;
/* The last two distinct framebuffers SurfaceFlinger held. Its BufferQueue hands out free
 * buffers in release order, so the third slot (held longest ago) is the one it renders into
 * next time it needs GPU composition. */
static buffer_handle_t sSfHeldNow = NULL;
static buffer_handle_t sSfHeldPrev = NULL;
static int sSetLogFrames = 0;  // set() log budget, armed with debug.hwc.prepare.log

static bool fb_rotate_enabled() {
    static int sRotate = -1;
    if (sRotate < 0) sRotate = property_get_bool("persist.hwc.fb_rotate", true) ? 1 : 0;
    return sRotate == 1;
}

static void remember_fb_slot(buffer_handle_t h) {
    for (int i = 0; i < sNumFbSlots; i++)
        if (sFbSlots[i] == h) return;
    const private_handle_t* ph = (const private_handle_t*)h;
    if (sNumFbSlots < MAX_FB_SLOTS && (ph->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER))
        sFbSlots[sNumFbSlots++] = h;
}

static bool needs_client_target(hwc_display_contents_1_t* d) {
    for (size_t i = 0; i + 1 < d->numHwLayers; i++)
        if (d->hwLayers[i].compositionType == HWC_FRAMEBUFFER) return true;
    return false;
}

static hwc_layer_1_t* fb_target_layer(hwc_display_contents_1_t* d) {
    if (!d || d->numHwLayers == 0) return NULL;
    hwc_layer_1_t* l = &d->hwLayers[d->numHwLayers - 1];
    return l->compositionType == HWC_FRAMEBUFFER_TARGET ? l : NULL;
}

/*
 * The GC blob shows black horizontal bands over the status bar while a video plays (NewPipe,
 * Gallery with the shade pulled): layers it composes around a YUV video layer come out partly
 * black. Waiting on fences, flips and the 2D engine did not change it; GPU composition does
 * not have it. So frames that contain a video layer are composed by SurfaceFlinger with GLES.
 * persist.hwc.gles_video: 0 = never, 1 = frames with a video layer (default), 2 = every frame.
 */
static int gles_video_mode() {
    static int sMode = -1;
    static int64_t sLastCheck = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    if (sMode < 0 || now - sLastCheck > 1000000000LL) {
        sLastCheck = now;
        sMode = property_get_int32("persist.hwc.gles_video", 1);
    }
    return sMode;
}

static bool is_rgb_format(int f) {
    return f >= HAL_PIXEL_FORMAT_RGBA_8888 && f <= HAL_PIXEL_FORMAT_BGRA_8888;
}

static void compose_video_frames_with_gles(hwc_display_contents_1_t* d) {
    int mode = gles_video_mode();
    if (mode == 0 || !d || d->numHwLayers < 2) return;
    bool video = mode == 2;
    for (size_t i = 0; !video && i + 1 < d->numHwLayers; i++) {
        const private_handle_t* ph = (const private_handle_t*)d->hwLayers[i].handle;
        if (ph && !is_rgb_format(ph->format)) video = true;
    }
    if (!video) return;
    for (size_t i = 0; i + 1 < d->numHwLayers; i++)
        if (d->hwLayers[i].compositionType == HWC_OVERLAY)
            d->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
}

static int hwc_prepare(hwc_composer_device_1_t *dev, size_t numDisplays, hwc_display_contents_1_t** displays) {
    ATRACE_CALL();
    if (displays) {
        struct hwc_context_t *ctx = (struct hwc_context_t *)dev;
        uint32_t numRestDisplays = numDisplays;
#ifdef ENABLE_OVERLAY
        if( !ctx->skip && ctx->overlayComposer ) {
            ctx->overlayComposer->prepare(numDisplays, displays);
        }
#endif
#ifdef ENABLE_WFD_OPTIMIZATION
        if( !ctx->skip && ctx->virtualComposer ) {
            numRestDisplays = ctx->virtualComposer->prepare(numDisplays, displays);
        }
#endif
#ifdef ENABLE_HWC_GC_PATH
        // Debug: setprop debug.hwc.prepare.log N logs the next N frames' layers before
        // (as requested, skip flag = 1) and after the GC prepare (0 = GLES, 1 = HWC).
        static int sLogFrames = 0;
        static int sCheck = 0;
        if (++sCheck >= 60) {
            sCheck = 0;
            int n = property_get_int32("debug.hwc.prepare.log", 0);
            if (n > 0) {
                sLogFrames = n;
                sSetLogFrames = n;
                property_set("debug.hwc.prepare.log", "0");
            }
        }
        hwc_display_contents_1_t* d0 = numRestDisplays > 0 ? displays[0] : NULL;
        bool log = sLogFrames > 0 && d0;
        uint32_t before[16];
        size_t nl = d0 ? (d0->numHwLayers < 16 ? d0->numHwLayers : 16) : 0;
        if (log) {
            for (size_t i = 0; i < nl; i++)
                before[i] = (d0->hwLayers[i].compositionType << 8) | (d0->hwLayers[i].flags & 0xff);
        }
        hwc_layer_1_t* fbt[HWC_NUM_PHYSICAL_DISPLAY_TYPES] = { NULL };
        for (size_t i = 0; i < numRestDisplays && i < HWC_NUM_PHYSICAL_DISPLAY_TYPES; i++) {
            hwc_layer_1_t* t = fb_target_layer(displays[i]);
            if (t && !t->handle && sLastFbTarget[i]) {
                t->handle = sLastFbTarget[i];
                fbt[i] = t;
            }
        }
        if(ctx->baseComposer)
            ctx->baseComposer->prepare(&ctx->device, numRestDisplays, displays);
        for (size_t i = 0; i < HWC_NUM_PHYSICAL_DISPLAY_TYPES; i++) {
            if (fbt[i]) fbt[i]->handle = NULL;
        }
        if (numRestDisplays > 0) compose_video_frames_with_gles(displays[0]);
        // Until every framebuffer slot has been seen, let SurfaceFlinger compose with the GPU:
        // each such frame dequeues the next framebuffer, which set() then remembers.
        if (fb_rotate_enabled() && sNumFbSlots < MAX_FB_SLOTS && numRestDisplays > 0
                && displays[0]) {
            hwc_display_contents_1_t* d = displays[0];
            for (size_t i = 0; i + 1 < d->numHwLayers; i++)
                if (d->hwLayers[i].compositionType == HWC_OVERLAY)
                    d->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
        }
        if (log) {
            sLogFrames--;
            ALOGD("prepare: %zu layers, display flags %08x", d0->numHwLayers, d0->flags);
            for (size_t i = 0; i < nl; i++) {
                hwc_layer_1_t* l = &d0->hwLayers[i];
                ALOGD("  layer %zu: in type=%u flags=%02x -> out type=%d flags=%08x fmt=%d",
                      i, before[i] >> 8, before[i] & 0xff, l->compositionType, l->flags,
                      l->handle ? ((private_handle_t*)l->handle)->format : -1);
            }
        }
#endif
    }
    return 0;
}

/*
 * Flip guard for HWC-only frames. The wrapper double-buffers between the two framebuffers
 * SurfaceFlinger is not holding, and the mmp driver's pan_display does not wait for the flip
 * to be latched. When a frame finishes late, the next one started drawing into the buffer still
 * being scanned out: the top of the screen (status bar) flickered during video. Before composing,
 * wait for one vsync if the previous flip has not been latched yet.
 */
#define FB_IOCTL_WAIT_VSYNC_MRVL _IO('m', 18)
static int64_t sLastFlipNs = 0;

static int64_t hwc_now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int64_t hwc_last_vsync_ns()
{
    static int fd = -1;
    char buf[32];
    if (fd < 0) fd = open("/sys/class/graphics/fb0/device/vsync_ts", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return -1;
    buf[n] = 0;
    return (int64_t)strtoull(buf, NULL, 16);
}

static bool flip_guard_enabled()
{
    char v[PROPERTY_VALUE_MAX];
    property_get("persist.hwc.flip_guard", v, "1");
    return v[0] != '0';
}

static void wait_previous_flip()
{
    static int fbFd = -1;
    if (!flip_guard_enabled()) return;
    int fence = hwc_take_flip_fence();
    if (fence >= 0) {
        /* Release fence of the buffer we are about to redraw: signals once it is no longer
         * scanned out. */
        ATRACE_NAME("hwc_wait_flip_fence");
        sync_wait(fence, 50);
        close(fence);
        return;
    }
    if (sLastFlipNs == 0) return;
    int64_t vsync = hwc_last_vsync_ns();
    if (vsync < 0 || vsync >= sLastFlipNs) return;
    if (hwc_now_ns() - sLastFlipNs > 50000000LL) return;  // vsync events off: long since latched
    if (fbFd < 0) fbFd = open("/dev/graphics/fb0", O_RDWR | O_CLOEXEC);
    if (fbFd >= 0) {
        ATRACE_NAME("hwc_wait_previous_flip");
        ioctl(fbFd, FB_IOCTL_WAIT_VSYNC_MRVL);
    }
}

/*
 * GPU-composed frames: SurfaceFlinger hands over the client target with an acquire fence that
 * signals when the GPU has finished drawing it. The GC blob flips it without waiting, so a
 * frame still being drawn (the Vivante GPU renders in tiles) reached the screen: square gaps
 * from the slot's previous frame over a video's controls. Wait for it first.
 * persist.hwc.wait_target=0 disables it. Every ~10 s the log says how many fences were seen.
 */
static void wait_client_target(hwc_layer_1_t* t) {
    static int sEnabled = -1;
    static int64_t sLastCheck = 0;
    static int sFrames = 0, sFenced = 0, sPending = 0;
    int64_t now = hwc_now_ns();
    if (sEnabled < 0 || now - sLastCheck > 10000000000LL) {
        if (sEnabled >= 0 && sFrames)
            ALOGI("client target: %d frames, %d with a fence, %d not yet signalled",
                  sFrames, sFenced, sPending);
        sFrames = sFenced = sPending = 0;
        sLastCheck = now;
        sEnabled = property_get_bool("persist.hwc.wait_target", true) ? 1 : 0;
    }
    if (!t) return;
    sFrames++;
    if (t->acquireFenceFd < 0) return;
    sFenced++;
    if (sync_wait(t->acquireFenceFd, 0) < 0) {
        sPending++;
        if (sEnabled) {
            ATRACE_NAME("hwc_wait_client_target");
            sync_wait(t->acquireFenceFd, 100);
        }
    }
}

static int hwc_set(struct hwc_composer_device_1 *dev,
                size_t numDisplays, hwc_display_contents_1_t** displays)
{
    ATRACE_CALL();
    int status = 0;
    uint32_t numRestDisplays = numDisplays;
    struct hwc_context_t *ctx = (struct hwc_context_t *)dev;
#ifdef ENABLE_WFD_OPTIMIZATION
    if(!ctx->skip && ctx->virtualComposer && ctx->virtualComposer->isRunning()){
        numRestDisplays = HWC_NUM_DISPLAY_TYPES;
    }
#endif
#ifdef ENABLE_HWC_GC_PATH
    for (size_t i = 0; i < numDisplays && i < HWC_NUM_PHYSICAL_DISPLAY_TYPES; i++) {
        hwc_layer_1_t* t = fb_target_layer(displays[i]);
        if (t && t->handle) sLastFbTarget[i] = t->handle;
    }
    hwc_layer_1_t* t0 = numDisplays > 0 ? fb_target_layer(displays[0]) : NULL;
    bool rotated = false;
    if (t0 && t0->handle) {
        remember_fb_slot(t0->handle);
        if (t0->handle != sSfHeldNow) {
            sSfHeldPrev = sSfHeldNow;
            sSfHeldNow = t0->handle;
        }
        if (fb_rotate_enabled() && sNumFbSlots == MAX_FB_SLOTS && !needs_client_target(displays[0])) {
            buffer_handle_t sfHeld = t0->handle;
            /* HWC-only frames rotate between the slot SurfaceFlinger holds and the one it
             * released last; the slot it will dequeue next for GPU composition is never put on
             * screen, otherwise its next GPU frame (e.g. the notification shade sliding over a
             * video) was rendered into the buffer being displayed and the top of the screen
             * flickered. Falls back to "not held, not shown" until the history is known. */
            buffer_handle_t sfNext = NULL;
            if (sSfHeldPrev) {
                for (int i = 0; i < sNumFbSlots; i++)
                    if (sFbSlots[i] != sSfHeldNow && sFbSlots[i] != sSfHeldPrev) sfNext = sFbSlots[i];
            }
            for (int i = 0; i < sNumFbSlots; i++) {
                bool ok = sfNext ? (sFbSlots[i] != sfNext && sFbSlots[i] != sShownFb)
                                 : (sFbSlots[i] != sfHeld && sFbSlots[i] != sShownFb);
                if (ok) {
                    // The original target was not drawn by the GPU this frame: drop its fence.
                    if (t0->acquireFenceFd >= 0) {
                        close(t0->acquireFenceFd);
                        t0->acquireFenceFd = -1;
                    }
                    t0->handle = sFbSlots[i];
                    rotated = true;
                    break;
                }
            }
        }
        sShownFb = t0->handle;
        if (sSetLogFrames > 0) {
            sSetLogFrames--;
            const private_handle_t* ph = (const private_handle_t*)t0->handle;
            ALOGD("set: target %p offset %lld flags %x slots %d client %d",
                  t0->handle, (long long)ph->offset, ph->flags, sNumFbSlots,
                  needs_client_target(displays[0]));
        }
    }
    if(ctx->baseComposer)
    {
        if (!rotated && t0 && t0->handle && numDisplays > 0 && needs_client_target(displays[0]))
            wait_client_target(t0);
        if (rotated) wait_previous_flip();
        status = ctx->baseComposer->set(&ctx->device, numRestDisplays, displays);
        if (rotated) sLastFlipNs = hwc_now_ns();
    }
    else
    {
#endif

#if !HWC_1_1
        /* Only swap buffer for HWC1.0 3d path, HWC1.1 3d Path swapbuffers in doComposite */
        EGLBoolean sucess = eglSwapBuffers((EGLDisplay)displays[0]->dpy, (EGLSurface)displays[0]->sur);
        status = sucess ? status : HWC_EGL_ERROR;
#endif
#ifdef ENABLE_HWC_GC_PATH
    }
#endif
#ifdef ENABLE_WFD_OPTIMIZATION
    if( !ctx->skip && ctx->virtualComposer && ctx->virtualComposer->isRunning()) {
        ctx->virtualComposer->set(numDisplays, displays);
    }
#endif
#ifdef ENABLE_OVERLAY
    if(!ctx->skip && ctx->overlayComposer) {
        ctx->overlayComposer->set(numDisplays, displays);
    }
#endif
#ifdef ENABLE_OVERLAY
    if(!ctx->skip && ctx->overlayComposer) {
        ctx->overlayComposer->finishCompose();
    }
#endif
    return status;
}

static int hwc_blank(struct hwc_composer_device_1* dev, int disp, int blank)
{
    int status = 0;
    struct hwc_context_t *ctx = (struct hwc_context_t *)dev;

    /* Check device. */
    if(ctx == NULL)
    {
        ALOGE("%s(%d): Invalid device!", __FUNCTION__, __LINE__);
        return -EINVAL;
    }

    if(disp > HWC_NUM_DISPLAY_TYPES)
    {
        /* Invalid parameters. */
        ALOGE("%s(%d): Invalid parameters!", __FUNCTION__, __LINE__);
        return -EINVAL;
    }

    ctx->disp_actived[disp] = !blank;

#ifdef ENABLE_HWC_GC_PATH
    if(ctx->baseComposer)
    {
        status = ctx->baseComposer->blank(&ctx->device, disp, blank);
    }
#endif

    return status;

}

static int hwc_getDisplayConfigs(struct hwc_composer_device_1* dev, int disp,
            uint32_t* configs, size_t* numConfigs)
{
    struct hwc_context_t *ctx = (struct hwc_context_t *)dev;

     /* Check device. */
    if (ctx == NULL)
    {
        ALOGE("%s(%d): Invalid device!", __FUNCTION__, __LINE__);
        return -EINVAL;
    }

    if (configs == NULL || numConfigs == NULL || *numConfigs == 0)
    {
        /* Invalid parameters. */
        ALOGE("%s(%d): Invalid parameters!", __FUNCTION__, __LINE__);
        return -EINVAL;
    }

    /*
     * TODO (VENDOR): Support external display.
     * Currenty only primary display is supported.
     */
    if (disp == HWC_DISPLAY_PRIMARY)
    {
        /*
         * TODO (VENDOR): Support multiple configs.
         * Current only one config is supported.
         */
        configs[0]  = 0;
        *numConfigs = 1;
        return 0;
    }
    else
    {
        /*
         * TODO (VENDOR): Support external display.
         * Currently external display is not supported.
         */
        ALOGE("%s(%d): TODO: disp %d not supported!",
             __FUNCTION__, __LINE__, disp);
        return -EINVAL;
    }

    return 0;
}

static int hwc_getDisplayAttributes(struct hwc_composer_device_1* dev, int disp,
            uint32_t config, const uint32_t* attributes, int32_t* values)
{
    struct hwc_context_t *ctx = (struct hwc_context_t *)dev;

    /* Check device. */
    if (ctx == NULL)
    {
        ALOGE("%s(%d): Invalid device!", __FUNCTION__, __LINE__);
        return -EINVAL;
    }

    if (attributes == NULL || values == 0)
    {
        /* Invalid parameters. */
        ALOGE("%s(%d): Invalid parameters!", __FUNCTION__, __LINE__);
        return -EINVAL;
    }

    /*
     * TODO (VENDOR): Support external display.
     * Currently only primary display is supported.
     * */
    if (disp != HWC_DISPLAY_PRIMARY)
    {
        ALOGE("%s(%d): TODO: disp %d not supported!",
             __FUNCTION__, __LINE__, disp);
        return -EINVAL;
    }

    /*
     * TODO (VENDOR): Support mutiple configs.
     * Currently only one config is supported.
     */
    if (config != 0)
    {
        ALOGE("%s(%d): TODO: config %d not supported!",
             __FUNCTION__, __LINE__, config);
        return 0;
    }

    for (int i = 0; attributes[i] != HWC_DISPLAY_NO_ATTRIBUTE; i++)
    {
        switch (attributes[i])
        {
        case HWC_DISPLAY_VSYNC_PERIOD:
            values[i] = nsecs_t(1e9/ctx->fbdev[disp]->fps);
            break;

        case HWC_DISPLAY_WIDTH:
            values[i] = ctx->fbdev[disp]->width;
            break;

        case HWC_DISPLAY_HEIGHT:
            values[i] = ctx->fbdev[disp]->height;
            break;

        case HWC_DISPLAY_DPI_X:
            values[i] = ctx->fbdev[disp]->xdpi;
            break;

        case HWC_DISPLAY_DPI_Y:
            values[i] = ctx->fbdev[disp]->ydpi;
            break;

        case HWC_DISPLAY_FORMAT:
            values[i] = ctx->fbdev[disp]->format;
            break;

        default:
            return -EINVAL;
        }
    }

    return 0;
}

static void hwc_dump(hwc_composer_device_1_t *dev,
        char *buff,
        int buff_len) {
    struct hwc_context_t *ctx = (struct hwc_context_t *)dev;
    String8 result;
    char buffer[1024];
#ifdef ENABLE_OVERLAY
    if(ctx->overlayComposer){
        ctx->overlayComposer->dump(result, buffer, 1024);
        strncpy(buff, result.string(), buff_len - 1);
    }
#endif
#ifdef ENABLE_WFD_OPTIMIZATION
    if(ctx->virtualComposer){
        ctx->virtualComposer->dump(result, buffer, 1024);
        strncpy(buff, result.string(), buff_len - 1);
    }
#endif
}

static int hwc_query(hwc_composer_device_1_t *dev,
        int what, int* value)
{
    struct hwc_context_t *ctx = (struct hwc_context_t *)dev;

    switch (what) {
    case HWC_BACKGROUND_LAYER_SUPPORTED:
        // we don't support the background layer yet
        value[0] = 0;
        break;

    case HWC_VSYNC_PERIOD:
        // vsync period in nanosecond
        value[0] = 1000000000.0 / ctx->fbdev[HWC_DISPLAY_PRIMARY]->fps;
        break;

    case HWC_DISPLAY_TYPES_SUPPORTED:
        /*
         * TODO (VENDOR): Support multiple displays.
         * Currently only primary display is supported.
         */
        *value = HWC_DISPLAY_PRIMARY_BIT;
        break;

    default:
        // unsupported query
        return -EINVAL;
    }
    return 0;
}

static void hwc_registerProcs(hwc_composer_device_1_t* dev,
                                    hwc_procs_t const* procs)
{
    struct hwc_context_t *ctx = (struct hwc_context_t *)dev;

    ctx->procs = (typeof(ctx->procs)) procs;
    if( !ctx->monitor.get() ) {
        ctx->monitor = new HWCDisplayEventMonitor(ctx->procs);
    }
}



static int hwc_device_close(struct hw_device_t *dev)
{
    struct hwc_context_t* ctx = (struct hwc_context_t*)dev;
    if (ctx) {
#ifdef ENABLE_OVERLAY
        if(ctx->overlayComposer)
            delete ctx->overlayComposer;
#endif
#ifdef ENABLE_WFD_OPTIMIZATION
        if(ctx->virtualComposer)
            delete ctx->virtualComposer;
#endif
#ifdef ENABLE_HWC_GC_PATH
        if(ctx->baseComposer)
        {
            ctx->baseComposer->close(dev);
            delete ctx->baseComposer;
        }
#endif

        for(int i = 0; i <= HWC_NUM_DISPLAY_TYPES; i++)
        {
            if(ctx->fbdev[i] != NULL)
                framebuffer_close(ctx->fbdev[i]);
        }

        free(ctx);
    }
    return 0;
}

static int hwc_event_control(hwc_composer_device_1_t* dev, int disp,
        int event, int enabled)
{
    struct hwc_context_t* ctx = (struct hwc_context_t*)dev;

    switch (event) {
    case HWC_EVENT_VSYNC:
    {
        if( ctx->monitor.get() ) {
            /* TO DO, need transfer the disp to correct fb device */
            ctx->monitor->eventControl(event, enabled);
        }
        return 0;
    }
    default:
        return -EINVAL;
    }
}

/*****************************************************************************/

static int hwc_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device)
{
    int status = 0;
    hw_module_t const *gralloc;

    if (!strcmp(name, HWC_HARDWARE_COMPOSER))
    {
        struct hwc_context_t *dev;
        dev = (hwc_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = HWC_VERSION;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = hwc_device_close;

        dev->device.prepare = hwc_prepare;
        dev->device.set = hwc_set;
        dev->device.dump = hwc_dump;
        dev->device.blank = hwc_blank;
        dev->device.getDisplayConfigs = hwc_getDisplayConfigs;
        dev->device.getDisplayAttributes = hwc_getDisplayAttributes;
        dev->device.registerProcs = hwc_registerProcs;
        dev->device.query = hwc_query;
        dev->device.eventControl = hwc_event_control;

        *device = &dev->device.common;

        // create different composers.
#ifdef ENABLE_OVERLAY
        dev->overlayComposer = new HWOverlayComposer();
#endif
#ifdef ENABLE_WFD_OPTIMIZATION
        dev->virtualComposer = new HWVirtualComposer();
#endif
#ifdef ENABLE_HWC_GC_PATH
        dev->baseComposer = new HWBaselayComposer();
        int st = dev->baseComposer->open(name, NULL);
        if(st < 0)
        {
            ALOGW("Construct BaselayComposer failed, disable it.");
            delete dev->baseComposer;
            dev->baseComposer = NULL;
        }
        char value[PROPERTY_VALUE_MAX];
        property_get( "persist.hwc.skip", value, "0" );
        if(strcmp(value,"1") == 0 ) {
            dev->skip = true;
        } else {
            dev->skip = false;
        }
#endif

#if HWC_1_1
        if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &gralloc) == 0) {
            status = 0;
        } else {
            status = -EINVAL;
            ALOGE("load gralloc module failed");
        }

        /* default always open the buildin screen */
        int err = framebuffer_open(gralloc, &dev->fbdev[HWC_DISPLAY_PRIMARY]);

        if (err) {
            ALOGE("framebuffer_open failed (%s)", strerror(-err));

            if( dev->baseComposer )
            {
                dev->baseComposer->close((hw_device_t*)dev);
                delete dev->baseComposer;
            }
            *device = NULL;
            free(dev);
            return -err;
        }

        private_module_t * m = (private_module_t *) gralloc;
#ifdef ENABLE_WFD_OPTIMIZATION
        if(dev->virtualComposer)
            dev->virtualComposer->setSourceDisplayInfo(&m->info);
#endif
#ifdef ENABLE_OVERLAY
        if(dev->overlayComposer)
            dev->overlayComposer->setSourceDisplayInfo(&m->info);
#endif
#endif
    }
    return status;
}

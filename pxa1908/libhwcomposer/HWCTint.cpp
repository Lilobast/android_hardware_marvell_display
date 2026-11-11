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
 * Hardware colour tint for LiveDisplay on the GC 2D composition path.
 *
 * LiveDisplay (cmhw DisplayColorCalibration) publishes per-channel gains in TINT_PROP as
 * "r g b" (0..32768, 32768 = 1.0). Without a hardware backend it would use SurfaceFlinger's
 * colour matrix instead, which forces every frame to GLES composition.
 *
 * libHWComposerGC (prebuilt) composes each layer with the Vivante 2D engine. For every source it
 * calls gco2D_SetPixelMultiplyModeAdvanced() and then gco2D_SetSourceGlobalColorAdvanced().
 * While a tint is active we switch the source global multiply to COLOR mode and fold the RGB
 * gains (and any plane alpha) into the global colour, so the tint is applied during composition.
 * Bypass mode (direct flip of a single full-screen buffer) is disabled while tinting.
 */

#define LOG_NDEBUG 0
#include <cutils/log.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>

#include "GotHook.h"
#include "HWCTint.h"

#define GC_LIB      "libHWComposerGC.so"
#define TINT_PROP   "sys.livedisplay.rgb"
#define DEBUG_PROP  "debug.hwc.tint.log"
#define BYPASS_PROP "persist.disable.bypassmode"
#define TINT_MAX    32768

// gce2D_GLOBAL_COLOR_MULTIPLY_MODE
#define GLOBAL_MULTIPLY_DISABLE 0
#define GLOBAL_MULTIPLY_ALPHA   1
#define GLOBAL_MULTIPLY_COLOR   2

/*
 * Call sites (return address offsets in libHWComposerGC.so) of the per-layer state setup that
 * precedes a composition blit. Other callers (state reset before a clear, etc.) are left alone.
 */
static const uint32_t kLayerSetupSites[] = {
    0x6a94, 0x6b28,     // multi-source blit path A
    0x771c, 0x77b2,     // multi-source blit path B
    0x7b30, 0x7bce,     // filter blit path
};

typedef int (*SetMulFn)(void*, int, int, int, int);
typedef int (*SetColorFn)(void*, uint32_t);
typedef int (*PropGetFn)(const char*, char*, const char*);

static SetMulFn sOrigSetMul;
static SetColorFn sOrigSetColor;
static PropGetFn sOrigPropGet;
static uintptr_t sGcBase;

static bool sTint;
static bool sDebug;
static uint32_t sGain[3] = { 255, 255, 255 };
static int sLastGlobalMode;
static bool sLastSiteTinted;

static const prop_info* sTintPi;
static uint32_t sTintSerial = ~0u;
static const prop_info* sDebugPi;
static uint32_t sDebugSerial = ~0u;

static int lib_base_cb(struct dl_phdr_info* info, size_t, void* data)
{
    if (info->dlpi_name && strstr(info->dlpi_name, GC_LIB)) {
        *(uintptr_t*)data = info->dlpi_addr;
        return 1;
    }
    return 0;
}

static bool is_layer_site(void* ret)
{
    uint32_t off = (uint32_t)(((uintptr_t)ret & ~(uintptr_t)1) - sGcBase);
    for (size_t i = 0; i < sizeof(kLayerSetupSites) / sizeof(kLayerSetupSites[0]); i++)
        if (kLayerSetupSites[i] == off)
            return true;
    return false;
}

static int hook_SetPixelMultiplyModeAdvanced(void* e, int srcPre, int dstPre, int globalMode, int dstDemul)
{
    void* ret = __builtin_return_address(0);
    sLastGlobalMode = globalMode;
    sLastSiteTinted = sTint && is_layer_site(ret);
    if (sDebug)
        ALOGD("SetMul site=0x%x src=%d dst=%d global=%d demul=%d tint=%d",
              (uint32_t)(((uintptr_t)ret & ~(uintptr_t)1) - sGcBase), srcPre, dstPre, globalMode,
              dstDemul, sLastSiteTinted);
    if (sLastSiteTinted)
        globalMode = GLOBAL_MULTIPLY_COLOR;
    return sOrigSetMul(e, srcPre, dstPre, globalMode, dstDemul);
}

static int hook_SetSourceGlobalColorAdvanced(void* e, uint32_t c)
{
    uint32_t out = c;
    if (sLastSiteTinted) {
        uint32_t a = c >> 24;
        uint32_t f[3], fa;
        switch (sLastGlobalMode) {
        case GLOBAL_MULTIPLY_ALPHA:
            f[0] = f[1] = f[2] = a; fa = a;
            break;
        case GLOBAL_MULTIPLY_COLOR:
            f[0] = (c >> 16) & 0xff; f[1] = (c >> 8) & 0xff; f[2] = c & 0xff; fa = a;
            break;
        default:
            f[0] = f[1] = f[2] = 255; fa = 255;
            break;
        }
        uint32_t r = (f[0] * sGain[0] + 127) / 255;
        uint32_t g = (f[1] * sGain[1] + 127) / 255;
        uint32_t b = (f[2] * sGain[2] + 127) / 255;
        out = (fa << 24) | (r << 16) | (g << 8) | b;
    }
    if (sDebug)
        ALOGD("SetSrcColor 0x%08x -> 0x%08x (mode %d)", c, out, sLastGlobalMode);
    return sOrigSetColor(e, out);
}

/*
 * Bypass mode is read once when the GC path opens. In bypass the GLES framebuffer target (or a
 * single full-screen layer) is flipped directly without a 2D pass, so a tint could not be
 * applied to those frames: keep it off. The extra cost is one 2D copy of such frames.
 */
static int hook_property_get(const char* key, char* value, const char* def)
{
    if (key && strcmp(key, BYPASS_PROP) == 0) {
        ALOGI("tint: %s forced to 1 (read from 0x%x)", key,
              (uint32_t)(((uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1) - sGcBase));
        strcpy(value, "1");
        return 1;
    }
    return sOrigPropGet(key, value, def);
}

void hwc_tint_install()
{
    dl_iterate_phdr(lib_base_cb, &sGcBase);
    if (sGcBase == 0) {
        ALOGE("tint: %s not loaded", GC_LIB);
        return;
    }
    void* orig = NULL;
    if (got_hook(GC_LIB, "gco2D_SetPixelMultiplyModeAdvanced", (void*)hook_SetPixelMultiplyModeAdvanced, &orig) != 0) {
        ALOGE("tint: hook SetPixelMultiplyModeAdvanced failed");
        return;
    }
    sOrigSetMul = (SetMulFn)orig;
    if (got_hook(GC_LIB, "gco2D_SetSourceGlobalColorAdvanced", (void*)hook_SetSourceGlobalColorAdvanced, &orig) != 0) {
        got_hook(GC_LIB, "gco2D_SetPixelMultiplyModeAdvanced", (void*)sOrigSetMul, NULL);
        ALOGE("tint: hook SetSourceGlobalColorAdvanced failed");
        return;
    }
    sOrigSetColor = (SetColorFn)orig;
    if (got_hook(GC_LIB, "property_get", (void*)hook_property_get, &orig) == 0)
        sOrigPropGet = (PropGetFn)orig;
    else
        ALOGW("tint: property_get hook failed, bypass mode stays enabled");
    ALOGI("tint: hooks installed (%s base 0x%x)", GC_LIB, (uint32_t)sGcBase);
}

static bool prop_changed(const char* name, const prop_info** pi, uint32_t* serial, char* value)
{
    if (*pi == NULL)
        *pi = __system_property_find(name);
    if (*pi == NULL)
        return false;
    uint32_t s = __system_property_serial(*pi);
    if (s == *serial)
        return false;
    *serial = s;
    __system_property_read(*pi, NULL, value);
    return true;
}

void hwc_tint_refresh()
{
    char v[PROP_VALUE_MAX];
    if (sOrigSetColor == NULL)
        return;
    if (prop_changed(DEBUG_PROP, &sDebugPi, &sDebugSerial, v))
        sDebug = atoi(v) != 0;
    if (sDebug)
        ALOGD("set: new frame (tint=%d)", sTint);
    if (!prop_changed(TINT_PROP, &sTintPi, &sTintSerial, v))
        return;

    int rgb[3];
    if (sscanf(v, "%d %d %d", &rgb[0], &rgb[1], &rgb[2]) != 3) {
        rgb[0] = rgb[1] = rgb[2] = TINT_MAX;
    }
    bool identity = true;
    for (int i = 0; i < 3; i++) {
        if (rgb[i] < 0) rgb[i] = 0;
        if (rgb[i] > TINT_MAX) rgb[i] = TINT_MAX;
        sGain[i] = (uint32_t)((rgb[i] * 255 + TINT_MAX / 2) / TINT_MAX);
        if (sGain[i] != 255)
            identity = false;
    }
    sTint = !identity;
    ALOGI("tint: \"%s\" -> gains %u %u %u (%s)", v, sGain[0], sGain[1], sGain[2], sTint ? "on" : "off");
}

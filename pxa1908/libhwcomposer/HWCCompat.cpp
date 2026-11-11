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

void hwc_compat_install()
{
    void* orig = NULL;
    if (got_hook(GC_LIB, "_Znwj", (void*)padded_new, &orig) != 0)
        ALOGE("operator new hook failed: GraphicBuffer allocations of %s stay %d bytes",
              GC_LIB, LOLLIPOP_GRAPHICBUFFER_SIZE);
    else
        ALOGI("%s: 128-byte allocations padded to %zu (GraphicBuffer)", GC_LIB,
              sizeof(android::GraphicBuffer));
}

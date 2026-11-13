#ifndef HWC_COMPAT_H
#define HWC_COMPAT_H

/* Pie compatibility hooks for libHWComposerGC; call before hwc_device_open_gc(). */
void hwc_compat_install();
/* Release fence of the framebuffer flipped two flips ago (signals once the latest flip replaced it
 * on the panel); -1 if none. Caller closes. */
int hwc_take_flip_fence();

#endif

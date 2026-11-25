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

#ifndef HWC_COMPAT_H
#define HWC_COMPAT_H

/* Pie compatibility hooks for libHWComposerGC; call before hwc_device_open_gc(). */
void hwc_compat_install();
/* Release fence of the framebuffer flipped two flips ago (signals once the latest flip replaced it
 * on the panel); -1 if none. Caller closes. */
int hwc_take_flip_fence();

#endif

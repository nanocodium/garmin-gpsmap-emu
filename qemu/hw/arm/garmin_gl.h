/*
 * Garmin GPSMAP 7x08: OpenGL ES 1.1 API interception ("GL hooks").
 *
 * The firmware renders with Imagination's OpenGL ES-CM 1.1 driver, whose
 * shading work ends up as USSE programs for the SGX540.  Instead of
 * emulating the GPU we replace the driver's public entry points with
 * trampolines that trap into QEMU (a store to a magic MMIO slot), execute
 * the call on the host and return the result through a second MMIO read.
 *
 * Hook table file (machine option gl-hooks=PATH), one entry per line:
 *     <name> <thumb-entry-address-hex> <nargs>
 * Lines starting with '#' are ignored.
 */
#ifndef HW_ARM_GARMIN_GL_H
#define HW_ARM_GARMIN_GL_H

#include "qemu/osdep.h"
#include "system/memory.h"

/* Base of the hook MMIO window (overlaid on the SGX register window, which
 * the firmware maps as device memory; 4 KiB: slots 0..0x3fe, result 0xffc). */
#define GARMIN_GL_MMIO_BASE   0x5600f000
#define GARMIN_GL_MMIO_SIZE   0x1000
#define GARMIN_GL_RESULT_OFF  0xffc
#define GARMIN_GL_MAX_HOOKS   (GARMIN_GL_RESULT_OFF / 4)

/* Load the hook table; returns number of hooks or -1. Safe to call before
 * the machine has RAM: patching happens at reset via garmin_gl_reset(). */
int garmin_gl_load_table(const char *path, Error **errp);

/* Map the MMIO trap window. */
void garmin_gl_mmio_init(MemoryRegion *sysmem);

/* Reset hook: write the trampolines over the driver entry points. Must run
 * after the ROM/firmware blobs were (re)loaded, i.e. register it after
 * load_image_targphys(). */
void garmin_gl_reset(void *opaque);

/* Framebuffer geometry the renderer targets (RGB565). */
void garmin_gl_set_display(uint32_t width, uint32_t height);

/* Current scan-out buffer, updated by the DISPC model on GFX_BA0 writes. */
void garmin_gl_set_scanout(uint32_t paddr);

extern bool garmin_gl_log;

#endif

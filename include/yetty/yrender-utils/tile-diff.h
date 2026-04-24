/*
 * tile-diff.h - GPU-accelerated tile-granularity frame diff engine.
 *
 * Compares a submitted texture against the previously-submitted one using
 * a compute shader, reads back both the rendered pixels and a per-tile
 * dirty bitmap, and hands the result to a caller-supplied sink. The sink
 * decides what to do with the dirty tiles: the VNC server encodes them and
 * ships them over the wire; a future X11-tile render target will XShmPutImage
 * them to a window; a debug tool could dump them to disk.
 *
 * The engine is output-agnostic. It owns the GPU pipeline, the previous-
 * frame texture, the dirty-flag storage buffer, the row-aligned readback
 * buffer, and the coroutine that awaits the maps. It does not know anything
 * about VNC, X11, or terminals.
 */

#ifndef YETTY_YRENDER_UTILS_TILE_DIFF_H
#define YETTY_YRENDER_UTILS_TILE_DIFF_H

#include <yetty/ycore/result.h>
#include <yetty/yplatform/ywebgpu.h>
#include <webgpu/webgpu.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct yetty_yrender_utils_tile_diff_engine;

YETTY_YRESULT_DECLARE(yetty_yrender_utils_tile_diff_engine_ptr,
                      struct yetty_yrender_utils_tile_diff_engine *);

/*
 * A single readback result, valid only for the duration of the sink callback.
 *
 * `pixels` is the mapped GPU buffer: BGRA8 rows padded to `aligned_bytes_per_row`
 * (the WebGPU 256-byte row alignment). Use `width * 4` to step within a row.
 * `dirty_bitmap` has one byte per tile in row-major order; 1 means dirty.
 */
struct yetty_yrender_utils_tile_diff_frame {
    const uint8_t *pixels;
    uint32_t aligned_bytes_per_row;
    uint32_t width;
    uint32_t height;
    uint32_t tile_size;
    uint32_t tiles_x;
    uint32_t tiles_y;
    const uint8_t *dirty_bitmap;
    uint32_t dirty_count;
};

typedef void (*yetty_yrender_utils_tile_diff_sink_fn)(
    void *ctx,
    const struct yetty_yrender_utils_tile_diff_frame *frame);

/*
 * Create the engine. `device`, `queue`, and `wgpu` are borrowed — the caller
 * must keep them alive for the engine's lifetime. `tile_size` is the edge
 * length (in pixels) of a single tile; must match the compute shader's
 * expectation (64 today).
 */
struct yetty_yrender_utils_tile_diff_engine_ptr_result
yetty_yrender_utils_tile_diff_engine_create(WGPUDevice device,
                                            WGPUQueue queue,
                                            struct yplatform_wgpu *wgpu,
                                            uint32_t tile_size);

void yetty_yrender_utils_tile_diff_engine_destroy(
    struct yetty_yrender_utils_tile_diff_engine *eng);

/* Mark the next submit as full-frame (all tiles dirty). Used by callers on
 * state they know invalidates the delta — e.g. a new VNC client connecting. */
void yetty_yrender_utils_tile_diff_engine_force_full(
    struct yetty_yrender_utils_tile_diff_engine *eng);

/* Force every submit to report all tiles dirty. Useful for testing/debug. */
void yetty_yrender_utils_tile_diff_engine_set_always_full(
    struct yetty_yrender_utils_tile_diff_engine *eng, bool on);

/*
 * Submit a texture for diffing + readback.
 *
 * Returns immediately after spawning a coroutine that runs the GPU diff
 * pass, awaits the readbacks through `wgpu`, and invokes `sink_fn(sink_ctx, frame)`
 * exactly once with the frame details. The sink callback runs on the event
 * loop thread; it must consume or copy the pixels before returning (the
 * mapped ranges are unmapped immediately afterwards).
 *
 * The engine consumes `texture` only during the synchronous prologue of the
 * coro (the encode + submit), so the caller does not need to keep it alive
 * past this call.
 */
struct yetty_ycore_void_result
yetty_yrender_utils_tile_diff_engine_submit(
    struct yetty_yrender_utils_tile_diff_engine *eng,
    WGPUTexture texture, uint32_t width, uint32_t height,
    yetty_yrender_utils_tile_diff_sink_fn sink_fn, void *sink_ctx);

#ifdef __cplusplus
}
#endif

#endif /* YETTY_YRENDER_UTILS_TILE_DIFF_H */

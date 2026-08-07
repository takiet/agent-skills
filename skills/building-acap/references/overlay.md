# Axoverlay 2 — Overlay API

Draw dynamic graphics (text, shapes, icons, custom rasters) on top of video streams. Version
2 has **no GLib/GMainLoop dependency of its own** and no built-in Cairo requirement — you own
the event loop and choose any graphics toolkit. You render into ARGB32 buffers that the
overlay system composites onto matching streams.

**API specification:** https://developer.axis.com/acap/acap-native-sdk-version-12/api/src/api/axoverlay_v2/html/index.html
(legacy v1: https://developer.axis.com/acap/acap-native-sdk-version-12/api/src/api/axoverlay_v1/html/index.html)

> For rectangles/paths on analytics results **that need no text**, [bbox.md](bbox.md) is much
> simpler. But bbox has no text-drawing API at all, so anything with labels — the usual object
> detection case — belongs here. Use Axoverlay 2 whenever you need control over pixels (text,
> images, gauges).

**Official example:** [axoverlay2-skia](https://github.com/AxisCommunications/acap-native-sdk-examples/tree/main/axoverlay2-skia)
— the reference implementation, and the one to copy build configuration from.

## Build Requirements

### Makefile

```make
PKGS = gio-2.0 glib-2.0 cairo vdostream axoverlay2
```

### Source files

```c
#include <axoverlay2.h>
#include <cairo/cairo.h>   // optional; any toolkit works
#include <vdo-stream.h>    // to discover streams
```

### manifest.json
declare the overlay resource:

```json
"resources": {
  "overlay": { "enabled": true, "required": true }
}
```

If you render on the GPU (see [GPU path](#gpu-path--zero-copy-via-dma-buf)), you also need the
`gpu` group — without it the app starts but EGL initialisation fails:

```json
"resources": {
  "overlay": { "enabled": true, "required": true },
  "linux": { "user": { "groups": ["gpu"] } }
}
```

## Core objects

| Type | Meaning |
|---|---|
| `axo_props` | Overlay properties: pixel format, size, upscale. |
| `axo_match` | Selects which stream(s) the overlay attaches to (e.g. by stream id). |
| `axo_buffer` | A drawable buffer obtained from the overlay system. |
| `axo_err` | Error object; read with `axo_err_get_code()` / `axo_err_get_message()`. |

## Lifecycle & stream discovery

Axoverlay 2 draws per stream, so you watch **VDO stream 0** to learn when streams that want
overlays appear/disappear, and create/remove an overlay for each:

```c
axo_err* aerr = NULL;
axo_start(NULL, &aerr);                                   // start overlay subsystem

// Watch stream 0 filtered to overlay-consuming streams
VdoStream* ev = vdo_stream_get(0, &error);
VdoMap* f = vdo_map_new();
vdo_map_set_string(f, "filter", "overlay");
vdo_stream_attach(ev, f, &error);
int efd = vdo_stream_get_event_fd(ev, &error);            // hook into your event loop

// In the event callback:
VdoMap* e = vdo_stream_get_event(ev, &error);
unsigned type = vdo_map_get_uint32(e, "event", 0);
unsigned sid  = vdo_map_get_uint32(e, "id", 0);
if (type == VDO_STREAM_EVENT_EXISTING || type == VDO_STREAM_EVENT_CREATED)
    create_overlay(sid, /* width,height from stream info */);
else if (type == VDO_STREAM_EVENT_CLOSED)
    remove_overlay(sid);

// On shutdown:
axo_stop(NULL);
```

## Creating an overlay

```c
// Sizes must be aligned — always compute padding with axo_get_aligned_size().
unsigned full_w, full_h;
axo_get_aligned_size(AXO_FORMAT_ARGB32, used_w, used_h, &full_w, &full_h, &aerr);

axo_props* props = axo_props_new();
axo_props_set_format(props, AXO_FORMAT_ARGB32);
axo_props_set_size(props, full_w, full_h);
axo_props_set_upscale_x2(props, use_upscale);             // draw at half-res for huge streams

axo_match* match = axo_match_new();
axo_match_stream_id(match, stream_id);

int overlay_id = axo_create_overlay(props, match, &aerr);
// overlay_id < 0 with code AXO_ERR_NO_STREAM => stream vanished; ignore, not an error.

axo_props_free(props);
axo_match_free(match);
```

## Drawing a frame

There are two ways to get pixels into the buffer, and the choice affects your manifest, your
Dockerfile and your build time — so make it before writing drawing code.

| | CPU path | GPU path |
|---|---|---|
| Toolkit | Cairo (in the SDK) | Skia + EGL/GLES (Skia **not** in the SDK) |
| Transfer | `memcpy` per frame | zero-copy via dma-buf |
| manifest | `resources.overlay` | + `linux.user.groups: ["gpu"]` |
| Build cost | none | ~6 min first build (see below) |

Start on the CPU path unless you need the GPU — at a few fps a `memcpy` of an ARGB32 buffer is
not what will limit you.

### CPU path — render, then copy

```c
axo_buffer* buf = axo_get_buffer(overlay_id, NULL, &aerr);
if (!buf) {
    axo_err_code c = axo_err_get_code(aerr);
    if (c == AXO_ERR_NO_STREAM || c == AXO_ERR_WAIT) { /* skip this frame */ }
}
char* target = axo_buffer_get_data(buf, &aerr);

// Draw into an OWN cairo surface, then memcpy into target.
// Overlay buffers are device memory; drawing directly from CPU may not work and caches
// may be incompatible. Keep a reusable cairo_surface_t per overlay.
render_with_cairo(surface);
memcpy(target, cairo_image_surface_get_data(surface), full_w * full_h * 4);

axo_submit_buffer(buf, NULL, &aerr);                      // show it
```

### GPU path — zero-copy via dma-buf

The overlay buffer is a dma-buf, and you can render into it directly on the GPU instead of
copying into it. `axo_buffer_get_dma_buf_fd()` hands you the fd; import it as an EGL image and
wrap that in a GPU-backed Skia surface:

```
axo_get_buffer()
  └─ axo_buffer_get_dma_buf_fd()
       └─ eglCreateImageKHR(..., EGL_LINUX_DMA_BUF_EXT, attribs)   // fd, format, stride, offset
            └─ glEGLImageTargetTexture2DOES()  -> GL texture
                 └─ SkSurfaces::WrapBackendRenderTarget()  -> draw with Skia
                      └─ flushAndSubmit(GrSyncCpu::kYes)
                           └─ axo_submit_buffer()
```

Request a GPU-suitable format up front — compression is recommended when rendering on the GPU:

```c
axo_format_flags flags =
    (axo_format_flags)(AXO_FORMAT_FLAGS_COMPRESSED | AXO_FORMAT_FLAGS_GPU);
axo_detailed_format* format = axo_suggest_detailed_format(AXO_FORMAT_ARGB32, flags, &aerr);
```

Take the EGL attribute list and the exact Skia surface construction from the `axoverlay2-skia`
example rather than reconstructing them — the failure mode for a wrong attribute is a black or
garbled overlay with no error, which is slow to diagnose.

Note that the flush is not optional: Skia records commands, and without
`flushAndSubmit(GrSyncCpu::kYes)` before `axo_submit_buffer()` you can submit a buffer the GPU
hasn't finished writing.

### Building Skia

Skia is **not part of the ACAP SDK and not on the device** — no header, no pkg-config file, no
library. Nothing tells you this until you look for it, so budget for it when someone specifies
Skia: you build it from source inside the Dockerfile.

```dockerfile
# Roughly: clone, fetch deps, configure, build. Take the gn args from axoverlay2-skia.
ARG SKIA_VERSION=chrome/m137
RUN git clone --depth 1 -b ${SKIA_VERSION} https://skia.googlesource.com/skia.git && \
    cd skia && python3 tools/git-sync-deps && \
    bin/gn gen out/Release --args='...' && \
    ninja -C out/Release skia
```

Measured on SDK 12.11.0 / `chrome/m137`, cross-compiling for aarch64:

| | |
|---|---|
| Skia compile | 353.5 s |
| Full `make build` | 6 min 21 s |
| `libskia.a` | 21.9 MiB |
| Rebuild, app code only | 34 s |

That last row is the point: keep the Skia stage **above `COPY app`** and make it depend only on
`SKIA_VERSION`. Docker's layer cache then rebuilds Skia only when you change the version, and a
single Dockerfile is enough — no multi-file split or external prebuilt image needed. Get this
ordering wrong and every code change costs six minutes.

## Cleanup

```c
axo_remove_overlay(overlay_id, &aerr);
axo_err_clear(&aerr);
```

## Notes & gotchas

- **Always align sizes** with `axo_get_aligned_size()`; raw computed sizes are often invalid.
  Clear the padding pixels to transparency.
- **On the CPU path, don't draw straight into `axo_buffer` memory** — render into a private
  (Cairo) surface and `memcpy`. Reuse the surface across frames for efficiency. This does *not*
  apply to the GPU path, where rendering into the buffer's dma-buf is exactly the point.
- **Overlays composite onto a viewer's stream**, so nothing is drawn while no one is watching.
  For hands-free testing, have your test binary open its own H.264 stream — otherwise every
  check needs someone with a live view open.
- Handle `AXO_ERR_NO_STREAM` (stream closed) and `AXO_ERR_WAIT` (buffer not yet free) as
  normal conditions, not fatal errors.
- Enable `axo_props_set_upscale_x2()` for very large streams (e.g. > 4 MP) to halve draw cost.
- Legacy v1 (`axoverlay_v1.h`, Cairo/GLib-bound) still exists but v2 is preferred for new code.

## Related

- Discover streams → [vdo.md](vdo.md)
- Simple analytics boxes, no text → [bbox.md](bbox.md)
- Verifying that text actually rendered (known issue with Skia's glyph atlas on ARTPEC-9, and a
  glyph-path workaround) → https://github.com/takiet/agent-skills/issues/2

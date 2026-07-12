# Axoverlay 2 — Overlay API

Draw dynamic graphics (text, shapes, icons, custom rasters) on top of video streams. Version
2 has **no GLib/GMainLoop dependency of its own** and no built-in Cairo requirement — you own
the event loop and choose any graphics toolkit. You render into ARGB32 buffers that the
overlay system composites onto matching streams.

**API specification:** https://developer.axis.com/acap/acap-native-sdk-version-12/api/src/api/axoverlay_v2/html/index.html
(legacy v1: https://developer.axis.com/acap/acap-native-sdk-version-12/api/src/api/axoverlay_v1/html/index.html)

> For simple rectangles/paths on analytics results, [bbox.md](bbox.md) is much simpler.
> Use Axoverlay 2 when you need full control over pixels (custom text, images, gauges).

## Build & manifest

```make
PKGS = gio-2.0 glib-2.0 cairo vdostream axoverlay2
```

```c
#include <axoverlay2.h>
#include <cairo/cairo.h>   // optional; any toolkit works
#include <vdo-stream.h>    // to discover streams
```

`manifest.json` — declare the overlay resource:

```json
"resources": { "overlay": { "enabled": true, "required": true } }
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

## Cleanup

```c
axo_remove_overlay(overlay_id, &aerr);
axo_err_clear(&aerr);
```

## Notes & gotchas

- **Always align sizes** with `axo_get_aligned_size()`; raw computed sizes are often invalid.
  Clear the padding pixels to transparency.
- **Don't draw straight into `axo_buffer` memory** — render into a private (Cairo) surface and
  `memcpy`. Reuse the surface across frames for efficiency.
- Handle `AXO_ERR_NO_STREAM` (stream closed) and `AXO_ERR_WAIT` (buffer not yet free) as
  normal conditions, not fatal errors.
- Enable `axo_props_set_upscale_x2()` for very large streams (e.g. > 4 MP) to halve draw cost.
- Legacy v1 (`axoverlay_v1.h`, Cairo/GLib-bound) still exists but v2 is preferred for new code.

## Related

- Discover streams → [vdo.md](vdo.md)
- Simple analytics boxes → [bbox.md](bbox.md)

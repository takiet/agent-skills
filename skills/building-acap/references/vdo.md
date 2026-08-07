# VDO — Video Capture API

Capture video and still images from the device. VDO is the entry point for almost any
video analytics application: it delivers encoded (H.264, H.265, AV1, JPEG, AVIF) or
un-encoded (NV12/YUV, Y800, RGB) frames from a sensor channel.

**API specification:** https://developer.axis.com/acap/acap-native-sdk-version-12/api/src/api/vdostream/html/index.html

## Build Requirements

### Makefile

```make
PKGS = gio-2.0 gio-unix-2.0 vdostream
```

### Source files

```c
#include <vdo-stream.h>
#include <vdo-map.h>
#include <vdo-types.h>
#include <vdo-error.h>
```

### manifest.json
the app user must belong to the `video` group:

```json
"resources": {
  "linux": { "user": { "groups": ["video"] } }
}
```

## Core objects

| Type | Meaning |
|---|---|
| `VdoStream` | A video stream owned by the client. Configured via a `VdoMap`. |
| `VdoChannel` | Video from one (or several composited) image sensors. Stream 0 is a magic pseudo-stream that reports events about all other streams. |
| `VdoMap` | Hash map with variant values; used for both settings and info/metadata. |
| `VdoBuffer` | A reference to a frame buffer owned by the VDO service. Must be returned with `vdo_stream_buffer_unref()`. |
| `VdoFrame` | Frame metadata (type, sequence number, timestamp, size). Its lifetime is tied to its `VdoBuffer`. |

## Typical workflow (continuous capture)

```c
// 1. Build settings map
g_autoptr(VdoMap) settings = vdo_map_new();
vdo_map_set_uint32(settings, "format", VDO_FORMAT_H264);   // or YUV/JPEG/RGB...
vdo_map_set_string(settings, "subformat", "NV12");         // only for VDO_FORMAT_YUV
vdo_map_set_uint32(settings, "channel", 1);
VdoPair32u resolution = { .w = 640, .h = 360 };
vdo_map_set_pair32u(settings, "resolution", resolution);
vdo_map_set_double(settings, "framerate", 30.0);
vdo_map_set_uint32(settings, "buffer.count", 2);           // keep low to save memory

// 2. Create + start the stream
g_autoptr(GError) error     = NULL;
g_autoptr(VdoStream) stream = vdo_stream_new(settings, NULL, &error);
g_autoptr(VdoMap) info      = vdo_stream_get_info(stream, &error);   // actual w/h/framerate
vdo_stream_start(stream, &error);

// 3. Pull buffers in a loop
for (;;) {
    g_autoptr(VdoBuffer) buffer = vdo_stream_get_buffer(stream, &error);
    if (!buffer && g_error_matches(error, VDO_ERROR, VDO_ERROR_NO_DATA)) {
        g_clear_error(&error);
        continue;                       // transient, retry
    }
    if (!buffer) { /* handle_vdo_failed(error) */ }

    VdoFrame* frame = vdo_buffer_get_frame(buffer);   // do NOT free frame
    // ... access frame data (see below) ...

    // 4. Return the buffer so the service can reuse it
    vdo_stream_buffer_unref(stream, &buffer, &error);
}
```

## Reading frame data

Encoded frames may be split into chunks:

```c
VdoFrame* frame = vdo_buffer_get_frame(buffer);
if (vdo_buffer_is_contiguous(frame)) {
    VdoChunk chunk = vdo_frame_take_chunk(frame, &error);   // single chunk
    fwrite(chunk.data, chunk.size, 1, f);
} else {
    for (;;) {                                              // multiple chunks
        VdoChunk chunk = vdo_frame_take_chunk(frame, &error);
        if (chunk.type == VDO_CHUNK_ERROR) break;
        if (chunk.size == 0u) break;
        fwrite(chunk.data, chunk.size, 1, f);
    }
}
```
Enable chunked frames with `vdo_map_set_boolean(settings, "frame.chunks", true);`.

For un-encoded frames destined for larod, pass the dma-buf fd directly:
`vdo_buffer_get_fd()`, `vdo_buffer_get_offset()`, `vdo_buffer_get_capacity()` — see [larod.md](larod.md).

## Frame metadata helpers

- `vdo_frame_get_frame_type()` → `VDO_FRAME_TYPE_H264_IDR/_I/_P`, `..._JPEG`, `..._YUV`, `..._RGB`, `..._AVIF`, `..._AV1_KEY/_INTER`.
- `vdo_frame_get_sequence_nbr()`, `vdo_frame_get_size()`, `vdo_frame_get_timestamp()` (PTS in µs).

## Formats (`format` setting)

`VDO_FORMAT_H264`, `VDO_FORMAT_H265`, `VDO_FORMAT_AV1`, `VDO_FORMAT_JPEG`,
`VDO_FORMAT_AVIF`, `VDO_FORMAT_YUV` (+ `subformat` = `"NV12"` / `"Y800"`),
`VDO_FORMAT_RGB`, `VDO_FORMAT_PLANAR_RGB`.

## Single snapshot

For exactly one image, skip the stream lifecycle:

```c
g_autoptr(VdoBuffer) buffer = vdo_stream_snapshot(settings, &error);
```

## Non-blocking capture (event-driven apps)

Add `vdo_map_set_boolean(settings, "socket.blocking", false);` and poll the fd from
`vdo_stream_get_fd()` (or integrate `vdo_stream_get_event_fd()` into a GLib main loop via
`g_io_add_watch`). `vdo_stream_get_buffer()` then returns immediately.

## Notes & gotchas

- Graceful shutdown / EINTR. ACAP apps receive SIGTERM on stop. If a blocking vdo_stream_get_buffer() is interrupted by the signal it fails with EINTR ("Interrupted system call") — this is expected during shutdown, not a real error. Install a SIGTERM/SIGINT handler that clears a running flag, and in the buffer-fetch failure path treat EINTR (alongside VDO_ERROR_NO_DATA / vdo_error_is_expected()) as benign: if !running, break the loop quietly instead of logging it at LOG_ERR. Call vdo_stream_stop() after the loop.
- **Always `vdo_stream_buffer_unref()`** every buffer you get, or the stream starves.
- **`g_autoptr(VdoBuffer)` + `vdo_stream_buffer_unref()` is not a double free.** `vdo_stream_buffer_unref()` takes `&buffer` and sets the pointer to `NULL` after releasing it, so when the `g_autoptr` cleanup runs at end of scope it acts on a `NULL` pointer and does nothing. Using both together is safe and is the recommended pattern: `g_autoptr` guarantees the buffer is released even on an early `break`/`return`, while the explicit unref returns it to VDO promptly inside the loop.
- Treat `VDO_ERROR_NO_DATA` as transient (retry); use `vdo_error_is_expected()` to
  distinguish benign errors (maintenance, global rotation) from fatal ones.
- Use `vdo_map_dump()` to log a settings/info map while debugging.
- `AVIF` is intended for single frames only.
- Prefer `g_autoptr(VdoStream/VdoBuffer/VdoMap)` to avoid leaks.

## Related

- Run inference on captured frames → [larod.md](larod.md)
- Draw results back on the stream → [overlay.md](overlay.md), [bbox.md](bbox.md)

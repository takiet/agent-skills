#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

// One NV12 1920x1080 frame, captured once at startup and reused for every
// benchmark iteration. The underlying VdoStream/VdoBuffer are kept alive for
// the lifetime of the process (see capture_shutdown()) since `fd` refers to
// memory owned by the vdo service.
typedef struct {
    int      fd;         // fd backing the buffer, owned by vdo (do not close)
    int64_t  offset;      // byte offset into fd where pixel data starts
    uint32_t capacity;    // total bytes available at fd/offset
    uint32_t pitch;       // row stride in bytes, as reported by vdo (may exceed width)
    uint32_t width;       // actual stream width, as reported by vdo
    uint32_t height;      // actual stream height, as reported by vdo
    bool     is_dmabuf;   // true if buffer.type == "dmabuf", false otherwise (vmem)
} CapturedFrame;

// Captures a single NV12 frame at the given resolution. Logs the negotiated
// resolution, pitch and buffer.type (as reported by vdo_stream_get_info()) to
// syslog. Returns true on success and fills *out.
bool capture_nv12_frame(uint32_t width, uint32_t height, CapturedFrame* out);

// Releases the VdoStream/VdoBuffer captured by capture_nv12_frame(). After
// this call, the fd in the CapturedFrame returned earlier is no longer valid.
void capture_shutdown(void);

#endif // CAPTURE_H

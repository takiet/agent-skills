#include "capture.h"

#include <glib.h>
#include <string.h>
#include <syslog.h>

#include <vdo-map.h>
#include <vdo-stream.h>
#include <vdo-types.h>

// The stream and the buffer it produced are kept alive for the whole process
// lifetime: the CapturedFrame's fd points into memory owned by the vdo
// service and becomes invalid as soon as either is released.
static VdoStream* g_stream = NULL;
static VdoBuffer*  g_buffer = NULL;

bool capture_nv12_frame(uint32_t width, uint32_t height, CapturedFrame* out) {
    memset(out, 0, sizeof(*out));

    GError* error = NULL;

    g_autoptr(VdoMap) settings = vdo_map_new();
    vdo_map_set_uint32(settings, "format", VDO_FORMAT_YUV);
    vdo_map_set_string(settings, "subformat", "NV12");
    VdoPair32u resolution = { .w = width, .h = height };
    vdo_map_set_pair32u(settings, "resolution", resolution);
    vdo_map_set_uint32(settings, "buffer.count", 2);

    g_stream = vdo_stream_new(settings, NULL, &error);
    if (!g_stream) {
        syslog(LOG_ERR, "capture: vdo_stream_new failed: %s",
               error ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }

    if (!vdo_stream_start(g_stream, &error)) {
        syslog(LOG_ERR, "capture: vdo_stream_start failed: %s",
               error ? error->message : "unknown error");
        g_clear_error(&error);
        g_clear_object(&g_stream);
        return false;
    }

    g_autoptr(VdoMap) info = vdo_stream_get_info(g_stream, &error);
    if (!info) {
        syslog(LOG_ERR, "capture: vdo_stream_get_info failed: %s",
               error ? error->message : "unknown error");
        g_clear_error(&error);
        vdo_stream_stop(g_stream);
        g_clear_object(&g_stream);
        return false;
    }

    uint32_t actual_width  = vdo_map_get_uint32(info, "width", width);
    uint32_t actual_height = vdo_map_get_uint32(info, "height", height);
    uint32_t pitch         = vdo_map_get_uint32(info, "pitch", 0);
    const char* subformat  = vdo_map_get_string(info, "subformat", NULL, "unknown");
    const char* buftype    = vdo_map_get_string(info, "buffer.type", NULL, "unknown");

    // syslog only: capture.c is linked into test_preproc too, whose stdout
    // must contain nothing but raw image bytes (see PLAN.md 2-B). Callers
    // that want a stdout echo (e.g. ppcomp's own summary) do it themselves.
    syslog(LOG_INFO,
           "capture: stream info width=%u height=%u pitch=%u subformat=%s buffer.type=%s",
           actual_width, actual_height, pitch, subformat, buftype);

    g_buffer = vdo_stream_get_buffer(g_stream, &error);
    if (!g_buffer) {
        syslog(LOG_ERR, "capture: vdo_stream_get_buffer failed: %s",
               error ? error->message : "unknown error");
        g_clear_error(&error);
        vdo_stream_stop(g_stream);
        g_clear_object(&g_stream);
        return false;
    }

    out->fd        = vdo_buffer_get_fd(g_buffer);
    out->offset    = vdo_buffer_get_offset(g_buffer);
    out->capacity  = (uint32_t)vdo_buffer_get_capacity(g_buffer);
    out->pitch     = pitch;
    out->width     = actual_width;
    out->height    = actual_height;
    out->is_dmabuf = (g_strcmp0(buftype, "dmabuf") == 0);

    syslog(LOG_INFO,
           "capture: got buffer fd=%d offset=%lld capacity=%u",
           out->fd, (long long)out->offset, out->capacity);

    return true;
}

void capture_shutdown(void) {
    GError* error = NULL;
    if (g_stream && g_buffer) {
        if (!vdo_stream_buffer_unref(g_stream, &g_buffer, &error)) {
            syslog(LOG_WARNING, "capture: vdo_stream_buffer_unref failed: %s",
                   error ? error->message : "unknown error");
            g_clear_error(&error);
        }
    }
    if (g_stream) {
        vdo_stream_stop(g_stream);
        g_clear_object(&g_stream);
    }
}

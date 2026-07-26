// Test binary: draw three fixed boxes with labels for a minute, so the result
// can be checked by eye in the live view. Nothing is drawn while no one is
// viewing, hence the wait: the user needs time to open the stream.
#include <cstdio>
#include <syslog.h>
#include <unistd.h>

#include <vdo-map.h>
#include <vdo-stream.h>
#include <vdo-types.h>

#include "overlay.h"

// The coordinate space the detector works in (see capture.cc).
#define CONTENT_WIDTH 640
#define CONTENT_HEIGHT 360

#define HOLD_SECONDS 60
#define TICK_US 100000

// Corners in the 640x360 capture frame: top-left, centre, and one crossing the
// bottom edge so the clamping is visible too.
static const Detection kBoxes[] = {
    {40.0f, 30.0f, 200.0f, 150.0f, 0.91f, 0, "top-left"},
    {240.0f, 120.0f, 400.0f, 240.0f, 0.75f, 1, "centre"},
    {430.0f, 260.0f, 620.0f, 420.0f, 0.55f, 2, "bottom-right"},
};

// An encoded stream of our own, so the overlay has something to draw on even
// when nobody has the live view open. Without this the test can only be run
// while a human is watching.
static VdoStream* start_self_view(void) {
    g_autoptr(GError) error    = NULL;
    g_autoptr(VdoMap) settings = vdo_map_new();
    vdo_map_set_uint32(settings, "format", VDO_FORMAT_H264);
    vdo_map_set_uint32(settings, "channel", 1);
    VdoPair32u resolution = {{CONTENT_WIDTH, CONTENT_HEIGHT}};
    vdo_map_set_pair32u(settings, "resolution", resolution);
    vdo_map_set_double(settings, "framerate", 5.0);

    VdoStream* stream = vdo_stream_new(settings, NULL, &error);
    if (!stream || !vdo_stream_start(stream, &error)) {
        syslog(LOG_WARNING,
               "test_overlay: no self view stream: %s",
               error ? error->message : "unknown");
        if (stream) {
            g_object_unref(stream);
        }
        return NULL;
    }
    syslog(LOG_INFO, "test_overlay: started a self view stream");
    return stream;
}

int main(void) {
    openlog("yolov5_detector", LOG_PID, LOG_USER);

    VdoStream* self_view = start_self_view();
    Overlay* overlay     = overlay_start(CONTENT_WIDTH, CONTENT_HEIGHT);
    if (!overlay) {
        fprintf(stderr, "test_overlay: overlay_start failed\n");
        return 1;
    }

    printf("drawing %zu boxes for %d seconds, open the live view now\n",
           sizeof(kBoxes) / sizeof(kBoxes[0]),
           HOLD_SECONDS);
    fflush(stdout);

    unsigned ticks = HOLD_SECONDS * 1000000 / TICK_US;
    for (unsigned i = 0; i < ticks; i++) {
        if (!overlay_draw(overlay, kBoxes, sizeof(kBoxes) / sizeof(kBoxes[0]))) {
            fprintf(stderr, "test_overlay: overlay_draw failed\n");
            overlay_stop(overlay);
            return 1;
        }
        // Once a second, so "nothing is visible" can be told apart from "nobody
        // is viewing".
        if (i % (1000000 / TICK_US) == 0) {
            syslog(LOG_INFO,
                   "test_overlay: %us elapsed, %zu viewer stream(s)",
                   i / (1000000 / TICK_US),
                   overlay_stream_count(overlay));
        }
        usleep(TICK_US);
    }

    overlay_stop(overlay);
    if (self_view) {
        vdo_stream_stop(self_view);
        g_object_unref(self_view);
    }
    printf("done\n");
    closelog();
    return 0;
}

// Test binary: capture and preprocess frames, write the last RGB result to
// stdout. Logs go to stderr/syslog so run.sh's stdout capture stays a clean dump.
#include <cstdio>
#include <syslog.h>

#include "capture.h"

// Discard the first frames so auto-exposure has settled on the dumped one.
#define WARMUP_FRAMES 4

int main(void) {
    // Log under the app name so view_log.sh (filters on appname) shows it.
    openlog("yolov5_detector", LOG_PID, LOG_USER);

    Capture* capture = capture_start(640, 360, 640, 640, 10.0);
    if (!capture) {
        fprintf(stderr, "test_capture: capture_start failed\n");
        return 1;
    }

    const uint8_t* rgb = NULL;
    for (int i = 0; i <= WARMUP_FRAMES; i++) {
        rgb = capture_next(capture);
        if (!rgb) {
            fprintf(stderr, "test_capture: capture_next %d failed\n", i);
            capture_stop(capture);
            return 1;
        }
    }

    size_t size = capture_output_size(capture);
    fprintf(stderr,
            "test_capture: %zu bytes, %u content rows, output fd %d\n",
            size,
            capture_content_height(capture),
            capture_output_fd(capture));
    syslog(LOG_INFO,
           "test_capture: %zu bytes, %u content rows",
           size,
           capture_content_height(capture));

    size_t written = fwrite(rgb, 1, size, stdout);
    fflush(stdout);
    capture_stop(capture);
    closelog();
    return written == size ? 0 : 1;
}

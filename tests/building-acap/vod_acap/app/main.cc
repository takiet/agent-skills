// Capture -> detect -> overlay, repeated for the configured number of loops.
#include <syslog.h>

#include <axsdk/axparameter.h>
#include <glib.h>

#include "capture.h"
#include "detector.h"
#include "overlay.h"

#define APP_DIR "/usr/local/packages/yolov5_detector/"
#define MODEL_PATH APP_DIR "models/yolov5s.tflite"
#define LABELS_PATH APP_DIR "models/labels.txt"

// The camera is captured in 16:9 and letterboxed into the model's square input.
#define SRC_WIDTH 640
#define SRC_HEIGHT 360
#define MODEL_WIDTH 640
#define MODEL_HEIGHT 640

#define MAX_DETECTIONS 32

// Used when a parameter is missing or holds something unusable.
#define DEFAULT_FRAME_RATE 1
#define DEFAULT_LOOP_COUNT 10

#define MIN_FRAME_RATE 1
#define MAX_FRAME_RATE 10

// Read once at startup: changes take effect on restart, and the app does not
// restart itself (design.md).
static guint64 read_uint_param(AXParameter* params,
                               const char* name,
                               guint64 fallback,
                               guint64 min,
                               guint64 max) {
    g_autoptr(GError) error = NULL;
    g_autofree gchar* value = NULL;
    if (!ax_parameter_get(params, name, &value, &error)) {
        syslog(LOG_WARNING,
               "main: cannot read %s (%s), using %" G_GUINT64_FORMAT,
               name,
               error->message,
               fallback);
        return fallback;
    }
    gchar* end     = NULL;
    guint64 parsed = g_ascii_strtoull(value, &end, 10);
    // Never run with a silently different number than the one configured.
    if (end == value || *end != '\0' || parsed < min || parsed > max) {
        syslog(LOG_WARNING,
               "main: %s is \"%s\", outside %" G_GUINT64_FORMAT "-%" G_GUINT64_FORMAT
               ", using %" G_GUINT64_FORMAT,
               name,
               value,
               min,
               max,
               fallback);
        return fallback;
    }
    // The settings page cannot carry an explanation (the manifest schema has no
    // description field), so the log spells out what 0 means.
    if (parsed == 0) {
        syslog(LOG_INFO, "main: %s = 0 (infinite)", name);
    } else {
        syslog(LOG_INFO, "main: %s = %" G_GUINT64_FORMAT, name, parsed);
    }
    return parsed;
}

int main(void) {
    openlog("yolov5_detector", LOG_PID, LOG_USER);

    unsigned frame_rate = DEFAULT_FRAME_RATE;
    guint64 loop_count  = DEFAULT_LOOP_COUNT;

    g_autoptr(GError) error = NULL;
    AXParameter* params     = ax_parameter_new("yolov5_detector", &error);
    if (params) {
        frame_rate = (unsigned)read_uint_param(params,
                                               "FrameRate",
                                               DEFAULT_FRAME_RATE,
                                               MIN_FRAME_RATE,
                                               MAX_FRAME_RATE);
        // 0 means run until stopped.
        loop_count = read_uint_param(params, "LoopCount", DEFAULT_LOOP_COUNT, 0, G_MAXUINT64);
        ax_parameter_free(params);
    } else {
        syslog(LOG_WARNING, "main: ax_parameter_new failed (%s), using defaults", error->message);
    }

    Capture* capture =
        capture_start(SRC_WIDTH, SRC_HEIGHT, MODEL_WIDTH, MODEL_HEIGHT, (double)frame_rate);
    if (!capture) {
        syslog(LOG_ERR, "main: capture_start failed");
        return 1;
    }

    // Loading the model onto the DLPU takes the better part of a minute, so say
    // so rather than looking hung.
    syslog(LOG_INFO, "main: loading the model, this takes a while");
    Detector* detector = detector_start(MODEL_PATH,
                                        LABELS_PATH,
                                        DETECTOR_DEFAULT_DEVICE,
                                        capture_output_fd(capture),
                                        capture_output_size(capture),
                                        MODEL_WIDTH,
                                        MODEL_HEIGHT);
    Overlay* overlay = detector ? overlay_start(SRC_WIDTH, SRC_HEIGHT) : NULL;
    if (!detector || !overlay) {
        syslog(LOG_ERR, "main: %s failed", detector ? "overlay_start" : "detector_start");
        overlay_stop(overlay);
        detector_stop(detector);
        capture_stop(capture);
        return 1;
    }

    if (loop_count == 0) {
        syslog(LOG_INFO, "main: ready, %u fps, looping until stopped", frame_rate);
    } else {
        syslog(LOG_INFO,
               "main: ready, %u fps, %" G_GUINT64_FORMAT " loops",
               frame_rate,
               loop_count);
    }

    Detection detections[MAX_DETECTIONS];
    int status = 0;
    for (guint64 loop = 0; loop_count == 0 || loop < loop_count; loop++) {
        // Paced by the stream: this blocks until the next frame arrives.
        if (!capture_next(capture)) {
            syslog(LOG_ERR, "main: capture_next failed on loop %" G_GUINT64_FORMAT, loop);
            status = 1;
            break;
        }
        size_t num = detector_run(detector, detections, MAX_DETECTIONS);
        overlay_draw(overlay, detections, num);

        syslog(LOG_INFO, "main: loop %" G_GUINT64_FORMAT ", %zu detection(s)", loop, num);
        for (size_t i = 0; i < num; i++) {
            const Detection* d = &detections[i];
            syslog(LOG_INFO,
                   "main:   %s %.2f (%.0f,%.0f)-(%.0f,%.0f)",
                   d->label,
                   (double)d->score,
                   (double)d->x1,
                   (double)d->y1,
                   (double)d->x2,
                   (double)d->y2);
        }
    }

    syslog(LOG_INFO, "main: done");
    overlay_stop(overlay);
    detector_stop(detector);
    capture_stop(capture);
    closelog();
    return status;
}

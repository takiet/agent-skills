// Verification tool: runs one pre-processing scenario on one device and
// dumps the raw original NV12 frame followed by the raw processed buffer to
// stdout, back to back. syslog gets everything else (ident: "test_preproc",
// deliberately not "ppcomp" so its log doesn't mix with the main app's).
//
// Usage: test_preproc <device> <scenario> [crop_x crop_y]
//   device:   e.g. cpu-proc, a9-gpu-proc (see larodListDevices() in ppcomp)
//   scenario: crop | scale | rgb-i | rgb-p
//   crop_x/crop_y: top-left of the 300x300 crop window (even numbers only);
//                  only used for scenario "crop", default 800 400.

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <syslog.h>
#include <unistd.h>

#include "capture.h"
#include "preproc.h"

static bool dump_raw(const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(STDOUT_FILENO, p + written, size - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "test_preproc: write failed: %s",
                   strerror(errno));
            return false;
        }
        written += (size_t)n;
    }
    return true;
}

int main(int argc, char** argv) {
    openlog("test_preproc", LOG_PID, LOG_USER);

    if (argc < 3) {
        syslog(LOG_ERR,
               "usage: test_preproc <device> <scenario> [crop_x crop_y]");
        closelog();
        return EXIT_FAILURE;
    }
    const char* device_name = argv[1];

    PreprocScenario scenario;
    if (!preproc_scenario_from_name(argv[2], &scenario)) {
        syslog(LOG_ERR, "test_preproc: unknown scenario '%s'", argv[2]);
        closelog();
        return EXIT_FAILURE;
    }

    int crop_x = 800;
    int crop_y = 400;
    if (argc >= 5) {
        crop_x = atoi(argv[3]);
        crop_y = atoi(argv[4]);
    }

    larodError* error = NULL;
    larodConnection* conn = NULL;
    if (!larodConnect(&conn, &error)) {
        syslog(LOG_ERR, "test_preproc: larodConnect failed: %s",
               error ? error->msg : "unknown error");
        larodClearError(&error);
        closelog();
        return EXIT_FAILURE;
    }

    const larodDevice* device = larodGetDevice(conn, device_name, 0, &error);
    if (!device) {
        syslog(LOG_ERR, "test_preproc: larodGetDevice(%s) failed: %s",
               device_name, error ? error->msg : "unknown error");
        larodClearError(&error);
        larodDisconnect(&conn, NULL);
        closelog();
        return EXIT_FAILURE;
    }

    CapturedFrame frame;
    if (!capture_nv12_frame(1920, 1080, &frame)) {
        syslog(LOG_ERR, "test_preproc: capture_nv12_frame failed");
        larodDisconnect(&conn, NULL);
        closelog();
        return EXIT_FAILURE;
    }

    PreprocJob* job = NULL;
    if (!preproc_create(conn, device, scenario, &frame, &job, &error)) {
        syslog(LOG_ERR, "test_preproc: preproc_create(%s, %s) failed: %s",
               device_name, argv[2], error ? error->msg : "unknown error");
        larodClearError(&error);
        capture_shutdown();
        larodDisconnect(&conn, NULL);
        closelog();
        return EXIT_FAILURE;
    }

    if (!preproc_prepare(job, conn, crop_x, crop_y, &error)) {
        syslog(LOG_ERR, "test_preproc: preproc_prepare failed: %s",
               error ? error->msg : "unknown error");
        larodClearError(&error);
        preproc_destroy(conn, &job);
        capture_shutdown();
        larodDisconnect(&conn, NULL);
        closelog();
        return EXIT_FAILURE;
    }

    if (!preproc_run(job, conn, &error)) {
        syslog(LOG_ERR, "test_preproc: preproc_run failed: %s",
               error ? error->msg : "unknown error");
        larodClearError(&error);
        preproc_destroy(conn, &job);
        capture_shutdown();
        larodDisconnect(&conn, NULL);
        closelog();
        return EXIT_FAILURE;
    }

    if (scenario == PREPROC_SCENARIO_CROP) {
        syslog(LOG_INFO, "test_preproc: crop via job params = %s",
               preproc_uses_job_params_for_crop(job) ? "true" : "false");
    }

    // mmap the original frame just long enough to dump it.
    void* orig = mmap(NULL, frame.capacity, PROT_READ, MAP_SHARED, frame.fd,
                       frame.offset);
    if (orig == MAP_FAILED) {
        syslog(LOG_ERR, "test_preproc: mmap(original frame) failed: %s",
               strerror(errno));
        preproc_destroy(conn, &job);
        capture_shutdown();
        larodDisconnect(&conn, NULL);
        closelog();
        return EXIT_FAILURE;
    }

    syslog(LOG_INFO,
           "test_preproc: device=%s scenario=%s crop_x=%d crop_y=%d "
           "original_bytes=%u processed_bytes=%zu",
           device_name, argv[2], crop_x, crop_y, frame.capacity,
           preproc_output_size(job));

    bool ok = dump_raw(orig, frame.capacity);
    ok = ok && dump_raw(preproc_output_data(job), preproc_output_size(job));

    munmap(orig, frame.capacity);
    preproc_destroy(conn, &job);
    capture_shutdown();
    larodDisconnect(&conn, &error);
    larodClearError(&error);
    closelog();

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ppcomp: benchmarks cpu-proc vs a9-gpu-proc pre-processing (crop, scale,
// convert to interleaved RGB, convert to planar RGB) per CLAUDE.md.
//
// One NV12 1920x1080 frame is captured at startup and reused for every
// iteration of every (scenario, device) combination. Each combination gets
// 3 warmup runs (excluded from stats, logged separately -- GPU's first run
// is typically much slower) followed by 50 measured runs. Only the
// larodRunJob() call is timed; tensor/model setup and any crop-fallback
// model reload happen in preproc_prepare(), outside the timed region. If a
// device rejects a scenario, that combination is logged as N/A with the
// larod error message and the run moves on -- see CLAUDE.md's "error
// handling" note: no retries, no crashes, just record the fact and proceed.

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "capture.h"
#include "larod.h"
#include "preproc.h"

#define NUM_WARMUP 3
#define NUM_MEASURED 50
#define NUM_CROP_POINTS (NUM_WARMUP + NUM_MEASURED)

// Crop window top-left ranges: 1920-300=1620, 1080-300=780. Rounded down to
// even since NV12 chroma is subsampled 2x2 (odd offsets are rejected or
// shift color planes).
#define CROP_MAX_X 1620
#define CROP_MAX_Y 780

static const char* const kDeviceNames[] = {"cpu-proc", "a9-gpu-proc"};
#define NUM_DEVICES (sizeof(kDeviceNames) / sizeof(kDeviceNames[0]))

static const PreprocScenario kScenarios[] = {
    PREPROC_SCENARIO_CROP,
    PREPROC_SCENARIO_SCALE,
    PREPROC_SCENARIO_RGB_INTERLEAVED,
    PREPROC_SCENARIO_RGB_PLANAR,
};
#define NUM_SCENARIOS (sizeof(kScenarios) / sizeof(kScenarios[0]))

static void log_both(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

static void log_both(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    syslog(LOG_INFO, "%s", line);
    printf("%s\n", line);
}

static bool list_larod_devices(larodConnection* conn) {
    larodError* error = NULL;
    size_t num_devices = 0;
    const larodDevice** devices = larodListDevices(conn, &num_devices, &error);
    if (!devices) {
        log_both("larod: larodListDevices failed: %s",
                 error ? error->msg : "unknown error");
        larodClearError(&error);
        return false;
    }

    log_both("larod: %zu device(s) found", num_devices);
    for (size_t i = 0; i < num_devices; i++) {
        const char* name = larodGetDeviceName(devices[i], &error);
        if (!name) {
            log_both("larod: device[%zu] name lookup failed: %s", i,
                     error ? error->msg : "unknown error");
            larodClearError(&error);
            continue;
        }
        uint32_t instance = 0;
        if (!larodGetDeviceInstance(devices[i], &instance, &error)) {
            log_both("larod: device[%zu] instance lookup failed: %s", i,
                     error ? error->msg : "unknown error");
            larodClearError(&error);
            continue;
        }
        log_both("larod: device[%zu] = %s (instance %u)", i, name, instance);
    }
    return true;
}

// Deterministic LCG (fixed seed), so the crop sequence used against
// cpu-proc and a9-gpu-proc is byte-for-byte identical.
static uint32_t lcg_state = 42u;

static uint32_t lcg_next(void) {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}

// Fills crop_x[n]/crop_y[n] with points in [0, CROP_MAX_X] / [0,
// CROP_MAX_Y], rounded down to even.
static void gen_crop_points(int* crop_x, int* crop_y, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t rx = lcg_next() % (CROP_MAX_X + 1);
        uint32_t ry = lcg_next() % (CROP_MAX_Y + 1);
        crop_x[i] = (int)(rx & ~1u);
        crop_y[i] = (int)(ry & ~1u);
    }
}

static double elapsed_ms(const struct timespec* start,
                          const struct timespec* end) {
    return (double)(end->tv_sec - start->tv_sec) * 1000.0 +
           (double)(end->tv_nsec - start->tv_nsec) / 1e6;
}

// Runs the full warmup+measured benchmark for one (scenario, device)
// combination and logs exactly one result line: either
// "<scenario> <device> mean=... sd=... (n=...)" or
// "<scenario> <device> N/A (<larod error>)".
static void run_benchmark(larodConnection* conn, const char* device_name,
                           PreprocScenario scenario,
                           const CapturedFrame* frame, const int* crop_x,
                           const int* crop_y) {
    const char* scenario_name = preproc_scenario_name(scenario);
    larodError* error = NULL;

    const larodDevice* device = larodGetDevice(conn, device_name, 0, &error);
    if (!device) {
        log_both("%-6s %-11s N/A (larodGetDevice: %s)", scenario_name,
                  device_name, error ? error->msg : "unknown error");
        larodClearError(&error);
        return;
    }

    PreprocJob* job = NULL;
    if (!preproc_create(conn, device, scenario, frame, &job, &error)) {
        log_both("%-6s %-11s N/A (preproc_create: %s)", scenario_name,
                  device_name, error ? error->msg : "unknown error");
        larodClearError(&error);
        return;
    }

    // Warmup: excluded from stats, logged individually as reference values.
    for (size_t i = 0; i < NUM_WARMUP; i++) {
        if (!preproc_prepare(job, conn, crop_x[i], crop_y[i], &error)) {
            log_both("%-6s %-11s N/A (warmup[%zu] prepare: %s)",
                      scenario_name, device_name, i,
                      error ? error->msg : "unknown error");
            larodClearError(&error);
            preproc_destroy(conn, &job);
            return;
        }
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        bool ok = preproc_run(job, conn, &error);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (!ok) {
            log_both("%-6s %-11s N/A (warmup[%zu] run: %s)", scenario_name,
                      device_name, i, error ? error->msg : "unknown error");
            larodClearError(&error);
            preproc_destroy(conn, &job);
            return;
        }
        log_both("%-6s %-11s warmup[%zu]=%.3f ms", scenario_name,
                  device_name, i, elapsed_ms(&t0, &t1));
    }

    // Measured runs.
    double times[NUM_MEASURED];
    const int* measured_x = crop_x + NUM_WARMUP;
    const int* measured_y = crop_y + NUM_WARMUP;
    for (size_t i = 0; i < NUM_MEASURED; i++) {
        if (!preproc_prepare(job, conn, measured_x[i], measured_y[i],
                              &error)) {
            log_both("%-6s %-11s N/A (measured[%zu] prepare: %s)",
                      scenario_name, device_name, i,
                      error ? error->msg : "unknown error");
            larodClearError(&error);
            preproc_destroy(conn, &job);
            return;
        }
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        bool ok = preproc_run(job, conn, &error);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (!ok) {
            log_both("%-6s %-11s N/A (measured[%zu] run: %s)", scenario_name,
                      device_name, i, error ? error->msg : "unknown error");
            larodClearError(&error);
            preproc_destroy(conn, &job);
            return;
        }
        times[i] = elapsed_ms(&t0, &t1);
    }

    double sum = 0.0;
    for (size_t i = 0; i < NUM_MEASURED; i++)
        sum += times[i];
    double mean = sum / (double)NUM_MEASURED;

    double sq_diff = 0.0;
    for (size_t i = 0; i < NUM_MEASURED; i++) {
        double d = times[i] - mean;
        sq_diff += d * d;
    }
    double sd = sqrt(sq_diff / (double)(NUM_MEASURED - 1)); // sample stddev

    log_both("%-6s %-11s mean=%.3f ms  sd=%.3f ms  (n=%d)", scenario_name,
              device_name, mean, sd, NUM_MEASURED);

    if (scenario == PREPROC_SCENARIO_CROP) {
        log_both("%-6s %-11s crop delivered via %s", scenario_name,
                  device_name,
                  preproc_uses_job_params_for_crop(job)
                      ? "job params (model loaded once)"
                      : "per-run model reload (fallback)");
    }

    preproc_destroy(conn, &job);
}

int main(void) {
    openlog("ppcomp", LOG_PID, LOG_USER);

    larodError* error = NULL;
    larodConnection* conn = NULL;
    if (!larodConnect(&conn, &error)) {
        log_both("larod: larodConnect failed: %s",
                 error ? error->msg : "unknown error");
        larodClearError(&error);
        closelog();
        return EXIT_FAILURE;
    }
    log_both("larod: connected");

    list_larod_devices(conn);

    CapturedFrame frame;
    if (!capture_nv12_frame(1920, 1080, &frame)) {
        log_both("capture: failed to capture NV12 frame");
        larodDisconnect(&conn, &error);
        larodClearError(&error);
        closelog();
        return EXIT_FAILURE;
    }
    log_both(
        "capture: summary width=%u height=%u pitch=%u capacity=%u "
        "is_dmabuf=%d",
        frame.width, frame.height, frame.pitch, frame.capacity,
        frame.is_dmabuf);

    // Same crop sequence for every (scenario, device) combination.
    int crop_x[NUM_CROP_POINTS];
    int crop_y[NUM_CROP_POINTS];
    gen_crop_points(crop_x, crop_y, NUM_CROP_POINTS);

    for (size_t s = 0; s < NUM_SCENARIOS; s++) {
        for (size_t d = 0; d < NUM_DEVICES; d++) {
            run_benchmark(conn, kDeviceNames[d], kScenarios[s], &frame,
                          crop_x, crop_y);
        }
    }

    capture_shutdown();

    larodDisconnect(&conn, &error);
    larodClearError(&error);

    closelog();
    return EXIT_SUCCESS;
}

#ifndef PREPROC_H
#define PREPROC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "capture.h"
#include "larod.h"

// The four benchmark scenarios from CLAUDE.md. Output geometry for each is
// fixed (see preproc.c): crop/scale produce a 300x300 NV12 buffer,
// rgb-interleaved/rgb-planar produce a full 1920x1080 RGB buffer.
typedef enum {
    PREPROC_SCENARIO_CROP = 0,
    PREPROC_SCENARIO_SCALE,
    PREPROC_SCENARIO_RGB_INTERLEAVED,
    PREPROC_SCENARIO_RGB_PLANAR,
} PreprocScenario;

// Short name used in logs and as the test_preproc CLI argument.
const char* preproc_scenario_name(PreprocScenario scenario);

// Parses a scenario name (as returned by preproc_scenario_name()). Returns
// false if `name` doesn't match any scenario.
bool preproc_scenario_from_name(const char* name, PreprocScenario* out);

typedef struct PreprocJob PreprocJob;

// Loads the pre-processing "model" (a larodMap, no file backing it) for
// `scenario` onto `device` and allocates the output buffer (memfd, sized per
// the scenario's fixed output geometry). Binds the (already captured) input
// frame's fd as the input tensor. All of this is meant to happen once,
// outside of any timed region.
bool preproc_create(larodConnection* conn, const larodDevice* device,
                     PreprocScenario scenario, const CapturedFrame* frame,
                     PreprocJob** out, larodError** error);

// Prepares `job` to be run with the given crop window. crop_x/crop_y select
// the top-left corner of the 300x300 crop window for PREPROC_SCENARIO_CROP
// (both must already be even, since NV12 chroma is subsampled 2x2); they
// are ignored for every other scenario, so this is a no-op for those.
//
// For CROP, the first call determines whether the device accepts
// image.input.crop via larodSetJobRequestParams() (fast path, cheap: just
// builds a small map and calls larodSetJobRequestParams()) or not (slow
// path: the model is reloaded with the crop baked into its map, on this and
// every subsequent call). See preproc_uses_job_params_for_crop().
//
// This is deliberately a separate call from preproc_run() so that callers
// benchmarking larodRunJob() can call preproc_prepare() outside the timed
// region and preproc_run() inside it -- otherwise a model reload on the
// slow path would dominate the measured time.
bool preproc_prepare(PreprocJob* job, larodConnection* conn, int crop_x,
                      int crop_y, larodError** error);

// Runs one pre-processing job (a single larodRunJob() call against the
// request most recently set up by preproc_create()/preproc_prepare()),
// writing into the output buffer owned by `job`. This is the only call
// benchmark callers should wrap with clock_gettime().
bool preproc_run(PreprocJob* job, larodConnection* conn, larodError** error);

// Meaningful only for PREPROC_SCENARIO_CROP jobs. True once at least one
// preproc_run() has succeeded via job params (the fast path); false if it
// fell back to per-call model reload.
bool preproc_uses_job_params_for_crop(const PreprocJob* job);

// mmap'd pointer to the output buffer and its size in bytes. The pointer is
// stable for the lifetime of `job`; its contents are whatever the most
// recent successful preproc_run() wrote.
void* preproc_output_data(const PreprocJob* job);
size_t preproc_output_size(const PreprocJob* job);

void preproc_destroy(larodConnection* conn, PreprocJob** job);

#endif // PREPROC_H

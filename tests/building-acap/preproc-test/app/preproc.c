#ifndef _GNU_SOURCE
#define _GNU_SOURCE // memfd_create() in <sys/mman.h>
#endif

#include "preproc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <syslog.h>
#include <unistd.h>

// Fixed output geometry per scenario, per CLAUDE.md / PLAN.md 2-B table.
#define CROP_SCALE_OUT_W 300u
#define CROP_SCALE_OUT_H 300u
#define CROP_SCALE_OUT_ROW_PITCH 300u // NV12, no padding
// NV12 buffer size = luma (w*h) + chroma (w*h/2).
#define CROP_SCALE_OUT_SIZE (CROP_SCALE_OUT_W * CROP_SCALE_OUT_H * 3u / 2u)

#define RGB_OUT_W 1920u
#define RGB_OUT_H 1080u
#define RGB_INTERLEAVED_ROW_PITCH (RGB_OUT_W * 3u) // 5760
#define RGB_PLANAR_ROW_PITCH RGB_OUT_W             // 1920
#define RGB_OUT_SIZE (RGB_OUT_W * RGB_OUT_H * 3u)  // 6220800, same for both

struct PreprocJob {
    PreprocScenario scenario;
    larodConnection* conn; // borrowed, not owned
    const larodDevice* device;

    // Input geometry, cached for model (re)loads.
    uint32_t in_width;
    uint32_t in_height;
    uint32_t in_pitch;

    // Output geometry, fixed for the lifetime of the job.
    uint32_t out_width;
    uint32_t out_height;
    uint32_t out_row_pitch;
    size_t output_size;

    larodModel* model;
    larodTensor** inputs;
    size_t num_inputs;
    larodTensor** outputs;
    size_t num_outputs;
    larodJobRequest* req;

    int output_fd;
    void* output_map;

    // CROP-only bookkeeping. crop_via_params starts optimistic (true);
    // preproc_run() flips it to false permanently the first time
    // larodSetJobRequestParams()/larodRunJob() rejects image.input.crop.
    bool crop_via_params;
};

const char* preproc_scenario_name(PreprocScenario scenario) {
    switch (scenario) {
        case PREPROC_SCENARIO_CROP:
            return "crop";
        case PREPROC_SCENARIO_SCALE:
            return "scale";
        case PREPROC_SCENARIO_RGB_INTERLEAVED:
            return "rgb-i";
        case PREPROC_SCENARIO_RGB_PLANAR:
            return "rgb-p";
        default:
            return "unknown";
    }
}

bool preproc_scenario_from_name(const char* name, PreprocScenario* out) {
    static const PreprocScenario all[] = {
        PREPROC_SCENARIO_CROP,
        PREPROC_SCENARIO_SCALE,
        PREPROC_SCENARIO_RGB_INTERLEAVED,
        PREPROC_SCENARIO_RGB_PLANAR,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (strcmp(name, preproc_scenario_name(all[i])) == 0) {
            *out = all[i];
            return true;
        }
    }
    return false;
}

static const char* preproc_output_format(PreprocScenario scenario) {
    switch (scenario) {
        case PREPROC_SCENARIO_CROP:
        case PREPROC_SCENARIO_SCALE:
            return "nv12";
        case PREPROC_SCENARIO_RGB_INTERLEAVED:
            return "rgb-interleaved";
        case PREPROC_SCENARIO_RGB_PLANAR:
            return "rgb-planar";
        default:
            return "unknown";
    }
}

static larodTensorLayout preproc_output_layout(PreprocScenario scenario) {
    switch (scenario) {
        case PREPROC_SCENARIO_CROP:
        case PREPROC_SCENARIO_SCALE:
            return LAROD_TENSOR_LAYOUT_420SP;
        case PREPROC_SCENARIO_RGB_INTERLEAVED:
            return LAROD_TENSOR_LAYOUT_NHWC;
        case PREPROC_SCENARIO_RGB_PLANAR:
            return LAROD_TENSOR_LAYOUT_NCHW;
        default:
            return LAROD_TENSOR_LAYOUT_INVALID;
    }
}

// Builds the larodMap describing the pre-processing "model" for `scenario`.
// If `crop` is non-NULL (only meaningful for PREPROC_SCENARIO_CROP), it is
// baked into the model itself via image.input.crop -- this is the fallback
// path used when the device rejects that key in job params instead.
static larodMap* build_model_map(const PreprocJob* job, const int* crop,
                                  larodError** error) {
    larodMap* map = larodCreateMap(error);
    if (!map)
        return NULL;

    bool ok = true;
    ok = ok && larodMapSetStr(map, "image.input.format", "nv12", error);
    ok = ok && larodMapSetIntArr2(map, "image.input.size", job->in_width,
                                   job->in_height, error);
    ok = ok &&
         larodMapSetInt(map, "image.input.row-pitch", job->in_pitch, error);
    if (crop) {
        ok = ok && larodMapSetIntArr4(map, "image.input.crop", crop[0],
                                       crop[1], crop[2], crop[3], error);
    }
    ok = ok && larodMapSetStr(map, "image.output.format",
                               preproc_output_format(job->scenario), error);
    ok = ok && larodMapSetIntArr2(map, "image.output.size", job->out_width,
                                   job->out_height, error);
    ok = ok && larodMapSetInt(map, "image.output.row-pitch",
                               job->out_row_pitch, error);
    if (!ok) {
        larodDestroyMap(&map);
        return NULL;
    }
    return map;
}

static bool load_pp_model(PreprocJob* job, const int* crop,
                           larodError** error) {
    larodMap* map = build_model_map(job, crop, error);
    if (!map)
        return false;

    larodModel* model =
        larodLoadModel(job->conn, -1, job->device, LAROD_ACCESS_PRIVATE,
                        "ppcomp-pp", map, error);
    larodDestroyMap(&map);
    if (!model)
        return false;

    job->model = model;
    return true;
}

static bool build_input_tensor(PreprocJob* job, const CapturedFrame* frame,
                                larodError** error) {
    larodTensor** t = larodCreateTensors(1, error);
    if (!t)
        return false;

    bool ok = true;
    ok = ok && larodSetTensorDataType(t[0], LAROD_TENSOR_DATA_TYPE_UINT8,
                                       error);
    ok = ok && larodSetTensorLayout(t[0], LAROD_TENSOR_LAYOUT_420SP, error);
    ok = ok && larodBuildTensorDims(t[0], LAROD_TENSOR_LAYOUT_420SP,
                                     frame->width, frame->height, 3, error);
    ok = ok && larodBuildTensorPitches(t[0], LAROD_TENSOR_LAYOUT_420SP,
                                        frame->pitch, frame->height, 3, error);
    // 2-A confirmed buffer.type == "dmabuf" on this device, so
    // larodConvertVmemFdToDmabuf() is not needed. Keep the vmem branch so a
    // future device with buffer.type == "vmem" doesn't silently misbehave.
    int fd = frame->fd;
    int64_t offset = frame->offset;
    if (!frame->is_dmabuf) {
        int converted = larodConvertVmemFdToDmabuf(frame->fd, frame->offset, error);
        if (converted == LAROD_INVALID_FD) {
            larodDestroyTensors(job->conn, &t, 1, NULL);
            return false;
        }
        fd = converted;
        offset = 0; // the conversion consumes the offset
    }

    ok = ok && larodSetTensorFdProps(t[0], LAROD_FD_TYPE_DMA, error);
    ok = ok && larodSetTensorFd(t[0], dup(fd), error);
    ok = ok && larodSetTensorFdOffset(t[0], offset, error);
    ok = ok && larodSetTensorFdSize(t[0], frame->capacity, error);
    ok = ok && larodTrackTensor(job->conn, t[0], error);

    if (!ok) {
        larodDestroyTensors(job->conn, &t, 1, NULL);
        return false;
    }

    job->inputs = t;
    job->num_inputs = 1;
    return true;
}

static bool build_output_tensor(PreprocJob* job, larodError** error) {
    larodTensor** t = larodCreateTensors(1, error);
    if (!t)
        return false;

    larodTensorLayout layout = preproc_output_layout(job->scenario);

    bool ok = true;
    ok = ok &&
         larodSetTensorDataType(t[0], LAROD_TENSOR_DATA_TYPE_UINT8, error);
    ok = ok && larodSetTensorLayout(t[0], layout, error);
    ok = ok && larodBuildTensorDims(t[0], layout, job->out_width,
                                     job->out_height, 3, error);
    ok = ok && larodBuildTensorPitches(t[0], layout, job->out_row_pitch,
                                        job->out_height, 3, error);
    ok = ok && larodSetTensorFdProps(
                   t[0], LAROD_FD_PROP_READWRITE | LAROD_FD_PROP_MAP, error);
    ok = ok && larodSetTensorFd(t[0], dup(job->output_fd), error);
    ok = ok && larodSetTensorFdOffset(t[0], 0, error);
    ok = ok && larodSetTensorFdSize(t[0], job->output_size, error);
    ok = ok && larodTrackTensor(job->conn, t[0], error);

    if (!ok) {
        larodDestroyTensors(job->conn, &t, 1, NULL);
        return false;
    }

    job->outputs = t;
    job->num_outputs = 1;
    return true;
}

bool preproc_create(larodConnection* conn, const larodDevice* device,
                     PreprocScenario scenario, const CapturedFrame* frame,
                     PreprocJob** out, larodError** error) {
    *out = NULL;

    PreprocJob* job = calloc(1, sizeof(*job));
    if (!job) {
        return false;
    }
    job->scenario = scenario;
    job->conn = conn;
    job->device = device;
    job->in_width = frame->width;
    job->in_height = frame->height;
    job->in_pitch = frame->pitch;
    job->crop_via_params = true;
    job->output_fd = -1;

    switch (scenario) {
        case PREPROC_SCENARIO_CROP:
        case PREPROC_SCENARIO_SCALE:
            job->out_width = CROP_SCALE_OUT_W;
            job->out_height = CROP_SCALE_OUT_H;
            job->out_row_pitch = CROP_SCALE_OUT_ROW_PITCH;
            job->output_size = CROP_SCALE_OUT_SIZE;
            break;
        case PREPROC_SCENARIO_RGB_INTERLEAVED:
            job->out_width = RGB_OUT_W;
            job->out_height = RGB_OUT_H;
            job->out_row_pitch = RGB_INTERLEAVED_ROW_PITCH;
            job->output_size = RGB_OUT_SIZE;
            break;
        case PREPROC_SCENARIO_RGB_PLANAR:
            job->out_width = RGB_OUT_W;
            job->out_height = RGB_OUT_H;
            job->out_row_pitch = RGB_PLANAR_ROW_PITCH;
            job->output_size = RGB_OUT_SIZE;
            break;
        default:
            syslog(LOG_ERR, "preproc: unknown scenario %d", scenario);
            free(job);
            return false;
    }

    // Output buffer: one memfd, sized once, reused for every run.
    char name[64];
    snprintf(name, sizeof(name), "ppcomp-pp-%s",
             preproc_scenario_name(scenario));
    job->output_fd = memfd_create(name, 0);
    if (job->output_fd < 0) {
        syslog(LOG_ERR, "preproc: memfd_create failed: %s", strerror(errno));
        free(job);
        return false;
    }
    if (ftruncate(job->output_fd, (off_t)job->output_size) != 0) {
        syslog(LOG_ERR, "preproc: ftruncate failed: %s", strerror(errno));
        close(job->output_fd);
        free(job);
        return false;
    }
    job->output_map = mmap(NULL, job->output_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, job->output_fd, 0);
    if (job->output_map == MAP_FAILED) {
        syslog(LOG_ERR, "preproc: mmap output failed: %s", strerror(errno));
        job->output_map = NULL;
        close(job->output_fd);
        free(job);
        return false;
    }

    // Model, without a baked-in crop: for non-crop scenarios this is the
    // whole story; for crop, this is the fast path (crop delivered per-job
    // via larodSetJobRequestParams()) that preproc_run() tries first.
    if (!load_pp_model(job, NULL, error)) {
        munmap(job->output_map, job->output_size);
        close(job->output_fd);
        free(job);
        return false;
    }

    if (!build_input_tensor(job, frame, error)) {
        larodDestroyModel(&job->model);
        munmap(job->output_map, job->output_size);
        close(job->output_fd);
        free(job);
        return false;
    }

    if (!build_output_tensor(job, error)) {
        larodDestroyTensors(conn, &job->inputs, job->num_inputs, NULL);
        larodDestroyModel(&job->model);
        munmap(job->output_map, job->output_size);
        close(job->output_fd);
        free(job);
        return false;
    }

    job->req = larodCreateJobRequest(job->model, job->inputs, job->num_inputs,
                                      job->outputs, job->num_outputs, NULL,
                                      error);
    if (!job->req) {
        larodDestroyTensors(conn, &job->outputs, job->num_outputs, NULL);
        larodDestroyTensors(conn, &job->inputs, job->num_inputs, NULL);
        larodDestroyModel(&job->model);
        munmap(job->output_map, job->output_size);
        close(job->output_fd);
        free(job);
        return false;
    }

    *out = job;
    return true;
}

// Fallback path for CROP: destroys the current model + job request (tensors
// are kept, they don't change) and reloads a model with image.input.crop
// baked into its map, then rebuilds the job request against it.
static bool reload_crop_model(PreprocJob* job, int crop_x, int crop_y,
                               larodError** error) {
    int crop[4] = {crop_x, crop_y, (int)job->out_width, (int)job->out_height};

    larodJobRequest* old_req = job->req;
    larodModel* old_model = job->model;

    if (!load_pp_model(job, crop, error)) {
        job->model = old_model; // keep old one usable, load failed
        return false;
    }

    larodJobRequest* new_req =
        larodCreateJobRequest(job->model, job->inputs, job->num_inputs,
                               job->outputs, job->num_outputs, NULL, error);
    if (!new_req) {
        larodDestroyModel(&job->model);
        job->model = old_model;
        return false;
    }

    larodDestroyJobRequest(&old_req);
    larodDestroyModel(&old_model);
    job->req = new_req;
    return true;
}

bool preproc_prepare(PreprocJob* job, larodConnection* conn, int crop_x,
                      int crop_y, larodError** error) {
    (void)conn; // unused on this path, kept for signature symmetry
    if (job->scenario != PREPROC_SCENARIO_CROP)
        return true;

    if (job->crop_via_params) {
        larodMap* params = larodCreateMap(error);
        bool ok = params != NULL;
        ok = ok && larodMapSetIntArr4(params, "image.input.crop", crop_x,
                                       crop_y, (int64_t)job->out_width,
                                       (int64_t)job->out_height, error);
        ok = ok && larodSetJobRequestParams(job->req, params, error);
        if (params)
            larodDestroyMap(&params);
        if (ok)
            return true;

        syslog(LOG_WARNING,
               "preproc: image.input.crop via job params rejected (%s); "
               "falling back to per-crop model reload",
               (error && *error) ? (*error)->msg : "unknown error");
        if (error)
            larodClearError(error);
        job->crop_via_params = false;
        // fall through to the reload path below
    }

    return reload_crop_model(job, crop_x, crop_y, error);
}

bool preproc_run(PreprocJob* job, larodConnection* conn, larodError** error) {
    return larodRunJob(conn, job->req, error);
}

bool preproc_uses_job_params_for_crop(const PreprocJob* job) {
    return job->crop_via_params;
}

void* preproc_output_data(const PreprocJob* job) { return job->output_map; }

size_t preproc_output_size(const PreprocJob* job) { return job->output_size; }

void preproc_destroy(larodConnection* conn, PreprocJob** job_ptr) {
    if (!job_ptr || !*job_ptr)
        return;
    PreprocJob* job = *job_ptr;

    larodDestroyJobRequest(&job->req);
    if (job->outputs)
        larodDestroyTensors(conn, &job->outputs, job->num_outputs, NULL);
    if (job->inputs)
        larodDestroyTensors(conn, &job->inputs, job->num_inputs, NULL);
    if (job->model)
        larodDestroyModel(&job->model);
    if (job->output_map)
        munmap(job->output_map, job->output_size);
    if (job->output_fd >= 0)
        close(job->output_fd);

    free(job);
    *job_ptr = NULL;
}

#include "capture.h"

#include <cstring>
#include <syslog.h>
#include <sys/mman.h>
#include <unistd.h>

#include <vdo-error.h>
#include <vdo-map.h>
#include <vdo-stream.h>
#include <vdo-types.h>

#include "larod.h"

// libyuv-based preprocessing backend; on ARTPEC-9 the DLPU has no
// preprocessing device (see the device list logged by capture_start).
#define PP_DEVICE "cpu-proc"

// One tracked input tensor per distinct VDO buffer fd.
#define MAX_VDO_BUFFERS 8

struct Capture {
    VdoStream* stream;
    VdoBuffer* held;  // buffer currently being preprocessed
    unsigned src_width;
    unsigned src_height;
    unsigned src_pitch;
    bool src_dmabuf;

    larodConnection* conn;
    larodModel* pp_model;
    larodJobRequest* pp_req;
    larodTensor** pp_output;
    larodTensor** pp_inputs[MAX_VDO_BUFFERS];
    int pp_input_fds[MAX_VDO_BUFFERS];  // VDO fd each input tensor is bound to
    size_t num_pp_inputs;

    int out_fd;
    uint8_t* out_data;
    size_t out_size;
    unsigned content_width;
    unsigned content_height;
};

static void log_larod_devices(larodConnection* conn) {
    larodError* error  = NULL;
    size_t num_devices = 0;
    const larodDevice** devices = larodListDevices(conn, &num_devices, &error);
    if (!devices) {
        syslog(LOG_ERR, "capture: larodListDevices failed: %s", error->msg);
        larodClearError(&error);
        return;
    }
    for (size_t i = 0; i < num_devices; i++) {
        syslog(LOG_INFO, "capture: larod device[%zu] = %s", i, larodGetDeviceName(devices[i], NULL));
    }
}

static bool start_stream(Capture* capture, double framerate) {
    g_autoptr(GError) error    = NULL;
    g_autoptr(VdoMap) settings = vdo_map_new();

    vdo_map_set_uint32(settings, "format", VDO_FORMAT_YUV);
    vdo_map_set_string(settings, "subformat", "NV12");
    vdo_map_set_uint32(settings, "channel", 1);
    VdoPair32u resolution = {{capture->src_width, capture->src_height}};
    vdo_map_set_pair32u(settings, "resolution", resolution);
    vdo_map_set_double(settings, "framerate", framerate);
    vdo_map_set_uint32(settings, "buffer.count", 2);

    capture->stream = vdo_stream_new(settings, NULL, &error);
    if (!capture->stream) {
        syslog(LOG_ERR, "capture: vdo_stream_new failed: %s", error->message);
        return false;
    }

    g_autoptr(VdoMap) info = vdo_stream_get_info(capture->stream, &error);
    if (!info) {
        syslog(LOG_ERR, "capture: vdo_stream_get_info failed: %s", error->message);
        return false;
    }
    capture->src_width  = vdo_map_get_uint32(info, "width", 0);
    capture->src_height = vdo_map_get_uint32(info, "height", 0);
    capture->src_pitch  = vdo_map_get_uint32(info, "pitch", 0);
    const char* buffer_type = vdo_map_get_string(info, "buffer.type", NULL, "memfd");
    capture->src_dmabuf     = g_strcmp0(buffer_type, "vmem") != 0;

    syslog(LOG_INFO,
           "capture: stream %ux%u pitch %u buffer.type %s",
           capture->src_width,
           capture->src_height,
           capture->src_pitch,
           buffer_type);

    if (!vdo_stream_start(capture->stream, &error)) {
        syslog(LOG_ERR, "capture: vdo_stream_start failed: %s", error->message);
        return false;
    }
    return true;
}

// Allocate the zero-filled RGB output buffer. Only the top
// content_height rows are ever written, so the rest stays zero (padding).
static bool alloc_output(Capture* capture, unsigned dst_width, unsigned dst_height) {
    capture->out_size = (size_t)dst_width * dst_height * 3;
    capture->out_fd   = memfd_create("capture-rgb", 0);
    if (capture->out_fd < 0) {
        syslog(LOG_ERR, "capture: memfd_create failed: %m");
        return false;
    }
    if (ftruncate(capture->out_fd, (off_t)capture->out_size) != 0) {
        syslog(LOG_ERR, "capture: ftruncate failed: %m");
        return false;
    }
    void* data =
        mmap(NULL, capture->out_size, PROT_READ | PROT_WRITE, MAP_SHARED, capture->out_fd, 0);
    if (data == MAP_FAILED) {
        syslog(LOG_ERR, "capture: mmap failed: %m");
        return false;
    }
    capture->out_data = static_cast<uint8_t*>(data);
    memset(capture->out_data, 0, capture->out_size);
    return true;
}

static larodModel* load_pp_model(Capture* capture) {
    larodError* error = NULL;
    larodMap* map     = larodCreateMap(&error);
    if (!map) {
        syslog(LOG_ERR, "capture: larodCreateMap failed: %s", error->msg);
        larodClearError(&error);
        return NULL;
    }

    bool ok = larodMapSetStr(map, "image.input.format", "nv12", &error) &&
              larodMapSetIntArr2(map,
                                 "image.input.size",
                                 capture->src_width,
                                 capture->src_height,
                                 &error) &&
              larodMapSetInt(map, "image.input.row-pitch", capture->src_pitch, &error) &&
              larodMapSetStr(map, "image.output.format", "rgb-interleaved", &error) &&
              larodMapSetIntArr2(map,
                                 "image.output.size",
                                 capture->content_width,
                                 capture->content_height,
                                 &error) &&
              larodMapSetInt(map, "image.output.row-pitch", capture->content_width * 3, &error);
    if (!ok) {
        syslog(LOG_ERR, "capture: failed setting preprocessing parameters: %s", error->msg);
        larodClearError(&error);
        larodDestroyMap(&map);
        return NULL;
    }

    const larodDevice* device = larodGetDevice(capture->conn, PP_DEVICE, 0, &error);
    if (!device) {
        syslog(LOG_ERR, "capture: no larod device %s: %s", PP_DEVICE, error->msg);
        larodClearError(&error);
        larodDestroyMap(&map);
        return NULL;
    }
    larodModel* model =
        larodLoadModel(capture->conn, -1, device, LAROD_ACCESS_PRIVATE, "preprocess", map, &error);
    if (!model) {
        syslog(LOG_ERR, "capture: larodLoadModel(%s) failed: %s", PP_DEVICE, error->msg);
        larodClearError(&error);
    }
    larodDestroyMap(&map);
    return model;
}

// Output tensor describing the content region only, backed by the top of the
// padded output buffer. RGB-interleaved rows are contiguous, so writing
// content_height rows from offset 0 leaves the bottom rows untouched.
static bool create_pp_output(Capture* capture) {
    larodError* error   = NULL;
    capture->pp_output  = larodCreateTensors(1, &error);
    if (!capture->pp_output) {
        syslog(LOG_ERR, "capture: larodCreateTensors failed: %s", error->msg);
        larodClearError(&error);
        return false;
    }
    larodTensor* tensor = capture->pp_output[0];
    int duped_fd        = dup(capture->out_fd);
    if (duped_fd < 0) {
        syslog(LOG_ERR, "capture: dup failed: %m");
        return false;
    }
    bool ok = larodSetTensorDataType(tensor, LAROD_TENSOR_DATA_TYPE_UINT8, &error) &&
              larodSetTensorLayout(tensor, LAROD_TENSOR_LAYOUT_NHWC, &error) &&
              larodBuildTensorDims(tensor,
                                   LAROD_TENSOR_LAYOUT_NHWC,
                                   capture->content_width,
                                   capture->content_height,
                                   3,
                                   &error) &&
              larodBuildTensorPitches(tensor,
                                      LAROD_TENSOR_LAYOUT_NHWC,
                                      capture->content_width * 3,
                                      capture->content_height,
                                      3,
                                      &error) &&
              larodSetTensorFd(tensor, duped_fd, &error) &&
              larodSetTensorFdOffset(tensor, 0, &error) &&
              larodSetTensorFdSize(tensor, capture->out_size, &error) &&
              larodSetTensorFdProps(tensor, LAROD_FD_PROP_READWRITE | LAROD_FD_PROP_MAP, &error) &&
              larodTrackTensor(capture->conn, tensor, &error);
    if (!ok) {
        syslog(LOG_ERR, "capture: failed setting up pp output tensor: %s", error->msg);
        larodClearError(&error);
        return false;
    }
    return true;
}

// Bind a VDO buffer to a tracked input tensor, creating one the first time a
// given buffer fd is seen.
static larodTensor** get_pp_input(Capture* capture, VdoBuffer* buffer) {
    larodError* error = NULL;
    int vdo_fd        = vdo_buffer_get_fd(buffer);

    for (size_t i = 0; i < capture->num_pp_inputs; i++) {
        if (capture->pp_input_fds[i] == vdo_fd) {
            return capture->pp_inputs[i];
        }
    }
    if (capture->num_pp_inputs == MAX_VDO_BUFFERS) {
        syslog(LOG_ERR, "capture: more than %d VDO buffers", MAX_VDO_BUFFERS);
        return NULL;
    }

    int64_t offset = vdo_buffer_get_offset(buffer);
    int buf_fd     = vdo_fd;
    if (!capture->src_dmabuf) {
        buf_fd = larodConvertVmemFdToDmabuf(vdo_fd, offset, &error);
        if (buf_fd == LAROD_INVALID_FD) {
            syslog(LOG_ERR, "capture: larodConvertVmemFdToDmabuf failed: %s", error->msg);
            larodClearError(&error);
            return NULL;
        }
        offset = 0;
    }
    int duped_fd = dup(buf_fd);
    if (duped_fd < 0) {
        syslog(LOG_ERR, "capture: dup failed: %m");
        return NULL;
    }

    larodTensor** tensors = larodCreateTensors(1, &error);
    if (!tensors) {
        syslog(LOG_ERR, "capture: larodCreateTensors failed: %s", error->msg);
        larodClearError(&error);
        close(duped_fd);
        return NULL;
    }
    bool ok = larodSetTensorDataType(tensors[0], LAROD_TENSOR_DATA_TYPE_UINT8, &error) &&
              larodSetTensorLayout(tensors[0], LAROD_TENSOR_LAYOUT_420SP, &error) &&
              larodBuildTensorDims(tensors[0],
                                   LAROD_TENSOR_LAYOUT_420SP,
                                   capture->src_width,
                                   capture->src_height,
                                   3,
                                   &error) &&
              larodBuildTensorPitches(tensors[0],
                                      LAROD_TENSOR_LAYOUT_420SP,
                                      capture->src_pitch,
                                      capture->src_height,
                                      3,
                                      &error) &&
              larodSetTensorFdProps(tensors[0],
                                    LAROD_FD_PROP_MAP | LAROD_FD_PROP_DMABUF,
                                    &error) &&
              larodSetTensorFd(tensors[0], duped_fd, &error) &&
              larodSetTensorFdOffset(tensors[0], offset, &error) &&
              larodSetTensorFdSize(tensors[0], vdo_buffer_get_capacity(buffer), &error) &&
              larodTrackTensor(capture->conn, tensors[0], &error);
    if (!ok) {
        syslog(LOG_ERR, "capture: failed setting up pp input tensor: %s", error->msg);
        larodClearError(&error);
        return NULL;
    }

    capture->pp_inputs[capture->num_pp_inputs]    = tensors;
    capture->pp_input_fds[capture->num_pp_inputs] = vdo_fd;
    capture->num_pp_inputs++;
    return tensors;
}

Capture* capture_start(unsigned src_width,
                       unsigned src_height,
                       unsigned dst_width,
                       unsigned dst_height,
                       double framerate) {
    Capture* capture   = g_new0(Capture, 1);
    capture->src_width = src_width;
    capture->src_height = src_height;
    capture->out_fd     = -1;

    // Scale to fit the width without stretching, then pad the bottom.
    capture->content_width  = dst_width;
    capture->content_height = (unsigned)((uint64_t)src_height * dst_width / src_width);
    if (capture->content_height > dst_height) {
        syslog(LOG_ERR,
               "capture: %ux%u does not fit in %ux%u when scaled to width",
               src_width,
               src_height,
               dst_width,
               dst_height);
        capture_stop(capture);
        return NULL;
    }

    if (!start_stream(capture, framerate)) {
        capture_stop(capture);
        return NULL;
    }
    if (!alloc_output(capture, dst_width, dst_height)) {
        capture_stop(capture);
        return NULL;
    }

    larodError* error = NULL;
    if (!larodConnect(&capture->conn, &error)) {
        syslog(LOG_ERR, "capture: larodConnect failed: %s", error->msg);
        larodClearError(&error);
        capture_stop(capture);
        return NULL;
    }
    log_larod_devices(capture->conn);

    capture->pp_model = load_pp_model(capture);
    if (!capture->pp_model || !create_pp_output(capture)) {
        capture_stop(capture);
        return NULL;
    }

    syslog(LOG_INFO,
           "capture: preprocessing %ux%u nv12 -> %ux%u rgb-interleaved in a %ux%u buffer "
           "(%zu bytes, bottom %u rows padded)",
           capture->src_width,
           capture->src_height,
           capture->content_width,
           capture->content_height,
           dst_width,
           dst_height,
           capture->out_size,
           dst_height - capture->content_height);
    return capture;
}

const uint8_t* capture_next(Capture* capture) {
    g_autoptr(GError) gerror = NULL;
    larodError* error        = NULL;

    if (capture->held && !vdo_stream_buffer_unref(capture->stream, &capture->held, &gerror)) {
        syslog(LOG_ERR, "capture: vdo_stream_buffer_unref failed: %s", gerror->message);
        return NULL;
    }
    for (;;) {
        VdoBuffer* buffer = vdo_stream_get_buffer(capture->stream, &gerror);
        if (buffer) {
            capture->held = buffer;
            break;
        }
        if (g_error_matches(gerror, VDO_ERROR, VDO_ERROR_NO_DATA)) {
            g_clear_error(&gerror);
            continue;  // transient
        }
        syslog(LOG_ERR, "capture: vdo_stream_get_buffer failed: %s", gerror->message);
        return NULL;
    }

    larodTensor** inputs = get_pp_input(capture, capture->held);
    if (!inputs) {
        return NULL;
    }
    if (!capture->pp_req) {
        capture->pp_req = larodCreateJobRequest(capture->pp_model,
                                                inputs,
                                                1,
                                                capture->pp_output,
                                                1,
                                                NULL,
                                                &error);
        if (!capture->pp_req) {
            syslog(LOG_ERR, "capture: larodCreateJobRequest failed: %s", error->msg);
            larodClearError(&error);
            return NULL;
        }
    } else if (!larodSetJobRequestInputs(capture->pp_req, inputs, 1, &error)) {
        syslog(LOG_ERR, "capture: larodSetJobRequestInputs failed: %s", error->msg);
        larodClearError(&error);
        return NULL;
    }

    if (!larodRunJob(capture->conn, capture->pp_req, &error)) {
        syslog(LOG_ERR, "capture: preprocessing job failed: %s (%d)", error->msg, error->code);
        larodClearError(&error);
        return NULL;
    }
    return capture->out_data;
}

int capture_output_fd(const Capture* capture) {
    return capture->out_fd;
}

size_t capture_output_size(const Capture* capture) {
    return capture->out_size;
}

unsigned capture_content_height(const Capture* capture) {
    return capture->content_height;
}

void capture_stop(Capture* capture) {
    if (!capture) {
        return;
    }
    larodDestroyJobRequest(&capture->pp_req);
    if (capture->conn) {
        for (size_t i = 0; i < capture->num_pp_inputs; i++) {
            larodDestroyTensors(capture->conn, &capture->pp_inputs[i], 1, NULL);
        }
        if (capture->pp_output) {
            larodDestroyTensors(capture->conn, &capture->pp_output, 1, NULL);
        }
        larodDestroyModel(&capture->pp_model);
        larodDisconnect(&capture->conn, NULL);
    }
    if (capture->out_data) {
        munmap(capture->out_data, capture->out_size);
    }
    if (capture->out_fd >= 0) {
        close(capture->out_fd);
    }
    if (capture->stream) {
        if (capture->held) {
            vdo_stream_buffer_unref(capture->stream, &capture->held, NULL);
        }
        vdo_stream_stop(capture->stream);
        g_object_unref(capture->stream);
    }
    g_free(capture);
}

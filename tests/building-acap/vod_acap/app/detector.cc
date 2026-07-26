#include "detector.h"

#include <cstdint>
#include <fcntl.h>
#include <syslog.h>
#include <sys/mman.h>
#include <unistd.h>

#include <glib.h>

#include "larod.h"

// 4 box coords + objectness + one score per class.
#define ATTRS_BEFORE_CLASSES 5

// Output dequantization: real = (quantized - zero_point) * scale. larod exposes
// no API for these, so they are taken from app/models/info.yaml, which matches
// what tools/host_infer.py reads back from the tflite.
#define OUTPUT_SCALE 0.004144445527344942f
#define OUTPUT_ZERO_POINT 0

// Same thresholds as the host reference (tools/host_infer.py defaults).
#define CONF_THRESHOLD 0.25f
#define IOU_THRESHOLD 0.45f

struct Detector {
    larodConnection* conn;
    larodModel* model;
    larodJobRequest* req;
    larodTensor** inputs;
    larodTensor** outputs;
    size_t num_inputs;
    size_t num_outputs;

    const uint8_t* out_data;  // mmapped output tensor
    size_t out_size;
    size_t num_boxes;  // output dims[1], e.g. 25200
    size_t num_attrs;  // output dims[2], e.g. 85

    unsigned width;
    unsigned height;
    GPtrArray* labels;
};

// Log what the loaded model actually asks for. Guessing the geometry does not
// crash, it silently shifts every box, so the real values go in the log.
static const larodTensorDims* describe_tensor(const larodTensor* tensor, const char* what) {
    larodError* error           = NULL;
    const larodTensorDims* dims = larodGetTensorDims(tensor, &error);
    if (!dims) {
        syslog(LOG_ERR, "detector: larodGetTensorDims(%s) failed: %s", what, error->msg);
        larodClearError(&error);
        return NULL;
    }
    larodTensorDataType type = larodGetTensorDataType(tensor, &error);
    size_t fd_size           = 0;
    larodGetTensorFdSize(tensor, &fd_size, &error);
    larodClearError(&error);

    g_autoptr(GString) text = g_string_new(NULL);
    for (size_t i = 0; i < dims->len; i++) {
        g_string_append_printf(text, i ? ",%zu" : "%zu", dims->dims[i]);
    }
    syslog(LOG_INFO,
           "detector: %s dims [%s] dtype %d fd size %zu",
           what,
           text->str,
           (int)type,
           fd_size);
    return dims;
}

// The label file is a 90-entry COCO map with "n/a" holes while the model has 80
// classes; dropping the holes makes class index -> label line up.
static bool load_labels(Detector* detector, const char* path) {
    g_autofree char* text    = NULL;
    g_autoptr(GError) gerror = NULL;
    if (!g_file_get_contents(path, &text, NULL, &gerror)) {
        syslog(LOG_ERR, "detector: cannot read %s: %s", path, gerror->message);
        return false;
    }
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    detector->labels    = g_ptr_array_new_with_free_func(g_free);
    for (char** line = lines; *line; line++) {
        const char* name = g_strstrip(*line);
        if (*name == '\0' || g_strcmp0(name, "n/a") == 0) {
            continue;
        }
        g_ptr_array_add(detector->labels, g_strdup(name));
    }
    return true;
}

static bool load_model(Detector* detector, const char* path, const char* device_name) {
    larodError* error         = NULL;
    const larodDevice* device = larodGetDevice(detector->conn, device_name, 0, &error);
    if (!device) {
        syslog(LOG_ERR, "detector: no larod device %s: %s", device_name, error->msg);
        larodClearError(&error);
        return false;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        syslog(LOG_ERR, "detector: cannot open %s: %m", path);
        return false;
    }
    syslog(LOG_INFO, "detector: loading %s onto %s (may take a while)", path, device_name);
    detector->model =
        larodLoadModel(detector->conn, fd, device, LAROD_ACCESS_PRIVATE, "yolov5s", NULL, &error);
    close(fd);
    if (!detector->model) {
        syslog(LOG_ERR, "detector: larodLoadModel failed: %s", error->msg);
        larodClearError(&error);
        return false;
    }
    return true;
}

// Input tensor with the model's geometry but our buffer behind it, so the
// preprocessing output is read in place. larodCreateModelInputs (rather than
// larodAllocModelInputs) leaves the fd unset, which is what allows pointing it
// at our fd: a tracked tensor's fd can no longer be replaced.
static bool setup_input(Detector* detector, int input_fd, size_t input_size) {
    larodError* error = NULL;
    detector->inputs  = larodCreateModelInputs(detector->model, &detector->num_inputs, &error);
    if (!detector->inputs) {
        syslog(LOG_ERR, "detector: larodCreateModelInputs failed: %s", error->msg);
        larodClearError(&error);
        return false;
    }
    if (detector->num_inputs != 1) {
        syslog(LOG_ERR, "detector: model has %zu inputs, expected 1", detector->num_inputs);
        return false;
    }
    const larodTensorDims* dims = describe_tensor(detector->inputs[0], "input");
    if (!dims) {
        return false;
    }
    if (dims->len != 4 || dims->dims[1] != detector->height || dims->dims[2] != detector->width ||
        dims->dims[3] != 3 ||
        larodGetTensorDataType(detector->inputs[0], NULL) != LAROD_TENSOR_DATA_TYPE_UINT8) {
        syslog(LOG_ERR,
               "detector: model input is not %ux%ux3 uint8 NHWC",
               detector->width,
               detector->height);
        return false;
    }

    int duped_fd = dup(input_fd);
    if (duped_fd < 0) {
        syslog(LOG_ERR, "detector: dup failed: %m");
        return false;
    }
    bool ok = larodSetTensorFdProps(detector->inputs[0],
                                    LAROD_FD_PROP_READWRITE | LAROD_FD_PROP_MAP,
                                    &error) &&
              larodSetTensorFd(detector->inputs[0], duped_fd, &error) &&
              larodSetTensorFdOffset(detector->inputs[0], 0, &error) &&
              larodSetTensorFdSize(detector->inputs[0], input_size, &error) &&
              larodTrackTensor(detector->conn, detector->inputs[0], &error);
    if (!ok) {
        syslog(LOG_ERR, "detector: failed setting up input tensor: %s", error->msg);
        larodClearError(&error);
        return false;
    }
    return true;
}

static bool setup_output(Detector* detector) {
    larodError* error = NULL;
    detector->outputs = larodAllocModelOutputs(detector->conn,
                                               detector->model,
                                               LAROD_FD_PROP_READWRITE | LAROD_FD_PROP_MAP,
                                               &detector->num_outputs,
                                               NULL,
                                               &error);
    if (!detector->outputs) {
        syslog(LOG_ERR, "detector: larodAllocModelOutputs failed: %s", error->msg);
        larodClearError(&error);
        return false;
    }
    if (detector->num_outputs != 1) {
        syslog(LOG_ERR, "detector: model has %zu outputs, expected 1", detector->num_outputs);
        return false;
    }
    const larodTensorDims* dims = describe_tensor(detector->outputs[0], "output");
    if (!dims) {
        return false;
    }
    if (dims->len != 3 || dims->dims[0] != 1 || dims->dims[2] <= ATTRS_BEFORE_CLASSES ||
        larodGetTensorDataType(detector->outputs[0], NULL) != LAROD_TENSOR_DATA_TYPE_UINT8) {
        syslog(LOG_ERR, "detector: model output is not [1,N,>5] uint8");
        return false;
    }
    detector->num_boxes = dims->dims[1];
    detector->num_attrs = dims->dims[2];
    detector->out_size  = detector->num_boxes * detector->num_attrs;

    int fd = larodGetTensorFd(detector->outputs[0], &error);
    if (fd == LAROD_INVALID_FD) {
        syslog(LOG_ERR, "detector: larodGetTensorFd failed: %s", error->msg);
        larodClearError(&error);
        return false;
    }
    void* data = mmap(NULL, detector->out_size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        syslog(LOG_ERR, "detector: mmap of the output tensor failed: %m");
        return false;
    }
    detector->out_data = static_cast<const uint8_t*>(data);
    return true;
}

Detector* detector_start(const char* model_path,
                         const char* labels_path,
                         const char* device,
                         int input_fd,
                         size_t input_size,
                         unsigned width,
                         unsigned height) {
    Detector* detector = g_new0(Detector, 1);
    detector->width    = width;
    detector->height   = height;

    larodError* error = NULL;
    if (!larodConnect(&detector->conn, &error)) {
        syslog(LOG_ERR, "detector: larodConnect failed: %s", error->msg);
        larodClearError(&error);
        detector_stop(detector);
        return NULL;
    }
    if (!load_model(detector, model_path, device) || !setup_input(detector, input_fd, input_size) ||
        !setup_output(detector) || !load_labels(detector, labels_path)) {
        detector_stop(detector);
        return NULL;
    }

    size_t num_classes = detector->num_attrs - ATTRS_BEFORE_CLASSES;
    if (detector->labels->len != num_classes) {
        syslog(LOG_ERR,
               "detector: %s has %u labels but the model has %zu classes",
               labels_path,
               detector->labels->len,
               num_classes);
        detector_stop(detector);
        return NULL;
    }

    detector->req = larodCreateJobRequest(detector->model,
                                          detector->inputs,
                                          1,
                                          detector->outputs,
                                          1,
                                          NULL,
                                          &error);
    if (!detector->req) {
        syslog(LOG_ERR, "detector: larodCreateJobRequest failed: %s", error->msg);
        larodClearError(&error);
        detector_stop(detector);
        return NULL;
    }

    syslog(LOG_INFO,
           "detector: ready, %zu boxes x %zu attrs (%zu classes) from a %ux%u input",
           detector->num_boxes,
           detector->num_attrs,
           num_classes,
           width,
           height);
    return detector;
}

static inline float dequant(uint8_t value) {
    return (float)(value - OUTPUT_ZERO_POINT) * OUTPUT_SCALE;
}

static int compare_by_score(const void* a, const void* b) {
    float diff = ((const Detection*)b)->score - ((const Detection*)a)->score;
    return diff < 0.0f ? -1 : diff > 0.0f ? 1 : 0;
}

static float intersection_over_union(const Detection* a, const Detection* b) {
    float w = MIN(a->x2, b->x2) - MAX(a->x1, b->x1);
    float h = MIN(a->y2, b->y2) - MAX(a->y1, b->y1);
    if (w <= 0.0f || h <= 0.0f) {
        return 0.0f;
    }
    float intersection = w * h;
    float area_a       = (a->x2 - a->x1) * (a->y2 - a->y1);
    float area_b       = (b->x2 - b->x1) * (b->y2 - b->y1);
    return intersection / (area_a + area_b - intersection + 1e-9f);
}

// One row per anchor: cx, cy, w, h, objectness, then one score per class.
static size_t decode(Detector* detector, Detection* out, size_t max_out) {
    g_autoptr(GArray) candidates = g_array_new(FALSE, FALSE, sizeof(Detection));
    size_t num_classes           = detector->num_attrs - ATTRS_BEFORE_CLASSES;

    for (size_t i = 0; i < detector->num_boxes; i++) {
        const uint8_t* row = detector->out_data + i * detector->num_attrs;
        // The quantization is monotonic, so the argmax can be taken before
        // dequantizing.
        uint8_t best      = 0;
        size_t best_class = 0;
        for (size_t c = 0; c < num_classes; c++) {
            if (row[ATTRS_BEFORE_CLASSES + c] > best) {
                best       = row[ATTRS_BEFORE_CLASSES + c];
                best_class = c;
            }
        }
        float score = dequant(row[4]) * dequant(best);
        if (score < CONF_THRESHOLD) {
            continue;
        }
        // This export normalizes the box to 0..1 of the model input.
        float cx = dequant(row[0]) * (float)detector->width;
        float cy = dequant(row[1]) * (float)detector->height;
        float w  = dequant(row[2]) * (float)detector->width;
        float h  = dequant(row[3]) * (float)detector->height;

        Detection detection = {cx - w / 2,
                               cy - h / 2,
                               cx + w / 2,
                               cy + h / 2,
                               score,
                               (int)best_class,
                               (const char*)g_ptr_array_index(detector->labels, best_class)};
        g_array_append_val(candidates, detection);
    }

    // Non-maximum suppression, highest score first. Comparing only within a
    // class matches the class-offset trick the host reference uses: boxes of
    // different classes never suppress each other.
    g_array_sort(candidates, compare_by_score);
    size_t kept = 0;
    for (guint i = 0; i < candidates->len && kept < max_out; i++) {
        const Detection* candidate = &g_array_index(candidates, Detection, i);
        bool suppressed            = false;
        for (size_t k = 0; k < kept && !suppressed; k++) {
            suppressed = out[k].class_id == candidate->class_id &&
                         intersection_over_union(&out[k], candidate) > IOU_THRESHOLD;
        }
        if (!suppressed) {
            out[kept++] = *candidate;
        }
    }
    syslog(LOG_INFO,
           "detector: %u candidates over %.2f, %zu kept after NMS",
           candidates->len,
           (double)CONF_THRESHOLD,
           kept);
    return kept;
}

size_t detector_run(Detector* detector, Detection* out, size_t max_out) {
    larodError* error = NULL;
    if (!larodRunJob(detector->conn, detector->req, &error)) {
        syslog(LOG_ERR, "detector: inference job failed: %s (%d)", error->msg, error->code);
        larodClearError(&error);
        return 0;
    }
    return decode(detector, out, max_out);
}

const unsigned char* detector_output(const Detector* detector, size_t* size) {
    *size = detector->out_size;
    return detector->out_data;
}

void detector_stop(Detector* detector) {
    if (!detector) {
        return;
    }
    larodDestroyJobRequest(&detector->req);
    if (detector->out_data) {
        munmap((void*)detector->out_data, detector->out_size);
    }
    if (detector->conn) {
        if (detector->inputs) {
            larodDestroyTensors(detector->conn, &detector->inputs, detector->num_inputs, NULL);
        }
        if (detector->outputs) {
            larodDestroyTensors(detector->conn, &detector->outputs, detector->num_outputs, NULL);
        }
        larodDestroyModel(&detector->model);
        larodDisconnect(&detector->conn, NULL);
    }
    if (detector->labels) {
        g_ptr_array_free(detector->labels, TRUE);
    }
    g_free(detector);
}

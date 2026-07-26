// Test binary: run inference on the packaged 640x640 RGB sample, which is the
// same byte-for-byte input the host reference (sample/host_ref.json) was made
// from, then on one live captured frame to smoke test the capture -> detector
// fd hand-off. Detections go to stdout, logs to stderr/syslog.
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <syslog.h>
#include <sys/mman.h>
#include <unistd.h>

#include "capture.h"
#include "detector.h"

#define APP_DIR "/usr/local/packages/yolov5_detector/"
#define MODEL_PATH APP_DIR "models/yolov5s.tflite"
#define LABELS_PATH APP_DIR "models/labels.txt"
#define SAMPLE_PATH APP_DIR "sample/image.rgb"

#define INPUT_WIDTH 640
#define INPUT_HEIGHT 640
#define INPUT_SIZE ((size_t)INPUT_WIDTH * INPUT_HEIGHT * 3)
#define MAX_DETECTIONS 32

// Copy the sample into a memfd, so the detector sees the same kind of buffer
// the preprocessing step hands it in the live pipeline.
static int load_sample(const char* path) {
    int file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        fprintf(stderr, "test_detect: cannot open %s: %m\n", path);
        return -1;
    }
    int fd = memfd_create("sample-rgb", 0);
    if (fd < 0 || ftruncate(fd, (off_t)INPUT_SIZE) != 0) {
        fprintf(stderr, "test_detect: memfd setup failed: %m\n");
        close(file_fd);
        return -1;
    }
    void* data = mmap(NULL, INPUT_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        fprintf(stderr, "test_detect: mmap failed: %m\n");
        close(file_fd);
        close(fd);
        return -1;
    }
    size_t got = 0;
    while (got < INPUT_SIZE) {
        ssize_t n = read(file_fd, (char*)data + got, INPUT_SIZE - got);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    // Byte sum of the input, to confirm the device really sees the same bytes
    // the host reference was made from.
    unsigned long long sum = 0;
    for (size_t i = 0; i < got; i++) {
        sum += ((const unsigned char*)data)[i];
    }
    syslog(LOG_INFO, "test_detect: sample %zu bytes, byte sum %llu", got, sum);
    munmap(data, INPUT_SIZE);
    close(file_fd);
    if (got != INPUT_SIZE) {
        fprintf(stderr, "test_detect: %s is %zu bytes, expected %zu\n", path, got, INPUT_SIZE);
        close(fd);
        return -1;
    }
    return fd;
}

static void print_detections(const char* what, const Detection* detections, size_t num) {
    printf("%s: %zu detections\n", what, num);
    for (size_t i = 0; i < num; i++) {
        const Detection* d = &detections[i];
        printf("%-16s %.4f  (%.2f,%.2f)-(%.2f,%.2f) class %d\n",
               d->label,
               (double)d->score,
               (double)d->x1,
               (double)d->y1,
               (double)d->x2,
               (double)d->y2,
               d->class_id);
    }
    fflush(stdout);
}

// Inference on the packaged sample, to be compared against the host reference.
// With dump_raw the quantized output tensor goes to stdout instead of the
// decoded detections, which tells a decode difference from an inference one.
static bool run_sample(const char* device, bool dump_raw) {
    int input_fd = load_sample(SAMPLE_PATH);
    if (input_fd < 0) {
        return false;
    }
    Detector* detector = detector_start(MODEL_PATH,
                                        LABELS_PATH,
                                        device,
                                        input_fd,
                                        INPUT_SIZE,
                                        INPUT_WIDTH,
                                        INPUT_HEIGHT);
    if (!detector) {
        fprintf(stderr, "test_detect: detector_start failed\n");
        close(input_fd);
        return false;
    }
    Detection detections[MAX_DETECTIONS];
    size_t num = detector_run(detector, detections, MAX_DETECTIONS);
    if (dump_raw) {
        size_t size            = 0;
        const unsigned char* q = detector_output(detector, &size);
        fwrite(q, 1, size, stdout);
    } else {
        print_detections("sample", detections, num);
    }
    fflush(stdout);
    detector_stop(detector);
    close(input_fd);
    return true;
}

// One live frame straight from the preprocessing buffer, to check the fd
// hand-off before the two are wired together in 2-D.
static bool run_live(const char* device) {
    Capture* capture = capture_start(640, 360, INPUT_WIDTH, INPUT_HEIGHT, 10.0);
    if (!capture) {
        fprintf(stderr, "test_detect: capture_start failed\n");
        return false;
    }
    Detector* detector = detector_start(MODEL_PATH,
                                        LABELS_PATH,
                                        device,
                                        capture_output_fd(capture),
                                        capture_output_size(capture),
                                        INPUT_WIDTH,
                                        INPUT_HEIGHT);
    if (!detector) {
        fprintf(stderr, "test_detect: detector_start failed\n");
        capture_stop(capture);
        return false;
    }
    bool ok = capture_next(capture) != NULL;
    if (ok) {
        Detection detections[MAX_DETECTIONS];
        print_detections("live", detections, detector_run(detector, detections, MAX_DETECTIONS));
    } else {
        fprintf(stderr, "test_detect: capture_next failed\n");
    }
    detector_stop(detector);
    capture_stop(capture);
    return ok;
}

int main(int argc, char** argv) {
    openlog("yolov5_detector", LOG_PID, LOG_USER);

    // run.sh rejects arguments starting with '-', hence the bare words:
    //   dump  write the raw output tensor to stdout instead of detections
    //   cpu   run on cpu-tflite instead of the DLPU, to tell the two apart
    bool dump_raw       = false;
    const char* device  = DETECTOR_DEFAULT_DEVICE;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "dump") == 0) {
            dump_raw = true;
        } else if (strcmp(argv[i], "cpu") == 0) {
            device = "cpu-tflite";
        } else {
            fprintf(stderr, "test_detect: unknown argument %s\n", argv[i]);
            return 1;
        }
    }

    // A raw dump must be the only thing on stdout, so skip the live run.
    bool ok = run_sample(device, dump_raw) && (dump_raw || run_live(device));
    closelog();
    return ok ? 0 : 1;
}

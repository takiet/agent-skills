#pragma once

#include <cstddef>

struct Detector;

struct Detection {
    // Corners in the model's padded input space (0..width, 0..height).
    float x1, y1, x2, y2;
    float score;
    int class_id;
    const char* label;  // owned by the Detector
};

// The DLPU, i.e. what detector_start uses unless a test asks for another one.
#define DETECTOR_DEFAULT_DEVICE "a9-dlpu-tflite"

// Load the model onto the named larod device and bind its input to input_fd,
// which must hold a width x height RGB-interleaved image of input_size bytes.
// The fd is not copied from, so whoever writes it (see capture.h) can keep
// writing in place between runs. Returns nullptr on failure.
Detector* detector_start(const char* model_path,
                         const char* labels_path,
                         const char* device,
                         int input_fd,
                         size_t input_size,
                         unsigned width,
                         unsigned height);

// Run inference on the current contents of the input fd and decode the result.
// Writes at most max_out detections and returns how many were written.
size_t detector_run(Detector* detector, Detection* out, size_t max_out);

// The raw, still quantized output tensor of the most recent run, for comparing
// against a host reference. Owned by the Detector.
const unsigned char* detector_output(const Detector* detector, size_t* size);

void detector_stop(Detector* detector);

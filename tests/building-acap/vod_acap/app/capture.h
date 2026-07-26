#pragma once

#include <cstddef>
#include <cstdint>

struct Capture;

// Start an NV12 VDO stream of src_width x src_height and a larod preprocessing
// job that converts each frame into an RGB-interleaved dst_width x dst_height
// buffer. The image is scaled to fit the width without stretching and placed at
// the top of the buffer; the remaining bottom rows stay zero.
// Returns nullptr on failure.
Capture* capture_start(unsigned src_width,
                       unsigned src_height,
                       unsigned dst_width,
                       unsigned dst_height,
                       double framerate);

// Capture and preprocess one frame. Returns a pointer to the RGB output buffer,
// valid until capture_stop(), or nullptr on failure.
const uint8_t* capture_next(Capture* capture);

// The memfd backing the RGB output buffer, so the inference job can read it
// without a copy. Owned by the Capture.
int capture_output_fd(const Capture* capture);
size_t capture_output_size(const Capture* capture);

// Number of output rows holding image data; the rows below are padding.
unsigned capture_content_height(const Capture* capture);

void capture_stop(Capture* capture);

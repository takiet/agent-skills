#pragma once

#include <cstddef>

#include "detector.h"

struct Overlay;

// Start drawing overlays on the streams that viewers open. content_width and
// content_height describe the coordinate space the detections are expressed in,
// i.e. the captured frame. Returns nullptr on failure.
Overlay* overlay_start(unsigned content_width, unsigned content_height);

// Replace what is drawn with the given detections, and pick up streams that
// appeared or disappeared since the last call. Nothing is drawn while no one is
// viewing, which is not an error. Returns false only on a fatal error.
bool overlay_draw(Overlay* overlay, const Detection* detections, size_t count);

// Number of viewer streams currently being drawn on. Zero means nobody is
// watching, which is the usual reason for "I cannot see anything".
size_t overlay_stream_count(const Overlay* overlay);

void overlay_stop(Overlay* overlay);

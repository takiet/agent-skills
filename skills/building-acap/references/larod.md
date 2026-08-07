# Larod — Machine Learning API

Unified C API for running machine-learning inference and hardware-accelerated image
pre-processing on Axis devices. Larod dispatches *jobs* to a *device* (DLPU, GPU, or CPU)
through a background service.

**API specification:** https://developer.axis.com/acap/acap-native-sdk-version-12/api/src/api/larod/html/index.html

## Build Requirements

### Makefile

```make
PKGS = gio-2.0 gio-unix-2.0 liblarod vdostream
```

### Source files

```c
#include "larod.h"
```

### manifest.json
request `video` group and (on DLPU chips) the deep-learning processor:

```json
"resources": {
  "linux": { "user": { "groups": ["video"] } },
  "deepLearningProcessor": { "enabled": true, "required": true }
}
```
`deepLearningProcessor` is needed only if a job actually runs on the DLPU. If **both** the
pre-processing and the inference job use CPU devices (`cpu-proc`, `cpu-tflite`), omit it — a
resource you don't use is one more thing that can stop the app from starting. Add it in the same
increment as the code that switches to a DLPU device. Larod models are chip-specific, so the
example ships one `manifest.json.<chip>` per target (`artpec8`, `artpec9`, `cv25`, `edgetpu`,
`cpu`) and selects it at build time.

## Device names

**Ask the device, don't trust this page.** Device names are not stable across chips or firmware
— ARTPEC-8 prefixes its DLPU with `axis-` and ARTPEC-9 does not — and `larodGetDevice()` simply
fails on a name the device doesn't have. Call `larodListDevices()` /
`larodGetDeviceName()` once at startup and log every name; then a wrong guess shows up as a log
line on the first run instead of as an unexplained failure later.

Measured lists, as examples of what you get back:

| Chip / device | `larodListDevices()` output |
|---|---|
| ARTPEC-9 (Q1728, OS 12.11) | `cpu-tflite`, `a9-dlpu-tflite`, `a9-dlpu-native`, `armnn-cpu-tflite`, `cpu-proc`, `a9-gpu-proc` |
| ARTPEC-8 | `cpu-tflite`, `axis-a8-dlpu-tflite`, `axis-a8-dlpu-native`, `axis-a8-dlpu-proc`, `axis-a8-gpu-proc`, `axis-ace-proc`, `cpu-proc` |

Two families, and you generally need one from each:

- **`*-tflite` / `*-native`** — inference backends. `cpu-tflite` is present everywhere and is your
  fallback and your debugging tool (see [Verifying inference output](#verifying-inference-output)).
- **`*-proc`** — image pre-processing backends (format conversion, scale, crop). A pre-processing
  job runs on one of these, *not* on the tflite device. `cpu-proc` always exists; `a9-gpu-proc` /
  `axis-a8-gpu-proc` / `axis-ace-proc` are the accelerated variants.

Other platforms: CV25 is `ambarella-cvflow` (PLANAR RGB, float32 output), Google Edge TPU is
`google-edge-tpu-tflite`.

## Core objects

| Type | Meaning |
|---|---|
| `larodConnection` | Session with the larod service. |
| `larodModel` | A loaded model bound to one device. |
| `larodTensor` | Input/output tensor (dims, pitches, data type, backing fd). |
| `larodJobRequest` | Binds a model + input tensors + output tensors for execution. |
| `larodMap` | Key/value params (e.g. crop settings for pre-processing). |
| `larodError` | Error object with `->msg` and `->code`. |

## Two chained jobs, two different patterns

A detection app is almost always **two** larod jobs, and the thing to understand before writing
either is that they have *opposite* buffer situations — which is why they're set up differently:

```
VDO buffer pool          your memfd                larod-owned
(rotates, N fds)         (one fd, stable)          (one fd, stable)
      │                        │                         │
      └── pre-processing job ──┴──── inference job ──────┘
          input: N tensors          input: 1 tensor
          (one per buffer)          (bound once, forever)
```

The pre-processing job's input rotates, so it needs one tracked tensor per buffer fd. The
inference job's input is *your own* buffer, which never moves — so it is bound exactly once at
startup and every subsequent frame is just `larodRunJob()`. Putting a per-frame rebinding loop
on the inference side is wasted work; putting none on the pre-processing side simply fails.

## Workflow — the inference job

```c
larodError* error = NULL;
larodConnection* conn = NULL;

// 1. Connect
larodConnect(&conn, &error);

// 2. Load model onto a device (name confirmed via larodListDevices(), see above)
const larodDevice* device = larodGetDevice(conn, "a9-dlpu-tflite", 0, &error);
int model_fd = open("model.tflite", O_RDONLY);
larodModel* model = larodLoadModel(conn, model_fd, device,
                                   LAROD_ACCESS_PRIVATE, "my model", NULL, &error);
close(model_fd);                    // larod has it now

// 3a. INPUT: create (not alloc) — the buffer is yours (the pre-processing output).
//     Created tensors carry the model's dims/dtype/layout with fd == -1.
size_t num_inputs;
larodTensor** inputs = larodCreateModelInputs(model, &num_inputs, &error);

// 3b. OUTPUT: alloc — let larod own the buffer, then mmap it to read results.
size_t num_outputs;
larodTensor** outputs = larodAllocModelOutputs(conn, model,
                          LAROD_FD_PROP_READWRITE | LAROD_FD_PROP_MAP,
                          &num_outputs, NULL, &error);

// 4. Check the geometry against what you built, and log it. A mismatch here is
//    silent: it shifts every box rather than failing.
const larodTensorDims* dims = larodGetTensorDims(inputs[0], &error);   // len==4: NHWC
larodTensorDataType    type = larodGetTensorDataType(inputs[0], &error);

// 5. Bind the input to your buffer — ONCE. dup() because larod takes ownership
//    of the fd it is given. Tracking freezes fd/offset/size/props for good,
//    which is fine: the same memfd is rewritten in place every frame.
larodSetTensorFdProps(inputs[0], LAROD_FD_PROP_READWRITE | LAROD_FD_PROP_MAP, &error);
larodSetTensorFd(inputs[0], dup(input_fd), &error);
larodSetTensorFdOffset(inputs[0], 0, &error);
larodSetTensorFdSize(inputs[0], input_size, &error);
larodTrackTensor(conn, inputs[0], &error);

// 6. Build the job request once...
larodJobRequest* req = larodCreateJobRequest(model, inputs, num_inputs,
                                             outputs, num_outputs, NULL, &error);

// 7. ...and per frame, that's the whole loop. No rebinding, no input switching.
larodRunJob(conn, req, &error);                         // blocking

// 8. Read outputs (mmap the output tensor fd once, at startup)
int    fd   = larodGetTensorFd(outputs[0], &error);
size_t size; larodGetTensorFdSize(outputs[0], &size, &error);
void*  data = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
```

### Create vs. Alloc — who owns the buffer

Step 3 is where the most common larod mistake happens, and it fails in a confusing way: pairing
`larodAllocModelInputs()` with `larodSetTensorFd()` looks like the natural way to feed your own
frame in, but it cannot work. The `Alloc*` calls allocate a buffer *and* track the tensor for
you — `larod.h` says they behave "as if `larodTrackTensor()` would have been called" — and a
tracked tensor's fd, size, offset and props can never be replaced. So the `Set...Fd` call is
rejected, and you also end up calling `larodTrackTensor()` a second time on a tensor the header
says should only be tracked once.

Pick by who owns the memory:

| You want | Use | Then |
|---|---|---|
| larod to allocate the buffer (normal for **outputs**) | `larodAllocModelInputs/Outputs()` | already tracked; mmap the fd to read. The fd is fixed for life |
| to supply your own buffer — `memfd`, VDO dma-buf (normal for **inputs**) | `larodCreateModelInputs/Outputs()` | returns fd = −1 tensors with the model's geometry; set props/fd/offset/size, then `larodTrackTensor()` once |
| to describe a buffer that has **no model** behind it (pre-processing input/output) | `larodCreateTensors(n, ...)` | returns blank tensors; you supply dtype, layout, dims and pitches yourself — see below |

## Cleanup

```c
larodDestroyJobRequest(&req);
munmap((void*)out_data, out_size);                       // the mmapped output
larodDestroyTensors(conn, &inputs, num_inputs, NULL);
larodDestroyTensors(conn, &outputs, num_outputs, NULL);
larodDestroyModel(&model);
larodDisconnect(&conn, NULL);
```

Order matters a little: destroy the tensors while the connection is still open, since tracked
tensors are registered with the service. The fds you `dup()`ed into tensors are larod's to close,
so don't close them yourself — but a `memfd` you created and mmapped is yours, so `munmap()` and
`close()` it. If you kept **one tensor array per VDO buffer** for a pre-processing job, destroy
each of them in a loop; that's the reason to cache the array pointer and its length rather than
just element 0.

## Pre-processing jobs (format/size conversion on-device)

If the VDO frame format or resolution doesn't match the model input, run a **second larod model**
as a pre-processing step (scale/convert/crop) before the inference job. Three pieces: the model
(a `larodMap`, no file), the input tensors (one per VDO buffer), and the output tensor (your
`memfd`, which then feeds inference).

Note that pre-processing tensors are built with **`larodCreateTensors(n, ...)`**, not
`larodCreateModelInputs()`. There's no model geometry to inherit — a pre-processing model is
defined by parameters — so you describe the buffer yourself: dtype, layout, dims, pitches.

Layouts: `LAROD_TENSOR_LAYOUT_NHWC` (RGB interleaved), `..._NCHW` (planar RGB / CV25),
`..._420SP` (NV12).

### Defining the pre-processing model

A pre-processing "model" isn't a file — it's a set of parameters, so you load it with
**`model_fd = -1`** and hand the conversion over in a `larodMap`. That inversion of
`larodLoadModel()`'s normal usage is not something you'd guess, and **the parameter key strings
are not declared in `larod.h`** either; they're only in the
[Preprocessing documentation](https://developer.axis.com/acap/acap-native-sdk-version-12/api/src/api/larod/html/md__opt_builder-doc_larod_doc_preprocessing.html)
and the official `object-detection-yolov5` example. So they're worth having here in full:

```c
larodMap* map = larodCreateMap(&error);
larodMapSetStr(map,     "image.input.format",    "nv12", &error);
larodMapSetIntArr2(map, "image.input.size",      640, 360, &error);
larodMapSetInt(map,     "image.input.row-pitch", 640, &error);
larodMapSetStr(map,     "image.output.format",   "rgb-interleaved", &error);
larodMapSetIntArr2(map, "image.output.size",     640, 360, &error);
larodMapSetInt(map,     "image.output.row-pitch", 1920, &error);   // bytes: 640 * 3

// fd = -1: this model is defined by the map, not by a file on disk.
// Note the device is a *-proc one (cpu-proc, a9-gpu-proc, ...), not the tflite device.
larodModel* pp = larodLoadModel(conn, -1, pp_device,
                                LAROD_ACCESS_PRIVATE, "pp", map, &error);
```

`row-pitch` is in **bytes**, not pixels — for RGB-interleaved that's `width * 3`. The *input*
pitch is not `width` either in general: take it from the stream (`vdo_stream_get_info()` reports
`pitch`), because VDO may hand you padded rows. Getting `image.output.format` wrong in the other
direction (`bgr-interleaved` vs `rgb-interleaved`) produces a perfectly plausible image with red
and blue swapped, which no numeric check catches. Dump the buffer and look at it —
`ffmpeg -f rawvideo -pix_fmt rgb24 -s WxH -i dump out.png`, then the same as `bgr24`, and keep
whichever has natural skin and sky tones.

Devices: use a `*-proc` device here, not the tflite one. `cpu-proc` (libyuv) is always present and
is a fine default at a few fps — on ARTPEC-9 the DLPU exposes no pre-processing device at all, so
the accelerated alternative is `a9-gpu-proc`.

### One input tensor per VDO buffer

This is where the rotating buffer pool has to be handled. Each distinct VDO buffer fd gets its own
tensor, built once and cached; per frame you look it up and, if the job request already exists,
point it at that tensor:

```c
// Once per distinct VDO buffer fd (VDO recycles a small pool, so this settles
// after a few frames). Keyed by the fd VDO reports, not the dup()ed one.
int     vdo_fd = vdo_buffer_get_fd(buffer);
int64_t offset = vdo_buffer_get_offset(buffer);

// VDO may hand out vmem rather than dma-buf -- check buffer.type from
// vdo_stream_get_info() at startup. The conversion consumes the offset, so
// reset it to 0 afterwards or the frame comes out shifted.
int buf_fd = vdo_fd;
if (!src_is_dmabuf) {                                   // buffer.type == "vmem"
    buf_fd = larodConvertVmemFdToDmabuf(vdo_fd, offset, &error);
    offset = 0;
}

larodTensor** t = larodCreateTensors(1, &error);        // blank: no model behind it
larodSetTensorDataType(t[0], LAROD_TENSOR_DATA_TYPE_UINT8, &error);
larodSetTensorLayout(t[0], LAROD_TENSOR_LAYOUT_420SP, &error);          // NV12
larodBuildTensorDims(t[0], LAROD_TENSOR_LAYOUT_420SP, width, height, 3, &error);
larodBuildTensorPitches(t[0], LAROD_TENSOR_LAYOUT_420SP, pitch, height, 3, &error);
larodSetTensorFdProps(t[0], LAROD_FD_PROP_MAP | LAROD_FD_PROP_DMABUF, &error);
larodSetTensorFd(t[0], dup(buf_fd), &error);            // larod owns and closes it
larodSetTensorFdOffset(t[0], offset, &error);
larodSetTensorFdSize(t[0], vdo_buffer_get_capacity(buffer), &error);
larodTrackTensor(conn, t[0], &error);                   // freezes the fd
cache_put(vdo_fd, t);                                   // keep the ARRAY, for cleanup
```

Then per frame, once you have the cached tensor array for this buffer:

```c
if (!pp_req) {
    pp_req = larodCreateJobRequest(pp_model, t, 1, pp_output, 1, NULL, &error);
} else {
    larodSetJobRequestInputs(pp_req, t, 1, &error);     // switch to this buffer
}
larodRunJob(conn, pp_req, &error);
```

A small fixed-size array is enough for the cache — the pool is a handful of buffers (`buffer.count`
in the VDO settings), so a linear scan over it costs nothing. Log an error if it ever overflows
rather than silently growing.

### Letterboxing a 16:9 stream into a square model input

Square model input from a 16:9 sensor needs no padding code at all. Allocate the full buffer with
`memfd_create()` and zero it once, then build the output tensor asymmetrically:

```c
larodBuildTensorDims(t, NHWC, content_w, content_h, 3, &error);        // content: 640x360
larodBuildTensorPitches(t, NHWC, content_w * 3, content_h, 3, &error); // content
larodSetTensorFdSize(t, dst_w * dst_h * 3, &error);                    // FULL: 640x640
```

RGB-interleaved rows are contiguous, so the rows the job never writes stay zero — that *is* the
padding, and it stays valid every frame because only the top is ever rewritten.

The same memfd is bound once as the inference input (step 5 above), so VDO → pre-process →
inference copies nothing. Boxes come back in the padded 640×640 space: undo the scale, and note
there is no offset to subtract because the padding is all at the bottom.

## Inspect the model — don't hardcode its geometry

You usually can't open a `.tflite` on the build host (no TF tooling), and a wrong guess about
input dtype/layout or output shape yields *silently wrong detections*, not a crash — so it's the
kind of bug you chase for hours. The loaded model is the source of truth: after
`larodAllocModelInputs/Outputs`, read the geometry back and log it once at startup, so a mismatch
shows up on the first run instead of in the decoded boxes.

- **Input** — `larodGetTensorDims` (4-D `NHWC` vs `NCHW` tells you RGB-interleaved vs planar),
  `larodGetTensorDataType` (uint8 vs float32 tells you whether the model wants normalized input),
  `larodGetTensorPitches` (row stride/padding). These drive your pre-processing and the VDO
  format — confirm them rather than assuming `640×640×3` float from the task text.
- **Output** — `larodGetTensorDims` gives the head shape (e.g. `[1,25200,85]` vs a transposed
  variant) and `larodGetTensorFdSize` the byte count. Assert the mmapped size equals what your
  decoder expects and bail loudly if not, rather than reading past/short of the buffer.
- **Quantization** — DLPU outputs are often uint8 with a per-tensor scale/zero-point baked into
  the model; you need both to recover real scores/coordinates. **larod cannot give you these** —
  read them off the tflite on the host instead, or take them from your model-conversion step (the
  `tensorflow-to-larod-*` examples). Guessing a fixed factor gives you an app that runs happily
  and detects the wrong things.

Want the shapes *before* deploying? A `.tflite` is a FlatBuffer — `netron model.tflite`, `flatc`,
or a few `python3` lines against the tflite schema print input/output shapes and quantization
params offline. Handy for writing the decoder up front, but treat runtime introspection as
authoritative since that's what actually runs on the device.

## Verifying inference output

"The device result should match the host result" is the obvious acceptance criterion for an
inference app, and on a DLPU it **fails even when everything is correct**. The DLPU is not
bit-exact with a CPU TFLite interpreter. Measured on YOLOv5s with byte-identical input
(1,228,800 bytes, verified by checksum), comparing the raw `[1,25200,85]` uint8 output tensor:

| Comparison | Elements exactly equal | mean \|Δ\| | max \|Δ\| |
|---|---|---|---|
| device `cpu-tflite` vs. host (ai-edge-litert) | 94.95% | 0.079 LSB | 42 |
| device `a9-dlpu-tflite` vs. host | 48.31% | 1.00 LSB | 221 |
| device `a9-dlpu-tflite` vs. device `cpu-tflite` | 48.37% | 1.00 LSB | 219 |

After decoding (conf 0.25), that 1 LSB shifted box coordinates by up to 11.9 px, scores by up to
0.025, and **dropped one detection that sat on the threshold** (0.271 → below 0.25). So:

- Write the acceptance criterion with **tolerances** — a few px on coordinates, ~0.03 on scores —
  and exclude detections near the confidence threshold, which can legitimately appear or vanish.
- **Bisect with `cpu-tflite`.** Keep the device name selectable in your test binary. Running the
  same code on `cpu-tflite` and getting near-host results proves your buffer handling, layout and
  decoder are all correct and the remaining difference is DLPU numerics. This is the single most
  useful debugging move here, and it costs one string.
- Compare **raw tensors, not decoded detection lists**. NMS and thresholding throw away exactly
  the information you need to tell "1 LSB of hardware noise" from "my decoder is wrong".
- Quantized scores are discrete, so **exact ties between anchors are normal** (measured: 3 tied
  pairs among 39 candidates). Which one survives NMS depends on sort stability — and NumPy's
  default `argsort` is not stable — so the host has no "correct" ordering either. Treat a tie
  that swaps which box represents the same object as a pass.

## Notes & gotchas

- **Power throttling:** `larodLoadModel()` and `larodRunJob()` can fail with
  `LAROD_ERROR_POWER_NOT_AVAILABLE`. This is expected under load — clear the error, back off
  (`usleep`), and retry rather than aborting.
- Loading a model can take **up to several minutes** on some devices; log a message.
- Output quantization differs by chip: e.g. uint8 (0-255, divide by 2.55 for %) on most
  DLPUs vs. float32 on CV25.
- Convert TensorFlow/PyTorch models to the device format first — see the
  `tensorflow-to-larod-*` examples.

## Related

- Source frames → [vdo.md](vdo.md)
- Visualize detections → [bbox.md](bbox.md) (rectangles only) / [overlay.md](overlay.md) (labels)

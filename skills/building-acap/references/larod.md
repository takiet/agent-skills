# Larod — Machine Learning API

Unified C API for running machine-learning inference and hardware-accelerated image
pre-processing on Axis devices. Larod dispatches *jobs* to a *device* (DLPU, GPU, or CPU)
through a background service.

**API specification:** https://developer.axis.com/acap/acap-native-sdk-version-12/api/src/api/larod/html/index.html

## Build & manifest

```make
PKGS = gio-2.0 gio-unix-2.0 liblarod vdostream
```

```c
#include "larod.h"
```

`manifest.json` — request `video` group and (on DLPU chips) the deep-learning processor:

```json
"resources": {
  "linux": { "user": { "groups": ["video"] } },
  "deepLearningProcessor": { "enabled": true, "required": true }
}
```
Omit `deepLearningProcessor` for a CPU-only build. Larod models are chip-specific, so the
example ships one `manifest.json.<chip>` per target (`artpec8`, `artpec9`, `cv25`, `edgetpu`,
`cpu`) and selects it at build time.

## Device names (`runOptions` / first arg)

| Chip | Device name |
|---|---|
| ARTPEC-8 DLPU (TFLite) | `axis-a8-dlpu-tflite` |
| ARTPEC-9 DLPU | `axis-a9-dlpu-tflite` |
| CV25 | `ambarella-cvflow` (uses PLANAR RGB, float32 output) |
| Google Edge TPU | `google-edge-tpu-tflite` |
| CPU | `cpu-tflite` |

List them at runtime with `larodListDevices()` / `larodGetDeviceName()`.

## Core objects

| Type | Meaning |
|---|---|
| `larodConnection` | Session with the larod service. |
| `larodModel` | A loaded model bound to one device. |
| `larodTensor` | Input/output tensor (dims, pitches, data type, backing fd). |
| `larodJobRequest` | Binds a model + input tensors + output tensors for execution. |
| `larodMap` | Key/value params (e.g. crop settings for pre-processing). |
| `larodError` | Error object with `->msg` and `->code`. |

## Workflow

```c
larodError* error = NULL;
larodConnection* conn = NULL;

// 1. Connect
larodConnect(&conn, &error);

// 2. Load model onto a device
const larodDevice* device = larodGetDevice(conn, "axis-a8-dlpu-tflite", 0, &error);
int model_fd = open("model.tflite", O_RDONLY);
larodModel* model = larodLoadModel(conn, model_fd, device,
                                   LAROD_ACCESS_PRIVATE, "my model", NULL, &error);

// 3. Allocate model input/output tensors
size_t num_inputs, num_outputs;
larodTensor** inputs  = larodAllocModelInputs(conn, model, 0, &num_inputs, NULL, &error);
larodTensor** outputs = larodAllocModelOutputs(conn, model,
                          LAROD_FD_PROP_READWRITE | LAROD_FD_PROP_MAP,
                          &num_outputs, NULL, &error);

// 4. Inspect input geometry (drives the VDO stream resolution/format)
const larodTensorDims*    dims    = larodGetTensorDims(inputs[0], &error);   // len==4: NHWC
const larodTensorPitches* pitches = larodGetTensorPitches(inputs[0], &error);

// 5. Build a reusable job request
larodJobRequest* req = larodCreateJobRequest(model, inputs, 1,
                                             outputs, num_outputs, NULL, &error);

// 6. Per frame: point the input tensor at the VDO buffer's dma-buf, then run
larodSetTensorFd(inputs[0], dup(vdo_buffer_get_fd(buf)), &error);
larodSetTensorFdOffset(inputs[0], vdo_buffer_get_offset(buf), &error);
larodSetTensorFdSize(inputs[0], vdo_buffer_get_capacity(buf), &error);
larodTrackTensor(conn, inputs[0], &error);              // once per distinct buffer
larodSetJobRequestInputs(req, inputs, 1, &error);
larodRunJob(conn, req, &error);                         // blocking

// 7. Read outputs (mmap the output tensor fd)
int    fd   = larodGetTensorFd(outputs[0], &error);
size_t size; larodGetTensorFdSize(outputs[0], &size, &error);
void*  data = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
larodTensorDataType t = larodGetTensorDataType(outputs[0], &error);
```

## Cleanup

```c
larodDestroyJobRequest(&req);
larodDestroyTensors(conn, &outputs, num_outputs, &error);
larodDestroyTensors(conn, &inputs, num_inputs, &error);
larodDestroyModel(&model);
close(model_fd);
larodDisconnect(&conn, NULL);
```

## Pre-processing jobs (format/size conversion on-device)

If the VDO frame format or resolution doesn't match the model input, run a **second larod
model** as a pre-processing step (crop/scale/convert) before the inference job:

- Build an input tensor describing the VDO frame: `larodCreateTensors()`,
  `larodSetTensorDataType(..., LAROD_TENSOR_DATA_TYPE_UINT8)`,
  `larodSetTensorLayout(...)`, `larodBuildTensorDims(...)`, `larodBuildTensorPitches(...)`,
  `larodSetTensorFdProps(..., LAROD_FD_PROP_MAP | LAROD_FD_PROP_DMABUF)`.
- Layouts: `LAROD_TENSOR_LAYOUT_NHWC` (RGB), `..._NCHW` (planar RGB / CV25),
  `..._420SP` (NV12).
- Chain: run the pre-processing job, whose output tensors become the inference job's inputs.

## dma-buf conversion

VDO may hand out `vmem` (not dma-buf) buffers. When `buffer.type` != `dmabuf`, convert with
`larodConvertVmemFdToDmabuf(vdo_fd, offset, &error)` before setting the tensor fd.
`dup()` the fd before `larodSetTensorFd()` because larod takes ownership of what it's given.

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
  the model; you need both to recover real scores/coordinates. Get them from your
  model-conversion step (the `tensorflow-to-larod-*` examples) or by inspecting the tflite — don't
  assume a fixed factor (see the quantization gotcha below).

Want the shapes *before* deploying? A `.tflite` is a FlatBuffer — `netron model.tflite`, `flatc`,
or a few `python3` lines against the tflite schema print input/output shapes and quantization
params offline. Handy for writing the decoder up front, but treat runtime introspection as
authoritative since that's what actually runs on the device.

## Notes & gotchas

- **Power throttling:** `larodLoadModel()` and `larodRunJob()` can fail with
  `LAROD_ERROR_POWER_NOT_AVAILABLE`. This is expected under load — clear the error, back off
  (`usleep`), and retry rather than aborting.
- Loading a model can take **up to several minutes** on some devices; log a message.
- Track each distinct VDO buffer fd only once (`larodTrackTensor`), then reuse the tensor.
- Output quantization differs by chip: e.g. uint8 (0-255, divide by 2.55 for %) on most
  DLPUs vs. float32 on CV25.
- Convert TensorFlow/PyTorch models to the device format first — see the
  `tensorflow-to-larod-*` examples.

## Related

- Source frames → [vdo.md](vdo.md)
- Visualize detections → [bbox.md](bbox.md)

# Preprocessing Benchmark: `cpu-proc` vs `a9-gpu-proc`

Comparison of larod preprocessing backends on an Axis Q1728 (ARTPEC-9, aarch64, AXIS OS 12.11.x).
`cpu-proc` is the libyuv-based backend; `a9-gpu-proc` is the OpenCL/GPU backend.

Date: 2026-07-31 · Application: `ppcomp` (ACAP Native SDK 12.11.0)

## Method

- A single 1920x1080 NV12 frame is captured once at startup (pitch 1920, `buffer.type=dmabuf`)
  and reused for every measurement, so capture jitter never enters the numbers.
- Each scenario runs 3 warm-up iterations (discarded) followed by 50 measured iterations.
- Only `larodRunJob()` is timed, using `clock_gettime(CLOCK_MONOTONIC)`. Model loading, tensor
  setup and job-parameter changes are all outside the timed region.
- The 50 crop positions are generated from a fixed-seed LCG and rounded to even coordinates
  (NV12 chroma is 2x2 subsampled). Both backends consume the identical sequence.
- Reported figures are the mean of 4 independent process runs; each run captured its own frame.

## Results

Mean per-operation latency, lower is better.

| Scenario | Output | `cpu-proc` | `a9-gpu-proc` | Outcome |
|---|---|---:|---:|---|
| Crop (300x300)      | NV12            | **1.015 ms**  | 3.838 ms | CPU **3.8x faster** |
| Scale (300x300)     | NV12            | 9.406 ms      | **4.487 ms** | GPU **2.1x faster** |
| Convert, interleaved | RGB 1920x1080  | **7.539 ms**  | 8.985 ms | CPU 1.2x faster |
| Convert, planar      | RGB 1920x1080  | **22.118 ms** | N/A | GPU unsupported |

Within-run standard deviation was 0.05–0.18 ms across all cells. Run-to-run variation of the
mean was of the same order and stayed under 3%, so the ranking above is stable.

Per-run means (ms):

| Scenario / device | Run 1 | Run 2 | Run 3 | Run 4 |
|---|---:|---:|---:|---:|
| crop / cpu-proc  | 0.997  | 1.036  | 0.996  | 1.032  |
| crop / gpu-proc  | 3.870  | 3.838  | 3.843  | 3.799  |
| scale / cpu-proc | 9.408  | 9.386  | 9.288  | 9.543  |
| scale / gpu-proc | 4.602  | 4.436  | 4.441  | 4.467  |
| rgb-i / cpu-proc | 7.595  | 7.553  | 7.500  | 7.506  |
| rgb-i / gpu-proc | 9.036  | 9.028  | 8.858  | 9.018  |
| rgb-p / cpu-proc | 22.084 | 22.141 | 22.048 | 22.199 |

## Findings

**Offloading preprocessing to the GPU pays off for scaling only.** Scaling a full-frame 1920x1080
NV12 down to 300x300 is roughly twice as fast on `a9-gpu-proc`. Every other operation measured
here is equal or better on `cpu-proc`.

**Cropping is markedly worse on the GPU** — 3.8x slower despite being the cheapest operation of
the four on the CPU. A plausible explanation is that per-job overhead dominates when the output
is only 300x300 and no resampling is required, but this was not investigated and remains a
hypothesis.

**`a9-gpu-proc` supports only one colour conversion: NV12 to interleaved RGB.** Requesting planar
RGB fails at job submission with:

> `Could not run job: This backend only supports color conversion from NV12 to RGB interleaved`

Operations that involve no colour conversion (NV12 to NV12 crop and scale) do run on the GPU.
If planar RGB is required, the GPU is not an option.

**Practical notes.** Both backends accept the crop window as a per-job parameter
(`image.input.crop`), so the preprocessing model is loaded once rather than per crop position.
Both also accept a `memfd`-backed output buffer directly; no dma-buf conversion was needed on
either the input or the output side.

## Correctness

Output correctness was verified visually rather than numerically, since channel-order and
plane-order mistakes do not show up in byte counts. Raw dumps of the original frame and of all
four processed outputs were converted to PNG on the host and reviewed. Crop placement was
additionally cross-checked against a host-side crop of the same region. Output sizes matched the
expected byte counts exactly (135,000 / 135,000 / 6,220,800 / 6,220,800).

## Limitations

- Results are from a single device and a single scene. Content-dependent effects, if any, were
  not explored.
- The GPU crop result is reported as measured; no profiling was done to explain it.
- Measurements cover steady-state throughput after warm-up. First-iteration cost is substantially
  higher on the GPU (up to 23 ms for RGB interleaved vs 9 ms steady-state) and matters for
  workloads that run preprocessing only occasionally.

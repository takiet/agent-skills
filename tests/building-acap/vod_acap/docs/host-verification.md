# Host-side verification of on-device inference

> Reference material from this project, not part of the `building-acap` skill. The procedure is
> written around this app's YOLOv5s pipeline (`tools/host_infer.py`, bottom-padded 640×640
> RGB-interleaved input), so treat it as a worked example rather than general guidance.

"The detections on the device should look like the ones from the host" is the standard acceptance
criterion for an inference app, and it needs a host side that actually exists. This is how to
build one, feed it exactly what the device saw, and compare without chasing differences that are
inherent to the hardware.

Two failure modes motivate all of it, and neither announces itself:

- A wrong **quantization scale** or a wrong **row-pitch** produces a running app with plausible
  but wrong output. No crash, no error in the log.
- A **DLPU is not bit-exact** with a host CPU interpreter, so a strict equality check fails even
  on correct code — see the `building-acap` skill's `references/larod.md` ("Verifying inference output") for measured numbers.

## Contents

- [Get the quantization parameters](#get-the-quantization-parameters) — the values larod won't give you
- [Feed both sides the same bytes](#feed-both-sides-the-same-bytes)
- [Look at the buffer](#look-at-the-buffer) — raw dump → PNG
- [Symptom → cause](#symptom--cause) — read this before debugging a wrong-looking image
- [Compare two dumps](#compare-two-dumps)
- [Make a fixed test input](#make-a-fixed-test-input)
- [Unverified](#unverified)

## Get the quantization parameters

A quantized model stores a per-tensor `scale` and `zero_point`, and your device decoder needs both
to turn uint8 tensor values back into scores and coordinates. **larod does not expose them.** Read
them off the `.tflite` on the host instead:

```bash
uv run tools/host_infer.py app/models/yolov5s.tflite app/sample/image.jpg
```

`tools/host_infer.py` carries a PEP 723 header, so `uv` installs `ai-edge-litert`, numpy and
pillow on first run and there is nothing to set up. It reports the values the decoder needs:

```
input : [1,640,640,3] uint8  scale=0.003921568859368563  zero_point=0
output: [1,25200,85]  uint8  scale=0.004144445527344942  zero_point=0
box coords: normalized 0..1  -> multiply by 640
```

That last line is worth the run on its own — whether a YOLO export emits normalized or pixel
coordinates varies between conversion toolchains, and assuming the wrong one gives you boxes that
are wrong by a factor of the input size. The script detects it from the data rather than assuming.

Do this **before** writing the decoder. These four facts are its entire contract.

## Feed both sides the same bytes

Once both sides run, a mismatch has at least two possible homes: pre-processing produced a
different image, or the decoder read the output differently. Separate them by making the input
identical rather than merely equivalent:

1. Have a test binary dump its **pre-processed buffer** to stdout; `run.sh` captures it to
   `output` (results on stdout, logs on stderr — see the testing section in `SKILL.md`).
2. Feed that exact buffer to the host: `uv run tools/host_infer.py <model> --raw-input output`.
3. Confirm the byte counts match, and ideally the checksums.

Now the host and the device have consumed the same bytes. If the results still differ,
pre-processing is exonerated and the difference is in inference or decode — at which point
re-running the device on `cpu-tflite` tells you whether even *that* is your code or just DLPU
numerics.

## Look at the buffer

Check the size before reaching for any tooling — a format or resolution mix-up shows up in the
byte count faster than anywhere else:

| Format | Bytes | 640×360 | 640×640 |
|---|---|---|---|
| RGB / BGR interleaved | `w × h × 3` | 691,200 | 1,228,800 |
| planar RGB | `w × h × 3` | 691,200 | 1,228,800 |
| NV12 (YUV420SP) | `w × h × 3 / 2` | 345,600 | 614,400 |
| Y800 / gray | `w × h` | 230,400 | 409,600 |

If the size doesn't match the formula, stop and fix that first. (A row-pitch larger than the width
breaks the formula legitimately — so log the pitch from the test binary.)

Then turn it into something you can look at:

```bash
ffmpeg -y -f rawvideo -pix_fmt rgb24 -s 640x640 -i output out.png   # RGB interleaved
ffmpeg -y -f rawvideo -pix_fmt nv12  -s 640x360 -i output out.png   # NV12
ffmpeg -y -f rawvideo -pix_fmt gray  -s 640x360 -i output out.png   # Y plane only
```

Reading an NV12 file as `gray` leaves the UV plane over and prints `Invalid buffer size`. The PNG
is still correct; ignore the warning.

**Check for an R/B swap by eye.** Render the same buffer as `bgr24` and compare: the one where
skin tones and sky look natural is right. Confusing `rgb-interleaved` with `bgr-interleaved` in
the larod pre-processing map survives every numeric check you're likely to write, and takes one
second to spot visually.

For letterboxing, crop the two regions and verify the padding is actually empty:

```bash
ffmpeg -y -f rawvideo -pix_fmt rgb24 -s 640x640 -i output -vf crop=640:360:0:0   content.png
ffmpeg -y -f rawvideo -pix_fmt rgb24 -s 640x640 -i output -vf crop=640:280:0:360 pad.png
```

A padding PNG of ~1 KB means a uniform region. To be strict about it, and to confirm the boundary
lands where you think:

```bash
python3 -c "
d = open('output','rb').read()
content = 640*360*3
print('size', len(d))
print('padding all zero:', set(d[content:]) == {0})
print('last content row max', max(d[359*1920:360*1920]))
print('first pad row max   ', max(d[360*1920:361*1920]))
"
```

## Symptom → cause

Wrong-looking output usually has one of a small number of causes, and the *shape* of the wrongness
identifies it. Consult this before starting a hunt:

| Symptom | Cause |
|---|---|
| Image shears diagonally / staircases | Row-pitch doesn't match the data. A one-byte error accumulates per row |
| `Invalid buffer size, packet size N < expected M` | Declared resolution/format disagrees with the file size — back to the table above |
| Red and blue swapped | `rgb24` vs `bgr24` — i.e. the wrong `image.output.format` in the larod map |
| Vertically squashed or stretched | Scaled without preserving aspect ratio; not actually letterboxing |
| Bottom half green or purple | NV12 with an uninitialized UV plane, or an RGB buffer being read as NV12 |
| Uniform noise | Offset error — check the fd offset, or a header being included |
| Top correct, bottom corrupt | Output buffer too small, or a write offset drifting between jobs |

For the shear case, sweep the width by ±1 — at the correct value the image snaps into alignment:

```bash
ffmpeg -y -f rawvideo -pix_fmt rgb24 -s 639x640 -i output skew.png
```

## Compare two dumps

Host expectation vs. device output, or before vs. after a change:

```bash
ffmpeg -y -i a.png -i b.png -filter_complex hstack sbs.png                       # side by side
ffmpeg -i a.png -i b.png -filter_complex psnr -f null - 2>&1 | grep -i psnr      # numeric
ffmpeg -y -i a.png -i b.png -filter_complex "blend=all_mode=difference,eq=contrast=8" diff.png
```

PSNR is `inf` for identical images; 40-ish dB is colour-conversion noise; below ~20 dB they're
different pictures. The difference render is the more useful of the two, because it shows *where*
— "only the bottom is wrong" and "everything is off by one row" are immediately visible and point
straight at the table above.

Compare **raw tensors rather than decoded detection lists** when checking inference. NMS and
confidence thresholds discard exactly the information that distinguishes 1 LSB of hardware noise
from a real decoder bug.

## Make a fixed test input

Going the other way — a deterministic input for a test binary:

```bash
# image -> RGB interleaved 640x640 with bottom padding
ffmpeg -y -i test.jpg -vf "scale=640:-1,pad=640:640:0:0:black" \
       -f rawvideo -pix_fmt rgb24 test_640x640.rgb      # expect 1228800 bytes

# image -> NV12 640x360
ffmpeg -y -i test.jpg -vf scale=640:360 -f rawvideo -pix_fmt nv12 test_640x360.nv12
```

In `pad=w:h:x:y:color`, the `x:y` is the paste position — `0:0` puts the content at the top and
the padding at the bottom, matching the device pipeline described in
the `building-acap` skill's `references/larod.md` ("Letterboxing").

## Unverified

Recorded honestly, so nobody trusts these more than they should:

- **Planar RGB.** ffmpeg's `gbrp` orders planes G, B, R, so handing it RGB-ordered planes shifts
  the colours. A `-vf shuffleplanes` reordering ought to fix it, but this hasn't been tried —
  confirm against real data before relying on it.
- **JPEG / H.264 dumps** should open directly with `ffmpeg -i output out.png`, untested here.
- **Row-pitch greater than width.** ffmpeg's `rawvideo` has no stride option, so a padded stride
  needs the rows sliced out in Python first.

## Related

- Raw dump inspection, in more detail and in Japanese → [docs/verify-raw-dumps.md](docs/verify-raw-dumps.md)
- Row-pitch → [docs/row-pitch.md](docs/row-pitch.md)
- Device-side inference and the DLPU-vs-host numbers → the `building-acap` skill's
  `references/larod.md`

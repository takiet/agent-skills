#!/usr/bin/env -S uv run --quiet
# /// script
# requires-python = ">=3.10,<3.13"
# dependencies = ["ai-edge-litert", "numpy", "pillow"]
# ///
"""Run the int8 YOLOv5s tflite model on the host, to produce the reference
output that the on-device implementation (Phase 2-B) is compared against.

Also prints the input/output quantization parameters, which the device-side
decoder needs and cannot get from larod.

Usage:
    uv run tools/host_infer.py <model.tflite> <image.jpg>
    uv run tools/host_infer.py <model.tflite> --raw-input output   # 640x640x3 RGB dump from the device

The preprocessing mirrors the device pipeline: scale to fit 640 wide keeping
the aspect ratio, place at the top of a zeroed 640x640 canvas (bottom padding,
no stretching). Use --raw-input to feed the device's own preprocessed buffer
instead, which isolates decode differences from preprocessing differences.
"""

import argparse
import json
import sys

import numpy as np
from ai_edge_litert.interpreter import Interpreter
from PIL import Image, ImageDraw

DEFAULT_LABELS = (
    "/Users/taki/Arbete/agent-skills/skills/building-acap/evals/files/models/labels.txt"
)


def load_labels(path):
    """The label file is a 90-entry COCO map with 'n/a' holes; the model has 80
    classes. Dropping the holes maps class index -> label correctly."""
    with open(path) as f:
        names = [line.strip() for line in f if line.strip()]
    return [n for n in names if n != "n/a"]


def letterbox(img, size):
    """Scale keeping aspect ratio, paste at top-left of a zeroed size x size
    canvas. Returns (rgb_array, scale) so boxes can be mapped back."""
    scale = min(size / img.width, size / img.height)
    w, h = round(img.width * scale), round(img.height * scale)
    canvas = Image.new("RGB", (size, size), (0, 0, 0))
    canvas.paste(img.convert("RGB").resize((w, h), Image.BILINEAR), (0, 0))
    return np.asarray(canvas), scale


def nms(boxes, scores, classes, iou_thr):
    """Class-wise NMS: offsetting boxes per class makes cross-class overlaps
    impossible, so a single global pass is equivalent to a per-class one."""
    offset = classes.astype(np.float32) * 8192.0
    b = boxes + offset[:, None]
    order = scores.argsort()[::-1]
    keep = []
    while order.size:
        i = order[0]
        keep.append(i)
        if order.size == 1:
            break
        rest = order[1:]
        xx1 = np.maximum(b[i, 0], b[rest, 0])
        yy1 = np.maximum(b[i, 1], b[rest, 1])
        xx2 = np.minimum(b[i, 2], b[rest, 2])
        yy2 = np.minimum(b[i, 3], b[rest, 3])
        inter = np.clip(xx2 - xx1, 0, None) * np.clip(yy2 - yy1, 0, None)
        area_i = (b[i, 2] - b[i, 0]) * (b[i, 3] - b[i, 1])
        area_r = (b[rest, 2] - b[rest, 0]) * (b[rest, 3] - b[rest, 1])
        iou = inter / (area_i + area_r - inter + 1e-9)
        order = rest[iou <= iou_thr]
    return keep


def main():
    p = argparse.ArgumentParser()
    p.add_argument("model")
    p.add_argument("image", nargs="?", help="test image (omit when using --raw-input)")
    p.add_argument("--raw-input", help="640x640x3 interleaved RGB dump from the device")
    p.add_argument("--labels", default=DEFAULT_LABELS)
    p.add_argument("--conf", type=float, default=0.25)
    p.add_argument("--iou", type=float, default=0.45)
    p.add_argument("--dump-input", help="write the preprocessed RGB buffer here")
    p.add_argument("--dump-output", help="write the dequantized raw output here (.npy)")
    p.add_argument("--save", help="write an annotated PNG here")
    p.add_argument("--json", dest="json_out", help="write detections here as JSON")
    args = p.parse_args()

    if not args.image and not args.raw_input:
        p.error("give an image or --raw-input")

    interp = Interpreter(model_path=args.model)
    interp.allocate_tensors()
    inp = interp.get_input_details()[0]
    out = interp.get_output_details()[0]

    in_scale, in_zp = inp["quantization"]
    out_scale, out_zp = out["quantization"]
    print("=== model ===", file=sys.stderr)
    print(f"input : shape={inp['shape'].tolist()} dtype={np.dtype(inp['dtype']).name} "
          f"scale={in_scale!r} zero_point={in_zp!r}", file=sys.stderr)
    print(f"output: shape={out['shape'].tolist()} dtype={np.dtype(out['dtype']).name} "
          f"scale={out_scale!r} zero_point={out_zp!r}", file=sys.stderr)

    size = int(inp["shape"][1])

    # --- preprocess -------------------------------------------------------
    if args.raw_input:
        raw = np.fromfile(args.raw_input, dtype=np.uint8)
        expected = size * size * 3
        if raw.size != expected:
            sys.exit(f"raw input is {raw.size} bytes, expected {expected}")
        rgb, scale = raw.reshape(size, size, 3), 1.0
        src_w = src_h = size
    else:
        img = Image.open(args.image)
        src_w, src_h = img.width, img.height
        rgb, scale = letterbox(img, size)

    if args.dump_input:
        rgb.tofile(args.dump_input)

    x = rgb.astype(np.float32) / 255.0
    if np.dtype(inp["dtype"]) != np.float32:
        x = np.round(x / in_scale + in_zp)
        info = np.iinfo(inp["dtype"])
        x = np.clip(x, info.min, info.max)
    x = x.astype(inp["dtype"])[None, ...]

    # --- inference --------------------------------------------------------
    interp.set_tensor(inp["index"], x)
    interp.invoke()
    q = interp.get_tensor(out["index"])
    pred = (q.astype(np.float32) - out_zp) * out_scale if out_scale else q.astype(np.float32)
    pred = pred[0]  # [25200, 85]

    if args.dump_output:
        np.save(args.dump_output, pred)

    # --- decode -----------------------------------------------------------
    # The ultralytics TF export emits box coords normalized to 0..1; some
    # variants emit pixels. Detect rather than assume, and say which.
    xywh = pred[:, :4]
    normalized = float(xywh.max()) <= 1.5
    print(f"box coords: {'normalized 0..1' if normalized else 'pixels'} "
          f"(max={float(xywh.max()):.4f})", file=sys.stderr)
    if normalized:
        xywh = xywh * size

    obj = pred[:, 4]
    cls_scores = pred[:, 5:]
    cls_id = cls_scores.argmax(1)
    conf = obj * cls_scores[np.arange(cls_scores.shape[0]), cls_id]

    m = conf >= args.conf
    xywh, conf, cls_id = xywh[m], conf[m], cls_id[m]
    print(f"candidates over conf {args.conf}: {int(m.sum())} / {pred.shape[0]}", file=sys.stderr)

    cx, cy, w, h = xywh.T
    boxes = np.stack([cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2], 1)
    keep = nms(boxes, conf, cls_id, args.iou)
    boxes, conf, cls_id = boxes[keep], conf[keep], cls_id[keep]

    labels = load_labels(args.labels)

    # --- report -----------------------------------------------------------
    dets = []
    for b, c, k in zip(boxes, conf, cls_id):
        x1, y1, x2, y2 = (float(v) for v in b)
        dets.append({
            "class_id": int(k),
            "label": labels[k] if k < len(labels) else f"#{int(k)}",
            "conf": round(float(c), 4),
            "box_640": [round(v, 2) for v in (x1, y1, x2, y2)],
            "box_src": [round(v / scale, 2) for v in (x1, y1, x2, y2)],
        })
    dets.sort(key=lambda d: -d["conf"])

    print(f"\n=== {len(dets)} detections (source {src_w}x{src_h}, scale {scale:.4f}) ===")
    print(f"{'label':<16}{'conf':>7}   box in 640x640 padded")
    for d in dets:
        x1, y1, x2, y2 = d["box_640"]
        print(f"{d['label']:<16}{d['conf']:>7.3f}   "
              f"({x1:7.1f},{y1:7.1f}) - ({x2:7.1f},{y2:7.1f})")

    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump({
                "input": {"shape": inp["shape"].tolist(),
                          "dtype": np.dtype(inp["dtype"]).name,
                          "scale": in_scale, "zero_point": in_zp},
                "output": {"shape": out["shape"].tolist(),
                           "dtype": np.dtype(out["dtype"]).name,
                           "scale": out_scale, "zero_point": out_zp},
                "normalized_boxes": bool(normalized),
                "conf_threshold": args.conf,
                "iou_threshold": args.iou,
                "detections": dets,
            }, f, indent=2)
        print(f"\nwrote {args.json_out}", file=sys.stderr)

    if args.save:
        vis = Image.fromarray(rgb)
        d = ImageDraw.Draw(vis)
        for det in dets:
            x1, y1, x2, y2 = det["box_640"]
            d.rectangle([x1, y1, x2, y2], outline=(255, 0, 0), width=2)
            d.text((x1 + 2, max(0, y1 - 11)), f"{det['label']} {det['conf']:.2f}",
                   fill=(255, 255, 0))
        vis.save(args.save)
        print(f"wrote {args.save}", file=sys.stderr)

    print("\n=== values needed by the device-side decoder (Phase 2-B) ===", file=sys.stderr)
    print(f"  input  quant: scale={in_scale!r} zero_point={in_zp!r}", file=sys.stderr)
    print(f"  output quant: scale={out_scale!r} zero_point={out_zp!r}", file=sys.stderr)
    print(f"  output shape: {out['shape'].tolist()}", file=sys.stderr)
    print(f"  box coords  : {'normalized (multiply by 640)' if normalized else 'pixels'}",
          file=sys.stderr)


if __name__ == "__main__":
    main()

# Bounding Box API

Fast, simple drawing of bounding boxes and polylines on video, optimized across all Axis chip
platforms. This is the go-to API for visualizing video-content-analytics results (from
[larod.md](larod.md)) — much simpler than [overlay.md](overlay.md) when you only need
rectangles, quads and lines.

**API specification:** https://developer.axis.com/acap/acap-native-sdk-version-12/api/src/api/bbox/html/index.html

## Build & manifest

```make
PKGS = bbox
```

```c
#include <bbox.h>
```

`manifest.json` — needs the graphics/overlay D-Bus methods and the `video` group:

```json
"resources": {
  "dbus": { "requiredMethods": ["com.axis.Graphics2.*", "com.axis.Overlay2.*"] },
  "linux": { "user": { "groups": ["video"] } }
}
```

## Coordinate systems

All coordinates are normalized floats in `[0,0]`–`[1,1]`.

- **Scene-normalized** (`bbox_coordinates_scene_normalized`): follows the filmed scene, so
  static objects keep the same coordinates regardless of global rotation.
- **Frame-normalized** (`bbox_coordinates_frame_normalized`): aligned with the camera frame
  (top-left `[0,0]`, bottom-right `[1,1]`).

## Workflow

```c
// Draw on a single view (channel 1)
bbox_t* bbox = bbox_view_new(1u);
// ...or on multiple channels, coordinate space = whole sensor:
//   bbox_t* bbox = bbox_new(2u, 1u, 2u);   // (num_channels, ch1, ch2)

bbox_coordinates_scene_normalized(bbox);   // or _frame_normalized
bbox_clear(bbox);                          // remove previously drawn boxes

// Colors are expensive to create — create them ONCE, up front.
const bbox_color_t red   = bbox_color_from_rgb(0xff, 0x00, 0x00);
const bbox_color_t green = bbox_color_from_rgb(0x00, 0xff, 0x00);

// Style/thickness/color switches are cheap.
bbox_style_outline(bbox);                  // or bbox_style_corners(bbox)
bbox_thickness_thin(bbox);                 // _thin / _medium / _thick
bbox_color(bbox, red);
bbox_rectangle(bbox, 0.05, 0.05, 0.95, 0.95);          // (left,top,right,bottom)

bbox_quad(bbox, 0.1,0.1, 0.3,0.12, 0.28,0.28, 0.11,0.30);   // arbitrary quadrilateral

bbox_color(bbox, green);                   // polyline
bbox_move_to(bbox, 0.2, 0.2);
bbox_line_to(bbox, 0.5, 0.5);
bbox_line_to(bbox, 0.8, 0.4);
bbox_draw_path(bbox);

// Commit ALL queued geometry atomically (0u = default/immediate)
bbox_commit(bbox, 0u);

bbox_destroy(bbox);
```

Multi-channel with video output:

```c
bbox_t* bbox = bbox_new(2u, 1u, 2u);
bbox_video_output(bbox, true);   // also render into the live video output, if the camera has one
```

## Function groups

| Group | Functions |
|---|---|
| Create/destroy | `bbox_view_new(view)`, `bbox_new(n, ch...)`, `bbox_destroy()` |
| Coordinates | `bbox_coordinates_scene_normalized()`, `bbox_coordinates_frame_normalized()` |
| Color | `bbox_color_from_rgb(r,g,b)` (slow), `bbox_color(bbox, color)` (fast) |
| Style | `bbox_style_outline()`, `bbox_style_corners()` |
| Thickness | `bbox_thickness_thin()`, `_medium()`, `_thick()` |
| Geometry | `bbox_rectangle()`, `bbox_quad()`, `bbox_move_to()`, `bbox_line_to()`, `bbox_draw_path()` |
| Frame control | `bbox_clear()`, `bbox_commit()`, `bbox_video_output()` |

## Notes & gotchas

- **`bbox_color_from_rgb()` is slow** — allocate every color once at startup, then switch with
  the cheap `bbox_color()`.
- Nothing is shown until `bbox_commit()`; all queued geometry appears simultaneously.
- Call `bbox_clear()` + `bbox_commit()` to erase previous boxes each frame.
- For per-channel analytics on multiple images, translate coordinates into the global sensor
  space yourself before drawing multi-channel.

## Related

- Produce detections to draw → [larod.md](larod.md)
- Pixel-level custom overlays → [overlay.md](overlay.md)

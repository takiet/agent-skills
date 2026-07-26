// Bounding box overlay, drawn with Skia on the GPU. The rendering path follows
// the official axoverlay2-skia example: an axoverlay buffer is exported as a
// dma-buf, imported as an EGLImage, bound to a GL texture and wrapped in a Skia
// Ganesh surface, so nothing is copied on the CPU.
#include "overlay.h"

#include <cstring>
#include <poll.h>
#include <syslog.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <axoverlay2.h>
#include <glib.h>
#include <vdo-error.h>
#include <vdo-map.h>
#include <vdo-stream.h>
#include <vdo-types.h>

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkRect.h>
#include <include/core/SkString.h>
#include <include/core/SkTypeface.h>
#include <include/core/SkSurface.h>
#include <include/gpu/ganesh/GrBackendSurface.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/GrTypes.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <include/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <include/gpu/ganesh/gl/GrGLDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLInterface.h>
#include <include/ports/SkFontMgr_empty.h>

// Bundled by acap-build; see the Dockerfile.
#define FONT_PATH "/usr/local/packages/yolov5_detector/DejaVuSans.ttf"

#define MAX_STREAMS 8
#define MAX_CACHED_SURFACES 8
#define MAX_DRAWN 32
#define MAX_LABEL 64

// Above this the overlay is drawn at half resolution and upscaled, as in the
// example: a full resolution overlay costs a lot of memory and GPU time.
#define UPSCALE_PIXEL_THRESHOLD 4000000

// One axoverlay buffer imported into the GPU. Doing this is expensive, so the
// small set of buffers an overlay cycles through is cached.
struct RenderSurface {
    unsigned long buffer_id;
    EGLImageKHR image;
    GLuint texture;
    sk_sp<SkSurface> surface;
};

// One overlay, on one viewer stream.
struct StreamOverlay {
    unsigned stream_id;
    int overlay_id;
    axo_detailed_format* format;
    unsigned used_width;  // area the drawing maps onto
    unsigned used_height;
    unsigned full_width;  // including alignment padding
    unsigned full_height;
    RenderSurface cache[MAX_CACHED_SURFACES];
    size_t num_cached;

};

struct Overlay {
    unsigned content_width;
    unsigned content_height;

    EGLDisplay display;
    EGLContext context;
    EGLSurface pbuffer;
    sk_sp<GrDirectContext> gr_context;
    sk_sp<SkTypeface> typeface;

    bool axo_started;
    VdoStream* event_stream;
    int event_fd;

    StreamOverlay streams[MAX_STREAMS];
    size_t num_streams;

    const Detection* detections;
    size_t num_detections;
    bool logged_text_diag;
};

static bool egl_failed(const char* what) {
    EGLint error = eglGetError();
    if (error == EGL_SUCCESS) {
        return false;
    }
    syslog(LOG_ERR, "overlay: failed to %s: EGL error 0x%04x", what, error);
    return true;
}

static bool gl_failed(const char* what) {
    GLenum error = glGetError();
    if (error == GL_NO_ERROR) {
        return false;
    }
    syslog(LOG_ERR, "overlay: failed to %s: GL error 0x%04x", what, error);
    return true;
}

// EGL context plus the Skia GPU context that renders through it. One per
// process is enough.
static bool create_render_context(Overlay* overlay) {
    overlay->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_failed("open EGL display")) {
        return false;
    }
    eglInitialize(overlay->display, NULL, NULL);
    if (egl_failed("initialize EGL display")) {
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    if (egl_failed("bind OpenGL ES API")) {
        return false;
    }

    static const EGLint config_attribs[] = {EGL_SURFACE_TYPE,
                                            EGL_PBUFFER_BIT,
                                            EGL_RENDERABLE_TYPE,
                                            EGL_OPENGL_ES3_BIT,
                                            EGL_NONE};
    EGLConfig config;
    EGLint num_configs;
    eglChooseConfig(overlay->display, config_attribs, &config, 1, &num_configs);
    if (egl_failed("choose EGL config")) {
        return false;
    }

    static const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    overlay->context = eglCreateContext(overlay->display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (egl_failed("create EGL context")) {
        return false;
    }
    static const EGLint pbuffer_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    overlay->pbuffer = eglCreatePbufferSurface(overlay->display, config, pbuffer_attribs);
    if (egl_failed("create EGL pbuffer surface")) {
        return false;
    }
    eglMakeCurrent(overlay->display, overlay->pbuffer, overlay->pbuffer, overlay->context);
    if (egl_failed("make EGL context current")) {
        return false;
    }

    auto interface = GrGLMakeNativeInterface();
    if (!interface) {
        syslog(LOG_ERR, "overlay: GrGLMakeNativeInterface failed");
        return false;
    }
    overlay->gr_context = GrDirectContexts::MakeGL(std::move(interface));
    if (!overlay->gr_context) {
        syslog(LOG_ERR, "overlay: GrDirectContexts::MakeGL failed");
        return false;
    }
    return true;
}

// Skia is built without fontconfig, so the font is loaded from the file we ship
// rather than looked up by name on the device.
static bool load_font(Overlay* overlay) {
    sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_Custom_Empty();
    overlay->typeface         = font_mgr->makeFromFile(FONT_PATH, 0);
    if (!overlay->typeface) {
        syslog(LOG_ERR, "overlay: cannot load font %s", FONT_PATH);
        return false;
    }
    return true;
}

// Stream 0 is a pseudo stream that reports events about all other streams.
static bool attach_stream_events(Overlay* overlay) {
    g_autoptr(GError) error = NULL;
    overlay->event_stream   = vdo_stream_get(0, &error);
    if (!overlay->event_stream) {
        syslog(LOG_ERR, "overlay: vdo_stream_get(0) failed: %s", error->message);
        return false;
    }
    g_autoptr(VdoMap) filter = vdo_map_new();
    vdo_map_set_string(filter, "filter", "overlay");
    if (!vdo_stream_attach(overlay->event_stream, filter, &error)) {
        syslog(LOG_ERR, "overlay: vdo_stream_attach failed: %s", error->message);
        return false;
    }
    overlay->event_fd = vdo_stream_get_event_fd(overlay->event_stream, &error);
    if (overlay->event_fd < 0) {
        syslog(LOG_ERR, "overlay: vdo_stream_get_event_fd failed: %s", error->message);
        return false;
    }
    return true;
}

static void add_stream(Overlay* overlay, unsigned stream_id, unsigned width, unsigned height) {
    axo_err* error = NULL;
    if (overlay->num_streams == MAX_STREAMS) {
        syslog(LOG_ERR, "overlay: more than %d streams", MAX_STREAMS);
        return;
    }

    // Compression is recommended whenever the GPU renders the overlay.
    axo_detailed_format* format = axo_suggest_detailed_format(
        AXO_FORMAT_ARGB32,
        (axo_format_flags)(AXO_FORMAT_FLAGS_COMPRESSED | AXO_FORMAT_FLAGS_GPU),
        &error);
    if (!format) {
        syslog(LOG_ERR, "overlay: no suitable format: %s", axo_err_get_message(error));
        axo_err_clear(&error);
        return;
    }

    // Cover the whole stream so a box can be drawn anywhere in the picture.
    bool upscale       = (uint64_t)width * height > UPSCALE_PIXEL_THRESHOLD;
    unsigned used_width  = upscale ? width / 2 : width;
    unsigned used_height = upscale ? height / 2 : height;

    unsigned full_width, full_height;
    axo_detailed_format_get_aligned_size(format, used_width, used_height, &full_width, &full_height);
    // The GPU driver can be stricter than axoverlay about alignment.
    full_width  = (full_width + 15) & ~15u;
    full_height = (full_height + 15) & ~15u;

    axo_props* props = axo_props_new();
    axo_props_set_detailed_format(props, format);
    axo_props_set_size(props, full_width, full_height);
    axo_props_set_upscale_x2(props, upscale);
    // The CPU never touches these buffers, so skip cache maintenance.
    axo_props_set_manual_dma_sync(props, true);

    axo_match* match = axo_match_new();
    axo_match_stream_id(match, (int)stream_id);

    int overlay_id = axo_create_overlay(props, match, &error);
    axo_props_free(props);
    axo_match_free(match);
    if (overlay_id < 0) {
        // The stream can close before we get to create the overlay on it; a
        // close event will follow, so this is not an error.
        if (axo_err_get_code(error) != AXO_ERR_NO_STREAM) {
            syslog(LOG_ERR,
                   "overlay: axo_create_overlay on stream %u failed: %s",
                   stream_id,
                   axo_err_get_message(error));
        }
        axo_err_clear(&error);
        axo_detailed_format_free(format);
        return;
    }

    StreamOverlay* stream = &overlay->streams[overlay->num_streams++];
    *stream               = {};
    stream->stream_id     = stream_id;
    stream->overlay_id    = overlay_id;
    stream->format        = format;
    stream->used_width    = used_width;
    stream->used_height   = used_height;
    stream->full_width    = full_width;
    stream->full_height   = full_height;

    syslog(LOG_INFO,
           "overlay: created overlay %d on stream %u, stream %ux%u, drawing area %ux%u, "
           "buffer %ux%u%s",
           overlay_id,
           stream_id,
           width,
           height,
           used_width,
           used_height,
           full_width,
           full_height,
           upscale ? " (upscaled x2)" : "");
}

static void free_stream(Overlay* overlay, StreamOverlay* stream) {
    static auto eglDestroyImageKHR =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");

    for (size_t i = 0; i < stream->num_cached; i++) {
        stream->cache[i].surface.reset();
        if (stream->cache[i].texture) {
            glDeleteTextures(1, &stream->cache[i].texture);
        }
        if (stream->cache[i].image) {
            eglDestroyImageKHR(overlay->display, stream->cache[i].image);
        }
    }
    stream->num_cached = 0;
    axo_detailed_format_free(stream->format);
    stream->format = NULL;
}

static void remove_stream(Overlay* overlay, unsigned stream_id) {
    for (size_t i = 0; i < overlay->num_streams; i++) {
        StreamOverlay* stream = &overlay->streams[i];
        if (stream->stream_id != stream_id) {
            continue;
        }
        axo_err* error = NULL;
        if (!axo_remove_overlay(stream->overlay_id, &error)) {
            syslog(LOG_ERR, "overlay: axo_remove_overlay failed: %s", axo_err_get_message(error));
            axo_err_clear(&error);
        }
        syslog(LOG_INFO, "overlay: removed overlay %d from stream %u", stream->overlay_id, stream_id);
        free_stream(overlay, stream);
        overlay->streams[i] = overlay->streams[--overlay->num_streams];
        return;
    }
}

// Non-blocking drain of the stream events, so this fits in a sequential loop
// without a GMainLoop.
static void pump_stream_events(Overlay* overlay) {
    for (;;) {
        struct pollfd pfd = {overlay->event_fd, POLLIN | POLLPRI, 0};
        if (poll(&pfd, 1, 0) <= 0) {
            return;
        }

        g_autoptr(GError) error  = NULL;
        g_autoptr(VdoMap) event  = vdo_stream_get_event(overlay->event_stream, &error);
        if (!event) {
            if (!g_error_matches(error, VDO_ERROR, VDO_ERROR_NO_EVENT)) {
                syslog(LOG_ERR, "overlay: vdo_stream_get_event failed: %s", error->message);
            }
            return;
        }

        unsigned type      = vdo_map_get_uint32(event, "event", 0);
        unsigned stream_id = vdo_map_get_uint32(event, "id", 0);
        if (type == VDO_STREAM_EVENT_EXISTING || type == VDO_STREAM_EVENT_CREATED) {
            g_autoptr(VdoStream) stream = vdo_stream_get(stream_id, &error);
            if (!stream) {
                syslog(LOG_ERR, "overlay: vdo_stream_get(%u) failed: %s", stream_id, error->message);
                continue;
            }
            g_autoptr(VdoMap) info = vdo_stream_get_info(stream, NULL);
            unsigned width         = info ? vdo_map_get_uint32(info, "width", 0) : 0;
            unsigned height        = info ? vdo_map_get_uint32(info, "height", 0) : 0;
            if (width && height) {
                add_stream(overlay, stream_id, width, height);
            }
        } else if (type == VDO_STREAM_EVENT_CLOSED) {
            remove_stream(overlay, stream_id);
        }
    }
}

// Import one axoverlay buffer into Skia, or return the cached import.
static RenderSurface* get_surface(Overlay* overlay, StreamOverlay* stream, axo_buffer* buffer) {
    unsigned long buffer_id = axo_buffer_get_id(buffer);
    for (size_t i = 0; i < stream->num_cached; i++) {
        if (stream->cache[i].buffer_id == buffer_id) {
            return &stream->cache[i];
        }
    }
    if (stream->num_cached == MAX_CACHED_SURFACES) {
        syslog(LOG_ERR, "overlay: more than %d buffers", MAX_CACHED_SURFACES);
        return NULL;
    }
    int dma_buf_fd = axo_buffer_get_dma_buf_fd(buffer);
    if (dma_buf_fd < 0) {
        syslog(LOG_ERR, "overlay: buffer has no dma-buf fd");
        return NULL;
    }

    uint32_t fourcc    = axo_detailed_format_get_drm_fourcc(stream->format);
    uint64_t modifier  = axo_detailed_format_get_drm_modifier(stream->format, 0);
    EGLint attribs[]   = {EGL_WIDTH,
                          (EGLint)stream->full_width,
                          EGL_HEIGHT,
                          (EGLint)stream->full_height,
                          EGL_LINUX_DRM_FOURCC_EXT,
                          (EGLint)fourcc,
                          EGL_DMA_BUF_PLANE0_FD_EXT,
                          dma_buf_fd,
                          EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                          0,
                          EGL_DMA_BUF_PLANE0_PITCH_EXT,
                          (EGLint)(stream->full_width * 4),
                          EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                          (EGLint)(modifier & 0xffffffff),
                          EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                          (EGLint)(modifier >> 32),
                          EGL_NONE};

    static auto eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    static auto glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    RenderSurface* cached = &stream->cache[stream->num_cached];
    *cached               = {};
    cached->buffer_id     = buffer_id;
    cached->image         = eglCreateImageKHR(overlay->display,
                                      EGL_NO_CONTEXT,
                                      EGL_LINUX_DMA_BUF_EXT,
                                      NULL,
                                      attribs);
    if (egl_failed("create EGL image") || cached->image == EGL_NO_IMAGE_KHR) {
        return NULL;
    }
    glGenTextures(1, &cached->texture);
    glBindTexture(GL_TEXTURE_2D, cached->texture);
    if (gl_failed("bind GL texture")) {
        return NULL;
    }
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, cached->image);
    if (gl_failed("set GL texture target")) {
        return NULL;
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    GrGLTextureInfo texture_info = {
        .fTarget = GL_TEXTURE_2D,
        .fID     = cached->texture,
        .fFormat = GL_RGBA8,
    };
    GrBackendTexture backend = GrBackendTextures::MakeGL((int)stream->full_width,
                                                         (int)stream->full_height,
                                                         skgpu::Mipmapped::kNo,
                                                         texture_info);
    if (!backend.isValid()) {
        syslog(LOG_ERR, "overlay: GrBackendTextures::MakeGL failed");
        return NULL;
    }
    cached->surface = SkSurfaces::WrapBackendTexture(overlay->gr_context.get(),
                                                     backend,
                                                     kTopLeft_GrSurfaceOrigin,
                                                     0,
                                                     kRGBA_8888_SkColorType,
                                                     NULL,
                                                     NULL);
    if (!cached->surface) {
        syslog(LOG_ERR, "overlay: SkSurfaces::WrapBackendTexture failed");
        return NULL;
    }
    stream->num_cached++;
    return cached;
}

static float clamp01(float value) {
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

// Draw text glyph by glyph as filled paths. This is not a stylistic choice:
// SkCanvas::drawString goes through Skia's GPU glyph atlas, which on this
// device produces no pixels at all -- measured at 0 covered pixels over 60
// frames, against 318 for the same string drawn as paths (PLAN.md concern #16).
// Rectangles are unaffected because they are drawn as geometry.
static void draw_text_as_paths(SkCanvas* canvas,
                               const SkFont& font,
                               const char* text,
                               float x,
                               float baseline,
                               const SkPaint& paint) {
    size_t length = strlen(text);
    int count     = font.countText(text, length, SkTextEncoding::kUTF8);
    if (count <= 0 || count > MAX_LABEL) {
        return;
    }
    SkGlyphID glyphs[MAX_LABEL];
    SkScalar xpos[MAX_LABEL];
    font.textToGlyphs(text, length, SkTextEncoding::kUTF8, glyphs, count);
    font.getXPos(glyphs, count, xpos, x);

    SkPath path;
    for (int i = 0; i < count; i++) {
        // Blank glyphs such as space have no outline.
        if (!font.getPath(glyphs[i], &path) || path.isEmpty()) {
            continue;
        }
        canvas->save();
        canvas->translate(xpos[i], baseline);
        canvas->drawPath(path, paint);
        canvas->restore();
    }
}

// The capture is scaled to the overlay's drawing area. Boxes that reach into
// the padding are clamped to the picture.
static void draw_detections(Overlay* overlay, const StreamOverlay* stream, SkCanvas* canvas) {
    canvas->clear(SK_ColorTRANSPARENT);

    float stroke    = MAX(2.0f, (float)stream->used_width / 320.0f);
    float text_size = MAX(12.0f, (float)stream->used_height / 20.0f);
    SkFont font(overlay->typeface, text_size);

    SkPaint box_paint;
    box_paint.setStyle(SkPaint::kStroke_Style);
    box_paint.setStrokeWidth(stroke);
    box_paint.setColor(SK_ColorGREEN);
    box_paint.setAntiAlias(true);

    SkPaint label_bg;
    label_bg.setStyle(SkPaint::kFill_Style);
    label_bg.setColor(SK_ColorGREEN);

    SkPaint text_paint;
    text_paint.setColor(SK_ColorBLACK);
    text_paint.setAntiAlias(true);

    for (size_t i = 0; i < overlay->num_detections; i++) {
        const Detection* detection = &overlay->detections[i];
        float x1 = clamp01(detection->x1 / (float)overlay->content_width) * (float)stream->used_width;
        float x2 = clamp01(detection->x2 / (float)overlay->content_width) * (float)stream->used_width;
        float y1 =
            clamp01(detection->y1 / (float)overlay->content_height) * (float)stream->used_height;
        float y2 =
            clamp01(detection->y2 / (float)overlay->content_height) * (float)stream->used_height;

        canvas->drawRect(SkRect::MakeLTRB(x1, y1, x2, y2), box_paint);

        char text[MAX_LABEL];
        g_snprintf(text, sizeof(text), "%s %.2f", detection->label, (double)detection->score);
        float text_width = font.measureText(text, strlen(text), SkTextEncoding::kUTF8);
        // Label above the box, or just inside it when there is no room above.
        float baseline = y1 > text_size * 1.2f ? y1 - text_size * 0.25f : y1 + text_size;
        canvas->drawRect(SkRect::MakeLTRB(x1,
                                          baseline - text_size * 0.8f,
                                          x1 + text_width + 4.0f,
                                          baseline + text_size * 0.25f),
                         label_bg);
        draw_text_as_paths(canvas, font, text, x1 + 2.0f, baseline, text_paint);


        if (!overlay->logged_text_diag) {
            overlay->logged_text_diag = true;
            SkString family;
            overlay->typeface->getFamilyName(&family);
            syslog(LOG_INFO,
                   "overlay: font '%s', %d glyphs, glyph('A')=%u, text \"%s\" -> %d glyphs, "
                   "measureText %.1f px at size %.1f",
                   family.c_str(),
                   overlay->typeface->countGlyphs(),
                   overlay->typeface->unicharToGlyph('A'),
                   text,
                   font.countText(text, strlen(text), SkTextEncoding::kUTF8),
                   (double)text_width,
                   (double)text_size);
        }
    }
}

static void draw_stream(Overlay* overlay, StreamOverlay* stream) {
    axo_err* error     = NULL;
    axo_buffer* buffer = axo_get_buffer(stream->overlay_id, NULL, &error);
    if (!buffer) {
        // No viewer any more, or no free buffer yet. Both are normal.
        axo_err_code code = axo_err_get_code(error);
        if (code != AXO_ERR_NO_STREAM && code != AXO_ERR_WAIT) {
            syslog(LOG_ERR, "overlay: axo_get_buffer failed: %s", axo_err_get_message(error));
        }
        axo_err_clear(&error);
        return;
    }

    RenderSurface* surface = get_surface(overlay, stream, buffer);
    if (!surface) {
        return;
    }
    SkCanvas* canvas = surface->surface->getCanvas();
    canvas->save();
    draw_detections(overlay, stream, canvas);
    canvas->restore();
    // Block until the drawing has actually landed in the buffer.
    overlay->gr_context->flushAndSubmit(GrSyncCpu::kYes);


    if (!axo_submit_buffer(buffer, NULL, &error)) {
        syslog(LOG_ERR, "overlay: axo_submit_buffer failed: %s", axo_err_get_message(error));
        axo_err_clear(&error);
    }
}

Overlay* overlay_start(unsigned content_width, unsigned content_height) {
    // new, not g_new0: the struct holds Skia smart pointers that need
    // constructing.
    Overlay* overlay        = new Overlay{};
    overlay->content_width  = content_width;
    overlay->content_height = content_height;
    overlay->event_fd       = -1;

    if (!create_render_context(overlay) || !load_font(overlay)) {
        overlay_stop(overlay);
        return NULL;
    }

    axo_err* error = NULL;
    if (!axo_start(NULL, &error)) {
        syslog(LOG_ERR, "overlay: axo_start failed: %s", axo_err_get_message(error));
        axo_err_clear(&error);
        overlay_stop(overlay);
        return NULL;
    }
    overlay->axo_started = true;

    if (!attach_stream_events(overlay)) {
        overlay_stop(overlay);
        return NULL;
    }
    syslog(LOG_INFO, "overlay: started, drawing %ux%u coordinates", content_width, content_height);
    return overlay;
}

bool overlay_draw(Overlay* overlay, const Detection* detections, size_t count) {
    overlay->detections     = detections;
    overlay->num_detections = MIN(count, (size_t)MAX_DRAWN);

    pump_stream_events(overlay);
    for (size_t i = 0; i < overlay->num_streams; i++) {
        draw_stream(overlay, &overlay->streams[i]);
    }

    overlay->detections     = NULL;
    overlay->num_detections = 0;
    return true;
}

size_t overlay_stream_count(const Overlay* overlay) {
    return overlay->num_streams;
}

// Count pixels that are much darker than the light label background, i.e.
// glyph coverage.
void overlay_stop(Overlay* overlay) {
    if (!overlay) {
        return;
    }
    while (overlay->num_streams > 0) {
        remove_stream(overlay, overlay->streams[0].stream_id);
    }
    if (overlay->event_stream) {
        g_object_unref(overlay->event_stream);
    }
    if (overlay->axo_started) {
        axo_stop(NULL);
    }
    overlay->typeface.reset();
    overlay->gr_context.reset();
    if (overlay->display) {
        eglMakeCurrent(overlay->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (overlay->pbuffer) {
            eglDestroySurface(overlay->display, overlay->pbuffer);
        }
        if (overlay->context) {
            eglDestroyContext(overlay->display, overlay->context);
        }
        eglTerminate(overlay->display);
    }
    delete overlay;
}

/*
 * mapper — renderer de video mapping para Raspberry Pi 3.
 *
 * DRM/KMS + GBM + EGL/GLES2, sin X ni compositor. libmpv decodifica el contenido
 * (H.264 por hardware) dentro de un FBO; cada superficie es una malla deformada que
 * muestrea ese FBO: esquinas con homografia (perspectiva correcta) + cuadricula fina
 * de puntos de control (Catmull-Rom o bilineal).
 *
 * Solo dibuja cuando hay cuadro nuevo de video o cambio de estado: con una imagen
 * fija el proceso queda en poll() sin consumir CPU.
 *
 * Estado: /var/lib/mapper/state.json al arrancar; despues por el socket datagrama
 * /run/mapper/ctl.sock (JSON completo, o {"cmd":"shot"|"restart"}).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <cjson/cJSON.h>

#define STATE_PATH "/var/lib/mapper/state.json"
#define MEDIA_DIR  "/var/lib/mapper/media"
#define RUN_DIR    "/run/mapper"
#define CTL_PATH   RUN_DIR "/ctl.sock"
#define INFO_PATH  RUN_DIR "/info.json"
#define SHOT_PATH  RUN_DIR "/shot.ppm"
#define MPV_SOCK   RUN_DIR "/mpv.sock"

#define MAXS 8   /* superficies */
#define MAXG 9   /* puntos de cuadricula por eje */

static void logf_(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}
#define LOG(...) logf_(__VA_ARGS__)
#define DIE(...) do { logf_(__VA_ARGS__); exit(1); } while (0)

typedef struct { float x, y; } V2;

typedef struct {
    int enabled, content;          /* content: 0 video, 1 patron, 2 color */
    float color[3], src[4];        /* src: u0 v0 u1 v1 del contenido */
    V2 c[4];                       /* esquinas TL TR BR BL, salida normalizada 0..1 */
    int cols, rows, bicubic;
    V2 p[MAXG * MAXG];             /* cuadricula en el cuadrado unidad (antes de la homografia) */
    float feather[4], opacity, bright, contrast, gamma;
    GLuint vbo, ibo; int nidx, segx, segy, dirty;
    float *mesh;                   /* x y s t por vertice (para el overlay) */
} Surface;

static struct {
    int n; Surface s[MAXS];
    int sel, selpt, edit_grid, overlay, pattern, blackout;
    char mode_req[32];
    char *media_sig;
} st;

/* ---------- DRM / GBM / EGL ---------- */
static int drm_fd = -1;
static drmModeModeInfo mode;
static uint32_t conn_id, crtc_id;
static struct gbm_device *gbm;
static struct gbm_surface *gsurf;
static EGLDisplay edpy; static EGLContext ectx; static EGLSurface esurf;
static struct gbm_bo *bo_cur, *bo_next;
static int flip_pending, crtc_set;
static int W, H;
static drmModeCrtc *saved_crtc;

static void fb_destroy(struct gbm_bo *bo, void *data) {
    uint32_t fb = (uint32_t)(uintptr_t)data;
    if (fb) drmModeRmFB(drm_fd, fb);
    (void)bo;
}
static uint32_t bo_fb(struct gbm_bo *bo) {
    uint32_t fb = (uint32_t)(uintptr_t)gbm_bo_get_user_data(bo);
    if (fb) return fb;
    if (drmModeAddFB(drm_fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), 24, 32,
                     gbm_bo_get_stride(bo), gbm_bo_get_handle(bo).u32, &fb))
        DIE("drmModeAddFB: %s", strerror(errno));
    gbm_bo_set_user_data(bo, (void *)(uintptr_t)fb, fb_destroy);
    return fb;
}

static int parse_mode(const char *s, int *w, int *h, int *r) {
    *r = 0;
    return sscanf(s, "%dx%d@%d", w, h, r) >= 2;
}

static void write_info(const char *extra);
static char modes_json[4096];

static int drm_open(void) {
    char path[32];
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof path, "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmModeRes *res = drmModeGetResources(fd);
        if (!res || res->count_connectors == 0) { if (res) drmModeFreeResources(res); close(fd); continue; }
        drmModeFreeResources(res);
        return fd;
    }
    return -1;
}

/* Devuelve 1 si encontro un conector conectado y eligio modo. */
static int drm_pick(void) {
    drmModeRes *res = drmModeGetResources(drm_fd);
    drmModeConnector *conn = NULL;
    /* primero lo que el kernel ya sondeo al arrancar (sin releer el EDID: sin proyector eso
     * tarda ~5 s por los reintentos de DDC); si no aparece nada, recien ahi se sondea */
    for (int probe = 0; probe < 2 && !conn; probe++)
        for (int i = 0; i < res->count_connectors && !conn; i++) {
            drmModeConnector *c = probe ? drmModeGetConnector(drm_fd, res->connectors[i])
                                        : drmModeGetConnectorCurrent(drm_fd, res->connectors[i]);
            if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 &&
                c->connector_type != DRM_MODE_CONNECTOR_WRITEBACK) conn = c;
            else drmModeFreeConnector(c);
        }
    if (!conn) { drmModeFreeResources(res); return 0; }
    conn_id = conn->connector_id;

    int rw = 0, rh = 0, rr = 0, want = strcmp(st.mode_req, "auto") && parse_mode(st.mode_req, &rw, &rh, &rr);
    int best = -1, pref = 0;
    for (int i = 0; i < conn->count_modes; i++) {
        drmModeModeInfo *m = &conn->modes[i];
        if (want && m->hdisplay == rw && m->vdisplay == rh && (!rr || (int)m->vrefresh == rr)) {
            if (best < 0 || (!rr && m->vrefresh == 60)) best = i;
        }
        if (m->type & DRM_MODE_TYPE_PREFERRED) pref = i;
    }
    if (best < 0) {
        if (want) LOG("modo %s no disponible, uso el preferido", st.mode_req);
        best = pref;
    }
    mode = conn->modes[best];
    W = mode.hdisplay; H = mode.vdisplay;

    /* lista de modos para la web, sin repetidos */
    char *p = modes_json; size_t left = sizeof modes_json;
    int k = snprintf(p, left, "["); p += k; left -= k;
    for (int i = 0; i < conn->count_modes && left > 40; i++) {
        drmModeModeInfo *m = &conn->modes[i]; int dup = 0;
        for (int j = 0; j < i; j++)
            if (conn->modes[j].hdisplay == m->hdisplay && conn->modes[j].vdisplay == m->vdisplay &&
                conn->modes[j].vrefresh == m->vrefresh) dup = 1;
        if (dup) continue;
        k = snprintf(p, left, "%s\"%dx%d@%d\"", p[-1] == '[' ? "" : ",", m->hdisplay, m->vdisplay, m->vrefresh);
        p += k; left -= k;
    }
    snprintf(p, left, "]");

    /* CRTC: el del encoder actual o el primero compatible */
    crtc_id = 0;
    if (conn->encoder_id) {
        drmModeEncoder *e = drmModeGetEncoder(drm_fd, conn->encoder_id);
        if (e) { crtc_id = e->crtc_id; drmModeFreeEncoder(e); }
    }
    for (int i = 0; !crtc_id && i < conn->count_encoders; i++) {
        drmModeEncoder *e = drmModeGetEncoder(drm_fd, conn->encoders[i]);
        if (!e) continue;
        for (int j = 0; j < res->count_crtcs; j++)
            if (e->possible_crtcs & (1u << j)) { crtc_id = res->crtcs[j]; break; }
        drmModeFreeEncoder(e);
    }
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    if (!crtc_id) DIE("sin CRTC para el conector");
    saved_crtc = drmModeGetCrtc(drm_fd, crtc_id);
    LOG("salida %dx%d@%d (pedido %s)", W, H, mode.vrefresh, st.mode_req);
    return 1;
}

/* El EDID anuncia audio si tiene una extension CEA-861 con el bit "basic audio" (byte 3, bit 6). */
static int hdmi_audio;
static void edid_audio(void) {
    hdmi_audio = 0;
    drmModeObjectProperties *pr = drmModeObjectGetProperties(drm_fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
    for (uint32_t i = 0; pr && i < pr->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(drm_fd, pr->props[i]);
        if (p && !strcmp(p->name, "EDID") && pr->prop_values[i]) {
            drmModePropertyBlobRes *b = drmModeGetPropertyBlob(drm_fd, (uint32_t)pr->prop_values[i]);
            const uint8_t *e = b ? b->data : NULL;
            for (uint32_t off = 128; e && off + 4 <= b->length; off += 128)
                if (e[off] == 0x02 && (e[off + 3] & 0x40)) hdmi_audio = 1;
            drmModeFreePropertyBlob(b);
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(pr);
    LOG("EDID: %s", hdmi_audio ? "el proyector acepta audio por HDMI" : "sin audio por HDMI (o sin EDID)");
}

static int conn_connected(void) {
    drmModeConnector *c = drmModeGetConnectorCurrent(drm_fd, conn_id);
    int ok = c && c->connection == DRM_MODE_CONNECTED;
    drmModeFreeConnector(c);
    return ok;
}

static void egl_init(void) {
    gbm = gbm_create_device(drm_fd);
    if (!gbm) DIE("gbm_create_device");
    gsurf = gbm_surface_create(gbm, W, H, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!gsurf) DIE("gbm_surface_create");
    edpy = eglGetDisplay((EGLNativeDisplayType)gbm);
    if (!eglInitialize(edpy, NULL, NULL)) DIE("eglInitialize");
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint attr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                      EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig cfgs[64], cfg = NULL; EGLint n = 0;
    eglChooseConfig(edpy, attr, cfgs, 64, &n);
    for (int i = 0; i < n; i++) {
        EGLint id; eglGetConfigAttrib(edpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &id);
        if (id == GBM_FORMAT_XRGB8888) { cfg = cfgs[i]; break; }
    }
    if (!cfg) DIE("sin EGLConfig XRGB8888");
    EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    ectx = eglCreateContext(edpy, cfg, EGL_NO_CONTEXT, cattr);
    esurf = eglCreateWindowSurface(edpy, cfg, (EGLNativeWindowType)gsurf, NULL);
    if (ectx == EGL_NO_CONTEXT || esurf == EGL_NO_SURFACE) DIE("contexto EGL");
    eglMakeCurrent(edpy, esurf, esurf, ectx);
    eglSwapInterval(edpy, 1);
    LOG("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
}

/* ---------- mpv ---------- */
static mpv_handle *mpv;
static mpv_render_context *mpv_gl;
static int wake_pipe[2];
static GLuint vfbo, vtex; static int vw, vh;   /* FBO del contenido */
static int video_dirty;

static void *get_proc(void *ctx, const char *name) { (void)ctx; return (void *)eglGetProcAddress(name); }
static void on_mpv_wakeup(void *ctx) { (void)ctx; char c = 'w'; if (write(wake_pipe[1], &c, 1)) {} }
static void on_mpv_render(void *ctx) { (void)ctx; char c = 'r'; if (write(wake_pipe[1], &c, 1)) {} }

static void mpv_init(void) {
    mpv = mpv_create();
    if (!mpv) DIE("mpv_create");
    const char *opts[][2] = {
        /* v4l2m2m (drm_prime): mpv no puede importar el dmabuf en VC4 (le falta GL_EXT_texture_rg)
         * y lo baja a RAM en su hilo "core"; asi la bajada y la subida a GL quedan en hilos
         * distintos y 720p30 entra entero. v4l2m2m-copy hace todo en un hilo y cae a 20 fps. */
        {"config", "no"}, {"load-scripts", "no"}, {"ytdl", "no"}, {"load-stats-overlay", "no"},
        {"load-console", "no"}, {"load-select", "no"}, {"load-auto-profiles", "no"},
        {"load-positioning", "no"}, {"load-commands", "no"}, {"sub-font-provider", "none"},
        {"vo", "libmpv"}, {"hwdec", "v4l2m2m"}, {"audio-device", "alsa/hdmi:CARD=vc4hdmi,DEV=0"}, {"ao", "alsa"}, {"audio-fallback-to-null", "yes"},
        {"loop-playlist", "inf"}, {"image-display-duration", "10"}, {"keep-open", "no"},
        {"osd-level", "0"}, {"osc", "no"}, {"input-default-bindings", "no"}, {"input-vo-keyboard", "no"},
        {"terminal", "no"}, {"idle", "yes"}, {"input-ipc-server", MPV_SOCK},
        {"scale", "bilinear"}, {"dscale", "bilinear"}, {"cscale", "bilinear"},
        {"dither-depth", "no"}, {"correct-downscaling", "no"}, {"linear-downscaling", "no"},
        {"sigmoid-upscaling", "no"}, {"deband", "no"}, {"hdr-compute-peak", "no"},
        {"vd-lavc-threads", "4"}, {"demuxer-max-bytes", "24MiB"}, {"demuxer-max-back-bytes", "4MiB"},
        {"sub-auto", "no"}, {"audio-file-auto", "no"}, {"cover-art-auto", "no"},
    };
    for (size_t i = 0; i < sizeof opts / sizeof *opts; i++)
        if (mpv_set_option_string(mpv, opts[i][0], opts[i][1]) < 0) LOG("mpv: opcion %s rechazada", opts[i][0]);
    const char *hw = getenv("MAPPER_HWDEC");
    if (hw) mpv_set_option_string(mpv, "hwdec", hw);
    const char *lv = getenv("MAPPER_MPVLOG");
    mpv_request_log_messages(mpv, lv ? lv : "warn");
    if (mpv_initialize(mpv) < 0) DIE("mpv_initialize");
    mpv_observe_property(mpv, 1, "dwidth", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, 2, "dheight", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, 3, "hwdec-current", MPV_FORMAT_STRING);
    mpv_observe_property(mpv, 4, "current-ao", MPV_FORMAT_STRING);

    mpv_opengl_init_params gl_init = { .get_proc_address = get_proc };
    mpv_opengl_drm_params_v2 drm = { .fd = drm_fd, .crtc_id = (int)crtc_id, .connector_id = (int)conn_id, .render_fd = -1 };
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
        {MPV_RENDER_PARAM_DRM_DISPLAY_V2, &drm},
        {0}
    };
    if (mpv_render_context_create(&mpv_gl, mpv, params) < 0) DIE("mpv_render_context_create");
    if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK)) DIE("pipe");
    mpv_set_wakeup_callback(mpv, on_mpv_wakeup, NULL);
    mpv_render_context_set_update_callback(mpv_gl, on_mpv_render, NULL);
}

/* El FBO del contenido toma el tamaño del video, achicado para entrar en la salida. */
static void video_fbo(int w, int h) {
    if (w <= 0 || h <= 0) return;
    float k = fminf(1.0f, fminf((float)W / w, (float)H / h));
    int nw = (int)(w * k + 0.5f) & ~1, nh = (int)(h * k + 0.5f) & ~1;
    if (nw == vw && nh == vh) return;
    vw = nw; vh = nh;
    if (!vtex) { glGenTextures(1, &vtex); glGenFramebuffers(1, &vfbo); }
    glBindTexture(GL_TEXTURE_2D, vtex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vw, vh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, vfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vtex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) LOG("FBO incompleto");
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    LOG("contenido %dx%d -> FBO %dx%d", w, h, vw, vh);
    video_dirty = 1;
}

static char hwdec_cur[32] = "?";

/* Audio: HDMI solo anda si el EDID del proyector anuncia audio (sin EDID ALSA devuelve 524 y mpv
 * queda mudo todo el loop). En "auto" se decide con el EDID antes de abrir el archivo: cambiar de
 * salida mientras mpv arranca el archivo (ao-reload) lo deja trabado en 0:00. */
#define AO_HDMI "alsa/hdmi:CARD=vc4hdmi,DEV=0"
#define AO_JACK "alsa/plughw:CARD=Headphones,DEV=0"
static int audio_mode = -1, audio_jack;   /* modo: 0 auto, 1 hdmi, 2 jack */
static char ao_cur[16] = "?";
static void audio_set(int jack, int reload) {
    audio_jack = jack;
    mpv_set_property_string(mpv, "audio-device", jack ? AO_JACK : AO_HDMI);
    if (reload) { const char *cmd[] = {"ao-reload", NULL}; mpv_command(mpv, cmd); }
}
static int64_t dw, dh;

static void mpv_events(void) {
    for (;;) {
        mpv_event *ev = mpv_wait_event(mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message *m = ev->data;
            fprintf(stderr, "mpv[%s] %s", m->prefix, m->text);
        } else if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property *p = ev->data;
            if (p->format == MPV_FORMAT_INT64 && ev->reply_userdata == 1) dw = *(int64_t *)p->data;
            if (p->format == MPV_FORMAT_INT64 && ev->reply_userdata == 2) dh = *(int64_t *)p->data;
            if (ev->reply_userdata == 3)
                snprintf(hwdec_cur, sizeof hwdec_cur, "%s", p->format == MPV_FORMAT_STRING ? *(char **)p->data : "no");
            if (ev->reply_userdata == 4)
                snprintf(ao_cur, sizeof ao_cur, "%s", p->format == MPV_FORMAT_STRING ? *(char **)p->data : "-");
            if (ev->reply_userdata <= 2 && dw > 0 && dh > 0) video_fbo((int)dw, (int)dh);
            write_info(NULL);
        }
    }
}

/* ---------- geometria ---------- */
typedef struct { float a, b, c, d, e, f, g, h; } Homo;

/* Cuadrado unidad -> cuadrilatero (Heckbert). */
static Homo homo(const V2 *q) {
    Homo m;
    float dx1 = q[1].x - q[2].x, dx2 = q[3].x - q[2].x, dx3 = q[0].x - q[1].x + q[2].x - q[3].x;
    float dy1 = q[1].y - q[2].y, dy2 = q[3].y - q[2].y, dy3 = q[0].y - q[1].y + q[2].y - q[3].y;
    float det = dx1 * dy2 - dx2 * dy1;
    if (fabsf(det) < 1e-9f || (fabsf(dx3) < 1e-9f && fabsf(dy3) < 1e-9f)) { m.g = m.h = 0; }
    else { m.g = (dx3 * dy2 - dx2 * dy3) / det; m.h = (dx1 * dy3 - dx3 * dy1) / det; }
    m.a = q[1].x - q[0].x + m.g * q[1].x; m.b = q[3].x - q[0].x + m.h * q[3].x; m.c = q[0].x;
    m.d = q[1].y - q[0].y + m.g * q[1].y; m.e = q[3].y - q[0].y + m.h * q[3].y; m.f = q[0].y;
    return m;
}
static V2 hmap(const Homo *m, V2 p) {
    float w = m->g * p.x + m->h * p.y + 1.0f;
    if (fabsf(w) < 1e-6f) w = 1e-6f;
    return (V2){ (m->a * p.x + m->b * p.y + m->c) / w, (m->d * p.x + m->e * p.y + m->f) / w };
}

/* Punto de la cuadricula con extrapolacion lineal fuera del borde (para Catmull-Rom). */
static V2 gp(const Surface *s, int c, int r) {
    if (c < 0) { V2 a = gp(s, 0, r), b = gp(s, 1, r); return (V2){2 * a.x - b.x, 2 * a.y - b.y}; }
    if (c >= s->cols) { V2 a = gp(s, s->cols - 1, r), b = gp(s, s->cols - 2, r); return (V2){2 * a.x - b.x, 2 * a.y - b.y}; }
    if (r < 0) { V2 a = gp(s, c, 0), b = gp(s, c, 1); return (V2){2 * a.x - b.x, 2 * a.y - b.y}; }
    if (r >= s->rows) { V2 a = gp(s, c, s->rows - 1), b = gp(s, c, s->rows - 2); return (V2){2 * a.x - b.x, 2 * a.y - b.y}; }
    return s->p[r * s->cols + c];
}
static V2 cr(V2 p0, V2 p1, V2 p2, V2 p3, float t) {
    float t2 = t * t, t3 = t2 * t;
    V2 o;
    o.x = 0.5f * (2 * p1.x + (-p0.x + p2.x) * t + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 + (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3);
    o.y = 0.5f * (2 * p1.y + (-p0.y + p2.y) * t + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 + (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3);
    return o;
}
static V2 grid_eval(const Surface *s, float u, float v) {
    float gx = u * (s->cols - 1), gy = v * (s->rows - 1);
    int c = (int)gx, r = (int)gy;
    if (c > s->cols - 2) c = s->cols - 2;
    if (r > s->rows - 2) r = s->rows - 2;
    float fx = gx - c, fy = gy - r;
    if (!s->bicubic) {
        V2 a = gp(s, c, r), b = gp(s, c + 1, r), d = gp(s, c, r + 1), e = gp(s, c + 1, r + 1);
        return (V2){ (a.x * (1 - fx) + b.x * fx) * (1 - fy) + (d.x * (1 - fx) + e.x * fx) * fy,
                     (a.y * (1 - fx) + b.y * fx) * (1 - fy) + (d.y * (1 - fx) + e.y * fx) * fy };
    }
    V2 row[4];
    for (int k = 0; k < 4; k++)
        row[k] = cr(gp(s, c - 1, r - 1 + k), gp(s, c, r - 1 + k), gp(s, c + 1, r - 1 + k), gp(s, c + 2, r - 1 + k), fx);
    return cr(row[0], row[1], row[2], row[3], fy);
}

static void build_mesh(Surface *s) {
    int k = 48 / (s->cols - 1); if (k < 4) k = 4;
    int l = 48 / (s->rows - 1); if (l < 4) l = 4;
    s->segx = (s->cols - 1) * k; s->segy = (s->rows - 1) * l;
    int nv = (s->segx + 1) * (s->segy + 1);
    free(s->mesh);
    s->mesh = malloc(sizeof(float) * 4 * nv);
    Homo m = homo(s->c);
    for (int j = 0; j <= s->segy; j++)
        for (int i = 0; i <= s->segx; i++) {
            float u = (float)i / s->segx, v = (float)j / s->segy;
            V2 q = hmap(&m, grid_eval(s, u, v));
            float *o = s->mesh + 4 * (j * (s->segx + 1) + i);
            o[0] = q.x; o[1] = q.y; o[2] = u; o[3] = v;
        }
    int ni = s->segx * s->segy * 6;
    GLushort *idx = malloc(sizeof(GLushort) * ni), *p = idx;
    for (int j = 0; j < s->segy; j++)
        for (int i = 0; i < s->segx; i++) {
            GLushort a = j * (s->segx + 1) + i, b = a + 1, c = a + s->segx + 1, d = c + 1;
            *p++ = a; *p++ = b; *p++ = c; *p++ = b; *p++ = d; *p++ = c;
        }
    if (!s->vbo) { glGenBuffers(1, &s->vbo); glGenBuffers(1, &s->ibo); }
    glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * nv, s->mesh, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLushort) * ni, idx, GL_STATIC_DRAW);
    free(idx);
    s->nidx = ni; s->dirty = 0;
}

/* ---------- shaders ---------- */
static const char *VS =
    "attribute vec2 a_pos; attribute vec2 a_st; varying vec2 v_st; uniform float u_ps;\n"
    "void main(){ v_st=a_st; gl_PointSize=u_ps; gl_Position=vec4(a_pos.x*2.0-1.0, 1.0-a_pos.y*2.0, 0.0, 1.0); }\n";

static const char *FS =
    "precision mediump float; varying vec2 v_st;\n"
    "uniform sampler2D u_tex; uniform vec4 u_src; uniform vec3 u_color; uniform vec4 u_feather;\n"
    "uniform vec4 u_adj; uniform vec2 u_grid;\n"
    "float edge(float x, float w){ return w > 0.0 ? smoothstep(0.0, w, x) : 1.0; }\n"
    "void main(){\n"
    "#if MODE == 0\n"
    "  vec3 c = texture2D(u_tex, mix(u_src.xy, u_src.zw, v_st)).rgb;\n"
    "#elif MODE == 1\n"
    "  vec2 g = v_st * u_grid; vec2 f = fract(g); vec2 fine = fract(g * 4.0);\n"
    "  float chk = mod(floor(g.x * 4.0) + floor(g.y * 4.0), 2.0);\n"
    "  vec3 c = mix(vec3(0.10), vec3(0.22), chk) * u_color * 2.0;\n"
    "  vec2 dm = min(fine, 1.0 - fine) / (u_grid * 4.0);\n"
    "  if (min(dm.x, dm.y) < 0.0015) c = u_color * 0.6;\n"
    "  vec2 d = min(f, 1.0 - f) / u_grid;\n"
    "  if (min(d.x, d.y) < 0.004) c = vec3(1.0);\n"
    "  vec2 b = min(v_st, 1.0 - v_st);\n"
    "  if (min(b.x, b.y) < 0.012) c = u_color;\n"
    "  if (abs(length(v_st - 0.5) - 0.3) < 0.004) c = vec3(1.0);\n"
    "#else\n"
    "  vec3 c = u_color;\n"
    "#endif\n"
    "  c = ((c - 0.5) * u_adj.z + 0.5) * u_adj.y;\n"
    "  c = pow(clamp(c, 0.0, 1.0), vec3(u_adj.w));\n"
    "  float a = u_adj.x * edge(v_st.x, u_feather.x) * edge(v_st.y, u_feather.y)\n"
    "          * edge(1.0 - v_st.x, u_feather.z) * edge(1.0 - v_st.y, u_feather.w);\n"
    "  gl_FragColor = vec4(c * a, a);\n"
    "}\n";

static const char *FS_FLAT =
    "precision mediump float; uniform vec4 u_col; void main(){ gl_FragColor = u_col; }\n";

typedef struct { GLuint p; GLint pos, st, tex, src, color, feather, adj, grid, ps, col; } Prog;
static Prog prog[3], flat;

static GLuint compile(GLenum type, const char *pre, const char *src) {
    GLuint sh = glCreateShader(type);
    const char *srcs[2] = { pre, src };
    glShaderSource(sh, 2, srcs, NULL);
    glCompileShader(sh);
    GLint ok; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(sh, sizeof log, NULL, log); DIE("shader: %s", log); }
    return sh;
}
static Prog link_prog(const char *pre, const char *fs) {
    Prog p; p.p = glCreateProgram();
    glAttachShader(p.p, compile(GL_VERTEX_SHADER, "", VS));
    glAttachShader(p.p, compile(GL_FRAGMENT_SHADER, pre, fs));
    glBindAttribLocation(p.p, 0, "a_pos"); glBindAttribLocation(p.p, 1, "a_st");
    glLinkProgram(p.p);
    GLint ok; glGetProgramiv(p.p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p.p, sizeof log, NULL, log); DIE("link: %s", log); }
    p.pos = 0; p.st = 1;
    p.tex = glGetUniformLocation(p.p, "u_tex"); p.src = glGetUniformLocation(p.p, "u_src");
    p.color = glGetUniformLocation(p.p, "u_color"); p.feather = glGetUniformLocation(p.p, "u_feather");
    p.adj = glGetUniformLocation(p.p, "u_adj"); p.grid = glGetUniformLocation(p.p, "u_grid");
    p.ps = glGetUniformLocation(p.p, "u_ps"); p.col = glGetUniformLocation(p.p, "u_col");
    return p;
}
static void gl_init(void) {
    prog[0] = link_prog("#define MODE 0\n", FS);
    prog[1] = link_prog("#define MODE 1\n", FS);
    prog[2] = link_prog("#define MODE 2\n", FS);
    flat = link_prog("", FS_FLAT);
}

/* ---------- estado ---------- */
static const float PCOL[MAXS][3] = {
    {0.2f, 0.6f, 1.0f}, {1.0f, 0.45f, 0.1f}, {0.3f, 0.9f, 0.3f}, {0.9f, 0.2f, 0.6f},
    {0.9f, 0.85f, 0.2f}, {0.2f, 0.9f, 0.9f}, {0.7f, 0.4f, 1.0f}, {1.0f, 0.3f, 0.3f},
};

static double jnum(const cJSON *o, const char *k, double d) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsBool(v) ? cJSON_IsTrue(v) : d;
}
static const char *jstr(const cJSON *o, const char *k, const char *d) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : d;
}
static void jarr(const cJSON *o, const char *k, float *out, int n) {
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, k);
    for (int i = 0; i < n && cJSON_IsArray(a) && i < cJSON_GetArraySize(a); i++) {
        const cJSON *v = cJSON_GetArrayItem(a, i);
        if (cJSON_IsNumber(v)) out[i] = (float)v->valuedouble;
    }
}

static void surface_default(Surface *s, int i) {
    GLuint vbo = s->vbo, ibo = s->ibo; float *mesh = s->mesh;
    memset(s, 0, sizeof *s);
    s->vbo = vbo; s->ibo = ibo; s->mesh = mesh;
    s->enabled = 1; s->content = 0;
    memcpy(s->color, PCOL[i % MAXS], sizeof s->color);
    s->src[0] = 0; s->src[1] = 0; s->src[2] = 1; s->src[3] = 1;
    s->c[0] = (V2){0, 0}; s->c[1] = (V2){1, 0}; s->c[2] = (V2){1, 1}; s->c[3] = (V2){0, 1};
    s->cols = s->rows = 2; s->bicubic = 1;
    for (int r = 0; r < 2; r++) for (int c = 0; c < 2; c++) s->p[r * 2 + c] = (V2){(float)c, (float)r};
    s->opacity = s->bright = s->contrast = s->gamma = 1;
    s->dirty = 1;
}

static void media_apply(const cJSON *media) {
    char *sig = media ? cJSON_PrintUnformatted(media) : strdup("null");
    if (st.media_sig && !strcmp(sig, st.media_sig)) { free(sig); return; }
    /* volumen y mute no recargan la lista */
    double vol = media ? jnum(media, "volume", 80) : 80;
    int mute = media ? (int)jnum(media, "mute", 0) : 0;
    char b[32]; snprintf(b, sizeof b, "%g", vol);
    mpv_set_property_string(mpv, "volume", b);
    mpv_set_property_string(mpv, "mute", mute ? "yes" : "no");

    const char *am = media ? jstr(media, "audio", "auto") : "auto";
    int mode = !strcmp(am, "hdmi") ? 1 : !strcmp(am, "jack") ? 2 : 0;
    if (mode != audio_mode) {
        int first = audio_mode < 0;
        audio_mode = mode;
        audio_set(mode == 2 || (mode == 0 && !hdmi_audio), !first);
    }

    cJSON *m2 = media ? cJSON_Duplicate(media, 1) : NULL;
    if (m2) {
        cJSON_DeleteItemFromObject(m2, "volume"); cJSON_DeleteItemFromObject(m2, "mute");
        cJSON_DeleteItemFromObject(m2, "audio");
    }
    char *core = m2 ? cJSON_PrintUnformatted(m2) : strdup("null");
    static char *last_core;
    if (!last_core || strcmp(core, last_core)) {
        free(last_core); last_core = strdup(core);
        const cJSON *pl = media ? cJSON_GetObjectItemCaseSensitive(media, "playlist") : NULL;
        int n = cJSON_IsArray(pl) ? cJSON_GetArraySize(pl) : 0;
        double img = media ? jnum(media, "image_secs", 10) : 10;
        snprintf(b, sizeof b, "%g", img);
        mpv_set_property_string(mpv, "image-display-duration", n == 1 ? "inf" : b);
        /* un solo archivo: loop-file da un loop sin cortes (no reabre el archivo) */
        mpv_set_property_string(mpv, "loop-file", n == 1 ? "inf" : "no");
        mpv_set_property_string(mpv, "loop-playlist", (!media || jnum(media, "loop", 1)) ? "inf" : "no");
        if (n == 0) {
            const char *cmd[] = {"stop", NULL}; mpv_command(mpv, cmd);
        }
        for (int i = 0; i < n; i++) {
            const cJSON *it = cJSON_GetArrayItem(pl, i);
            if (!cJSON_IsString(it) || strchr(it->valuestring, '/')) continue;
            char path[512]; snprintf(path, sizeof path, MEDIA_DIR "/%s", it->valuestring);
            const char *cmd[] = {"loadfile", path, i == 0 ? "replace" : "append", NULL};
            mpv_command(mpv, cmd);
        }
        LOG("lista: %d archivo(s)", n);
    }
    cJSON_Delete(m2); free(core);
    free(st.media_sig); st.media_sig = sig;
}

static int state_apply(const cJSON *j, int startup) {
    const cJSON *out = cJSON_GetObjectItemCaseSensitive(j, "output");
    const char *mreq = out ? jstr(out, "mode", "1280x720") : "1280x720";
    if (startup) snprintf(st.mode_req, sizeof st.mode_req, "%s", mreq);
    else if (strcmp(mreq, st.mode_req)) { LOG("cambio de modo %s -> %s: reinicio", st.mode_req, mreq); exit(0); }

    const cJSON *show = cJSON_GetObjectItemCaseSensitive(j, "show");
    st.sel = (int)jnum(show, "selected", 0);
    st.selpt = (int)jnum(show, "point", -1);
    st.edit_grid = !strcmp(jstr(show, "edit", "corners"), "grid");
    st.overlay = (int)jnum(show, "overlay", 0);
    st.pattern = (int)jnum(show, "pattern", 0);
    int bo = (int)jnum(show, "blackout", 0);
    if (bo != st.blackout) {        /* apagado: negro, sin sonido y sin decodificar */
        int f = bo;
        mpv_set_property(mpv, "pause", MPV_FORMAT_FLAG, &f);
        LOG("imagen %s", bo ? "apagada (video en pausa)" : "encendida");
    }
    st.blackout = bo;

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, "surfaces");
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    if (n > MAXS) n = MAXS;
    for (int i = 0; i < n; i++) {
        const cJSON *o = cJSON_GetArrayItem(arr, i);
        Surface *s = &st.s[i];
        surface_default(s, i);
        s->enabled = (int)jnum(o, "enabled", 1);
        const char *ct = jstr(o, "content", "video");
        s->content = !strcmp(ct, "pattern") ? 1 : !strcmp(ct, "color") ? 2 : 0;
        jarr(o, "color", s->color, 3);
        jarr(o, "src", s->src, 4);
        const cJSON *cs = cJSON_GetObjectItemCaseSensitive(o, "corners");
        for (int k = 0; k < 4 && cJSON_IsArray(cs) && k < cJSON_GetArraySize(cs); k++) {
            float xy[2] = { s->c[k].x, s->c[k].y };
            const cJSON *pt = cJSON_GetArrayItem(cs, k);
            for (int q = 0; q < 2 && q < cJSON_GetArraySize(pt); q++) xy[q] = (float)cJSON_GetArrayItem(pt, q)->valuedouble;
            s->c[k] = (V2){xy[0], xy[1]};
        }
        const cJSON *g = cJSON_GetObjectItemCaseSensitive(o, "grid");
        int cols = (int)jnum(g, "cols", 2), rows = (int)jnum(g, "rows", 2);
        if (cols < 2) cols = 2;
        if (cols > MAXG) cols = MAXG;
        if (rows < 2) rows = 2;
        if (rows > MAXG) rows = MAXG;
        s->cols = cols; s->rows = rows;
        s->bicubic = strcmp(jstr(g, "interp", "bicubic"), "bilinear") != 0;
        const cJSON *pts = cJSON_GetObjectItemCaseSensitive(g, "pts");
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++) {
                int k = r * cols + c;
                V2 d = { (float)c / (cols - 1), (float)r / (rows - 1) };
                const cJSON *pt = cJSON_IsArray(pts) && cJSON_GetArraySize(pts) == cols * rows ? cJSON_GetArrayItem(pts, k) : NULL;
                if (cJSON_IsArray(pt) && cJSON_GetArraySize(pt) >= 2)
                    d = (V2){ (float)cJSON_GetArrayItem(pt, 0)->valuedouble, (float)cJSON_GetArrayItem(pt, 1)->valuedouble };
                s->p[k] = d;
            }
        jarr(o, "feather", s->feather, 4);
        s->opacity = (float)jnum(o, "opacity", 1);
        s->bright = (float)jnum(o, "brightness", 1);
        s->contrast = (float)jnum(o, "contrast", 1);
        s->gamma = (float)jnum(o, "gamma", 1);
        if (s->gamma < 0.1f) s->gamma = 0.1f;
        s->dirty = 1;
    }
    st.n = n;
    media_apply(cJSON_GetObjectItemCaseSensitive(j, "media"));
    return 1;
}

static cJSON *state_read(void) {
    FILE *f = fopen(STATE_PATH, "r");
    cJSON *j = NULL;
    if (f) {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        char *b = malloc(n + 1);
        if (fread(b, 1, n, f) == (size_t)n) { b[n] = 0; j = cJSON_Parse(b); }
        free(b); fclose(f);
    }
    if (!j) {
        LOG("sin estado valido: una superficie a pantalla completa con patron");
        j = cJSON_Parse("{\"show\":{\"pattern\":true,\"overlay\":true},\"surfaces\":[{}]}");
    }
    return j;
}

/* ---------- info / captura ---------- */
static void write_info(const char *extra) {
    char tmp[] = INFO_PATH ".tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "{\"mode\":\"%dx%d@%d\",\"requested\":\"%s\",\"modes\":%s,\"connected\":%s,"
               "\"video\":[%lld,%lld],\"fbo\":[%d,%d],\"hwdec\":\"%s\",\"audio\":\"%s\",\"pid\":%d%s%s}\n",
            W, H, mode.vrefresh, st.mode_req, modes_json, conn_connected() ? "true" : "false",
            (long long)dw, (long long)dh, vw, vh, hwdec_cur,
            !strcmp(ao_cur, "null") || !strcmp(ao_cur, "-") ? "sin salida" : audio_jack ? "jack 3,5 mm" : "HDMI", getpid(), extra ? "," : "", extra ? extra : "");
    fclose(f);
    rename(tmp, INFO_PATH);
}

static int want_shot;
static void do_shot(void) {
    int w2 = W / 2, h2 = H / 2;
    unsigned char *px = malloc((size_t)W * H * 4), *out = malloc((size_t)w2 * h2 * 3);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < h2; y++)
        for (int x = 0; x < w2; x++) {
            const unsigned char *a = px + ((size_t)(H - 1 - 2 * y) * W + 2 * x) * 4;
            const unsigned char *b = a - (size_t)W * 4;
            for (int c = 0; c < 3; c++)
                out[((size_t)y * w2 + x) * 3 + c] = (a[c] + a[4 + c] + b[c] + b[4 + c]) / 4;
        }
    FILE *f = fopen(SHOT_PATH ".tmp", "wb");
    if (f) { fprintf(f, "P6\n%d %d\n255\n", w2, h2); fwrite(out, 1, (size_t)w2 * h2 * 3, f); fclose(f); rename(SHOT_PATH ".tmp", SHOT_PATH); }
    free(px); free(out);
}

/* ---------- dibujo ---------- */
static GLuint ovbo;

static void draw_overlay(void) {
    if (!ovbo) glGenBuffers(1, &ovbo);
    glUseProgram(flat.p);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, ovbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(0);
    float *v = malloc(sizeof(float) * 2 * 8192);
    for (int si = 0; si < st.n; si++) {
        Surface *s = &st.s[si];
        if (!s->mesh) continue;
        int selected = si == st.sel, n = 0, sx = s->segx + 1;
        int kx = s->segx / (s->cols - 1), ky = s->segy / (s->rows - 1);
        /* lineas de la cuadricula de control (solo el contorno si no esta seleccionada) */
        for (int c = 0; c < s->cols; c++) {
            if (!selected && c && c != s->cols - 1) continue;
            for (int j = 0; j < s->segy && n < 8180; j++) {
                float *a = s->mesh + 4 * (j * sx + c * kx), *b = s->mesh + 4 * ((j + 1) * sx + c * kx);
                v[2 * n] = a[0]; v[2 * n + 1] = a[1]; n++;
                v[2 * n] = b[0]; v[2 * n + 1] = b[1]; n++;
            }
        }
        for (int r = 0; r < s->rows; r++) {
            if (!selected && r && r != s->rows - 1) continue;
            for (int i = 0; i < s->segx && n < 8180; i++) {
                float *a = s->mesh + 4 * (r * ky * sx + i), *b = s->mesh + 4 * (r * ky * sx + i + 1);
                v[2 * n] = a[0]; v[2 * n + 1] = a[1]; n++;
                v[2 * n] = b[0]; v[2 * n + 1] = b[1]; n++;
            }
        }
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 2 * n, v, GL_STREAM_DRAW);
        const float *pc = PCOL[si % MAXS];
        float k = selected ? 1.0f : 0.5f;
        glUniform4f(flat.col, pc[0] * k, pc[1] * k, pc[2] * k, 1);
        glLineWidth(selected ? 2.0f : 1.0f);
        glDrawArrays(GL_LINES, 0, n);
        if (!selected) continue;

        /* puntos: de la cuadricula si se edita la cuadricula, si no las esquinas */
        Homo m = homo(s->c);
        n = 0;
        if (st.edit_grid) for (int q = 0; q < s->cols * s->rows; q++) { V2 p = hmap(&m, s->p[q]); v[2 * n] = p.x; v[2 * n + 1] = p.y; n++; }
        else for (int q = 0; q < 4; q++) { v[2 * n] = s->c[q].x; v[2 * n + 1] = s->c[q].y; n++; }
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 2 * n, v, GL_STREAM_DRAW);
        glUniform1f(flat.ps, st.edit_grid ? 10.0f : 16.0f);
        glUniform4f(flat.col, 0, 0, 0, 1);
        glDrawArrays(GL_POINTS, 0, n);
        glUniform1f(flat.ps, st.edit_grid ? 6.0f : 10.0f);
        glUniform4f(flat.col, 1, 1, 1, 1);
        glDrawArrays(GL_POINTS, 0, n);
        if (st.selpt >= 0 && st.selpt < n) {
            glUniform1f(flat.ps, st.edit_grid ? 14.0f : 20.0f);
            glUniform4f(flat.col, 1, 0.1f, 0.1f, 1);
            glDrawArrays(GL_POINTS, st.selpt, 1);
        }
    }
    free(v);
    glDisableVertexAttribArray(0);
}

static double tcpu(void);
static int stats_on, n_vid; static double t_mpv;
static void draw(void) {
    double t0 = stats_on ? tcpu() : 0;
    if (video_dirty && vfbo) {
        n_vid++;
        mpv_opengl_fbo fbo = { .fbo = (int)vfbo, .w = vw, .h = vh };
        int flip = 0;
        mpv_render_param p[] = { {MPV_RENDER_PARAM_OPENGL_FBO, &fbo}, {MPV_RENDER_PARAM_FLIP_Y, &flip}, {0} };
        mpv_render_context_render(mpv_gl, p);
        video_dirty = 0;
    }
    if (stats_on) t_mpv += tcpu() - t0;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!st.blackout) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        for (int i = 0; i < st.n; i++) {
            Surface *s = &st.s[i];
            if (!s->enabled) continue;
            if (s->dirty) build_mesh(s);
            int md = st.pattern ? 1 : s->content;
            if (md == 0 && !vfbo) continue;
            Prog *p = &prog[md];
            glUseProgram(p->p);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, vtex);
            glUniform1i(p->tex, 0);
            glUniform4fv(p->src, 1, s->src);
            glUniform3fv(p->color, 1, md == 1 ? PCOL[i % MAXS] : s->color);
            glUniform4fv(p->feather, 1, s->feather);
            glUniform4f(p->adj, s->opacity, s->bright, s->contrast, 1.0f / s->gamma);
            glUniform2f(p->grid, s->cols - 1, s->rows - 1);
            glUniform1f(p->ps, 1);
            glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->ibo);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
            glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
            glDrawElements(GL_TRIANGLES, s->nidx, GL_UNSIGNED_SHORT, 0);
        }
        glDisable(GL_BLEND);
        for (int i = 0; i < st.n; i++) if (st.s[i].dirty) build_mesh(&st.s[i]);
        if (st.overlay) draw_overlay();
    }
    if (want_shot) { do_shot(); want_shot = 0; }
}

static void on_flip(int fd, unsigned int seq, unsigned int sec, unsigned int usec, void *data) {
    (void)fd; (void)seq; (void)sec; (void)usec; (void)data;
    flip_pending = 0;
    if (bo_cur) gbm_surface_release_buffer(gsurf, bo_cur);
    bo_cur = bo_next; bo_next = NULL;
    mpv_render_context_report_swap(mpv_gl);
}

static void present(void) {
    eglSwapBuffers(edpy, esurf);
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(gsurf);
    uint32_t fb = bo_fb(bo);
    if (!crtc_set) {
        if (drmModeSetCrtc(drm_fd, crtc_id, fb, 0, 0, &conn_id, 1, &mode)) DIE("drmModeSetCrtc: %s", strerror(errno));
        crtc_set = 1;
        if (bo_cur) gbm_surface_release_buffer(gsurf, bo_cur);
        bo_cur = bo;
        mpv_render_context_report_swap(mpv_gl);
        return;
    }
    if (drmModePageFlip(drm_fd, crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT, NULL)) {
        LOG("page flip: %s", strerror(errno));
        gbm_surface_release_buffer(gsurf, bo);
        return;
    }
    bo_next = bo; flip_pending = 1;
}

/* ---------- control ---------- */
static int ctl_fd = -1;
static void ctl_open(void) {
    mkdir(RUN_DIR, 0775);
    unlink(CTL_PATH);
    ctl_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", CTL_PATH);
    if (bind(ctl_fd, (struct sockaddr *)&a, sizeof a)) DIE("bind %s: %s", CTL_PATH, strerror(errno));
    chmod(CTL_PATH, 0660);
    int sz = 1 << 20; setsockopt(ctl_fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
}

static int ctl_read(void) {
    static char buf[1 << 18];
    int changed = 0;
    for (;;) {
        ssize_t n = recv(ctl_fd, buf, sizeof buf - 1, 0);
        if (n <= 0) break;
        buf[n] = 0;
        cJSON *j = cJSON_Parse(buf);
        if (!j) { LOG("ctl: JSON invalido (%zd bytes)", n); continue; }
        const char *cmd = jstr(j, "cmd", NULL);
        if (cmd && !strcmp(cmd, "shot")) want_shot = 1;
        else if (cmd && !strcmp(cmd, "restart")) { LOG("reinicio pedido"); exit(0); }
        else if (!cmd) state_apply(j, 0);
        cJSON_Delete(j);
        changed = 1;
    }
    return changed;
}

/* MAPPER_STATS=1: cada 5 s, cuadros dibujados y CPU del hilo principal por etapa */
static double t_draw, t_present; static int n_draw;
static double tcpu(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void stats_tick(void) {
    static time_t last;
    time_t now = time(NULL);
    if (!last) last = now;
    if (now - last < 5) return;
    double dt = now - last;
    LOG("stats: %.1f draw/s, %.1f video/s | CPU ms por cuadro: mpv %.2f, superficies %.2f, present %.2f",
        n_draw / dt, n_vid / dt, n_draw ? t_mpv * 1e3 / n_draw : 0, n_draw ? t_draw * 1e3 / n_draw : 0,
        n_draw ? t_present * 1e3 / n_draw : 0);
    t_mpv = t_draw = t_present = 0; n_draw = n_vid = 0; last = now;
}

static void on_term(int sig) { (void)sig; _exit(0); }

int main(void) {
    signal(SIGTERM, on_term); signal(SIGINT, on_term); signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, NULL, _IOLBF, 0);
    stats_on = getenv("MAPPER_STATS") != NULL;
    cJSON *j0 = state_read();
    snprintf(st.mode_req, sizeof st.mode_req, "%s",
             jstr(cJSON_GetObjectItemCaseSensitive(j0, "output"), "mode", "1280x720"));
    drm_fd = drm_open();
    if (drm_fd < 0) DIE("sin dispositivo DRM");
    int warned = 0;
    while (!drm_pick()) {
        if (!warned++) LOG("ningun conector conectado: espero el proyector");
        sleep(2);
    }
    edid_audio();
    egl_init();
    gl_init();
    ctl_open();
    mpv_init();
    state_apply(j0, 1);
    cJSON_Delete(j0);
    write_info(NULL);

    int need = 1;
    time_t last_check = time(NULL);
    for (;;) {
        if (need && !flip_pending) {
            double a = stats_on ? tcpu() : 0;
            draw();
            double b = stats_on ? tcpu() : 0;
            present();
            if (stats_on) { t_draw += b - a; t_present += tcpu() - b; n_draw++; }
            need = 0;
        }
        if (stats_on) stats_tick();
        struct pollfd fds[3] = { {drm_fd, POLLIN, 0}, {ctl_fd, POLLIN, 0}, {wake_pipe[0], POLLIN, 0} };
        int r = poll(fds, 3, 2000);
        if (r < 0 && errno != EINTR) DIE("poll: %s", strerror(errno));
        if (fds[0].revents & POLLIN) {
            drmEventContext ev = { .version = 2, .page_flip_handler = on_flip };
            drmHandleEvent(drm_fd, &ev);
        }
        if (fds[1].revents & POLLIN) need |= ctl_read();
        if (fds[2].revents & POLLIN) {
            char b[64];
            while (read(wake_pipe[0], b, sizeof b) > 0) {}
            mpv_events();
            uint64_t fl = mpv_render_context_update(mpv_gl);
            if (fl & MPV_RENDER_UPDATE_FRAME) { video_dirty = 1; need = 1; }
        }
        if (video_dirty) need = 1;
        time_t now = time(NULL);
        if (now - last_check >= 2) {
            last_check = now;
            static int was = 1;
            int is = conn_connected();
            if (is != was) {
                LOG("conector %s", is ? "conectado" : "desconectado");
                if (is) exit(0);  /* se reconecto: reinicio para releer el EDID y el modo */
                was = is;
            }
        }
    }
}

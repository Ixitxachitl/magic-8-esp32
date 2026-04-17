/*
 *  magic8ball_ui.c – Magic 8 Ball LVGL UI
 *
 *  Wireframe icosahedron (d20) with 3D rotation, perspective projection,
 *  depth-based edge brightness, bubble particles, text only when settled.
 */

#include "magic8ball_ui.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* ── layout constants (computed in _init from screen_size) ──────── */
static int scr_sz;
static int cx, cy;            /* centre of screen */
static int circle_d;          /* circle diameter  */

/* ── Icosahedron geometry ──────────────────────────────────────── */
#define ICO_VERTS 12
#define ICO_EDGES 30

static float ico_v[ICO_VERTS][3];   /* 3D vertices (scaled to screen) */
static float ico_radius;             /* circumradius in pixels          */

/* Edge connectivity – golden-ratio icosahedron vertex pairs */
static const int ico_edge[ICO_EDGES][2] = {
    {0,2},{0,4},{0,5},{0,8},{0,10},
    {1,3},{1,4},{1,5},{1,9},{1,11},
    {2,6},{2,7},{2,8},{2,10},
    {3,6},{3,7},{3,9},{3,11},
    {4,5},{4,8},{4,9},
    {5,10},{5,11},
    {6,7},{6,8},{6,9},
    {7,10},{7,11},
    {8,9},
    {10,11}
};

/* ── LVGL objects ──────────────────────────────────────────────── */
static lv_obj_t  *bg_circle;
static lv_obj_t  *ico_line[ICO_EDGES];    /* 30 wireframe edge segments  */
static lv_point_t ico_pts[ICO_EDGES][2];  /* projected 2D endpoints      */
static lv_obj_t  *answer_lbl;
static lv_obj_t  *thinking_lbl;

static float center_x, center_y;   /* icosahedron centre in bg_circle */

#define PERSP_D  500.0f

/* ── state ─────────────────────────────────────────────────────── */
static bool       is_thinking  = false;
static lv_timer_t *think_tmr   = NULL;
static int         dot_phase   = 0;

/* callbacks */
static void (*tap_cb)(void)       = NULL;
static void (*longpress_cb)(void) = NULL;

/* ── animation state ───────────────────────────────────────────── */
static bool        animating    = false;
static lv_timer_t *anim_timer   = NULL;
static int         spin_angle   = 0;       /* 0.1-degree units           */
static int         spin_rate    = 50;      /* 0.1 deg per tick           */

/* ── settle (deceleration) state ───────────────────────────────── */
static bool        settling     = false;
static void      (*settle_cb)(void) = NULL;

/* ── bubble particles ──────────────────────────────────────────── */
#define NUM_BUBBLES 14

typedef struct {
    lv_obj_t *obj;
    float     x, y;
    float     dx, dy;
    float     speed;
    int       life;
    int       max_life;
    bool      active;
} bubble_t;

static bubble_t bubbles[NUM_BUBBLES];
static int bubble_spawn_cd = 0;

/* ── helpers ───────────────────────────────────────────────────── */
static void _set_opa(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/* ── current visual scale (1.0 = normal, >1 = enlarged) ────────── */
static float visual_scale    = 1.0f;
static float target_scale    = 1.0f;
static bool  scale_animating = false;
static bool  pending_spin    = false;   /* start spinning after zoom-out */

/* Scale that makes the settled front triangle fill the circle */
#define SETTLED_SCALE  1.4f

/* Front-face edge indices in ico_edge[] for face (2,6,7) */
static const int front_edge[3] = {10, 11, 23};  /* {2,6}, {2,7}, {6,7} */

/* ── 3D rotation + perspective projection for all 12 vertices ──── */
static void update_icosahedron_ex(float ry, float rx, float scl)
{
    float cy_r = cosf(ry), sy_r = sinf(ry);
    float cx_r = cosf(rx), sx_r = sinf(rx);

    /* Project all 12 vertices */
    float pv[ICO_VERTS][2];
    float pz[ICO_VERTS];

    for (int i = 0; i < ICO_VERTS; i++) {
        float x = ico_v[i][0] * scl, y = ico_v[i][1] * scl, z = ico_v[i][2] * scl;

        /* Ry rotation */
        float x1 = x * cy_r + z * sy_r;
        float y1 = y;
        float z1 = -x * sy_r + z * cy_r;

        /* Rx rotation */
        float x2 = x1;
        float y2 = y1 * cx_r - z1 * sx_r;
        float z2 = y1 * sx_r + z1 * cx_r;

        /* Perspective projection */
        float s = PERSP_D / (PERSP_D + z2);
        pv[i][0] = x2 * s;
        pv[i][1] = y2 * s;
        pz[i] = z2;
    }

    /* Blend factor: 0 = normal wireframe, 1 = settled triangle only */
    float t = (scl - 1.0f) / (SETTLED_SCALE - 1.0f);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    /* Update each of the 30 edge line segments */
    for (int e = 0; e < ICO_EDGES; e++) {
        int a = ico_edge[e][0], b = ico_edge[e][1];

        ico_pts[e][0].x = (lv_coord_t)(center_x + pv[a][0]);
        ico_pts[e][0].y = (lv_coord_t)(center_y + pv[a][1]);
        ico_pts[e][1].x = (lv_coord_t)(center_x + pv[b][0]);
        ico_pts[e][1].y = (lv_coord_t)(center_y + pv[b][1]);
        lv_line_set_points(ico_line[e], ico_pts[e], 2);

        /* Is this one of the 3 front-face triangle edges? */
        bool is_front_face = (e == front_edge[0] || e == front_edge[1] || e == front_edge[2]);

        /* Back-face cull: hide edges whose midpoint is behind centre */
        float avg_z = (pz[a] + pz[b]) * 0.5f;
        bool behind = (avg_z > ico_radius * scl * 0.15f);

        lv_opa_t opa;
        if (behind) {
            opa = LV_OPA_TRANSP;
        } else if (is_front_face) {
            opa = LV_OPA_COVER;
        } else {
            /* Non-front-face visible edges fade out as we zoom in */
            opa = (lv_opa_t)(LV_OPA_COVER * (1.0f - t));
        }
        lv_obj_set_style_line_opa(ico_line[e], opa, 0);
    }
}

static void update_icosahedron(int angle_tenth)
{
    float ry = (float)angle_tenth * M_PI / 1800.0f;
    float rx = ry * 0.37f;
    update_icosahedron_ex(ry, rx, visual_scale);
}

/* ── Settled pose: face (2,6,7) facing camera, vertex 2 at top ─── */
/*  Rx rotation of +1.931 rad orients face (2,6,7) toward -Z
    with v2=(0,-1,φ) at the top (upward-pointing triangle).        */
#define SETTLE_RY  0.0f
#define SETTLE_RX  1.931f

static void show_settled_pose(float scl)
{
    update_icosahedron_ex(SETTLE_RY, SETTLE_RX, scl);
}

/* ── animation timer (33 ms ≈ 30 fps) ─────────────────────────── */
static void anim_tick(lv_timer_t *t)
{
    (void)t;

    /* --- scale animation (zoom in/out) runs before everything else --- */
    if (scale_animating) {
        float diff = target_scale - visual_scale;
        if (fabsf(diff) < 0.02f) {
            visual_scale = target_scale;
            scale_animating = false;
            if (pending_spin) {
                /* zoom-out complete → start actual spinning */
                pending_spin = false;
                animating = true;
            } else if (!animating) {
                /* zoom-in complete, idle → pause timer */
                lv_timer_pause(anim_timer);
            }
        } else {
            visual_scale += diff * 0.12f;
        }
        show_settled_pose(visual_scale);
        return;
    }

    if (!animating) return;

    /* --- handle settling (deceleration) --- */
    if (settling && spin_rate > 0) {
        spin_rate -= 3;
        if (spin_rate <= 0) {
            spin_rate = 0;
            settling  = false;
            /* snap to settled pose + begin enlarge */
            visual_scale = 1.0f;
            target_scale = SETTLED_SCALE;
            scale_animating = true;
            animating = false;
            void (*cb)(void) = settle_cb;
            settle_cb = NULL;
            show_settled_pose(visual_scale);
            if (cb) cb();
            return;
        }
    }

    /* --- spin icosahedron --- */
    spin_angle = (spin_angle + spin_rate) % 3600;
    update_icosahedron(spin_angle);

    /* --- subtle colour pulse on all edges --- */
    float pulse = (sinf((float)spin_angle * M_PI / 1800.0f) + 1.0f) / 2.0f;
    uint8_t r = (uint8_t)(0x10 + pulse * 0x20);
    uint8_t g = (uint8_t)(0x40 + pulse * 0x30);
    uint8_t b = (uint8_t)(0x70 + pulse * 0x30);
    for (int e = 0; e < ICO_EDGES; e++)
        lv_obj_set_style_line_color(ico_line[e], lv_color_make(r, g, b), 0);

    /* --- bubble management --- */
    float half = (float)circle_d / 2.0f;
    for (int i = 0; i < NUM_BUBBLES; i++) {
        if (!bubbles[i].active) continue;

        bubbles[i].x += bubbles[i].dx * bubbles[i].speed;
        bubbles[i].y += bubbles[i].dy * bubbles[i].speed;
        bubbles[i].x += (float)((rand() % 3) - 1) * 0.3f;
        bubbles[i].y += (float)((rand() % 3) - 1) * 0.3f;
        bubbles[i].life--;

        float dx = bubbles[i].x - half;
        float dy = bubbles[i].y - half;
        float dist = sqrtf(dx * dx + dy * dy);
        if (bubbles[i].life <= 0 || dist < 40.0f) {
            lv_obj_add_flag(bubbles[i].obj, LV_OBJ_FLAG_HIDDEN);
            bubbles[i].active = false;
            continue;
        }

        lv_obj_set_pos(bubbles[i].obj, (int)bubbles[i].x, (int)bubbles[i].y);
        float progress = 1.0f - (float)bubbles[i].life / (float)bubbles[i].max_life;
        lv_opa_t opa = (lv_opa_t)(LV_OPA_60 * (1.0f - progress * 0.7f));
        lv_obj_set_style_bg_opa(bubbles[i].obj, opa, 0);
    }

    /* spawn new bubble from a random edge */
    if (--bubble_spawn_cd <= 0) {
        bubble_spawn_cd = 2 + (rand() % 4);
        for (int i = 0; i < NUM_BUBBLES; i++) {
            if (!bubbles[i].active) {
                float angle = (float)(rand() % 3600) * M_PI / 1800.0f;
                float edge_r = half - 10.0f;
                float bx = half + cosf(angle) * edge_r;
                float by = half + sinf(angle) * edge_r;
                float nx = half - bx;
                float ny = half - by;
                float len = sqrtf(nx * nx + ny * ny);
                if (len < 1.0f) len = 1.0f;
                nx /= len; ny /= len;

                int sz = 5 + (rand() % 8);
                int life = 50 + (rand() % 40);

                bubbles[i].x        = bx;
                bubbles[i].y        = by;
                bubbles[i].dx       = nx;
                bubbles[i].dy       = ny;
                bubbles[i].speed    = 1.5f + (float)(rand() % 20) / 10.0f;
                bubbles[i].life     = life;
                bubbles[i].max_life = life;
                bubbles[i].active   = true;

                lv_obj_set_size(bubbles[i].obj, sz, sz);
                lv_obj_set_pos(bubbles[i].obj, (int)bx, (int)by);
                lv_obj_set_style_bg_opa(bubbles[i].obj, LV_OPA_60, 0);
                lv_obj_clear_flag(bubbles[i].obj, LV_OBJ_FLAG_HIDDEN);
                break;
            }
        }
    }
}

/* ── event handler (tap / long-press on the circle) ────────────── */
static void circle_evt_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SHORT_CLICKED && tap_cb && !is_thinking)
        tap_cb();
    if (code == LV_EVENT_LONG_PRESSED && longpress_cb)
        longpress_cb();
}

/* ── public API ────────────────────────────────────────────────── */

void magic8ball_ui_init(lv_obj_t *parent, int screen_size)
{
    scr_sz   = screen_size;
    cx       = scr_sz / 2;
    cy       = scr_sz / 2;
    circle_d = scr_sz;

    /* -- screen background: true black (AMOLED off) -- */
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    /* -- dark-blue circular "window" -- */
    bg_circle = lv_obj_create(parent);
    lv_obj_remove_style_all(bg_circle);
    lv_obj_set_size(bg_circle, circle_d, circle_d);
    lv_obj_align(bg_circle, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(bg_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bg_circle, lv_color_hex(0x030810), 0);
    lv_obj_set_style_bg_opa(bg_circle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bg_circle, lv_color_hex(0x0c1830), 0);
    lv_obj_set_style_border_width(bg_circle, 3, 0);
    lv_obj_set_style_border_opa(bg_circle, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(bg_circle, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(bg_circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bg_circle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_clip_corner(bg_circle, true, 0);
    lv_obj_add_event_cb(bg_circle, circle_evt_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(bg_circle, circle_evt_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* -- create bubble objects (behind icosahedron) -- */
    for (int i = 0; i < NUM_BUBBLES; i++) {
        bubbles[i].obj = lv_obj_create(bg_circle);
        lv_obj_remove_style_all(bubbles[i].obj);
        lv_obj_set_size(bubbles[i].obj, 10, 10);
        lv_obj_set_style_radius(bubbles[i].obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(bubbles[i].obj, lv_color_hex(0x3080d0), 0);
        lv_obj_set_style_bg_opa(bubbles[i].obj, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(bubbles[i].obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bubbles[i].obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(bubbles[i].obj, LV_OBJ_FLAG_SCROLLABLE);
        bubbles[i].active = false;
    }

    /* -- build icosahedron vertices (golden ratio construction) -- */
    ico_radius = (float)circle_d * 0.35f;
    float phi = (1.0f + sqrtf(5.0f)) / 2.0f;
    float scale = ico_radius / sqrtf(1.0f + phi * phi);

    /* 12 vertices: cyclic permutations of (0, ±1, ±φ) */
    float raw[12][3] = {
        { 0,  1,  phi}, { 0,  1, -phi}, { 0, -1,  phi}, { 0, -1, -phi},
        { 1,  phi, 0 }, {-1,  phi, 0 }, { 1, -phi, 0 }, {-1, -phi, 0 },
        { phi, 0,  1 }, { phi, 0, -1 }, {-phi, 0,  1 }, {-phi, 0, -1 }
    };
    for (int i = 0; i < ICO_VERTS; i++) {
        ico_v[i][0] = raw[i][0] * scale;
        ico_v[i][1] = raw[i][1] * scale;
        ico_v[i][2] = raw[i][2] * scale;
    }

    center_x = (float)circle_d / 2.0f;
    center_y = (float)circle_d / 2.0f;

    /* -- create 30 edge line objects -- */
    for (int e = 0; e < ICO_EDGES; e++) {
        ico_line[e] = lv_line_create(bg_circle);
        lv_obj_set_style_line_color(ico_line[e], lv_color_hex(0x1a4a80), 0);
        lv_obj_set_style_line_width(ico_line[e], 2, 0);
        lv_obj_set_style_line_rounded(ico_line[e], true, 0);
        lv_obj_clear_flag(ico_line[e], LV_OBJ_FLAG_CLICKABLE);
    }

    /* initial pose: settled (enlarged, face-up triangle) */
    visual_scale = SETTLED_SCALE;
    show_settled_pose(visual_scale);

    /* -- answer text (centred inside icosahedron) -- */
    int text_w = (int)(ico_radius * 1.2f);
    answer_lbl = lv_label_create(bg_circle);
    lv_label_set_text(answer_lbl, "SHAKE\nOR TAP");
    lv_obj_set_style_text_color(answer_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(answer_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(answer_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(answer_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(answer_lbl, text_w);
    lv_obj_align(answer_lbl, LV_ALIGN_CENTER, 0, 0);

    /* -- thinking dots (hidden) -- */
    thinking_lbl = lv_label_create(bg_circle);
    lv_label_set_text(thinking_lbl, "");
    lv_obj_set_style_text_color(thinking_lbl, lv_color_hex(0x2060a0), 0);
    lv_obj_set_style_text_font(thinking_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(thinking_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(thinking_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(thinking_lbl, LV_OBJ_FLAG_HIDDEN);
}

/* ── set answer text with fade-in (only called when settled) ───── */
void magic8ball_ui_set_answer(const char *text)
{
    if (think_tmr) { lv_timer_del(think_tmr); think_tmr = NULL; }
    is_thinking = false;
    lv_obj_add_flag(thinking_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(answer_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(answer_lbl, text);

    int len = (int)strlen(text);
    if (len > 50)
        lv_obj_set_style_text_font(answer_lbl, &lv_font_montserrat_20, 0);
    else if (len > 30)
        lv_obj_set_style_text_font(answer_lbl, &lv_font_montserrat_24, 0);
    else
        lv_obj_set_style_text_font(answer_lbl, &lv_font_montserrat_28, 0);

    /* Ensure anim timer is fully stopped (prevents crash if TTS blocks) */
    animating       = false;
    settling        = false;
    scale_animating = false;
    pending_spin    = false;
    visual_scale    = SETTLED_SCALE;
    show_settled_pose(visual_scale);
    if (anim_timer) lv_timer_pause(anim_timer);

    /* Set opacity directly (no animation – avoids crash when TTS blocks loop) */
    lv_obj_set_style_opa(answer_lbl, LV_OPA_COVER, 0);

    /* restore default edge colour */
    for (int e = 0; e < ICO_EDGES; e++)
        lv_obj_set_style_line_color(ico_line[e], lv_color_hex(0x1a4a80), 0);
}

/* ── toggle thinking state (spinning = visual indicator) ───────── */
void magic8ball_ui_set_thinking(bool thinking)
{
    is_thinking = thinking;

    if (thinking) {
        spin_rate = 35;
        /* hide all text while spinning */
        lv_obj_add_flag(answer_lbl,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(thinking_lbl, LV_OBJ_FLAG_HIDDEN);
        if (think_tmr) { lv_timer_del(think_tmr); think_tmr = NULL; }
    } else {
        lv_obj_add_flag(thinking_lbl, LV_OBJ_FLAG_HIDDEN);
        if (think_tmr) { lv_timer_del(think_tmr); think_tmr = NULL; }
    }
}

/* ── register callbacks ────────────────────────────────────────── */
void magic8ball_ui_set_tap_cb(void (*cb)(void))       { tap_cb = cb; }
void magic8ball_ui_set_longpress_cb(void (*cb)(void)) { longpress_cb = cb; }

/* ── listening mode (no text while spinning) ───────────────────── */
void magic8ball_ui_set_listening(bool listening)
{
    if (listening) {
        if (think_tmr) { lv_timer_del(think_tmr); think_tmr = NULL; }
        is_thinking = false;
        /* hide all text – icosahedron spin is the visual indicator */
        lv_obj_add_flag(answer_lbl,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(thinking_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── show transcription text (only when settled) ───────────────── */
void magic8ball_ui_set_transcript(const char *text)
{
    if (animating) return;   /* no text while spinning */

    lv_obj_clear_flag(answer_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(answer_lbl, text);

    int len = (int)strlen(text);
    if (len > 60)
        lv_obj_set_style_text_font(answer_lbl, &lv_font_montserrat_16, 0);
    else if (len > 30)
        lv_obj_set_style_text_font(answer_lbl, &lv_font_montserrat_20, 0);
    else
        lv_obj_set_style_text_font(answer_lbl, &lv_font_montserrat_24, 0);
}

/* ── audio level → border glow ─────────────────────────────────── */
void magic8ball_ui_set_audio_level(int level)
{
    /* map 0-100 to dim…bright border */
    uint8_t v = (uint8_t)(0x0c + (0x40 - 0x0c) * level / 100);
    lv_obj_set_style_border_color(bg_circle, lv_color_hex((v/3) << 16 | (v/2) << 8 | v), 0);
}

/* ── start animation (spinning d20 + bubbles, hide text) ──────── */
void magic8ball_ui_start_anim(void)
{
    if (animating || pending_spin) return;
    settling        = false;
    settle_cb       = NULL;
    spin_angle      = 0;
    spin_rate       = 50;
    bubble_spawn_cd = 0;

    /* animate from settled-enlarged down to normal size, then spin */
    pending_spin     = true;
    scale_animating  = true;
    target_scale     = 1.0f;
    /* visual_scale stays at SETTLED_SCALE from the settled state */

    /* hide all text while spinning */
    lv_obj_add_flag(answer_lbl,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(thinking_lbl, LV_OBJ_FLAG_HIDDEN);
    if (think_tmr) { lv_timer_del(think_tmr); think_tmr = NULL; }

    /* brighten border during animation */
    lv_obj_set_style_border_color(bg_circle, lv_color_hex(0x1a3060), 0);

    if (anim_timer)
        lv_timer_resume(anim_timer);
    else
        anim_timer = lv_timer_create(anim_tick, 33, NULL);
}

/* ── stop animation, reset to default pose ─────────────────────── */
void magic8ball_ui_stop_anim(void)
{
    animating       = false;
    settling        = false;
    settle_cb       = NULL;
    pending_spin    = false;
    scale_animating = false;
    spin_angle      = 0;
    spin_rate       = 0;

    /* hide all bubbles */
    for (int i = 0; i < NUM_BUBBLES; i++) {
        lv_obj_add_flag(bubbles[i].obj, LV_OBJ_FLAG_HIDDEN);
        bubbles[i].active = false;
    }

    /* reset icosahedron to settled pose (enlarged, face-up triangle) */
    visual_scale = SETTLED_SCALE;
    show_settled_pose(visual_scale);
    for (int e = 0; e < ICO_EDGES; e++) {
        lv_obj_set_style_line_color(ico_line[e], lv_color_hex(0x1a4a80), 0);
    }

    /* restore border */
    lv_obj_set_style_border_color(bg_circle, lv_color_hex(0x0c1830), 0);

    /* pause timer instead of deleting (safe from within timer callback) */
    if (anim_timer) {
        lv_timer_pause(anim_timer);
    }
}

/* ── decelerate spin then call done_cb ─────────────────────────── */
void magic8ball_ui_settle_then(void (*done_cb)(void))
{
    if (!animating || spin_rate == 0) {
        magic8ball_ui_stop_anim();
        if (done_cb) done_cb();
        return;
    }
    settling  = true;
    settle_cb = done_cb;
}

#include "dash_home.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "apps.controller.h"
#include "appitem.view.h"
#include "launcher.controller.h"
#include "steamart.h"
#include "ui/dash_theme.h"
#include "ui/settings/settings.controller.h"
#include "lvgl/theme/lv_theme_moonlight.h"
#include "lv_gridview.h"
#include "util/i18n.h"
#include "app_settings.h"
#include "backend/pcmanager.h"
#include "lvgl/font/material_icons_regular_symbols.h"
#include <SDL.h>

#define DASH_TABS 4
#define DASH_QUICK 4
#define DASH_ROWS 4
#define DASH_MAX_RECENT 64

enum {
    ROW_TABS = 0, ROW_SHELF, ROW_QUICK, ROW_GAMES,
};

enum {
    TAB_GAMES = 0, TAB_RECENT, TAB_FAV, TAB_SETTINGS,
};

typedef struct recent_t {
    char name[128];
    long long ts;
} recent_t;

typedef struct dash_home_t {
    apps_fragment_t *controller;
    lv_obj_t *view;
    lv_obj_t *backdrop, *shade_h, *shade_v;
    lv_obj_t *tabs[DASH_TABS];
    lv_obj_t *clock;
    lv_obj_t *shelf, *chip, *logo, *title, *tagline, *info;
    lv_obj_t *btn_primary, *btn_primary_lbl, *btn_secondary, *btn_secondary_lbl;
    lv_obj_t *quick[DASH_QUICK], *quick_sub[DASH_QUICK];
    lv_obj_t *games_label, *games_scroll;
    lv_obj_t *empty_label;
    lv_timer_t *clock_timer, *art_timer;

    int tab;
    int *visible;
    int visible_count, visible_cap;

    int focused_app_id;      /* app whose info is on the shelf */
    int shelf_mode_running;  /* shelf shows the running game */
    char shelf_name[128];

    int row;
    int col[DASH_ROWS];

    recent_t recent[DASH_MAX_RECENT];
    int recent_count;

    lv_coord_t tile_w, tile_h;
    lv_style_t st_pill, st_pill_focus, st_tab, st_tab_checked, st_tab_focus, st_quick, st_quick_focus;
} dash_home_t;

static void key_cb(lv_event_t *e);

static void tab_click_cb(lv_event_t *e);

static void primary_click_cb(lv_event_t *e);

static void secondary_click_cb(lv_event_t *e);

static void quick_click_cb(lv_event_t *e);

static void clock_tick(lv_timer_t *t);

static void art_timer_cb(lv_timer_t *t);

static void focus_row(dash_home_t *home, int row);

static void shelf_update(dash_home_t *home);

static dash_home_t *home_of(apps_fragment_t *c) {
    return c ? c->home : NULL;
}

static lv_color_t color_a(uint32_t hex, uint8_t alpha) {
    lv_color_t c = lv_color_hex(hex);
#if LV_COLOR_DEPTH == 32
    c.ch.alpha = alpha;
#else
    LV_UNUSED(alpha);
#endif
    return c;
}

static const lv_font_t *font_display(lv_obj_t *obj) {
    const lv_font_t *f = global->ui.fonts.fonts.display;
    return f ? f : lv_theme_get_font_large(obj);
}

static const lv_font_t *font_title(lv_obj_t *obj) {
    const lv_font_t *f = global->ui.fonts.fonts.title;
    return f ? f : lv_theme_get_font_large(obj);
}

static lv_group_t *detail_group(apps_fragment_t *c) {
    launcher_fragment_t *parent = (launcher_fragment_t *) lv_fragment_get_parent(&c->base);
    return parent ? parent->detail_group : NULL;
}

static void group_add(dash_home_t *home, lv_obj_t *obj) {
    lv_group_t *g = detail_group(home->controller);
    if (lv_obj_get_group(obj)) { lv_group_remove_obj(obj); }
    if (g) { lv_group_add_obj(g, obj); }
    lv_obj_add_event_cb(obj, key_cb, LV_EVENT_KEY, home);
}

/* ------------------------------------------------------------------ recent history */

static void recent_path(char *out, size_t len) {
    snprintf(out, len, "%s/dash_recent.txt", app_configuration->conf_dir);
}

static void recent_load(dash_home_t *home) {
    char path[4096];
    recent_path(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    home->recent_count = 0;
    if (!fp) { return; }
    char line[256];
    while (home->recent_count < DASH_MAX_RECENT && fgets(line, sizeof(line), fp)) {
        char *tab = strchr(line, '\t');
        if (!tab) { continue; }
        *tab = '\0';
        recent_t *r = &home->recent[home->recent_count++];
        snprintf(r->name, sizeof(r->name), "%s", line);
        r->ts = atoll(tab + 1);
    }
    fclose(fp);
}

static void recent_save(dash_home_t *home) {
    char path[4096];
    recent_path(path, sizeof(path));
    FILE *fp = fopen(path, "w");
    if (!fp) { return; }
    for (int i = 0; i < home->recent_count; i++) {
        fprintf(fp, "%s\t%lld\n", home->recent[i].name, home->recent[i].ts);
    }
    fclose(fp);
}

static long long recent_ts(dash_home_t *home, const char *name) {
    for (int i = 0; i < home->recent_count; i++) {
        if (strcmp(home->recent[i].name, name) == 0) { return home->recent[i].ts; }
    }
    return 0;
}

void dash_home_record_launch(apps_fragment_t *controller, const apploader_item_t *app) {
    dash_home_t *home = home_of(controller);
    if (!home || !app) { return; }
    long long now = (long long) time(NULL);
    for (int i = 0; i < home->recent_count; i++) {
        if (strcmp(home->recent[i].name, app->base.name) == 0) {
            home->recent[i].ts = now;
            recent_save(home);
            return;
        }
    }
    if (home->recent_count >= DASH_MAX_RECENT) {
        int oldest = 0;
        for (int i = 1; i < home->recent_count; i++) {
            if (home->recent[i].ts < home->recent[oldest].ts) { oldest = i; }
        }
        home->recent[oldest] = home->recent[--home->recent_count];
    }
    recent_t *r = &home->recent[home->recent_count++];
    snprintf(r->name, sizeof(r->name), "%s", app->base.name);
    r->ts = now;
    recent_save(home);
}

/* ------------------------------------------------------------------ styles & widgets */

static void styles_init(dash_home_t *h) {
    lv_style_init(&h->st_pill);
    lv_style_set_radius(&h->st_pill, LV_DPX(10));
    lv_style_set_bg_color(&h->st_pill, lv_color_white());
    lv_style_set_bg_opa(&h->st_pill, LV_OPA_20);
    lv_style_set_text_color(&h->st_pill, lv_color_white());
    lv_style_set_border_width(&h->st_pill, 0);
    lv_style_set_shadow_width(&h->st_pill, 0);
    lv_style_set_outline_width(&h->st_pill, 0);
    lv_style_set_pad_hor(&h->st_pill, LV_DPX(22));
    lv_style_set_pad_ver(&h->st_pill, LV_DPX(10));

    lv_style_init(&h->st_pill_focus);
    lv_style_set_bg_opa(&h->st_pill_focus, LV_OPA_COVER);
    lv_style_set_bg_color(&h->st_pill_focus, lv_color_white());
    lv_style_set_text_color(&h->st_pill_focus, lv_color_hex(0x14151A));
    lv_style_set_transform_width(&h->st_pill_focus, LV_DPX(3));
    lv_style_set_transform_height(&h->st_pill_focus, LV_DPX(3));
    lv_style_set_shadow_width(&h->st_pill_focus, LV_DPX(18));
    lv_style_set_shadow_color(&h->st_pill_focus, lv_color_black());
    lv_style_set_shadow_opa(&h->st_pill_focus, LV_OPA_40);

    lv_style_init(&h->st_tab);
    lv_style_set_radius(&h->st_tab, LV_RADIUS_CIRCLE);
    lv_style_set_bg_opa(&h->st_tab, LV_OPA_TRANSP);
    lv_style_set_text_color(&h->st_tab, lv_color_hex(0xE8E8EE));
    lv_style_set_border_width(&h->st_tab, 0);
    lv_style_set_shadow_width(&h->st_tab, 0);
    lv_style_set_outline_width(&h->st_tab, 0);
    lv_style_set_pad_hor(&h->st_tab, LV_DPX(16));
    lv_style_set_pad_ver(&h->st_tab, LV_DPX(6));

    lv_style_init(&h->st_tab_checked);
    lv_style_set_bg_color(&h->st_tab_checked, lv_color_white());
    lv_style_set_bg_opa(&h->st_tab_checked, LV_OPA_20);

    lv_style_init(&h->st_tab_focus);
    lv_style_set_bg_color(&h->st_tab_focus, lv_color_white());
    lv_style_set_bg_opa(&h->st_tab_focus, LV_OPA_COVER);
    lv_style_set_text_color(&h->st_tab_focus, lv_color_hex(0x14151A));

    lv_style_init(&h->st_quick);
    lv_style_set_radius(&h->st_quick, LV_DPX(12));
    lv_style_set_bg_color(&h->st_quick, lv_color_white());
    lv_style_set_bg_opa(&h->st_quick, LV_OPA_10);
    lv_style_set_border_width(&h->st_quick, 0);
    lv_style_set_shadow_width(&h->st_quick, 0);
    lv_style_set_outline_width(&h->st_quick, 0);
    lv_style_set_text_color(&h->st_quick, lv_color_white());
    lv_style_set_pad_hor(&h->st_quick, LV_DPX(16));
    lv_style_set_pad_ver(&h->st_quick, LV_DPX(10));

    lv_style_init(&h->st_quick_focus);
    lv_style_set_bg_opa(&h->st_quick_focus, LV_OPA_COVER);
    lv_style_set_bg_color(&h->st_quick_focus, lv_color_white());
    lv_style_set_text_color(&h->st_quick_focus, lv_color_hex(0x14151A));
    lv_style_set_transform_width(&h->st_quick_focus, LV_DPX(3));
    lv_style_set_transform_height(&h->st_quick_focus, LV_DPX(3));
}

static lv_obj_t *make_pill(dash_home_t *h, lv_obj_t *parent, lv_obj_t **label_out, bool primary) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &h->st_pill, 0);
    lv_obj_add_style(btn, &h->st_pill_focus, LV_STATE_FOCUS_KEY);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, LV_DPX(8), 0);
    if (primary) {
        lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0x14151A), 0);
        lv_obj_t *icon = lv_label_create(btn);
        lv_obj_set_style_text_font(icon, lv_theme_moonlight_get_iconfont_large(btn), 0);
        lv_label_set_text_static(icon, MAT_SYMBOL_PLAY_ARROW);
    }
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, lv_theme_get_font_large(btn), 0);
    *label_out = lbl;
    group_add(h, btn);
    return btn;
}

static lv_obj_t *make_shade(lv_obj_t *parent, lv_grad_dir_t dir, lv_color_t from, lv_color_t to, uint8_t from_frac) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_grad_dsc_t *grad = lv_mem_alloc(sizeof(lv_grad_dsc_t));
    memset(grad, 0, sizeof(*grad));
    grad->dir = dir;
    grad->stops_count = 2;
    grad->stops[0].color = from;
    grad->stops[0].frac = from_frac;
    grad->stops[1].color = to;
    grad->stops[1].frac = 255;
    lv_obj_set_style_bg_grad(obj, grad, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_user_data(obj, grad);
    return obj;
}

static void shade_free_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    void *grad = lv_obj_get_user_data(obj);
    if (grad) { lv_mem_free(grad); }
}

/* ------------------------------------------------------------------ create / destroy */

void dash_home_create(apps_fragment_t *c, lv_obj_t *view) {
    dash_home_t *h = calloc(1, sizeof(dash_home_t));
    c->home = h;
    h->controller = c;
    h->view = view;
    h->tab = TAB_GAMES;
    h->row = ROW_GAMES;
    styles_init(h);
    recent_load(h);
    steamart_init();

    lv_obj_set_style_bg_color(view, lv_color_hex(DASH_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(view, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(view, true, 0);

    /* backdrop: Steam hero art of the shelf game */
    lv_obj_t *box = lv_obj_create(view);
    lv_obj_remove_style_all(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, LV_PCT(100), LV_PCT(DASH_BACKDROP_PCT));
    h->backdrop = lv_img_create(box);
    lv_obj_clear_flag(h->backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(h->backdrop, LV_OBJ_FLAG_HIDDEN);

    /* shades: navy from the left and from the bottom so text stays readable */
    h->shade_h = make_shade(view, LV_GRAD_DIR_HOR, color_a(DASH_COLOR_BG, 245), color_a(DASH_COLOR_BG, 0), 0);
    lv_obj_set_size(h->shade_h, LV_PCT(75), LV_PCT(DASH_BACKDROP_PCT));
    lv_obj_add_event_cb(h->shade_h, shade_free_cb, LV_EVENT_DELETE, NULL);
    h->shade_v = make_shade(view, LV_GRAD_DIR_VER, color_a(DASH_COLOR_BG, 0), color_a(DASH_COLOR_BG, 255), 90);
    lv_obj_set_size(h->shade_v, LV_PCT(100), LV_PCT(DASH_BACKDROP_PCT));
    lv_obj_add_event_cb(h->shade_v, shade_free_cb, LV_EVENT_DELETE, NULL);

    /* top bar: tabs + clock */
    lv_obj_t *bar = lv_obj_create(view);
    lv_obj_remove_style_all(bar);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(bar, LV_PCT(100), LV_PCT(9));
    lv_obj_set_pos(bar, 0, LV_PCT(2));
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, LV_DPX(10), 0);
    static const char *tab_names[DASH_TABS] = {"Gry", "Ostatnie", "Ulubione", "Ustawienia"};
    for (int i = 0; i < DASH_TABS; i++) {
        lv_obj_t *tab = lv_btn_create(bar);
        lv_obj_remove_style_all(tab);
        lv_obj_add_style(tab, &h->st_tab, 0);
        lv_obj_add_style(tab, &h->st_tab_checked, LV_STATE_CHECKED);
        lv_obj_add_style(tab, &h->st_tab_focus, LV_STATE_FOCUS_KEY);
        lv_obj_t *lbl = lv_label_create(tab);
        lv_obj_set_style_text_font(lbl, lv_theme_get_font_large(tab), 0);
        lv_label_set_text_static(lbl, tab_names[i]);
        lv_obj_add_event_cb(tab, tab_click_cb, LV_EVENT_CLICKED, h);
        group_add(h, tab);
        h->tabs[i] = tab;
    }
    lv_obj_add_state(h->tabs[TAB_GAMES], LV_STATE_CHECKED);
    h->clock = lv_label_create(view);
    lv_obj_set_style_text_font(h->clock, lv_theme_get_font_large(view), 0);
    lv_obj_set_style_text_color(h->clock, lv_color_hex(0xE8E8EE), 0);
    lv_obj_align(h->clock, LV_ALIGN_TOP_RIGHT, -LV_DPX(28), LV_DPX(22));
    h->clock_timer = lv_timer_create(clock_tick, 10000, h);
    clock_tick(h->clock_timer);

    /* shelf */
    lv_obj_t *shelf = h->shelf = lv_obj_create(view);
    lv_obj_remove_style_all(shelf);
    lv_obj_clear_flag(shelf, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(shelf, LV_PCT(70), LV_SIZE_CONTENT);
    lv_obj_update_layout(view);
    lv_obj_align(shelf, LV_ALIGN_BOTTOM_LEFT, LV_DPX(36),
                 -(lv_obj_get_height(view) * (100 - DASH_QUICK_Y_PCT + 3) / 100));
    lv_obj_set_flex_flow(shelf, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(shelf, LV_DPX(8), 0);

    h->chip = lv_label_create(shelf);
    lv_obj_set_style_bg_color(h->chip, lv_color_hex(0x50DC8C), 0);
    lv_obj_set_style_bg_opa(h->chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(h->chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(h->chip, LV_DPX(10), 0);
    lv_obj_set_style_pad_ver(h->chip, LV_DPX(3), 0);
    lv_obj_set_style_text_color(h->chip, lv_color_hex(0x0A1E14), 0);
    lv_obj_set_style_text_font(h->chip, lv_theme_get_font_small(shelf), 0);
    lv_label_set_text_static(h->chip, "DZIAŁA NA PC");
    lv_obj_add_flag(h->chip, LV_OBJ_FLAG_HIDDEN);

    h->logo = lv_img_create(shelf);
    lv_obj_add_flag(h->logo, LV_OBJ_FLAG_HIDDEN);

    h->title = lv_label_create(shelf);
    lv_obj_set_style_text_font(h->title, font_title(shelf), 0);
    lv_obj_set_style_text_color(h->title, lv_color_white(), 0);
    lv_obj_set_width(h->title, LV_PCT(100));
    lv_label_set_long_mode(h->title, LV_LABEL_LONG_DOT);
    lv_label_set_text(h->title, "");

    h->tagline = lv_label_create(shelf);
    lv_obj_set_style_text_font(h->tagline, font_display(shelf), 0);
    lv_obj_set_style_text_color(h->tagline, lv_color_white(), 0);
    lv_obj_set_width(h->tagline, LV_PCT(100));
    lv_label_set_long_mode(h->tagline, LV_LABEL_LONG_WRAP);
    lv_label_set_text(h->tagline, "");

    h->info = lv_label_create(shelf);
    lv_obj_set_style_text_font(h->info, lv_theme_get_font_normal(shelf), 0);
    lv_obj_set_style_text_color(h->info, lv_color_hex(0xD2D4DC), 0);

    lv_obj_t *btns = lv_obj_create(shelf);
    lv_obj_remove_style_all(btns);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, LV_DPX(14), 0);
    lv_obj_set_style_pad_top(btns, LV_DPX(6), 0);
    h->btn_primary = make_pill(h, btns, &h->btn_primary_lbl, true);
    h->btn_secondary = make_pill(h, btns, &h->btn_secondary_lbl, false);
    lv_obj_add_event_cb(h->btn_primary, primary_click_cb, LV_EVENT_CLICKED, h);
    lv_obj_add_event_cb(h->btn_secondary, secondary_click_cb, LV_EVENT_CLICKED, h);

    /* quick actions */
    lv_obj_t *quick = lv_obj_create(view);
    lv_obj_remove_style_all(quick);
    lv_obj_clear_flag(quick, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(quick, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_pos(quick, LV_DPX(36), LV_PCT(DASH_QUICK_Y_PCT));
    lv_obj_set_flex_flow(quick, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(quick, LV_DPX(12), 0);
    static const char *quick_icons[DASH_QUICK] = {MAT_SYMBOL_TV, MAT_SYMBOL_DESKTOP_WINDOWS, MAT_SYMBOL_SETTINGS,
                                                  MAT_SYMBOL_VIDEO_SETTINGS};
    static const char *quick_names[DASH_QUICK] = {"Wybudź PC", "Pulpit", "Ustawienia", "Statystyki"};
    for (int i = 0; i < DASH_QUICK; i++) {
        lv_obj_t *btn = lv_btn_create(quick);
        lv_obj_remove_style_all(btn);
        lv_obj_add_style(btn, &h->st_quick, 0);
        lv_obj_add_style(btn, &h->st_quick_focus, LV_STATE_FOCUS_KEY);
        lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(btn, LV_DPX(10), 0);
        lv_obj_t *icon = lv_label_create(btn);
        lv_obj_set_style_text_font(icon, lv_theme_moonlight_get_iconfont_normal(btn), 0);
        lv_label_set_text_static(icon, quick_icons[i]);
        lv_obj_t *col = lv_obj_create(btn);
        lv_obj_remove_style_all(col);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_t *name = lv_label_create(col);
        lv_obj_set_style_text_font(name, lv_theme_get_font_normal(btn), 0);
        lv_label_set_text_static(name, quick_names[i]);
        lv_obj_t *sub = lv_label_create(col);
        lv_obj_set_style_text_font(sub, lv_theme_get_font_small(btn), 0);
        lv_obj_set_style_text_opa(sub, LV_OPA_70, 0);
        lv_label_set_text(sub, "");
        h->quick_sub[i] = sub;
        lv_obj_add_event_cb(btn, quick_click_cb, LV_EVENT_CLICKED, h);
        group_add(h, btn);
        h->quick[i] = btn;
    }

    /* games row */
    h->games_label = lv_label_create(view);
    lv_obj_set_style_text_font(h->games_label, lv_theme_get_font_large(view), 0);
    lv_obj_set_style_text_color(h->games_label, lv_color_white(), 0);
    lv_obj_set_pos(h->games_label, LV_DPX(36), LV_PCT(DASH_GAMES_LABEL_Y_PCT));
    lv_label_set_text_static(h->games_label, "Twoje gry");

    lv_obj_t *scroll = h->games_scroll = lv_obj_create(view);
    lv_obj_remove_style_all(scroll);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(scroll, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(scroll, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(scroll, LV_PCT(100), LV_PCT(100 - DASH_GAMES_Y_PCT));
    lv_obj_set_pos(scroll, 0, LV_PCT(DASH_GAMES_Y_PCT));

    lv_obj_t *applist = c->applist = lv_gridview_create(scroll);
    lv_obj_add_flag(applist, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_scroll_dir(applist, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(applist, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_hor(applist, LV_DPX(36), 0);
    lv_obj_set_style_pad_ver(applist, LV_DPX(18), 0);
    lv_obj_set_style_pad_column(applist, LV_DPX(22), 0);
    lv_obj_set_style_radius(applist, 0, 0);
    lv_obj_set_style_border_side(applist, LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_bg_opa(applist, 0, 0);
    lv_obj_set_height(applist, LV_PCT(100));
    lv_obj_add_event_cb(applist, key_cb, LV_EVENT_KEY, h);

    h->empty_label = lv_label_create(scroll);
    lv_obj_set_style_text_color(h->empty_label, lv_color_hex(DASH_COLOR_MUTED), 0);
    lv_obj_set_style_text_font(h->empty_label, lv_theme_get_font_normal(scroll), 0);
    lv_obj_set_pos(h->empty_label, LV_DPX(36), LV_DPX(30));
    lv_obj_add_flag(h->empty_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_update_layout(view);
    lv_coord_t scroll_h = lv_obj_get_height(scroll);
    h->tile_h = (lv_coord_t) (scroll_h * 0.66f);
    h->tile_w = (lv_coord_t) (h->tile_h * 2.14f);

    shelf_update(h);
}

void dash_home_destroy(apps_fragment_t *c) {
    dash_home_t *h = home_of(c);
    if (!h) { return; }
    if (h->clock_timer) { lv_timer_del(h->clock_timer); }
    if (h->art_timer) { lv_timer_del(h->art_timer); }
    lv_style_reset(&h->st_pill);
    lv_style_reset(&h->st_pill_focus);
    lv_style_reset(&h->st_tab);
    lv_style_reset(&h->st_tab_checked);
    lv_style_reset(&h->st_tab_focus);
    lv_style_reset(&h->st_quick);
    lv_style_reset(&h->st_quick_focus);
    free(h->visible);
    free(h);
    c->home = NULL;
}

/* ------------------------------------------------------------------ data */

typedef struct sort_entry_t {
    int index;
    long long ts;
} sort_entry_t;

static int cmp_recent(const void *a, const void *b) {
    const sort_entry_t *x = a, *y = b;
    return (x->ts < y->ts) - (x->ts > y->ts);
}

void dash_home_rebuild(apps_fragment_t *c, const apploader_list_t *list) {
    dash_home_t *h = home_of(c);
    if (!h) { return; }
    h->visible_count = 0;
    if (!list) {
        lv_obj_add_flag(h->empty_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (h->visible_cap < list->count) {
        h->visible = realloc(h->visible, sizeof(int) * (list->count + 1));
        h->visible_cap = list->count;
    }
    sort_entry_t *tmp = malloc(sizeof(sort_entry_t) * (list->count + 1));
    int n = 0;
    for (int i = 0; i < list->count; i++) {
        const apploader_item_t *app = &list->items[i];
        if (app->hidden && !c->show_hidden_apps) { continue; }
        if (h->tab == TAB_FAV && !app->fav) { continue; }
        long long ts = recent_ts(h, app->base.name);
        if (h->tab == TAB_RECENT && ts == 0) { continue; }
        tmp[n].index = i;
        tmp[n].ts = ts;
        n++;
    }
    if (h->tab == TAB_RECENT) { qsort(tmp, n, sizeof(sort_entry_t), cmp_recent); }
    for (int i = 0; i < n && i < 255; i++) { h->visible[h->visible_count++] = tmp[i].index; }
    free(tmp);

    if (h->visible_count == 0) {
        lv_label_set_text_static(h->empty_label, h->tab == TAB_RECENT ? "Tu pojawią się ostatnio grane gry."
                                                 : h->tab == TAB_FAV ? "Przytrzymaj OK na grze i wybierz \"Star\", "
                                                                       "żeby dodać ją do ulubionych."
                                                 : "Brak gier na tym komputerze.");
        lv_obj_clear_flag(h->empty_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(c->applist, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(h->empty_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(c->applist, LV_OBJ_FLAG_HIDDEN);
    }
}

int dash_home_visible_count(apps_fragment_t *c) {
    dash_home_t *h = home_of(c);
    return h ? h->visible_count : 0;
}

int dash_home_visible_index(apps_fragment_t *c, int position) {
    dash_home_t *h = home_of(c);
    if (!h || position < 0 || position >= h->visible_count) { return -1; }
    return h->visible[position];
}

void dash_home_tile_size(apps_fragment_t *c, lv_coord_t *w, lv_coord_t *hh) {
    dash_home_t *h = home_of(c);
    *w = h ? h->tile_w : LV_DPX(200);
    *hh = h ? h->tile_h : LV_DPX(94);
}

/* ------------------------------------------------------------------ tiles */

static void tile_art_cb(lv_obj_t *owner, const lv_img_dsc_t *img, const char *genre, void *userdata) {
    LV_UNUSED(genre);
    LV_UNUSED(userdata);
    if (!img) {
        lv_obj_add_flag(owner, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_img_set_src(owner, img);
    lv_obj_set_pos(owner, 0, 0);
    lv_obj_clear_flag(owner, LV_OBJ_FLAG_HIDDEN);
}

void dash_home_bind_tile(apps_fragment_t *c, lv_obj_t *item, const apploader_item_t *app) {
    appitem_viewholder_t *holder = lv_obj_get_user_data(item);
    if (!holder || !holder->art) { return; }
    lv_obj_add_flag(holder->art, LV_OBJ_FLAG_HIDDEN);
    if (steamart_is_builtin(app->base.name)) { return; }
    steamart_request_sized(app->base.name, STEAMART_HEADER, c->col_width, c->col_height, true, holder->art,
                           tile_art_cb, NULL);
}

/* ------------------------------------------------------------------ shelf */

static const apploader_item_t *app_by_id(apps_fragment_t *c, int id) {
    if (!c->apploader_apps || id == 0) { return NULL; }
    return apploader_list_item_by_id(c->apploader_apps, id);
}

static void backdrop_cb(lv_obj_t *owner, const lv_img_dsc_t *img, const char *genre, void *userdata) {
    LV_UNUSED(genre);
    LV_UNUSED(userdata);
    if (!img) {
        lv_obj_add_flag(owner, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_img_set_src(owner, img);
    lv_obj_set_pos(owner, 0, 0);
    bool was_hidden = lv_obj_has_flag(owner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(owner, LV_OBJ_FLAG_HIDDEN);
    if (was_hidden) { lv_obj_fade_in(owner, 350, 0); }
}

static void logo_cb(lv_obj_t *owner, const lv_img_dsc_t *img, const char *genre, void *userdata) {
    dash_home_t *h = userdata;
    if (genre && !h->shelf_mode_running) {
        lv_label_set_text(h->tagline, steamart_tagline(h->shelf_name, genre));
    }
    if (!img) {
        lv_obj_add_flag(owner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(h->title, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_img_set_src(owner, img);
    lv_obj_clear_flag(owner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(h->title, LV_OBJ_FLAG_HIDDEN);
}

static void art_timer_cb(lv_timer_t *t) {
    dash_home_t *h = t->user_data;
    h->art_timer = NULL;
    if (!h->shelf_name[0] || steamart_is_builtin(h->shelf_name)) {
        lv_obj_add_flag(h->backdrop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(h->logo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(h->title, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_t *box = lv_obj_get_parent(h->backdrop);
    steamart_request_sized(h->shelf_name, STEAMART_HERO, lv_obj_get_width(box), lv_obj_get_height(box), true,
                           h->backdrop, backdrop_cb, h);
    steamart_request_sized(h->shelf_name, STEAMART_LOGO, LV_DPX(340), LV_DPX(80), false, h->logo, logo_cb, h);
}

static void info_text(char *buf, size_t len) {
    const STREAM_CONFIGURATION *s = &app_configuration->stream;
    const char *res;
    char tmp[24];
    switch (s->height) {
        case 2160: res = "4K"; break;
        case 1440: res = "1440p"; break;
        case 1080: res = "1080p"; break;
        case 720: res = "720p"; break;
        default:
            snprintf(tmp, sizeof(tmp), "%dx%d ", s->width, s->height);
            res = tmp;
    }
    const char *codec = app_configuration->av1 ? "AV1" : app_configuration->hevc ? "HEVC" : "H.264";
    snprintf(buf, len, "%s%d  ·  %s  ·  %d Mb/s  ·  audio: %s", res, s->fps, codec, s->bitrate / 1000,
             app_configuration->audio_backend ? app_configuration->audio_backend : "auto");
}

static void quick_update(dash_home_t *h) {
    char info[96];
    snprintf(info, sizeof(info), "%s%d  ·  %d Mb/s", app_configuration->stream.height == 2160 ? "4K" : "",
             app_configuration->stream.fps, app_configuration->stream.bitrate / 1000);
    const SERVER_STATE *state = pcmanager_state(pcmanager, &h->controller->uuid);
    bool online = state && (state->code & SERVER_STATE_ONLINE);
    lv_label_set_text_static(h->quick_sub[0], online ? "PC jest online" : "Wake-on-LAN");
    lv_label_set_text_static(h->quick_sub[1], "Windows na TV");
    lv_label_set_text(h->quick_sub[2], info);
    lv_label_set_text_static(h->quick_sub[3], app_configuration->dash_show_stats ? "Włączone" : "Wyłączone");
}

static void shelf_update(dash_home_t *h) {
    apps_fragment_t *c = h->controller;
    int running = pcmanager_server_current_app(pcmanager, &c->uuid);
    if (SDL_getenv("DASH_IGNORE_RUNNING")) { running = 0; } /* dev screenshots only */
    const apploader_item_t *running_app = app_by_id(c, running);
    const apploader_item_t *app = running_app ? running_app : app_by_id(c, h->focused_app_id);
    if (!app && h->visible_count > 0 && c->apploader_apps) {
        app = &c->apploader_apps->items[h->visible[0]];
    }
    h->shelf_mode_running = running_app != NULL;

    char info[160];
    info_text(info, sizeof(info));
    lv_label_set_text(h->info, info);
    quick_update(h);

    const char *name = app ? app->base.name : "";
    bool changed = strcmp(name, h->shelf_name) != 0;
    snprintf(h->shelf_name, sizeof(h->shelf_name), "%s", name);
    lv_label_set_text(h->title, name);

    if (h->shelf_mode_running) {
        lv_obj_clear_flag(h->chip, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_static(h->tagline, SDL_strcasecmp(name, "Desktop") == 0 ? "Wróć do pulpitu."
                                                                                 : "Wróć do gry.");
        lv_label_set_text_static(h->btn_primary_lbl, "Wznów");
        lv_label_set_text_static(h->btn_secondary_lbl, "Zakończ grę");
    } else {
        lv_obj_add_flag(h->chip, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(h->tagline, app ? steamart_tagline(name, NULL) : "Wybierz grę.");
        lv_label_set_text_static(h->btn_primary_lbl, "Graj");
        lv_label_set_text_static(h->btn_secondary_lbl, "Szczegóły");
    }
    if (app) {
        lv_obj_clear_flag(h->btn_primary, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(h->btn_secondary, LV_OBJ_FLAG_HIDDEN);
    }
    if (changed) {
        if (h->art_timer) { lv_timer_del(h->art_timer); }
        h->art_timer = lv_timer_create(art_timer_cb, 220, h);
        lv_timer_set_repeat_count(h->art_timer, 1);
    }
}

void dash_home_focus_app(apps_fragment_t *c, int app_id) {
    dash_home_t *h = home_of(c);
    if (!h) { return; }
    if (app_id != 0) { h->focused_app_id = app_id; }
    shelf_update(h);
}

/* ------------------------------------------------------------------ actions */

static void open_settings(void) {
    lv_fragment_t *top = lv_fragment_manager_get_top(global->ui.fm);
    if (!top) { return; }
    lv_fragment_t *fragment = lv_fragment_create(&settings_controller_cls, global);
    lv_fragment_manager_push(global->ui.fm, fragment, lv_fragment_get_container(top));
}

static void set_tab(dash_home_t *h, int tab) {
    if (tab == TAB_SETTINGS) {
        open_settings();
        return;
    }
    if (tab == h->tab) { return; }
    lv_obj_clear_state(h->tabs[h->tab], LV_STATE_CHECKED);
    h->tab = tab;
    lv_obj_add_state(h->tabs[h->tab], LV_STATE_CHECKED);
    static const char *labels[] = {"Twoje gry", "Ostatnio grane", "Ulubione"};
    lv_label_set_text_static(h->games_label, labels[tab]);
    apps_controller_reload_grid(h->controller);
}

static void tab_click_cb(lv_event_t *e) {
    dash_home_t *h = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_current_target(e);
    for (int i = 0; i < DASH_TABS; i++) {
        if (h->tabs[i] == target) { set_tab(h, i); }
    }
}

static void primary_click_cb(lv_event_t *e) {
    dash_home_t *h = lv_event_get_user_data(e);
    apps_fragment_t *c = h->controller;
    int running = pcmanager_server_current_app(pcmanager, &c->uuid);
    const apploader_item_t *app = app_by_id(c, running ? running : h->focused_app_id);
    if (!app && h->visible_count > 0 && c->apploader_apps) { app = &c->apploader_apps->items[h->visible[0]]; }
    if (app) { apps_controller_launch(c, app); }
}

static void secondary_click_cb(lv_event_t *e) {
    dash_home_t *h = lv_event_get_user_data(e);
    apps_fragment_t *c = h->controller;
    int running = pcmanager_server_current_app(pcmanager, &c->uuid);
    if (running) {
        apps_controller_quit(c);
        return;
    }
    const apploader_item_t *app = app_by_id(c, h->focused_app_id);
    if (!app && h->visible_count > 0 && c->apploader_apps) { app = &c->apploader_apps->items[h->visible[0]]; }
    if (app) { apps_controller_open_menu(c, app->base.id); }
}

static void quick_click_cb(lv_event_t *e) {
    dash_home_t *h = lv_event_get_user_data(e);
    apps_fragment_t *c = h->controller;
    lv_obj_t *target = lv_event_get_current_target(e);
    if (target == h->quick[0]) {
        apps_controller_wake(c);
        lv_label_set_text_static(h->quick_sub[0], "Wysłano sygnał...");
    } else if (target == h->quick[1]) {
        if (!c->apploader_apps) { return; }
        for (int i = 0; i < c->apploader_apps->count; i++) {
            const apploader_item_t *app = &c->apploader_apps->items[i];
            if (SDL_strcasecmp(app->base.name, "Desktop") == 0) {
                apps_controller_launch(c, app);
                return;
            }
        }
    } else if (target == h->quick[2]) {
        open_settings();
    } else if (target == h->quick[3]) {
        app_configuration->dash_show_stats = !app_configuration->dash_show_stats;
        settings_save(app_configuration);
        quick_update(h);
    }
}

static void clock_tick(lv_timer_t *t) {
    dash_home_t *h = t->user_data;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M", tm);
    lv_label_set_text(h->clock, buf);
}

/* ------------------------------------------------------------------ remote navigation */

static int row_len(dash_home_t *h, int row) {
    switch (row) {
        case ROW_TABS: return DASH_TABS;
        case ROW_SHELF: return lv_obj_has_flag(h->btn_primary, LV_OBJ_FLAG_HIDDEN) ? 0 : 2;
        case ROW_QUICK: return DASH_QUICK;
        case ROW_GAMES: return h->visible_count > 0 ? 1 : 0;
        default: return 0;
    }
}

static lv_obj_t *row_obj(dash_home_t *h, int row, int col) {
    switch (row) {
        case ROW_TABS: return h->tabs[col];
        case ROW_SHELF: return col == 0 ? h->btn_primary : h->btn_secondary;
        case ROW_QUICK: return h->quick[col];
        case ROW_GAMES: return h->controller->applist;
        default: return NULL;
    }
}

static bool locate(dash_home_t *h, lv_obj_t *obj, int *row, int *col) {
    for (int r = 0; r < DASH_ROWS; r++) {
        for (int i = 0; i < row_len(h, r) || (r == ROW_GAMES && i == 0); i++) {
            if (row_obj(h, r, i) == obj) {
                *row = r;
                *col = i;
                return true;
            }
        }
    }
    return false;
}

static void focus_row(dash_home_t *h, int row) {
    int len = row_len(h, row);
    if (len <= 0) { return; }
    int col = LV_CLAMP(0, h->col[row], len - 1);
    lv_obj_t *obj = row_obj(h, row, col);
    if (!obj) { return; }
    h->row = row;
    lv_group_focus_obj(obj);
    if (row != ROW_GAMES) { lv_obj_add_state(obj, LV_STATE_FOCUS_KEY); }
    if (row == ROW_TABS || row == ROW_SHELF) {
        lv_obj_scroll_to_y(h->games_scroll, 0, LV_ANIM_OFF);
    }
}

static void key_cb(lv_event_t *e) {
    dash_home_t *h = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_current_target(e);
    int row, col;
    if (!locate(h, target, &row, &col)) { return; }
    uint32_t key = lv_event_get_key(e);
    switch (key) {
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT: {
            if (row == ROW_GAMES) { return; } /* gridview handles its own left/right */
            int next = col + (key == LV_KEY_RIGHT ? 1 : -1);
            if (next < 0 || next >= row_len(h, row)) { return; }
            h->col[row] = next;
            focus_row(h, row);
            lv_event_stop_processing(e);
            break;
        }
        case LV_KEY_UP:
        case LV_KEY_DOWN: {
            h->col[row] = col;
            int dir = key == LV_KEY_DOWN ? 1 : -1;
            for (int r = row + dir; r >= 0 && r < DASH_ROWS; r += dir) {
                if (row_len(h, r) > 0) {
                    focus_row(h, r);
                    lv_event_stop_processing(e);
                    return;
                }
            }
            break;
        }
        default:
            break;
    }
}

#pragma once

#include "lvgl.h"
#include "coverloader.h"
#include "backend/apploader/apploader.h"
#include "uuidstr.h"

typedef struct app_t app_t;

typedef struct {
    lv_style_t cover;
    lv_style_t btn;
    lv_style_transition_dsc_t tr_pressed;
    lv_style_transition_dsc_t tr_released;
    lv_img_dsc_t fav_indicator_src;
    lv_img_dsc_t defcover_src;
} appitem_styles_t;


typedef struct apps_fragment_t {
    lv_fragment_t base;
    app_t *global;
    uuidstr_t uuid;

    int def_app;
    bool def_app_launched;

    bool show_hidden_apps;

    apploader_t *apploader;
    coverloader_t *coverloader;
    apploader_cb_t apploader_cb;

    apploader_list_t *apploader_apps;
    const char *apploader_error;

    lv_obj_t *applist, *appload, *apperror;
    lv_obj_t *errortitle, *errorhint, *errordetail;
    lv_obj_t *actions;

    lv_obj_t *quit_progress;

    /* Dash Games: hero header with the focused game */
    lv_obj_t *hero, *hero_title, *hero_info;
    struct dash_home_t *home;

    appitem_styles_t appitem_style;
    int col_count;
    lv_coord_t col_width, col_height;
    int focus_backup;
} apps_fragment_t;

typedef struct {
    int app_id;
    apps_fragment_t *controller;
    const appitem_styles_t *styles;
    lv_obj_t *play_indicator;
    lv_obj_t *title;
    lv_obj_t *art; /* Dash Games: landscape Steam header over the cover */
} appitem_viewholder_t;

typedef struct {
    app_t *global;
    uuidstr_t host;
    int def_app;
} apps_fragment_arg_t;

extern const lv_fragment_class_t apps_controller_class;

void apps_controller_hero_update(apps_fragment_t *controller, int app_id);

/* Dash Games home helpers */
void apps_controller_reload_grid(apps_fragment_t *controller);

void apps_controller_launch(apps_fragment_t *controller, const apploader_item_t *app);

void apps_controller_quit(apps_fragment_t *controller);

void apps_controller_open_menu(apps_fragment_t *controller, int app_id);

void apps_controller_wake(apps_fragment_t *controller);
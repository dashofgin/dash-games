#pragma once

/*
 * Dash Games: Steam artwork for the launcher (hero background, logo, landscape header, genre).
 *
 * The host only gives us the app name, so the Steam app id is looked up by name through the public store
 * search API. Results and images are cached on disk; decoded textures are cached in memory (small LRU for the
 * large hero images). All network/disk work runs on a background thread, callbacks run on the UI thread.
 */

#include <stdbool.h>
#include "lvgl.h"

typedef enum steamart_kind_t {
    STEAMART_HERO = 0,
    STEAMART_LOGO,
    STEAMART_HEADER,
    STEAMART_KIND_COUNT,
} steamart_kind_t;

/**
 * Called on the UI thread. `img` is NULL when the artwork is not available. `genre` is the first Steam genre
 * (English) or NULL. The image descriptor stays valid until steamart_purge() or until it is evicted; callers
 * should set it as an lv_img source and request it again on the next bind.
 */
typedef void (*steamart_cb_t)(lv_obj_t *owner, const lv_img_dsc_t *img, const char *genre, void *userdata);

void steamart_init(void);

void steamart_deinit(void);

/**
 * Request artwork for an app. `owner` must be a live object; if it is deleted before completion the callback is
 * skipped. A new request for the same owner+kind supersedes the previous one.
 */
void steamart_request(const char *app_name, steamart_kind_t kind, lv_obj_t *owner, steamart_cb_t cb,
                      void *userdata);

/**
 * Same as steamart_request, but the image is scaled on the worker thread to exactly w x h:
 * cover crops to fill the box (hero, tiles), otherwise it fits inside the box keeping the aspect (logo).
 */
void steamart_request_sized(const char *app_name, steamart_kind_t kind, lv_coord_t w, lv_coord_t h, bool cover,
                            lv_obj_t *owner, steamart_cb_t cb, void *userdata);

/** Whether this app name is a Vibepollo/Sunshine built-in (Desktop, Steam Big Picture...) with no Steam page. */
bool steamart_is_builtin(const char *app_name);

/** Tagline for the shelf, chosen by known app or Steam genre. Never NULL. */
const char *steamart_tagline(const char *app_name, const char *genre);

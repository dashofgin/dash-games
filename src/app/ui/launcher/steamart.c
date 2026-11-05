#include "steamart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#include <SDL.h>
#include <SDL_image.h>
#include <curl/curl.h>

#include "app.h"
#include "util/bus.h"
#include "util/path.h"
#include "logging.h"

#include "lv_sdl_img.h"
#include "draw/sdl/lv_draw_sdl.h"
#include "draw/sdl/lv_draw_sdl_img.h"
#include "misc/lv_lru.h"

#define STEAMART_TAG "SteamArt"
#define STEAMART_HERO_CACHE 6
#define STEAMART_MAX_META 512

/* ---------- metadata (UI thread + worker, guarded by meta_lock) ---------- */

typedef struct steamart_meta_t {
    char *name;
    int appid;              /* 0 = unknown/not on Steam, -1 = lookup failed (retry later) */
    char genre[48];
    char header_url[512];
} steamart_meta_t;

/* ---------- decoded textures (UI thread only) ---------- */

typedef struct steamart_tex_t {
    char *name;
    steamart_kind_t kind;
    int w, h;
    bool missing;           /* artwork does not exist */
    lv_img_dsc_t dsc;
    lv_sdl_img_data_t data;
    uint32_t last_used;
    struct steamart_tex_t *next;
} steamart_tex_t;

typedef struct steamart_req_t {
    char *name;
    steamart_kind_t kind;
    int w, h;
    bool cover;
    lv_obj_t *owner;
    steamart_cb_t cb;
    void *userdata;
    bool superseded;
    /* worker results */
    SDL_Surface *surface;
    bool missing;
    char genre[48];
    struct steamart_req_t *next;
} steamart_req_t;

static SDL_mutex *meta_lock = NULL;
static steamart_meta_t metas[STEAMART_MAX_META];
static int meta_count = 0;
static char *cache_root = NULL;
static steamart_tex_t *textures = NULL;
static steamart_req_t *pending = NULL;
static uint32_t use_clock = 0;
static const char *ca_bundle = NULL;

static const char *const builtin_names[] = {
        "Desktop", "Steam Big Picture", "Remote Input", "Remote Monitor", "Virtual Desktop", "Playnite", NULL,
};

/* ---------------------------------------------------------------- helpers */

static char *str_dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    memcpy(d, s, n);
    return d;
}

static unsigned long name_hash(const char *s) {
    unsigned long h = 5381;
    for (; *s; s++) { h = ((h << 5) + h) + (unsigned char) tolower((unsigned char) *s); }
    return h;
}

static void normalize(const char *in, char *out, size_t len) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < len; i++) {
        unsigned char c = (unsigned char) in[i];
        if (isalnum(c)) { out[j++] = (char) tolower(c); }
    }
    out[j] = '\0';
}

bool steamart_is_builtin(const char *app_name) {
    for (int i = 0; builtin_names[i]; i++) {
        if (SDL_strcasecmp(builtin_names[i], app_name) == 0) { return true; }
    }
    return false;
}

static void ensure_dir(const char *path) {
    mkdir(path, 0755);
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

/* ---------------------------------------------------------------- network (worker thread) */

typedef struct membuf_t {
    char *data;
    size_t len;
} membuf_t;

static size_t membuf_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
    membuf_t *buf = userdata;
    size_t n = size * nmemb;
    if (buf->len + n > 4 * 1024 * 1024) { return 0; }
    char *p = realloc(buf->data, buf->len + n + 1);
    if (!p) { return 0; }
    buf->data = p;
    memcpy(buf->data + buf->len, ptr, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return n;
}

static CURL *new_curl(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) { return NULL; }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DashGames/1.0");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (ca_bundle) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);
    } else {
        /* Public read-only artwork/metadata; no CA bundle on this platform */
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    return curl;
}

static bool http_get_mem(const char *url, membuf_t *out) {
    CURL *curl = new_curl(url);
    if (!curl) { return false; }
    out->data = NULL;
    out->len = 0;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, membuf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || status != 200) {
        commons_log_info(STEAMART_TAG, "GET %s failed: curl=%d http=%ld", url, rc, status);
        free(out->data);
        out->data = NULL;
        return false;
    }
    return out->data != NULL;
}

static bool http_get_file(const char *url, const char *path) {
    membuf_t buf;
    if (!http_get_mem(url, &buf)) { return false; }
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        free(buf.data);
        return false;
    }
    size_t w = fwrite(buf.data, 1, buf.len, fp);
    fclose(fp);
    free(buf.data);
    if (w != buf.len) {
        remove(tmp);
        return false;
    }
    return rename(tmp, path) == 0;
}

static void url_encode(const char *in, char *out, size_t len) {
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 4 < len; i++) {
        unsigned char c = (unsigned char) in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            out[j++] = (char) c;
        } else {
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 15];
        }
    }
    out[j] = '\0';
}

/* Minimal JSON helpers for the two small store API responses */
static const char *json_find_after(const char *s, const char *key) {
    const char *p = strstr(s, key);
    return p ? p + strlen(key) : NULL;
}

static void json_unescape_str(const char *p, char *out, size_t len) {
    size_t j = 0;
    for (; *p && *p != '"' && j + 1 < len; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == '/') { out[j++] = '/'; }
            else if (*p == 'u') { p += 4; } /* drop non-ASCII escapes */
            else { out[j++] = *p; }
        } else {
            out[j++] = *p;
        }
    }
    out[j] = '\0';
}

static int lookup_appid(const char *name) {
    char enc[512], url[1024];
    url_encode(name, enc, sizeof(enc));
    snprintf(url, sizeof(url), "https://store.steampowered.com/api/storesearch/?term=%s&l=english&cc=US", enc);
    membuf_t buf;
    if (!http_get_mem(url, &buf)) { return -1; }
    char want[256];
    normalize(name, want, sizeof(want));
    int best = 0;
    /* items: [{"type":"app","name":"...","id":123,...}, ...] */
    const char *p = buf.data;
    while ((p = strstr(p, "\"name\":\"")) != NULL) {
        char item_name[256], norm[256];
        json_unescape_str(p + 8, item_name, sizeof(item_name));
        normalize(item_name, norm, sizeof(norm));
        const char *idp = json_find_after(p, "\"id\":");
        if (!idp) { break; }
        int id = atoi(idp);
        if (norm[0] && (strcmp(norm, want) == 0 || strstr(want, norm) || strstr(norm, want))) {
            best = id;
            if (strcmp(norm, want) == 0) { break; }
        }
        p = idp;
    }
    free(buf.data);
    return best;
}

static void fetch_details(steamart_meta_t *meta) {
    char url[256];
    snprintf(url, sizeof(url), "https://store.steampowered.com/api/appdetails?appids=%d&filters=basic,genres&l=english",
             meta->appid);
    membuf_t buf;
    if (!http_get_mem(url, &buf)) { return; }
    const char *h = json_find_after(buf.data, "\"header_image\":\"");
    if (h) { json_unescape_str(h, meta->header_url, sizeof(meta->header_url)); }
    const char *g = json_find_after(buf.data, "\"genres\":[");
    if (g) {
        const char *d = json_find_after(g, "\"description\":\"");
        if (d) { json_unescape_str(d, meta->genre, sizeof(meta->genre)); }
    }
    free(buf.data);
}

/* Meta file: line1 appid, line2 genre, line3 header url */
static void meta_file_path(const char *name, char *out, size_t len) {
    snprintf(out, len, "%s/meta_%08lx.txt", cache_root, name_hash(name) & 0xffffffffUL);
}

static bool meta_load(steamart_meta_t *meta) {
    char path[4096];
    meta_file_path(meta->name, path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) { return false; }
    char line[600];
    if (fgets(line, sizeof(line), fp)) { meta->appid = atoi(line); }
    if (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = 0;
        snprintf(meta->genre, sizeof(meta->genre), "%s", line);
    }
    if (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = 0;
        snprintf(meta->header_url, sizeof(meta->header_url), "%s", line);
    }
    fclose(fp);
    return true;
}

static void meta_save(const steamart_meta_t *meta) {
    char path[4096];
    meta_file_path(meta->name, path, sizeof(path));
    FILE *fp = fopen(path, "w");
    if (!fp) { return; }
    fprintf(fp, "%d\n%s\n%s\n", meta->appid, meta->genre, meta->header_url);
    fclose(fp);
}

/* Returns a snapshot copy of the meta for `name`, resolving it if needed (worker thread). */
static steamart_meta_t meta_resolve(const char *name) {
    steamart_meta_t snap;
    memset(&snap, 0, sizeof(snap));
    SDL_LockMutex(meta_lock);
    steamart_meta_t *found = NULL;
    for (int i = 0; i < meta_count; i++) {
        if (strcmp(metas[i].name, name) == 0) {
            found = &metas[i];
            break;
        }
    }
    if (found && found->appid >= 0) {
        snap = *found;
        SDL_UnlockMutex(meta_lock);
        return snap;
    }
    SDL_UnlockMutex(meta_lock);

    steamart_meta_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.name = (char *) name;
    if (!meta_load(&fresh)) {
        fresh.appid = steamart_is_builtin(name) ? 0 : lookup_appid(name);
        if (fresh.appid > 0) { fetch_details(&fresh); }
        if (fresh.appid >= 0) { meta_save(&fresh); }
    }

    SDL_LockMutex(meta_lock);
    if (!found && meta_count < STEAMART_MAX_META) {
        found = &metas[meta_count++];
        memset(found, 0, sizeof(*found));
        found->name = str_dup(name);
    }
    if (found) {
        found->appid = fresh.appid;
        memcpy(found->genre, fresh.genre, sizeof(found->genre));
        memcpy(found->header_url, fresh.header_url, sizeof(found->header_url));
        snap = *found;
    }
    SDL_UnlockMutex(meta_lock);
    return snap;
}

static const char *kind_file(steamart_kind_t kind) {
    switch (kind) {
        case STEAMART_HERO: return "hero.jpg";
        case STEAMART_LOGO: return "logo.png";
        default: return "header.jpg";
    }
}

/* Bounding box of pixels with visible alpha (Steam logos often have big transparent margins). */
static SDL_Rect opaque_bounds(SDL_Surface *s) {
    SDL_Rect r = {0, 0, s->w, s->h};
    if (SDL_LockSurface(s) != 0) { return r; }
    int x0 = s->w, y0 = s->h, x1 = -1, y1 = -1;
    for (int y = 0; y < s->h; y++) {
        const Uint32 *row = (const Uint32 *) ((const Uint8 *) s->pixels + y * s->pitch);
        for (int x = 0; x < s->w; x++) {
            if ((row[x] >> 24) > 16) {
                if (x < x0) { x0 = x; }
                if (x > x1) { x1 = x; }
                if (y < y0) { y0 = y; }
                if (y > y1) { y1 = y; }
            }
        }
    }
    SDL_UnlockSurface(s);
    if (x1 >= x0 && y1 >= y0) {
        r.x = x0;
        r.y = y0;
        r.w = x1 - x0 + 1;
        r.h = y1 - y0 + 1;
    }
    return r;
}

static SDL_Surface *scale_surface(SDL_Surface *src, int box_w, int box_h, bool cover) {
    SDL_Surface *conv = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(src);
    if (!conv) { return NULL; }
    if (box_w <= 0 || box_h <= 0) { return conv; }
    SDL_Rect crop = {0, 0, conv->w, conv->h};
    if (!cover) { crop = opaque_bounds(conv); }
    int dw, dh;
    if (cover) {
        double box_ar = (double) box_w / box_h, src_ar = (double) conv->w / conv->h;
        if (src_ar > box_ar) {
            crop.w = (int) (conv->h * box_ar);
            crop.x = (conv->w - crop.w) / 2;
        } else {
            crop.h = (int) (conv->w / box_ar);
            crop.y = 0;
        }
        dw = box_w;
        dh = box_h;
    } else {
        double s = LV_MIN((double) box_w / crop.w, (double) box_h / crop.h);
        if (s > 1.0) { s = 1.0; }
        dw = LV_MAX(1, (int) (crop.w * s));
        dh = LV_MAX(1, (int) (crop.h * s));
    }
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, dw, dh, 32, SDL_PIXELFORMAT_ARGB8888);
    if (dst) {
        SDL_SetSurfaceBlendMode(conv, SDL_BLENDMODE_NONE);
        SDL_BlitScaled(conv, &crop, dst, NULL);
    }
    SDL_FreeSurface(conv);
    return dst;
}

static void req_complete_main(void *arg);

static int worker_main(void *arg) {
    steamart_req_t *req = arg;
    steamart_meta_t meta = meta_resolve(req->name);
    snprintf(req->genre, sizeof(req->genre), "%s", meta.genre);
    req->missing = true;
    if (meta.appid > 0) {
        char dir[4096], path[4096];
        snprintf(dir, sizeof(dir), "%s/%d", cache_root, meta.appid);
        ensure_dir(dir);
        snprintf(path, sizeof(path), "%s/%s", dir, kind_file(req->kind));
        char miss[4200];
        snprintf(miss, sizeof(miss), "%s.missing", path);
        if (!file_exists(path) && !file_exists(miss)) {
            char url[1024];
            const char *base = "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps";
            bool ok;
            if (req->kind == STEAMART_HEADER && meta.header_url[0]) {
                ok = http_get_file(meta.header_url, path);
            } else {
                snprintf(url, sizeof(url), "%s/%d/%s", base, meta.appid,
                         req->kind == STEAMART_HERO ? "library_hero.jpg" :
                         req->kind == STEAMART_LOGO ? "logo.png" : "header.jpg");
                ok = http_get_file(url, path);
            }
            if (!ok) {
                FILE *fp = fopen(miss, "w");
                if (fp) { fclose(fp); }
            }
        }
        if (file_exists(path)) {
            SDL_Surface *s = IMG_Load(path);
            if (s) {
                int bw = req->w, bh = req->h;
                bool cover = req->cover;
                if (bw <= 0 || bh <= 0) {
                    bw = req->kind == STEAMART_HERO ? 1920 : req->kind == STEAMART_LOGO ? 900 : 920;
                    bh = req->kind == STEAMART_HERO ? 620 : req->kind == STEAMART_LOGO ? 300 : 430;
                    cover = false;
                }
                req->surface = scale_surface(s, bw, bh, cover);
                req->missing = req->surface == NULL;
            }
        }
    }
    app_bus_post(global, req_complete_main, req);
    return 0;
}

/* ---------------------------------------------------------------- textures (UI thread) */

static void purge_draw_cache(const lv_img_dsc_t *dsc) {
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp) { return; }
    lv_draw_sdl_ctx_t *ctx = (lv_draw_sdl_ctx_t *) disp->driver->draw_ctx;
    struct __attribute__ ((__packed__)) {
        lv_draw_sdl_cache_key_head_img_t header;
        const void *src;
    } key;
    memset(&key, 0, sizeof(key));
    key.header.magic = LV_GPU_CACHE_KEY_MAGIC_IMG;
    key.header.type = LV_IMG_SRC_VARIABLE;
    key.header.frame_id = 0;
    key.src = dsc;
    lv_lru_remove(ctx->internals->texture_cache, &key, sizeof(key));
}

static void tex_free(steamart_tex_t *t) {
    if (!t->missing && t->data.data.texture) {
        purge_draw_cache(&t->dsc);
        SDL_DestroyTexture(t->data.data.texture);
    }
    free(t->name);
    free(t);
}

static steamart_tex_t *tex_find(const char *name, steamart_kind_t kind, int w, int h) {
    for (steamart_tex_t *t = textures; t; t = t->next) {
        if (t->kind == kind && t->w == w && t->h == h && strcmp(t->name, name) == 0) { return t; }
    }
    return NULL;
}

static void tex_evict_heroes(void) {
    int count = 0;
    for (steamart_tex_t *t = textures; t; t = t->next) {
        if (t->kind == STEAMART_HERO && !t->missing) { count++; }
    }
    while (count > STEAMART_HERO_CACHE) {
        steamart_tex_t *oldest = NULL, *oldest_prev = NULL, *prev = NULL;
        for (steamart_tex_t *t = textures; t; prev = t, t = t->next) {
            if (t->kind != STEAMART_HERO || t->missing) { continue; }
            if (!oldest || t->last_used < oldest->last_used) {
                oldest = t;
                oldest_prev = prev;
            }
        }
        if (!oldest) { break; }
        if (oldest_prev) { oldest_prev->next = oldest->next; } else { textures = oldest->next; }
        tex_free(oldest);
        count--;
    }
}

static steamart_tex_t *tex_insert(const char *name, steamart_kind_t kind, int w, int h, SDL_Surface *surface) {
    steamart_tex_t *t = calloc(1, sizeof(steamart_tex_t));
    t->name = str_dup(name);
    t->kind = kind;
    t->w = w;
    t->h = h;
    t->last_used = ++use_clock;
    if (surface) {
        lv_draw_sdl_drv_param_t *param = lv_disp_get_default()->driver->user_data;
        t->data.type = LV_SDL_IMG_TYPE_TEXTURE;
        t->data.data.texture = SDL_CreateTextureFromSurface(param->renderer, surface);
        t->dsc.header.cf = SDL_ISPIXELFORMAT_ALPHA(surface->format->format) ? LV_IMG_CF_TRUE_COLOR_ALPHA
                                                                            : LV_IMG_CF_TRUE_COLOR;
        t->dsc.header.w = surface->w;
        t->dsc.header.h = surface->h;
        t->dsc.data_size = sizeof(lv_sdl_img_data_t);
        t->dsc.data = (const uint8_t *) &t->data;
        t->missing = t->data.data.texture == NULL;
    } else {
        t->missing = true;
    }
    t->next = textures;
    textures = t;
    if (kind == STEAMART_HERO) { tex_evict_heroes(); }
    return t;
}

static void owner_deleted_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    for (steamart_req_t *r = pending; r; r = r->next) {
        if (r->owner == obj) { r->owner = NULL; }
    }
}

static void pending_remove(steamart_req_t *req) {
    steamart_req_t **pp = &pending;
    while (*pp) {
        if (*pp == req) {
            *pp = req->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static bool owner_has_pending(lv_obj_t *owner) {
    for (steamart_req_t *r = pending; r; r = r->next) {
        if (r->owner == owner) { return true; }
    }
    return false;
}

static void req_complete_main(void *arg) {
    steamart_req_t *req = arg;
    pending_remove(req);
    steamart_tex_t *t = tex_find(req->name, req->kind, req->w, req->h);
    if (!t) {
        t = tex_insert(req->name, req->kind, req->w, req->h, req->missing ? NULL : req->surface);
    }
    if (req->surface) { SDL_FreeSurface(req->surface); }
    if (req->owner) {
        if (!owner_has_pending(req->owner)) {
            lv_obj_remove_event_cb(req->owner, owner_deleted_cb);
        }
        if (!req->superseded) {
            t->last_used = ++use_clock;
            req->cb(req->owner, t->missing ? NULL : &t->dsc, req->genre[0] ? req->genre : NULL, req->userdata);
        }
    }
    free(req->name);
    free(req);
}

/* ---------------------------------------------------------------- public API */

void steamart_init(void) {
    if (meta_lock) { return; }
    meta_lock = SDL_CreateMutex();
    char *base = path_cache();
    size_t n = strlen(base) + 16;
    cache_root = malloc(n);
    snprintf(cache_root, n, "%s/steamart", base);
    free(base);
    ensure_dir(cache_root);
    static const char *const bundles[] = {
            "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem",
            "/usr/share/ca-certificates/ca-bundle.crt", NULL,
    };
    for (int i = 0; bundles[i]; i++) {
        if (file_exists(bundles[i])) {
            ca_bundle = bundles[i];
            break;
        }
    }
    commons_log_info(STEAMART_TAG, "cache=%s ca=%s", cache_root, ca_bundle ? ca_bundle : "(none)");
}

void steamart_deinit(void) {
    while (textures) {
        steamart_tex_t *next = textures->next;
        tex_free(textures);
        textures = next;
    }
}

void steamart_request(const char *app_name, steamart_kind_t kind, lv_obj_t *owner, steamart_cb_t cb,
                      void *userdata) {
    steamart_request_sized(app_name, kind, 0, 0, false, owner, cb, userdata);
}

void steamart_request_sized(const char *app_name, steamart_kind_t kind, lv_coord_t w, lv_coord_t h, bool cover,
                            lv_obj_t *owner, steamart_cb_t cb, void *userdata) {
    if (!meta_lock) { steamart_init(); }
    /* supersede older requests for the same owner and kind */
    for (steamart_req_t *r = pending; r; r = r->next) {
        if (r->owner == owner && r->kind == kind) { r->superseded = true; }
    }
    steamart_tex_t *t = tex_find(app_name, kind, w, h);
    if (t) {
        /* genre is needed by callers too; it's cheap to read from the meta table */
        char genre[48] = "";
        SDL_LockMutex(meta_lock);
        for (int i = 0; i < meta_count; i++) {
            if (strcmp(metas[i].name, app_name) == 0) {
                memcpy(genre, metas[i].genre, sizeof(genre));
                break;
            }
        }
        SDL_UnlockMutex(meta_lock);
        t->last_used = ++use_clock;
        cb(owner, t->missing ? NULL : &t->dsc, genre[0] ? genre : NULL, userdata);
        return;
    }
    steamart_req_t *req = calloc(1, sizeof(steamart_req_t));
    req->name = str_dup(app_name);
    req->kind = kind;
    req->w = w;
    req->h = h;
    req->cover = cover;
    req->owner = owner;
    req->cb = cb;
    req->userdata = userdata;
    if (!owner_has_pending(owner)) {
        lv_obj_add_event_cb(owner, owner_deleted_cb, LV_EVENT_DELETE, NULL);
    }
    req->next = pending;
    pending = req;
    SDL_Thread *th = SDL_CreateThread(worker_main, "steamart", req);
    if (th) {
        SDL_DetachThread(th);
    } else {
        pending_remove(req);
        free(req->name);
        free(req);
    }
}

/* ---------------------------------------------------------------- taglines */

typedef struct tagline_t {
    const char *key;
    const char *text;
} tagline_t;

/* Known games first (normalized name contains key) */
static const tagline_t known_taglines[] = {
        {"assettocorsa",     "Wracaj na odcinek."},
        {"crimsondesert",    "Pywel czeka na powrót."},
        {"davethediver",     "Pora zanurkować."},
        {"fallout",          "Wojna. Wojna nigdy się nie zmienia."},
        {"flightsimulator",  "Niebo jest dziś twoje."},
        {"projectzomboid",   "Tak umarłeś. Spróbuj jeszcze raz."},
        {"newstower",        "Wydanie nie napisze się samo."},
        {"valheim",          "Odyn patrzy."},
        {"minecraft",        "Jeszcze jeden blok."},
        {NULL,               NULL},
};

/* Steam genres (English descriptions) */
static const tagline_t genre_taglines[] = {
        {"Racing",                "Gaz do dechy."},
        {"Sports",                "Twoja kolej na boisku."},
        {"RPG",                   "Przygoda czeka."},
        {"Action",                "Akcja zaczyna się teraz."},
        {"Adventure",             "Przygoda czeka."},
        {"Strategy",              "Czas na wielki plan."},
        {"Simulation",            "Wszystko pod kontrolą."},
        {"Casual",                "Na chwilę relaksu."},
        {"Indie",                 "Mała gra, wielka frajda."},
        {"Massively Multiplayer", "Ekipa już czeka."},
        {NULL,                    NULL},
};

const char *steamart_tagline(const char *app_name, const char *genre) {
    char norm[256];
    normalize(app_name, norm, sizeof(norm));
    for (int i = 0; known_taglines[i].key; i++) {
        if (strstr(norm, known_taglines[i].key)) { return known_taglines[i].text; }
    }
    if (SDL_strcasecmp(app_name, "Desktop") == 0) { return "Twój pulpit na dużym ekranie."; }
    if (genre) {
        for (int i = 0; genre_taglines[i].key; i++) {
            if (SDL_strcasecmp(genre, genre_taglines[i].key) == 0) { return genre_taglines[i].text; }
        }
    }
    return "Gotowy do gry.";
}

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
    char genre[160];        /* all genres, comma separated (Polish names from the store API) */
    char header_url[512];
    char desc[400];         /* short description in Polish */
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
    char *cover_path; /* cover tile mode */
    lv_obj_t *owner;
    steamart_cb_t cb;
    void *userdata;
    bool superseded;
    /* worker results */
    SDL_Surface *surface;
    bool missing;
    char genre[160];
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
    snprintf(url, sizeof(url), "https://store.steampowered.com/api/appdetails?appids=%d&filters=basic,genres&l=polish",
             meta->appid);
    membuf_t buf;
    if (!http_get_mem(url, &buf)) { return; }
    const char *h = json_find_after(buf.data, "\"header_image\":\"");
    if (h) { json_unescape_str(h, meta->header_url, sizeof(meta->header_url)); }
    const char *sd = json_find_after(buf.data, "\"short_description\":\"");
    if (sd) { json_unescape_str(sd, meta->desc, sizeof(meta->desc)); }
    const char *g = json_find_after(buf.data, "\"genres\":[");
    meta->genre[0] = '\0';
    const char *end = g ? strchr(g, ']') : NULL;
    while (g && end) {
        const char *d = json_find_after(g, "\"description\":\"");
        if (!d || d > end) { break; }
        char one[48];
        json_unescape_str(d, one, sizeof(one));
        size_t len = strlen(meta->genre);
        snprintf(meta->genre + len, sizeof(meta->genre) - len, "%s%s", len ? "," : "", one);
        g = d;
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
    bool v2 = false;
    if (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = 0;
        v2 = strncmp(line, "v2:", 3) == 0;
        if (v2) { snprintf(meta->desc, sizeof(meta->desc), "%s", line + 3); }
    }
    fclose(fp);
    return v2 || meta->appid == 0;
}

static void meta_save(const steamart_meta_t *meta) {
    char path[4096];
    meta_file_path(meta->name, path, sizeof(path));
    FILE *fp = fopen(path, "w");
    if (!fp) { return; }
    fprintf(fp, "%d\n%s\n%s\nv2:%s\n", meta->appid, meta->genre, meta->header_url, meta->desc);
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
        memcpy(found->desc, fresh.desc, sizeof(found->desc));
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


/* ARGB8888 resample of `sr` in src into a w x h surface. Area-average for downscale, bilinear for upscale. */
static SDL_Surface *resample(SDL_Surface *src, SDL_Rect sr, int w, int h) {
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!dst || w <= 0 || h <= 0 || sr.w <= 0 || sr.h <= 0) { return dst; }
    SDL_LockSurface(src);
    SDL_LockSurface(dst);
    const Uint8 *sp = src->pixels;
    Uint8 *dp = dst->pixels;
    double fx = (double) sr.w / w, fy = (double) sr.h / h;
    for (int y = 0; y < h; y++) {
        Uint32 *drow = (Uint32 *) (dp + y * dst->pitch);
        for (int x = 0; x < w; x++) {
            double acc[4] = {0, 0, 0, 0}, wsum = 0;
            if (fx > 1.0 || fy > 1.0) {
                int x0 = sr.x + (int) (x * fx), x1 = sr.x + (int) ((x + 1) * fx);
                int y0 = sr.y + (int) (y * fy), y1 = sr.y + (int) ((y + 1) * fy);
                if (x1 <= x0) { x1 = x0 + 1; }
                if (y1 <= y0) { y1 = y0 + 1; }
                for (int yy = y0; yy < y1 && yy < src->h; yy++) {
                    const Uint32 *srow = (const Uint32 *) (sp + yy * src->pitch);
                    for (int xx = x0; xx < x1 && xx < src->w; xx++) {
                        Uint32 c = srow[xx];
                        double a = (c >> 24) & 0xff;
                        acc[0] += a;
                        acc[1] += ((c >> 16) & 0xff) * a;
                        acc[2] += ((c >> 8) & 0xff) * a;
                        acc[3] += (c & 0xff) * a;
                        wsum += 1;
                    }
                }
            } else {
                double sx = sr.x + (x + 0.5) * fx - 0.5, sy = sr.y + (y + 0.5) * fy - 0.5;
                int ix = (int) sx, iy = (int) sy;
                double ax = sx - ix, ay = sy - iy;
                for (int k = 0; k < 4; k++) {
                    int xx = LV_CLAMP(0, ix + (k & 1), src->w - 1), yy = LV_CLAMP(0, iy + (k >> 1), src->h - 1);
                    double wt = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
                    Uint32 c = ((const Uint32 *) (sp + yy * src->pitch))[xx];
                    double a = ((c >> 24) & 0xff) * wt;
                    acc[0] += a;
                    acc[1] += ((c >> 16) & 0xff) * a;
                    acc[2] += ((c >> 8) & 0xff) * a;
                    acc[3] += (c & 0xff) * a;
                    wsum += wt;
                }
            }
            Uint32 out = 0;
            if (acc[0] > 0) {
                Uint32 a = (Uint32) (acc[0] / wsum + 0.5);
                Uint32 r = (Uint32) (acc[1] / acc[0] + 0.5), g = (Uint32) (acc[2] / acc[0] + 0.5),
                        bl = (Uint32) (acc[3] / acc[0] + 0.5);
                out = (LV_MIN(a, 255u) << 24) | (LV_MIN(r, 255u) << 16) | (LV_MIN(g, 255u) << 8) | LV_MIN(bl, 255u);
            }
            drow[x] = out;
        }
    }
    SDL_UnlockSurface(dst);
    SDL_UnlockSurface(src);
    return dst;
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
    SDL_Surface *dst = resample(conv, crop, dw, dh);
    SDL_FreeSurface(conv);
    return dst;
}

/* Cover tile: blurred + darkened full-bleed background, whole cover fitted in the middle. */
static SDL_Surface *compose_cover_tile(SDL_Surface *src, int w, int h) {
    SDL_Surface *conv = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(src);
    if (!conv) { return NULL; }
    /* background: crop to the tile aspect, shrink a lot (cheap blur), enlarge back bilinearly */
    double box_ar = (double) w / h, src_ar = (double) conv->w / conv->h;
    SDL_Rect crop = {0, 0, conv->w, conv->h};
    if (src_ar > box_ar) {
        crop.w = (int) (conv->h * box_ar);
        crop.x = (conv->w - crop.w) / 2;
    } else {
        crop.h = (int) (conv->w / box_ar);
        crop.y = (conv->h - crop.h) / 2;
    }
    SDL_Surface *tiny = resample(conv, crop, LV_MAX(4, w / 28), LV_MAX(3, h / 28));
    SDL_Rect all_tiny = {0, 0, tiny ? tiny->w : 0, tiny ? tiny->h : 0};
    SDL_Surface *dst = tiny ? resample(tiny, all_tiny, w, h) : NULL;
    if (tiny) { SDL_FreeSurface(tiny); }
    if (!dst) {
        SDL_FreeSurface(conv);
        return NULL;
    }
    /* darken + force opaque */
    SDL_LockSurface(dst);
    for (int y = 0; y < h; y++) {
        Uint32 *row = (Uint32 *) ((Uint8 *) dst->pixels + y * dst->pitch);
        for (int x = 0; x < w; x++) {
            Uint32 c = row[x];
            Uint32 r = ((c >> 16) & 0xff) * 45 / 100, g = ((c >> 8) & 0xff) * 45 / 100, b = (c & 0xff) * 45 / 100;
            row[x] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
    }
    SDL_UnlockSurface(dst);
    /* foreground: whole cover, fitted to 86% of the tile height */
    int fh = h * 86 / 100, fw = (int) ((double) conv->w * fh / conv->h);
    if (fw > w * 92 / 100) {
        fw = w * 92 / 100;
        fh = (int) ((double) conv->h * fw / conv->w);
    }
    SDL_Rect all = {0, 0, conv->w, conv->h};
    SDL_Surface *fg = resample(conv, all, fw, fh);
    SDL_FreeSurface(conv);
    if (fg) {
        SDL_SetSurfaceBlendMode(fg, SDL_BLENDMODE_BLEND);
        SDL_Rect at = {(w - fw) / 2, (h - fh) / 2, fw, fh};
        SDL_BlitSurface(fg, NULL, dst, &at);
        SDL_FreeSurface(fg);
    }
    return dst;
}

static void req_complete_main(void *arg);

static int worker_main(void *arg) {
    steamart_req_t *req = arg;
    if (req->cover_path) {
        req->missing = true;
        for (int i = 0; i < 20 && !file_exists(req->cover_path); i++) { SDL_Delay(500); }
        SDL_Surface *s = file_exists(req->cover_path) ? IMG_Load(req->cover_path) : NULL;
        if (s) {
            req->surface = compose_cover_tile(s, req->w, req->h);
            req->missing = req->surface == NULL;
        }
        app_bus_post(global, req_complete_main, req);
        return 0;
    }
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
    if (t && t->missing && req->cover_path && !req->missing) { t = NULL; }
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
    free(req->cover_path);
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
        char genre[160] = "";
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

void steamart_request_cover_tile(const char *cover_path, lv_coord_t w, lv_coord_t h, lv_obj_t *owner,
                                 steamart_cb_t cb, void *userdata) {
    if (!meta_lock) { steamart_init(); }
    char key[600];
    snprintf(key, sizeof(key), "cover:%s", cover_path);
    for (steamart_req_t *r = pending; r; r = r->next) {
        if (r->owner == owner && r->kind == STEAMART_HEADER) { r->superseded = true; }
    }
    steamart_tex_t *t = tex_find(key, STEAMART_HEADER, w, h);
    if (t && !t->missing) {
        t->last_used = ++use_clock;
        cb(owner, &t->dsc, NULL, userdata);
        return;
    }
    steamart_req_t *req = calloc(1, sizeof(steamart_req_t));
    req->name = str_dup(key);
    req->cover_path = str_dup(cover_path);
    req->kind = STEAMART_HEADER;
    req->w = w;
    req->h = h;
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
        free(req->cover_path);
        free(req->name);
        free(req);
    }
}

bool steamart_get_desc(const char *app_name, char *out, size_t len) {
    out[0] = '\0';
    if (!meta_lock) { return false; }
    SDL_LockMutex(meta_lock);
    for (int i = 0; i < meta_count; i++) {
        if (strcmp(metas[i].name, app_name) == 0) {
            snprintf(out, len, "%s", metas[i].desc);
            break;
        }
    }
    SDL_UnlockMutex(meta_lock);
    return out[0] != '\0';
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

/* Genre taglines in priority order (a racing game that is also "Action" gets a racing line). */
typedef struct genre_lines_t {
    const char *pl, *en;
    const char *lines[3];
} genre_lines_t;

static const genre_lines_t genre_lines[] = {
        {"Wy\u015bcigowe",      "Racing",                {"Gaz do dechy.", "Na start, gotowi, jazda.", "Pora na szybkie okr\u0105\u017cenie."}},
        {"Sportowe",           "Sports",                {"Twoja kolej na boisku.", "Mecz si\u0119 zaczyna.", "Wejd\u017a do gry."}},
        {"Symulacje",          "Simulation",            {"Wszystko pod kontrol\u0105.", "Twoje zasady, tw\u00f3j \u015bwiat.", "Jeszcze tylko jedna zmiana."}},
        {"Strategie",          "Strategy",              {"Czas na wielki plan.", "Ka\u017cdy ruch ma znaczenie.", "Imperium samo si\u0119 nie zbuduje."}},
        {"RPG",                "RPG",                   {"Przygoda czeka.", "Twoja historia trwa.", "Wr\u00f3\u0107 do swojej dru\u017cyny."}},
        {"MMO",                "Massively Multiplayer", {"Ekipa ju\u017c czeka.", "Serwer czeka na ciebie.", "Do\u0142\u0105cz do reszty."}},
        {"Przygodowe",         "Adventure",             {"Przygoda czeka.", "Co jest za nast\u0119pnym zakr\u0119tem?", "Ruszaj dalej."}},
        {"Akcja",              "Action",                {"Akcja zaczyna si\u0119 teraz.", "Czas na rozgrywk\u0119.", "Pe\u0142na moc."}},
        {"Swobodne",           "Casual",                {"Na chwil\u0119 relaksu.", "Spokojna partyjka.", "Odpocznij przy grze."}},
        {"Niezale\u017cne",     "Indie",                 {"Ma\u0142a gra, wielka frajda.", "Odkryj co\u015b nowego.", "Ma\u0142a gra, wielka przygoda."}},
        {NULL,                 NULL,                    {NULL, NULL, NULL}},
};

static unsigned long tag_hash(const char *s) {
    unsigned long h = 5381;
    for (; *s; s++) { h = ((h << 5) + h) + (unsigned char) *s; }
    return h;
}

const char *steamart_tagline(const char *app_name, const char *genre) {
    char norm[256];
    normalize(app_name, norm, sizeof(norm));
    for (int i = 0; known_taglines[i].key; i++) {
        if (strstr(norm, known_taglines[i].key)) { return known_taglines[i].text; }
    }
    if (SDL_strcasecmp(app_name, "Desktop") == 0) { return "Twój pulpit na dużym ekranie."; }
    if (genre && genre[0]) {
        for (int i = 0; genre_lines[i].pl; i++) {
            if (strstr(genre, genre_lines[i].pl) || strstr(genre, genre_lines[i].en)) {
                return genre_lines[i].lines[tag_hash(app_name) % 3];
            }
        }
    }
    return "Gotowy do gry.";
}

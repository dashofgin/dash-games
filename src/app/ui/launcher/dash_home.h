#pragma once

/*
 * Dash Games home screen (Apple TV inspired): full-bleed game art, top tabs, shelf with logo/tagline and
 * Play / Continue actions, quick actions row and a horizontal row of game tiles (the existing gridview
 * configured as a single row inside a horizontal scroller).
 */

#include "lvgl.h"
#include "backend/apploader/apploader.h"

typedef struct apps_fragment_t apps_fragment_t;

/** Builds the layout inside `view` and creates controller->applist (the games row). */
void dash_home_create(apps_fragment_t *controller, lv_obj_t *view);

void dash_home_destroy(apps_fragment_t *controller);

/** Rebuild visible tile list for the current tab. Call before handing `list` to the gridview. */
void dash_home_rebuild(apps_fragment_t *controller, const apploader_list_t *list);

int dash_home_visible_count(apps_fragment_t *controller);

/** Maps a gridview position to an index into the apploader list (-1 if out of range). */
int dash_home_visible_index(apps_fragment_t *controller, int position);

/** Tile size for the games row. */
void dash_home_tile_size(apps_fragment_t *controller, lv_coord_t *w, lv_coord_t *h);

/** Called when a tile gets focus (or with 0 to refresh the shelf for the running game / first game). */
void dash_home_focus_app(apps_fragment_t *controller, int app_id);

/** Called when a tile is bound to an app: loads the landscape Steam header into the tile. */
void dash_home_bind_tile(apps_fragment_t *controller, lv_obj_t *item, const apploader_item_t *app);

/** Launch (or resume) an app; asks for confirmation if a different app is running on the PC. */
void dash_home_launch(apps_fragment_t *controller, const apploader_item_t *app);

/** Remember that an app was launched (for the "Ostatnie" tab). */
void dash_home_record_launch(apps_fragment_t *controller, const apploader_item_t *app);

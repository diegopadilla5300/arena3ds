/* Images by URL: fetched and decoded on worker threads, turned into
 * textures on the main thread, kept in a small least-recently-used cache. */
#pragma once

#include <citro2d.h>
#include <stdbool.h>

void img_start(void);
bool img_stop(void);

/* The image for url if it is ready; otherwise asks for it and returns NULL.
 * w and h, when not NULL, get its size in pixels. Call every frame the
 * image is on screen: what goes unasked for a while is let go. */
const C2D_Image *img_get(const char *url, int *w, int *h);

/* True once url has failed to load, so the caller can stop waiting. */
bool img_failed(const char *url);

/* Main thread, once a frame: uploads what the workers finished. */
void img_update(void);

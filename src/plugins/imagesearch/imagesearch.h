#pragma once
#include <gdk/gdk.h>
#include <glib.h>

/* Reverse image search: take an image sitting on the clipboard, upload it to
 * Google Lens, and hand back the results URL to open in a tab. */

/* Called once the upload finishes. `uri` is the results page on success and
 * NULL on failure, in which case `error` is a short human-readable reason. */
typedef void (*ImageSearchDone)(const char* uri, const char* error, gpointer user_data);

/* TRUE when the clipboard is offering an image and *not* plain text -- i.e. a
 * paste that has nothing to type into an entry. Synchronous (it only inspects
 * the advertised formats), so it's safe to call from a key handler. */
gboolean imagesearch_clipboard_has_image(GdkClipboard* clipboard);

/* Read the clipboard image, upload it, and invoke `done` on the main loop.
 * `done` runs exactly once, success or failure. */
void imagesearch_from_clipboard(GdkClipboard* clipboard, ImageSearchDone done, gpointer user_data);

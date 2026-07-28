#pragma once
#include <gdk/gdk.h>
#include <glib.h>
#include <webkit/webkit.h>

/* Reverse image search: take an image sitting on the clipboard and hand it to
 * Google Lens in a tab. */

/* Called instead of navigating when the clipboard image can't be used at all;
 * `reason` is a short human-readable message. */
typedef void (*ImageSearchFailed)(const char* reason);

/* TRUE when the clipboard is offering an image and *not* plain text -- i.e. a
 * paste that has nothing to type into an entry. Synchronous (it only inspects
 * the advertised formats), so it's safe to call from a key handler. */
gboolean imagesearch_clipboard_has_image(GdkClipboard* clipboard);

/* Reverse-search the clipboard's image in `view`. Returns straight away: the
 * view first loads a Lens page, then submits the image from inside it and lands
 * on the results. `on_error` runs only for a clipboard/encoding failure -- once
 * the navigation is under way, any further trouble is the page's to report. */
void imagesearch_run(GdkClipboard* clipboard, WebKitWebView* view, ImageSearchFailed on_error);

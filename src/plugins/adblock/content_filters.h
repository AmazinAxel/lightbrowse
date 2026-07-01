#ifndef LIGHTBROWSE_ADBLOCK_CONTENT_FILTERS_H
#define LIGHTBROWSE_ADBLOCK_CONTENT_FILTERS_H

#include <webkit/webkit.h>

/* Ad blocking via WebKit's native content-blocker engine.
 *
 * The browser ships combined-part*.json (WebKit content-blocker rules, generated
 * at build time from EasyList/uBO lists by the ublock-webkit-filters converter).
 * This module compiles them once through a WebKitUserContentFilterStore (WebKit
 * caches the compiled bytecode on disk) and adds the resulting filters to each
 * tab's WebKitUserContentManager. Matching then happens inside WebKit — no
 * per-request work in our code.
 *
 *   filters_dir : dir holding combined-part*.json + a `version` marker file
 *   store_dir   : writable dir for WebKit's compiled-filter cache
 *
 * Compilation is async; requests before it finishes pass through unfiltered.
 * Views created before filters are ready are back-filled once compilation lands. */
void adblock_content_init(const char* filters_dir, const char* store_dir);

/* Register a view for ad blocking: adds every ready filter to its content
 * manager now, and arranges for filters still compiling to be added when ready.
 * Safe to call before adblock_content_init (no-op until then). */
void adblock_apply_to_view(WebKitWebView* view);

#endif

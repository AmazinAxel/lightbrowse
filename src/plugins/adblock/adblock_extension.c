/*
 * Lightbrowse Adblock Web Extension
 *
 * This is a WebKit web extension that runs in the web process and
 * intercepts HTTP requests to block ads using the combined filter list.
 *
 * License: GPL-3.0
 */

#include <webkit/webkit-web-process-extension.h>
#include <string.h>
#include "uri_tester.h"

/* Configuration - must match ADBLOCK_FILTERLIST_PATH in config.h.
 * Overridden at build time via -DADBLOCK_FILTERLIST_PATH=... */
#ifndef ADBLOCK_FILTERLIST_PATH
#define ADBLOCK_FILTERLIST_PATH "/opt/lightbrowse/filterlist.txt"
#endif

static AdblockUriTester* tester = NULL;
static gboolean adblock_enabled = TRUE;

/* Microsoft Rewards credits searches via attribution/tracking requests (e.g. to
 * bat.bing.com) that EasyPrivacy would otherwise block, so points never register
 * unless you click links from the Rewards sidebar. Whitelist the Rewards + Bing
 * attribution hosts: any request TO one of these, or any request made while one of
 * these is the page, bypasses the filter entirely. Add hosts here as needed. */
static const char* const WHITELIST_DOMAINS[] = {
    "rewards.bing.com",
    "rewards.microsoft.com",
    "bat.bing.com",
    "c.bing.com",
};

/* TRUE if uri's host equals, or is a subdomain of, a whitelisted domain. */
static gboolean
uri_host_whitelisted(const char* uri)
{
    if (!uri)
        return FALSE;
    const char* host = strstr(uri, "://");
    if (!host)
        return FALSE;
    host += 3;
    gsize host_len = strcspn(host, "/?#"); /* host ends at the path/query/fragment */
    for (gsize i = 0; i < G_N_ELEMENTS(WHITELIST_DOMAINS); i++) {
        gsize dlen = strlen(WHITELIST_DOMAINS[i]);
        if (host_len < dlen)
            continue;
        const char* tail = host + (host_len - dlen); /* match against the end of the host */
        if (strncmp(tail, WHITELIST_DOMAINS[i], dlen) == 0
            && (host_len == dlen || tail[-1] == '.')) /* exact host or a .subdomain boundary */
            return TRUE;
    }
    return FALSE;
}

/*
 * Called for each resource request (images, scripts, stylesheets, etc.)
 * Returns TRUE to block the request, FALSE to allow it.
 */
static gboolean
on_send_request(WebKitWebPage* page,
    WebKitURIRequest* request,
    WebKitURIResponse* redirect_response G_GNUC_UNUSED,
    gpointer user_data G_GNUC_UNUSED)
{
    if (!adblock_enabled || !tester)
        return FALSE;

    const char* request_uri = webkit_uri_request_get_uri(request);
    const char* page_uri = webkit_web_page_get_uri(page);

    /* Don't block the main page itself */
    if (g_strcmp0(request_uri, page_uri) == 0)
        return FALSE;

    /* Skip non-HTTP(S) requests */
    if (!g_str_has_prefix(request_uri, "http://") &&
        !g_str_has_prefix(request_uri, "https://"))
        return FALSE;

    /* Never filter Microsoft Rewards / Bing attribution requests (see above). */
    if (uri_host_whitelisted(request_uri) || uri_host_whitelisted(page_uri))
        return FALSE;

    /* Check if this URI should be blocked */
    if (adblock_uri_tester_test_uri(tester, request_uri, page_uri)) {
        g_debug("[lightbrowse-adblock] Blocked: %s", request_uri);
        return TRUE; /* Block the request */
    }

    return FALSE; /* Allow the request */
}

/* Lines compiled per idle tick. The combined EasyList is ~137k rules and each
 * rule compiles to a GRegex, which is far too much to do in one go on the
 * web-process main thread — that thread also parses, lays out and PAINTS the
 * page, so a single bulk compile freezes the first navigation on a white screen
 * for seconds. Compiling a small batch per idle tick yields back to the run loop
 * between batches, so rendering proceeds and the load just gets filtered a beat
 * later. Keep this small enough that one batch is well under a frame. */
#define ADBLOCK_LOAD_BATCH 800

/*
 * Compile the filter list incrementally, off the web-process startup path and
 * without ever blocking the main thread for long. Requests that arrive before it
 * finishes simply pass through unfiltered (adblock_uri_tester_test_uri returns
 * FALSE until the tester is fully loaded).
 */
static gboolean
load_filters_idle(gpointer user_data G_GNUC_UNUSED)
{
    if (adblock_uri_tester_load_step(tester, ADBLOCK_LOAD_BATCH))
        return G_SOURCE_CONTINUE; /* more rules remain; compile the next batch */
    g_debug("[lightbrowse-adblock] Loaded filters from %s", ADBLOCK_FILTERLIST_PATH);
    return G_SOURCE_REMOVE;
}

/*
 * Called when a new web page is created.
 * We connect to the send-request signal to intercept all resource requests.
 */
static void
on_page_created(WebKitWebProcessExtension* extension,
    WebKitWebPage* page,
    gpointer user_data)
{
    (void)extension;
    (void)user_data;
    g_signal_connect(page, "send-request", G_CALLBACK(on_send_request), NULL);
}

/*
 * Extension initialization - called by WebKit when loading the extension.
 * 
 * The user_data variant can contain configuration from the main browser process:
 * - "enabled": gboolean to enable/disable adblock
 */
G_MODULE_EXPORT void
webkit_web_process_extension_initialize_with_user_data(WebKitWebProcessExtension* extension,
    GVariant* user_data)
{
    /* Parse configuration from user data */
    if (user_data && g_variant_is_of_type(user_data, G_VARIANT_TYPE_VARDICT)) {
        GVariant* enabled_var = g_variant_lookup_value(user_data, "enabled", G_VARIANT_TYPE_BOOLEAN);
        if (enabled_var) {
            adblock_enabled = g_variant_get_boolean(enabled_var);
            g_variant_unref(enabled_var);
        }
    }

    if (!adblock_enabled) {
        g_debug("[lightbrowse-adblock] Adblock disabled by configuration");
        return;
    }

    /* Check if filter file exists */
    if (!g_file_test(ADBLOCK_FILTERLIST_PATH, G_FILE_TEST_EXISTS)) {
        g_warning("[lightbrowse-adblock] Filter file not found: %s", ADBLOCK_FILTERLIST_PATH);
        g_warning("[lightbrowse-adblock] Adblock disabled (the Nix package builds this list automatically)");
        return;
    }

    /* Create the tester now but compile the filters at the next idle, so the
     * first page load isn't blocked behind the (large) list compile. */
    tester = adblock_uri_tester_new(ADBLOCK_FILTERLIST_PATH);
    /* Low priority so the in-flight page load always wins the main thread. */
    g_idle_add_full(G_PRIORITY_LOW, load_filters_idle, NULL, NULL);

    /* Connect to page-created to hook into each new page */
    g_signal_connect(extension, "page-created", G_CALLBACK(on_page_created), NULL);
}

/*
 * Fallback initialization without user data
 */
G_MODULE_EXPORT void
webkit_web_process_extension_initialize(WebKitWebProcessExtension* extension)
{
    webkit_web_process_extension_initialize_with_user_data(extension, NULL);
}

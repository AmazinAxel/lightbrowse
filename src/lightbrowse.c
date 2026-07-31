#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <webkit/webkit.h>

#include "config.h"
#include "plugins/plugins.h"

/* The chrome is hardcoded to Graphite-nord-dark colours so the UI is always dark,
 * independent of the system light/dark setting. Nord palette: bg #2E3440, a lighter
 * surface #3B4252, fg #ECEFF4, accent #5E81AC. The loading canvas is pure black
 * (see apply_view_background) so a navigation flash is never a bright white. */
static const char* CSS =
    /* Root: pin the window dark so the GTK theme's light bg never shows in any gap. */
    "window { background: #2E3440; color: #ECEFF4; }"
    ".webarea, .webarea > stack { background: #2E3440; }"
    ".tabbar { background: #3B4252; border-right: 0.25rem solid #5e81ac; padding: 4px; }"
    ".tab { padding: 4px; border: 0.2rem solid #4C566A; border-radius: 4px; background: #3B4252; color: #ECEFF4; outline: none; box-shadow: none; transition: border-color .1s ease; }"
    ".tab.active { border-color: #5e81ac; }"
    ".tab image { color: #ECEFF4; }" /* recolour the symbolic globe placeholder white (real favicons are untouched) */
    ".tab.asleep { opacity: 0.5; }" /* slept tab: web process freed, reopen to reload */
    ".dim { background: alpha(black, 0.3); }"
    ".modal { background: #2E3440; color: #ECEFF4; border: 2px solid #5e81ac; border-radius: 12px; padding: 12px; }"
    ".modal entry { min-width: 280px; background: #3B4252; color: #ECEFF4; border: 1px solid #4C566A; caret-color: #ECEFF4; }"
    ".modal entry selection { background: #81A1C1; color: #2E3440; }"
    ".modal label { padding: 8px; border: 1px solid #4C566A; background-color: #3B4252; border-radius: 6px; transition: all 120ms ease; }"
    ".modal .selected { background: #81A1C1; color: #ECEFF4; border-color: #D8DEE9; transform: translateY(-2px) scale(1.01); outline: 2px solid #81A1C1; outline-offset: 1px; }"
    /* Calculator result: a plain label under the search box, not a result row. */
    ".modal .calc { padding: 4px 0 0 4px; border: none; background: none; color: #5e81ac; font-weight: bold; font-size: 1.3rem; }"

    ".findbar { background: #2E3440; color: #ECEFF4; border: 1px solid #5e81ac; border-radius: 8px; padding: 4px; margin-bottom: 12px; outline: 2px solid #5E81AC; }"
    ".findbar entry { background: #3B4252; color: #ECEFF4; border: 1px solid #4C566A; caret-color: #ECEFF4; }"
    ".findbar entry selection { background: #81A1C1; color: #2E3440; }"
    ".findbar label { color: #ECEFF4; }"

    ".statusbar label { color: #ECEFF4; font-size: 0.85em; padding: 2px 4px; background: #3B4252; border-top-right-radius: 6px; }"
    "progressbar.loadbar trough { border: none; background: transparent; border-radius: 0; padding: 0; min-height: 3px; }"
    "progressbar.loadbar progress { border: none; border-radius: 0; background: #5e81ac; }"
    "progressbar.downloadbar trough { border: none; background: transparent; border-radius: 0; padding: 0; min-height: 3px; }"
    "progressbar.downloadbar progress { border: none; border-radius: 0; background: #a3be8c; }";

/* Everything below belongs to one browser window. The process can own several:
 * the main window (whose tabs are the saved session) plus any number of scratch
 * windows, which start empty and are never written to the session. They all share
 * one WebKit engine, network session and ad-block filter set, so a second window
 * costs a fraction of a second process — and opens in the time it takes to map it.
 *
 * User-driven code (keys, the modal, find) acts on cur(): the window the user is
 * focused on. Anything driven by a web view instead resolves its own window with
 * win_of(view), so a page loading in a background window can't repaint the chrome
 * of the one you're looking at. */
typedef enum { MODAL_NONE, MODAL_SEARCH, MODAL_BOOKMARK, MODAL_PASSWORD, MODAL_PERMISSION } ModalMode;

typedef struct {
    gboolean is_main; /* the session-backed window; scratch windows are throwaway */

    GtkWindow* window;
    GtkOverlay* overlay;
    GtkNotebook* notebook;
    GtkBox* tabbar; /* vertical favicon strip */
    gboolean tabbar_visible;
    int num_tabs;

    /* Most-recently-used tab history (alt+tab walks back through it). The front
     * (index 0) is the current tab, except during an active alt+tab walk: then the
     * displayed tab is mru[alt_walk] and the list is only reshuffled once the user
     * releases Alt, so repeated Tab presses keep stepping deeper into history. */
    GtkWidget* mru[MRU_HISTORY];
    int mru_len;
    int alt_walk;           /* index into mru during a walk; -1 when idle */
    gboolean alt_switch;    /* TRUE while the walk drives the page switch itself */

    /* Modal (search / bookmark / password picker) */
    ModalMode modal_mode;
    gboolean modal_new_tab; /* search: open in a new tab vs current */
    gboolean modal_blocked; /* tab limit reached: don't open on submit */
    GtkWidget* dim;
    GtkWidget* modal_box;
    GtkLabel* modal_info;
    GtkEntry* modal_entry1; /* search text / bookmark name */
    GtkEntry* modal_entry2; /* bookmark url (hidden in search mode) */
    GtkWidget* modal_focus; /* the modal entry the keyboard belongs to (see modal_keep_focus) */
    GtkBox* modal_results;
    GtkLabel* calc_label;   /* search: live calculation result, shown under the entry */
    gboolean calc_active;   /* a valid calculation is currently displayed */
    char calc_result[64];   /* its formatted value, for the clipboard */
    const char* fuzzy_urls[FUZZY_RESULTS];
    guint fuzzy_count;
    int fuzzy_sel;

    /* Password picker (MODAL_PASSWORD): pass_entries mirrors fuzzy_urls but holds
     * `pass` entry paths; pass_host is the current page's host we match against;
     * pass_target is the view to inject the filled credentials into. */
    const char* pass_entries[FUZZY_RESULTS];
    char pass_host[256];
    WebKitWebView* pass_target;

    /* Find bar */
    GtkWidget* findbar;
    GtkEntry* find_entry;
    GtkLabel* find_label;
    guint find_total;
    guint find_current;

    /* Bottom status / loading bar (the whole bar hides when idle) */
    GtkWidget* statusbar;   /* container: hidden unless loading or hovering */
    GtkLabel* status_label; /* hovered or keyboard-focused link */
    GtkProgressBar* progress;          /* page load progress (hidden when idle) */
    GtkProgressBar* download_progress; /* green download progress, below the load bar */
    char* status_link;      /* hovered or keyboard-focused link URI */
    char* status_flash;     /* transient message (e.g. "Download started") */
    guint status_flash_source;
    gboolean page_loading;
} Win;

static Win* main_win = NULL;    /* the session-backed window; NULL until it is built */
static GPtrArray* wins = NULL;  /* every live Win, main first */

/* Permission prompts (camera/microphone getUserMedia + Storage Access API).
 * WebKitGTK denies every permission request unless the app allows it explicitly,
 * so without this a page (Zoom, Meet, ...) silently gets no mic/camera and its
 * whole call collapses. Each request is shown in the shared modal (Enter allows,
 * Esc denies); a grant is remembered for the browser session only (perm_allowed,
 * an in-memory set gone on quit) so repeating it within the session doesn't
 * re-prompt. Requests are keyed by host (media) or requesting|current domain
 * (storage) in "perm-key"; a NULL key is never remembered. */
static WebKitPermissionRequest* perm_current = NULL; /* request awaiting a decision, or NULL */
static GQueue perm_queue = G_QUEUE_INIT;             /* further requests, shown one at a time */
static GHashTable* perm_allowed = NULL;              /* set of perm-keys granted this session */

static int active_downloads = 0;

/* Closed-tab ring (full WebKit session state, so the webview is freed) */
static WebKitWebViewSessionState* closed_tabs[CLOSED_TAB_HISTORY];
static int closed_count = 0;

/* System color scheme watcher (the chrome stays dark; only websites follow it). */
static GSettings* iface_settings = NULL;

/* Resident mode (--prewarm): closing the window hides it and frees the tabs
 * instead of quitting, so the process stays warm for the next launch. A plain
 * launch keeps the old behaviour and exits when its window closes. */
static gboolean resident = FALSE;
static gboolean tearing_down = FALSE; /* tabs are being dropped; ignore the churn */

/* Forward declarations */
static void notebook_create_new_tab(Win* w, const char* uri);
static void session_save_queue(void);
static void window_hide_to_resident(Win* w);
static void window_destroy_scratch(Win* w);
static WebKitWebView* current_view(Win* w);
static void do_find(Win* w, const char* text);
static void session_save(void);
static void update_status(Win* w);
static void on_download_started(WebKitNetworkSession* session, WebKitDownload* download, gpointer data);
static void mru_promote(Win* w, GtkWidget* page);
static void mru_remove(Win* w, GtkWidget* page);
static void tab_set_asleep(WebKitWebView* view, gboolean asleep);
static void modal_show_permission(Win* w, const char* markup);
static void modal_hide(Win* w);
static void modal_hide_for_new_tab(Win* w);

/* The window a web view lives in (stashed on the view when its tab is built). */
static Win* win_of(WebKitWebView* view)
{
    return view ? g_object_get_data(G_OBJECT(view), "win") : NULL;
}

/* The window the user is acting on: GTK tracks which of our windows has focus,
 * and everything keyboard- or modal-driven follows it. Falls back to the main
 * window, which is the only one that exists before anything is focused. */
static Win* cur(void)
{
    GtkWindow* active = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    Win* w = active ? g_object_get_data(G_OBJECT(active), "win") : NULL;
    return w ? w : main_win;
}

/* ---------------------------------------------------------------- URI load */
/* Does `s` carry an explicit URI scheme we should navigate to as-is? Accepts a
 * valid RFC-3986 scheme ("[a-z][a-z0-9+.-]*:") in two forms: "scheme://…" (any
 * scheme, so custom app URLs like myapp:// navigate — the decide-policy handler
 * then hands non-web ones to the OS), and "scheme:rest" without a slash for the
 * schemeless-authority schemes (about:, data:, mailto:, tel:, …). A bare
 * "host:1234" would look like "scheme:digits", so an all-digit remainder is
 * treated as a host:port, not a scheme. */
static bool has_uri_scheme(const char* s)
{
    if (!g_ascii_isalpha((guchar)s[0]))
        return false;
    const char* p = s + 1;
    while (g_ascii_isalnum((guchar)*p) || *p == '+' || *p == '-' || *p == '.')
        p++;
    if (*p != ':')
        return false;
    if (p[1] == '/' && p[2] == '/')
        return true;
    if (p[1] == '\0')
        return false;
    for (const char* d = p + 1; *d != '\0'; d++)
        if (!g_ascii_isdigit((guchar)*d))
            return true; /* non-digit remainder -> a real scheme (mailto:, data:, …) */
    return false; /* all digits -> host:port, not a scheme */
}

/* Does `s` look like a bare hostname the user meant to visit (vs. a search)?
 * True when it has no whitespace and its authority is a plausible host: an
 * alphabetic TLD of 2+ chars (.com .dev .net .local .io …), a dotted-quad /
 * bracketed-IPv6 literal, or exactly "localhost" — with an optional :port. This
 * replaces the old hardcoded ".com"/".org" check with a general heuristic. */
static bool looks_like_host(const char* s)
{
    for (const char* p = s; *p != '\0'; p++)
        if (g_ascii_isspace((guchar)*p))
            return false;

    gsize alen = strcspn(s, "/?#"); /* the authority, before any path/query/fragment */
    if (alen == 0)
        return false;
    char* auth = g_strndup(s, alen);
    char* at = strrchr(auth, '@');
    char* host = at ? at + 1 : auth; /* drop any userinfo */

    bool ok = false;
    if (host[0] == '[') {
        ok = strchr(host, ']') != NULL; /* bracketed IPv6 literal */
    } else {
        char* colon = strrchr(host, ':'); /* strip an all-digit :port */
        if (colon != NULL && colon[1] != '\0') {
            bool digits = true;
            for (char* d = colon + 1; *d != '\0'; d++)
                if (!g_ascii_isdigit((guchar)*d)) {
                    digits = false;
                    break;
                }
            if (digits)
                *colon = '\0';
        }
        if (g_ascii_strcasecmp(host, "localhost") == 0) {
            ok = true;
        } else {
            char* dot = strrchr(host, '.');
            if (dot != NULL && dot != host && dot[1] != '\0') {
                int tldlen = 0;
                bool tld_alpha = true;
                for (char* d = dot + 1; *d != '\0'; d++, tldlen++)
                    if (!g_ascii_isalpha((guchar)*d)) {
                        tld_alpha = false;
                        break;
                    }
                if (tld_alpha && tldlen >= 2) {
                    ok = true;
                } else { /* not an alpha TLD -> only accept an IPv4-ish dotted number */
                    bool ipish = true;
                    for (char* d = host; *d != '\0'; d++)
                        if (!g_ascii_isdigit((guchar)*d) && *d != '.') {
                            ipish = false;
                            break;
                        }
                    ok = ipish;
                }
            }
        }
    }
    g_free(auth);
    return ok;
}

static void load_uri_resolved(WebKitWebView* view, const char* uri)
{
    if (uri[0] == '\0')
        return;

    if (has_uri_scheme(uri)) {
        webkit_web_view_load_uri(view, uri);
        return;
    }

    if (looks_like_host(uri)) {
        char* tmp = g_strconcat("https://", uri, NULL);
        webkit_web_view_load_uri(view, tmp);
        g_free(tmp);
        return;
    }

    char* expanded = shortcut_expand(uri);
    if (expanded != NULL) {
        webkit_web_view_load_uri(view, expanded);
        g_free(expanded);
        return;
    }

    /* Encode the query so special characters ('+', '&', '~', spaces, ...) reach
     * the search engine intact instead of being mangled into URL syntax. */
    char* q = g_uri_escape_string(uri, NULL, TRUE);
    char* search = g_strdup_printf(SEARCH, q);
    webkit_web_view_load_uri(view, search);
    g_free(q);
    g_free(search);
}

/* Trim surrounding whitespace before resolving, so a pasted "  https://..." (or a
 * value with a stray leading space / trailing newline) navigates straight to the
 * URL instead of being rejected as a host and handed to the search engine. */
static void load_uri(WebKitWebView* view, const char* uri)
{
    char* trimmed = g_strstrip(g_strdup(uri));
    load_uri_resolved(view, trimmed);
    g_free(trimmed);
}

/* ------------------------------------------------------------ shared state */
static WebKitWebContext* get_shared_web_context(void)
{
    static WebKitWebContext* context = NULL;
    if (context == NULL) {
        context = webkit_web_context_new();
        /* Most aggressive caching: "improve document load speed substantially by
         * caching a very large number of resources and previously viewed content." */
        webkit_web_context_set_cache_model(context, WEBKIT_CACHE_MODEL_WEB_BROWSER);
        /* Ad blocking is no longer a web-process extension: it runs as native
         * WebKit content filters attached per view (see adblock_apply_to_view). */

        /* Screen sharing: getDisplayMedia gets its stream from the xdg-desktop-portal
         * ScreenCast interface, which hands back a PipeWire node that WebKit reads
         * with GStreamer's pipewiresrc. pipewiresrc still has to connect to the
         * PipeWire *daemon* socket, and WebKit's bubblewrap sandbox binds PulseAudio's
         * socket into the web process but not PipeWire's -- without this the connection
         * never comes up and capture dies with "Unable to open pipewire remote. Error:
         * Timeout was reached". Bind the socket in explicitly; it must be writable, as
         * a socket is useless read-only.
         *
         * Necessary but, on wlroots compositors, not sufficient: xdg-desktop-portal-wlr
         * leaves buffer->size[plane] at 0 for the DMABuf frames it exports, and
         * pipewiresrc sizes its GstBuffers from exactly that, so every frame arrives
         * empty and is dropped -- the web process spams "gst_buffer_insert_memory:
         * assertion 'mem != NULL' failed" and the shared video stays black. Nothing on
         * this side can reach that: WebKit pins the capture caps to memory:DMABuf at
         * build time, so there is no negotiating it down to the shm path that works.
         * It needs the portal to report the real dmabuf size (lseek on the fd). */
        const char* runtime_dir = g_get_user_runtime_dir();
        if (runtime_dir != NULL) {
            /* PIPEWIRE_REMOTE names the socket when it isn't the default pipewire-0.
             * An absolute value is a full path; otherwise it's relative to the runtime dir. */
            const char* remote = g_getenv("PIPEWIRE_REMOTE");
            if (remote == NULL || remote[0] == '\0')
                remote = "pipewire-0";
            char* socket_path = g_path_is_absolute(remote)
                ? g_strdup(remote)
                : g_build_filename(runtime_dir, remote, NULL);
            if (g_file_test(socket_path, G_FILE_TEST_EXISTS))
                webkit_web_context_add_path_to_sandbox(context, socket_path, FALSE);
            g_free(socket_path);
        }
    }
    return context;
}

static WebKitNetworkSession* get_shared_network_session(void)
{
    static WebKitNetworkSession* session = NULL;
    if (session == NULL) {
        /* Bound WebKit's memory before the first network/web process spawns (the
         * setter is global and must run before any session). WebKit reclaims a
         * tab's process on close fine; this caps the *inherent* per-process cost
         * so a few heavy pages can't exhaust a small-RAM machine. As a process
         * climbs toward the per-process limit, caches are released early; a single
         * runaway page is killed (sad-tab) before the whole system OOMs. Typical
         * tabs (100-400MB) stay well under these thresholds, so normal browsing
         * and repeat-visit speed are unaffected. */
        WebKitMemoryPressureSettings* mp = webkit_memory_pressure_settings_new();
        webkit_memory_pressure_settings_set_memory_limit(mp, 2560);           /* MB per process */
        /* Set thresholds top-down (kill > strict > conservative): each setter asserts
         * its value sits below the *current* next-higher threshold, and the defaults
         * (conservative 0.33, strict 0.5) would reject conservative=0.65 if set first.
         * Thresholds sit high so heavy-but-honest pages keep their decoded-image and
         * JIT caches (dropping those made repeat views slow); only a true runaway page
         * crosses strict/kill. Sized for an 8GB machine: one process at the 2.5GB cap
         * still leaves room, and the kill threshold fires before the system swaps. */
        webkit_memory_pressure_settings_set_kill_threshold(mp, 0.95);         /* ~2.4GB: kill a runaway process */
        webkit_memory_pressure_settings_set_strict_threshold(mp, 0.8);        /* >2.0GB: release aggressively */
        webkit_memory_pressure_settings_set_conservative_threshold(mp, 0.65); /* >1.66GB: start releasing caches */
        webkit_network_session_set_memory_pressure_settings(mp);
        webkit_memory_pressure_settings_free(mp);

        session = webkit_network_session_new(DATA_DIR, DATA_DIR);
        WebKitCookieManager* cm = webkit_network_session_get_cookie_manager(session);
        webkit_cookie_manager_set_persistent_storage(cm, DATA_DIR "/cookies.sqlite", WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
        webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
        webkit_website_data_manager_set_favicons_enabled(
            webkit_network_session_get_website_data_manager(session), TRUE);
        g_signal_connect(session, "download-started", G_CALLBACK(on_download_started), NULL);
    }
    return session;
}

/* Flip on the in-development features listed in WEBKIT_ENABLED_FEATURES. They have
 * no GObject properties, so they can only be reached through the feature list; walk
 * it once and match by identifier. */
static void enable_extra_features(WebKitSettings* settings)
{
    static const char* const wanted[] = { WEBKIT_ENABLED_FEATURES };
    WebKitFeatureList* features = webkit_settings_get_all_features();
    for (gsize i = 0; i < webkit_feature_list_get_length(features); i++) {
        WebKitFeature* feature = webkit_feature_list_get(features, i);
        const char* id = webkit_feature_get_identifier(feature);
        for (gsize w = 0; w < G_N_ELEMENTS(wanted); w++) {
            if (g_strcmp0(id, wanted[w]) == 0) {
                webkit_settings_set_feature_enabled(settings, feature, TRUE);
                break;
            }
        }
    }
    webkit_feature_list_unref(features);
}

static WebKitSettings* get_shared_settings(void)
{
    static WebKitSettings* settings = NULL;
    if (settings == NULL) {
        settings = webkit_settings_new_with_settings(WEBKIT_DEFAULT_SETTINGS, NULL);
        enable_extra_features(settings);
        /* Keep the GPU compositor always on rather than ramping it up on demand per
         * page. Safe here: the target hardware has a working GPU (amdgpu); on a box
         * with no GL this would force the slow software path, so it's deliberate. */
        webkit_settings_set_hardware_acceleration_policy(settings,
            WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS);
        /* Expose the EME (Encrypted Media Extensions) API so DRM sites stop
         * reporting "protected content is not enabled". This only turns the API
         * on; actually decrypting Widevine streams (Spotify, Netflix) additionally
         * needs a Widevine CDM, which upstream WebKitGTK/nixpkgs does not ship. */
        webkit_settings_set_enable_encrypted_media(settings, TRUE);
    }
    return settings;
}

static WebKitWebView* current_view(Win* w)
{
    if (w == NULL)
        return NULL;
    GtkWidget* page = gtk_notebook_get_nth_page(w->notebook, gtk_notebook_get_current_page(w->notebook));
    return page ? WEBKIT_WEB_VIEW(page) : NULL;
}

/* ----------------------------------- status bar (link hover + load progress) */
static void update_status(Win* w)
{
    const char* text = w->status_flash ? w->status_flash : w->status_link; /* flash wins over hover */
    gtk_label_set_text(w->status_label, text ? text : "");
    gtk_widget_set_visible(GTK_WIDGET(w->status_label), text != NULL);
    gtk_widget_set_visible(GTK_WIDGET(w->progress), w->page_loading);
    gtk_widget_set_visible(w->statusbar, text != NULL || w->page_loading || active_downloads > 0);
}

static gboolean status_flash_clear(gpointer data)
{
    Win* w = data;
    g_clear_pointer(&w->status_flash, g_free);
    w->status_flash_source = 0;
    update_status(w);
    return G_SOURCE_REMOVE;
}

/* Show a transient message in the status bar for 2 seconds. */
static void status_flash_message(Win* w, const char* msg)
{
    if (w == NULL)
        return;
    g_clear_pointer(&w->status_flash, g_free);
    w->status_flash = g_strdup(msg);
    if (w->status_flash_source != 0)
        g_source_remove(w->status_flash_source);
    w->status_flash_source = g_timeout_add_seconds(2, status_flash_clear, w);
    update_status(w);
}

/* ----------------------------------------------------------------- downloads */
/* Pick the destination path under DOWNLOADS_DIR, uniquifying on collision so an
 * existing "file.zip" becomes "file (1).zip", "file (2).zip", ... rather than
 * being clobbered. An already-numbered "file (1).zip" renumbers to "file (2).zip"
 * instead of growing into "file (1) (1).zip". */
static gboolean on_download_destination(WebKitDownload* download, gchar* suggested_filename, gpointer data)
{
    const char* base = (suggested_filename != NULL && suggested_filename[0] != '\0')
        ? suggested_filename : "download";

    const char* ext = strrchr(base, '.'); /* keep the extension when numbering */
    int stem_len = ext ? (int)(ext - base) : (int)strlen(base);

    /* Strip a trailing " (N)" from the stem so we renumber rather than append. */
    if (stem_len >= 4 && base[stem_len - 1] == ')') {
        int j = stem_len - 2;
        while (j > 0 && g_ascii_isdigit(base[j]))
            j -= 1;
        if (j >= 1 && j <= stem_len - 3 && base[j] == '(' && base[j - 1] == ' ')
            stem_len = j - 1;
    }

    char* dest = g_build_filename(DOWNLOADS_DIR, base, NULL);
    for (int i = 1; g_file_test(dest, G_FILE_TEST_EXISTS); i++) {
        g_free(dest);
        char* name = g_strdup_printf("%.*s (%d)%s", stem_len, base, i, ext ? ext : "");
        dest = g_build_filename(DOWNLOADS_DIR, name, NULL);
        g_free(name);
    }

    webkit_download_set_destination(download, dest);
    g_free(dest);
    return TRUE;
}

/* A download's progress belongs in the window it was started from, which is
 * stamped on the download when it begins (see on_download_started). */
static void on_download_progress(GObject* obj, GParamSpec* pspec, gpointer data)
{
    Win* w = g_object_get_data(obj, "win");
    if (w == NULL)
        return;
    gtk_progress_bar_set_fraction(w->download_progress,
        webkit_download_get_estimated_progress(WEBKIT_DOWNLOAD(obj)));
}

/* "finished" fires on success, cancel, and after "failed" alike, so it's the one
 * place to drop the active count and hide the bar once nothing is downloading. */
static void on_download_finished(WebKitDownload* download, gpointer data)
{
    if (active_downloads > 0)
        active_downloads -= 1;
    Win* w = g_object_get_data(G_OBJECT(download), "win");
    if (w == NULL)
        return;
    if (active_downloads == 0)
        gtk_widget_set_visible(GTK_WIDGET(w->download_progress), FALSE);
    update_status(w);
}

/* A download opened in a fresh tab (target=_blank / window.open) leaves that tab
 * blank, since the response converts to a download and never commits a page. Such
 * a tab has no back/forward history, so close it; tabs with real content stay. */
static gboolean close_blank_download_tab(gpointer data)
{
    WebKitWebView* view = WEBKIT_WEB_VIEW(data);
    Win* w = win_of(view);
    if (w == NULL) {
        g_object_unref(view);
        return G_SOURCE_REMOVE;
    }
    int n = gtk_notebook_page_num(w->notebook, GTK_WIDGET(view));
    if (n >= 0 && w->num_tabs > 1
        && webkit_back_forward_list_get_current_item(webkit_web_view_get_back_forward_list(view)) == NULL) {
        mru_remove(w, GTK_WIDGET(view));
        GtkWidget* btn = g_object_get_data(G_OBJECT(view), "button");
        if (btn != NULL)
            gtk_box_remove(w->tabbar, btn);
        gtk_notebook_remove_page(w->notebook, n);
        w->num_tabs -= 1;
    }
    g_object_unref(view);
    return G_SOURCE_REMOVE;
}

static void on_download_started(WebKitNetworkSession* session, WebKitDownload* download, gpointer data)
{
    WebKitWebView* view = webkit_download_get_web_view(download);
    /* Report it in the window that asked for it; a download with no view of its
     * own (rare, but possible) falls back to whichever window is focused. */
    Win* w = win_of(view);
    if (w == NULL)
        w = cur();
    g_object_set_data(G_OBJECT(download), "win", w);

    g_signal_connect(download, "decide-destination", G_CALLBACK(on_download_destination), NULL);
    g_signal_connect(download, "notify::estimated-progress", G_CALLBACK(on_download_progress), NULL);
    g_signal_connect(download, "finished", G_CALLBACK(on_download_finished), NULL);

    active_downloads += 1;
    if (w != NULL) {
        gtk_progress_bar_set_fraction(w->download_progress, 0.0);
        gtk_widget_set_visible(GTK_WIDGET(w->download_progress), TRUE);
        status_flash_message(w, "Download started");
    }

    if (view != NULL)
        g_idle_add(close_blank_download_tab, g_object_ref(view));
}

/* NULL clears it; ignored for background tabs so they can't hijack the status. */
static void status_set_link(WebKitWebView* view, const char* uri)
{
    Win* w = win_of(view);
    if (w == NULL || view != current_view(w))
        return;
    g_clear_pointer(&w->status_link, g_free);
    if (uri != NULL && uri[0] != '\0')
        w->status_link = g_strdup(uri);
    update_status(w);
}

/* Restyle the page's text selection to a dark-blue wash with white text. Applies
 * to any selection, including the current match WebKit selects while finding --
 * which otherwise shows the faint, low-contrast default selection colour. */
static const char* SELECTION_CSS =
    "::selection { background-color: #5E81AC !important; color: #FFFFFF !important; }";

/* Our user agent claims macOS Safari (see webkit_settings_set_user_agent), and the
 * whole navigator object already matches real Mac Safari -- except navigator.platform,
 * which WebKit derives from the host OS and reports as "Linux x86_64". That single
 * UA-vs-platform mismatch is a classic bot tell that trips Cloudflare's challenge on
 * nearly every visit. Redefine platform to "MacIntel" at document-start (before any
 * page script reads it) so the fingerprint is internally consistent. Injected into
 * the page's main world so site scripts see the patched value. */
static const char* NAVIGATOR_SPOOF_JS =
    "try{Object.defineProperty(Navigator.prototype,'platform',"
    "{get:function(){return 'MacIntel';},configurable:true});}"
    "catch(e){try{Object.defineProperty(navigator,'platform',"
    "{get:function(){return 'MacIntel';},configurable:true});}catch(e2){}}";

/* WebKitGTK fires the paste event with an empty DataTransfer for images and video —
 * the editor pastes them fine, but a site reading e.clipboardData.items sees nothing
 * and does nothing. navigator.clipboard.read() does return the data (see the clipboard
 * branch in on_permission_request), so swallow the blank event and re-dispatch one
 * carrying the real files. Text is unaffected: WebKit populates types for it. */
static const char* PASTE_SHIM_JS =
    "(function(){var mine=false;"
    "document.addEventListener('paste',function(e){"
    "if(mine||e.clipboardData.types.length)return;"
    "var target=e.target;e.preventDefault();e.stopImmediatePropagation();"
    "navigator.clipboard.read().then(async function(items){"
    "var dt=new DataTransfer();"
    "for(var item of items)for(var type of item.types)"
    "if(/^(image|video)\\//.test(type))"
    "dt.items.add(new File([await item.getType(type)],'pasted.'+type.split('/')[1],{type:type}));"
    "if(!dt.files.length)return;mine=true;"
    "target.dispatchEvent(new ClipboardEvent('paste',{clipboardData:dt,bubbles:true,cancelable:true}));"
    "mine=false;}).catch(function(){});"
    "},true);})();";

/* Report the focused link (keyboard tabbing) the way hover reports it. */
static const char* LINK_FOCUS_JS =
    "document.addEventListener('focusin',function(e){"
    "var a=e.target.closest?e.target.closest('a[href]'):null;"
    "window.webkit.messageHandlers.linkFocus.postMessage(a?a.href:'');},true);"
    "document.addEventListener('focusout',function(){"
    "window.webkit.messageHandlers.linkFocus.postMessage('');},true);";

static void on_load_progress(GObject* obj, GParamSpec* pspec, gpointer data)
{
    WebKitWebView* view = WEBKIT_WEB_VIEW(obj);
    Win* w = win_of(view);
    if (w == NULL || view != current_view(w))
        return;
    gtk_progress_bar_set_fraction(w->progress, webkit_web_view_get_estimated_load_progress(view));
}

static void view_set_dark_chrome(WebKitWebView* view, gboolean dark);
static void view_probe_page_background(WebKitWebView* view);

static void on_load_changed(WebKitWebView* view, WebKitLoadEvent event, gpointer data)
{
    if (event == WEBKIT_LOAD_STARTED) {
        g_object_set_data(G_OBJECT(view), "load-error", NULL); /* assume this load is fine */
        /* Bump the load id so a background probe from the previous page can't flip
         * the canvas white mid-load and reintroduce the white flash. */
        g_object_set_data(G_OBJECT(view), "load-id",
            GUINT_TO_POINTER(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(view), "load-id")) + 1));
        view_set_dark_chrome(view, TRUE); /* dark pre-paint loading flash */
    } else if (event == WEBKIT_LOAD_COMMITTED) {
        /* Keep dark chrome only where the page has no background of its own: the
         * (transparent) error page, and about: pages like the blank new tab. A real
         * site keeps the dark canvas for the pre-paint gap, then the probe below
         * hands it the white UA canvas iff it paints no background of its own. */
        const char* uri = webkit_web_view_get_uri(view);
        gboolean dark = g_object_get_data(G_OBJECT(view), "load-error") != NULL
            || uri == NULL || g_str_has_prefix(uri, "about:");
        view_set_dark_chrome(view, dark);
        if (!dark)
            view_probe_page_background(view);
        session_save_queue(); /* this tab's URL just changed */
    }

    Win* w = win_of(view);
    if (w == NULL || view != current_view(w))
        return;
    if (event == WEBKIT_LOAD_STARTED) {
        w->page_loading = TRUE;
        g_object_set_data(G_OBJECT(view), "dead", NULL); /* a load means it's alive again */
        tab_set_asleep(view, FALSE);                     /* and no longer dimmed */
        gtk_progress_bar_set_fraction(w->progress, 0.0);
    } else if (event == WEBKIT_LOAD_FINISHED) {
        w->page_loading = FALSE;
    }
    update_status(w);
}

/* A failed load (DNS failure, TLS error, ...) makes WebKit swap in its built-in
 * error page, which is transparent and relies on the UA white canvas + black text.
 * Flag the view so the COMMITTED of that error page keeps the dark chrome (legible
 * white-on-dark); FALSE lets WebKit render its default error page. */
static gboolean on_load_failed(WebKitWebView* view, WebKitLoadEvent event,
    char* failing_uri, GError* error, gpointer data)
{
    g_object_set_data(G_OBJECT(view), "load-error", GINT_TO_POINTER(1));
    return FALSE;
}

static void prefetch_host(const char* text);

static void on_mouse_target(WebKitWebView* view, WebKitHitTestResult* hit, guint modifiers, gpointer data)
{
    const char* uri = webkit_hit_test_result_context_is_link(hit)
        ? webkit_hit_test_result_get_link_uri(hit)
        : NULL;
    /* Hover precedes click: warm DNS for the hovered link so the lookup is done
     * (or in flight) by the time it's clicked. WebKit dedups repeat requests. */
    if (uri != NULL)
        prefetch_host(uri);
    status_set_link(view, uri);
}

static void on_link_focus_message(WebKitUserContentManager* ucm, JSCValue* value, gpointer data)
{
    char* uri = jsc_value_to_string(value);
    status_set_link(WEBKIT_WEB_VIEW(data), uri);
    g_free(uri);
}

/* ----------------------------------------------------------------- tabs */
/* When a web process is killed (e.g. the memory kill-threshold) the view stays
 * alive but blank, so the tab stays in the bar. We remember its URL and mark it
 * "dead"; the page is reloaded lazily only when the user navigates back to that
 * tab — so a runaway background page can't immediately OOM again. */
/* Dim a tab's button to 50% (kept favicon and all) while it's slept/dead. */
static void tab_set_asleep(WebKitWebView* view, gboolean asleep)
{
    GtkWidget* btn = g_object_get_data(G_OBJECT(view), "button");
    if (btn == NULL)
        return;
    if (asleep)
        gtk_widget_add_css_class(btn, "asleep");
    else
        gtk_widget_remove_css_class(btn, "asleep");
}

/* Record when a tab was last the active one, so the sweep sleeps the least-recently-used one first. */
static void tab_touch(WebKitWebView* view)
{
    g_object_set_data(G_OBJECT(view), "last-active",
        GSIZE_TO_POINTER((gsize)(g_get_monotonic_time() / G_USEC_PER_SEC)));
}

static void revive_if_dead(WebKitWebView* view)
{
    if (view == NULL || g_object_get_data(G_OBJECT(view), "dead") == NULL)
        return;
    g_object_set_data(G_OBJECT(view), "dead", NULL);
    tab_set_asleep(view, FALSE);
    const char* uri = g_object_get_data(G_OBJECT(view), "reload-uri");
    if (uri != NULL && uri[0] != '\0')
        webkit_web_view_load_uri(view, uri);
    else
        webkit_web_view_reload(view);
}

/* Both a memory-kill and a proactive sleep land here: keep the tab but free its
 * web process, remember the URL, mark it dead, and dim it. revive_if_dead (driven
 * by on_tab_pressed / on_switch_page) reloads it only when the user goes back. */
static void on_web_process_terminated(WebKitWebView* view,
    WebKitWebProcessTerminationReason reason, gpointer data)
{
    const char* uri = webkit_web_view_get_uri(view);
    g_object_set_data_full(G_OBJECT(view), "reload-uri", g_strdup(uri ? uri : ""), g_free);
    g_object_set_data(G_OBJECT(view), "dead", GINT_TO_POINTER(1));
    tab_set_asleep(view, TRUE);
    /* Sleeping must never leave the tab you're looking at blank/dimmed: if WebKit's
     * memory-pressure killer (or a crash) takes the foreground process, reload it
     * right away instead of waiting for the user to navigate back. */
    if (view == current_view(win_of(view)))
        revive_if_dead(view);
}

/* A tab is worth sleeping only if it actually holds a live page we can reload:
 * not already slept/crashed, not playing audio (don't cut off music/video), and
 * showing a real URL (a blank/about tab has nothing to free). */
static gboolean tab_sleepable(WebKitWebView* view)
{
    if (g_object_get_data(G_OBJECT(view), "dead") != NULL)
        return FALSE;
    if (webkit_web_view_is_playing_audio(view))
        return FALSE;
    const char* uri = webkit_web_view_get_uri(view);
    return uri != NULL && uri[0] != '\0' && !g_str_has_prefix(uri, "about:");
}

/* Sleep a background tab: terminating its web process hands the RAM back to the OS
 * (the bulk of a heavy page's cost). The web-process-terminated handler above
 * remembers the URL and dims the tab; it reloads when the user reselects it. */
static void sleep_tab(WebKitWebView* view)
{
    if (tab_sleepable(view))
        webkit_web_view_terminate_web_process(view);
}

/* Current memory-pressure reading: the PSI "some avg10" percentage — the share of
 * the last 10s in which at least one task stalled waiting on memory. ~0 on a
 * healthy machine (even one sitting at 100% RAM with free swap), climbing only
 * under genuine thrash. Returns -1 if PSI isn't available (pre-4.20 kernel or
 * CONFIG_PSI off), so the caller falls back to the absolute OOM floor. */
static double system_mem_pressure(void)
{
    char* contents = NULL;
    if (!g_file_get_contents("/proc/pressure/memory", &contents, NULL, NULL))
        return -1;
    double pressure = -1;
    char* p = strstr(contents, "some avg10=");
    if (p != NULL)
        pressure = g_ascii_strtod(p + strlen("some avg10="), NULL);
    g_free(contents);
    return pressure;
}

/* Free RAM *plus* free swap in MiB (everywhere the kernel can still put a page),
 * or -1 if /proc/meminfo can't be read. This is the true "about to OOM" gauge:
 * unlike MemAvailable alone it doesn't false-alarm while swap has headroom. */
static long system_oom_headroom_mb(void)
{
    char* contents = NULL;
    if (!g_file_get_contents("/proc/meminfo", &contents, NULL, NULL))
        return -1;
    long mb = 0;
    gboolean got = FALSE;
    const char* fields[] = { "MemAvailable:", "SwapFree:" };
    for (int i = 0; i < 2; i++) {
        char* p = strstr(contents, fields[i]);
        if (p != NULL) {
            mb += strtol(p + strlen(fields[i]), NULL, 10) / 1024; /* fields are in kB */
            got = TRUE;
        }
    }
    g_free(contents);
    return got ? mb : -1;
}

/* The crash defense: every few seconds, gauge real memory pressure. While the
 * machine is comfortable (low PSI, swap headroom) leave every tab loaded — a
 * swap-backed laptop can sit near 100% RAM without trouble, so we don't reap
 * tabs just because free RAM is low. Under genuine pressure, sleep the
 * least-recently-used background tab (one per sweep — self-correcting as the OS
 * reclaims memory), skipping any tab seen too recently so tab-switching stays
 * snappy. If the machine is thrashing or has run clean out of RAM *and* swap,
 * dump every background tab at once so it can't OOM out from under us. */
static gboolean sleep_sweep(gpointer data)
{
    if (wins == NULL)
        return G_SOURCE_CONTINUE;

    double pressure = system_mem_pressure();
    long headroom = system_oom_headroom_mb();
    gboolean oom_floor = headroom >= 0 && headroom < TAB_SLEEP_OOM_FLOOR_MB;

    if (!oom_floor && (pressure < 0 || pressure < TAB_SLEEP_PRESSURE))
        return G_SOURCE_CONTINUE; /* comfortable (or PSI unavailable and swap fine): leave tabs be */
    gboolean critical = oom_floor || pressure >= TAB_SLEEP_PRESSURE_CRITICAL;

    /* One sweep covers every window: the pressure is machine-wide, so the tab worth
     * sleeping is the least recently used of all of them, not of one window. */
    gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
    GtkWidget* lru = NULL;
    gint64 lru_time = G_MAXINT64;
    for (guint iw = 0; iw < wins->len; iw++) {
        Win* w = wins->pdata[iw];
        GtkWidget* front = gtk_notebook_get_nth_page(w->notebook, gtk_notebook_get_current_page(w->notebook));
        int n = gtk_notebook_get_n_pages(w->notebook);
        for (int i = 0; i < n; i++) {
            GtkWidget* page = gtk_notebook_get_nth_page(w->notebook, i);
            if (page == front || !tab_sleepable(WEBKIT_WEB_VIEW(page)))
                continue; /* never sleep the tab you're looking at */
            if (critical) {
                sleep_tab(WEBKIT_WEB_VIEW(page)); /* OOM imminent: dump them all */
                continue;
            }
            gint64 last = (gint64)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(page), "last-active"));
            if (now - last < TAB_SLEEP_MIN_AGE_SECONDS)
                continue; /* just left this tab — keep it warm so switching back is instant */
            if (last < lru_time) {
                lru_time = last;
                lru = page;
            }
        }
    }
    if (!critical && lru != NULL)
        sleep_tab(WEBKIT_WEB_VIEW(lru));
    return G_SOURCE_CONTINUE;
}

/* Switch on press (capture phase) rather than on the button's release-driven
 * "clicked", so mouse selection feels as instant as the keyboard. */
static void on_tab_pressed(GtkGestureClick* gesture, int n_press, double x, double y, WebKitWebView* view)
{
    Win* w = win_of(view);
    if (w == NULL)
        return;
    int n = gtk_notebook_page_num(w->notebook, GTK_WIDGET(view));
    if (n >= 0)
        gtk_notebook_set_current_page(w->notebook, n);
    revive_if_dead(view); /* clicking a killed tab reloads it (same page if current) */
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void update_favicon(WebKitWebView* view)
{
    GtkImage* img = g_object_get_data(G_OBJECT(view), "icon");
    GdkTexture* fav = webkit_web_view_get_favicon(view);
    if (fav != NULL)
        gtk_image_set_from_paintable(img, GDK_PAINTABLE(fav));
    else
        gtk_image_set_from_icon_name(img, "folder-earth-symbolic");
}

static void on_favicon_notify(GObject* view, GParamSpec* pspec, gpointer data)
{
    update_favicon(WEBKIT_WEB_VIEW(view));
}

static void on_switch_page(GtkNotebook* nb, GtkWidget* page, guint n, gpointer data)
{
    /* Dropping every tab walks the notebook's selection across the survivors on its
     * way down; none of that is a real tab switch, and reviving the pages it lands
     * on would spawn web processes we are in the middle of freeing. */
    if (tearing_down)
        return;

    Win* w = data;
    for (GtkWidget* c = gtk_widget_get_first_child(GTK_WIDGET(w->tabbar)); c != NULL; c = gtk_widget_get_next_sibling(c))
        gtk_widget_remove_css_class(c, "active");
    GtkWidget* btn = g_object_get_data(G_OBJECT(page), "button");
    if (btn != NULL)
        gtk_widget_add_css_class(btn, "active");

    /* Track most-recently-used order so alt+tab can walk back through it. While a
     * walk is driving the switch itself, leave the order untouched: it's committed
     * only when the user releases Alt (see handle_signal_keyrelease). */
    if (!w->alt_switch) {
        mru_promote(w, page);
        w->alt_walk = -1; /* a genuine switch ends any in-progress walk */
    }

    /* Resync the status bar to the now-current tab. */
    WebKitWebView* view = WEBKIT_WEB_VIEW(page);
    tab_touch(view); /* mark active now so the sleep sweep leaves it alone */
    g_clear_pointer(&w->status_link, g_free); /* no hover/focus on the new tab yet */
    w->page_loading = webkit_web_view_is_loading(view);
    if (w->page_loading)
        gtk_progress_bar_set_fraction(w->progress, webkit_web_view_get_estimated_load_progress(view));
    update_status(w);
    revive_if_dead(view); /* reload a killed tab when the user switches to it */

    /* Re-run the find against the now-current tab so its matches/highlight
     * reflect the page the user is actually looking at. */
    if (gtk_widget_get_visible(w->findbar))
        do_find(w, gtk_editable_get_text(GTK_EDITABLE(w->find_entry)));
}

static void on_counted_matches(WebKitFindController* fc, guint count, gpointer data);
static GtkWidget* on_create_tab(WebKitWebView* self, WebKitNavigationAction* action, gpointer data);

/* Middle-click a link -> open it in a new tab (other clicks navigate normally). */
static gboolean on_decide_policy(WebKitWebView* view, WebKitPolicyDecision* decision,
    WebKitPolicyDecisionType type, gpointer data)
{
    /* A response WebKit can't render (zip, exe, ...) should download, not show a
     * blank page. WebKit doesn't do this on its own, so force it here. */
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision* rpd = WEBKIT_RESPONSE_POLICY_DECISION(decision);
        /* application/pdf isn't reported as a "supported" MIME, but WebKit's bundled
         * PDF.js viewer (PDFJSViewer, on by default) renders it — so let it through
         * and the PDF opens in the tab instead of downloading. PDF.js binds Ctrl+S
         * itself to save the file (through the normal download pipeline). */
        WebKitURIResponse* resp = webkit_response_policy_decision_get_response(rpd);
        const char* mime = resp ? webkit_uri_response_get_mime_type(resp) : NULL;
        if (mime != NULL && g_ascii_strcasecmp(mime, "application/pdf") == 0)
            return FALSE;
        /* Only the resource the user actually navigated to can become a download.
         * Subresources (beacons, pings, prefetch) that come back empty are tagged
         * application/x-zerosize by WebKit -- an unsupported MIME -- and would
         * otherwise be turned into bogus 0-byte "download" files (e.g. on YouTube). */
        if (!webkit_response_policy_decision_is_main_frame_main_resource(rpd))
            return FALSE;
        if (!webkit_response_policy_decision_is_mime_type_supported(rpd)) {
            webkit_policy_decision_download(decision);
            return TRUE;
        }
        return FALSE;
    }

    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
        return FALSE;
    WebKitNavigationAction* a = webkit_navigation_policy_decision_get_navigation_action(
        WEBKIT_NAVIGATION_POLICY_DECISION(decision));

    /* A navigation to a scheme WebKit can't load itself (an app callback like
     * myapp://, or mailto:/tel:) would otherwise error. Hand it to the OS's
     * registered handler instead and drop the in-page navigation. */
    const char* nav_uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(a));
    const char* scheme = nav_uri ? g_uri_peek_scheme(nav_uri) : NULL;
    /* "webkit" covers internal pages like webkit://gpu (GPU diagnostics). */
    static const char* web_schemes[] = { "http", "https", "file", "about", "data", "blob", "ws", "wss", "webkit" };
    if (scheme != NULL) {
        bool web = false;
        for (size_t i = 0; i < G_N_ELEMENTS(web_schemes); i++)
            if (g_ascii_strcasecmp(scheme, web_schemes[i]) == 0) {
                web = true;
                break;
            }
        if (!web) {
            g_app_info_launch_default_for_uri(nav_uri, NULL, NULL);
            webkit_policy_decision_ignore(decision);
            return TRUE;
        }
    }

    if (webkit_navigation_action_get_mouse_button(a) != 2) /* 2 = middle */
        return FALSE;
    modal_hide_for_new_tab(win_of(view));
    notebook_create_new_tab(win_of(view), /* beside the tab that was middle-clicked */
        webkit_uri_request_get_uri(webkit_navigation_action_get_request(a)));
    webkit_policy_decision_ignore(decision);
    return TRUE;
}

/* WebKit's built-in error page sets no colours of its own — it relied on the UA
 * default white canvas + black text, so on our dark canvas its text is invisible.
 * This user-level (low-priority) stylesheet gives a Nord-white default text colour:
 * it only takes effect where the page itself sets no colour (the error page, plain
 * documents), and is overridden by any real site that styles its own text. */
static const char* DEFAULT_TEXT_CSS = "html { color: #ECEFF4; }";

/* Dark chrome = Nord polar-night canvas (#2E3440) + the white-text default above.
 * It keeps the pre-paint loading flash, about: pages and WebKit's transparent error
 * page legible on dark. We deliberately DON'T keep it on a real site: many sites set
 * no background and lean on the UA default white canvas + black text, where the dark
 * canvas would show through and the forced white text would wash their text out. So
 * those pages get plain white chrome instead (toggled per-load in on_load_changed).
 *
 * Selection styling is always on; the white-text default only in dark mode. We
 * remove-all + re-add rather than toggle a single sheet to stay off the newer
 * remove-single-sheet API; our user scripts (link focus) live separately and are
 * left untouched by remove_all_style_sheets. */
static void view_set_dark_chrome(WebKitWebView* view, gboolean dark)
{
    /* Background is dark at the start of every load so the pre-paint gap (and a
     * flip to white at COMMITTED, which lands before the page's first frame) never
     * shows as a white flash. Sites that paint their own background cover it; sites
     * that paint none would wrongly show through dark, so view_probe_page_background
     * flips the canvas to the UA white for exactly those once their DOM is ready.
     * The `dark` flag only gates the white-text helper, which the transparent
     * about:/error pages need but a real site (black-on-white default) must not get. */
    GdkRGBA dark_c = { 0.180, 0.204, 0.251, 1.0 }; /* Nord polar night #2E3440 */
    webkit_web_view_set_background_color(view, &dark_c);

    WebKitUserContentManager* ucm = webkit_web_view_get_user_content_manager(view);
    webkit_user_content_manager_remove_all_style_sheets(ucm);
    WebKitUserStyleSheet* sel = webkit_user_style_sheet_new(SELECTION_CSS,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_STYLE_LEVEL_AUTHOR, NULL, NULL);
    webkit_user_content_manager_add_style_sheet(ucm, sel);
    webkit_user_style_sheet_unref(sel);
    if (dark) {
        WebKitUserStyleSheet* text = webkit_user_style_sheet_new(DEFAULT_TEXT_CSS,
            WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);
        webkit_user_content_manager_add_style_sheet(ucm, text);
        webkit_user_style_sheet_unref(text);
    }
}

/* Resolves (once the DOM is ready) to true iff neither <html> nor <body> paints a
 * background, i.e. the page leans on the UA default white canvas. */
static const char* BG_PROBE_JS =
    "return new Promise(resolve => {\n"
    "  const check = () => {\n"
    "    const bare = el => { if (!el) return true; const s = getComputedStyle(el);\n"
    "      return s.backgroundImage === 'none'\n"
    "          && (s.backgroundColor === 'transparent' || s.backgroundColor === 'rgba(0, 0, 0, 0)'); };\n"
    "    resolve(bare(document.documentElement) && bare(document.body));\n"
    "  };\n"
    "  if (document.readyState === 'loading')\n"
    "    document.addEventListener('DOMContentLoaded', check, { once: true });\n"
    "  else check();\n"
    "});\n";

static void on_bg_probe_done(GObject* src, GAsyncResult* res, gpointer data)
{
    WebKitWebView* view = WEBKIT_WEB_VIEW(src);
    JSCValue* val = webkit_web_view_call_async_javascript_function_finish(view, res, NULL);
    if (val == NULL)
        return;
    /* Only act if the probe belongs to the load still on screen. */
    if (jsc_value_to_boolean(val)
        && GPOINTER_TO_UINT(data) == GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(view), "load-id"))) {
        GdkRGBA white = { 1.0, 1.0, 1.0, 1.0 };
        webkit_web_view_set_background_color(view, &white);
    }
    g_object_unref(val);
}

/* A page that paints no background of its own expects the UA's white canvas, not
 * our dark pre-paint one — detect that case and hand it the white canvas. */
static void view_probe_page_background(WebKitWebView* view)
{
    gpointer load_id = g_object_get_data(G_OBJECT(view), "load-id");
    webkit_web_view_call_async_javascript_function(view, BG_PROBE_JS, -1,
        NULL, NULL, NULL, NULL, on_bg_probe_done, load_id);
}

/* ------------------------------------------------ camera/mic permissions */
/* The host of the view's current page (for the prompt text + session memory), or
 * NULL for a page without one (about:, file:, ...). Caller frees. */
static char* view_host(WebKitWebView* view)
{
    const char* uri = view ? webkit_web_view_get_uri(view) : NULL;
    if (uri == NULL)
        return NULL;
    GUri* u = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
    if (u == NULL)
        return NULL;
    const char* host = g_uri_get_host(u);
    char* out = (host != NULL && host[0] != '\0') ? g_strdup(host) : NULL;
    g_uri_unref(u);
    return out;
}

/* Pop and display the next queued permission request in the shared modal, or hide
 * the modal once the queue is drained. */
static void perm_show_next(Win* asked_in)
{
    if (perm_current != NULL)
        return; /* one decision at a time */
    perm_current = g_queue_pop_head(&perm_queue);
    if (perm_current == NULL) {
        if (asked_in != NULL)
            modal_hide(asked_in);
        return;
    }
    /* Each request remembers the window whose page raised it, so the prompt lands
     * over that page rather than over whichever window happens to be focused. */
    Win* w = g_object_get_data(G_OBJECT(perm_current), "perm-win");
    const char* msg = g_object_get_data(G_OBJECT(perm_current), "perm-msg");
    modal_show_permission(w ? w : cur(), msg ? msg : "This site wants access");
}

/* Resolve the shown request (Enter=allow, Esc=deny), remember an allow for its key
 * this session, then show any request that queued up behind it. */
static void perm_decide(gboolean allow)
{
    if (perm_current == NULL)
        return;
    Win* shown_in = g_object_get_data(G_OBJECT(perm_current), "perm-win");
    if (allow) {
        webkit_permission_request_allow(perm_current);
        const char* key = g_object_get_data(G_OBJECT(perm_current), "perm-key");
        if (key != NULL && key[0] != '\0')
            g_hash_table_add(perm_allowed, g_strdup(key));
    } else {
        webkit_permission_request_deny(perm_current);
    }
    g_clear_object(&perm_current);
    perm_show_next(shown_in); /* re-shows the modal for the next request, or hides it */
}

/* Build the prompt for a request, or return FALSE to leave it at WebKit's default.
 * On TRUE, *key is the session-memory key (caller owns; NULL = never remember) and
 * *msg is the Pango markup to show (caller owns). */
static gboolean perm_describe(WebKitWebView* view, WebKitPermissionRequest* request,
    char** key, char** msg)
{
    const char* hint = "\n<span size='small' foreground='#81A1C1'>"
                       "Enter to allow · Esc to deny</span>";
    if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request)) {
        WebKitUserMediaPermissionRequest* um = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(request);
        gboolean audio = webkit_user_media_permission_is_for_audio_device(um);
        gboolean video = webkit_user_media_permission_is_for_video_device(um);
        /* getDisplayMedia arrives as a user-media request too, flagged as a display
         * device — without this branch a screen-share prompt reads "microphone". */
        gboolean display = webkit_user_media_permission_is_for_display_device(um);
        const char* kind = display ? "to share your screen"
            : (audio && video)     ? "microphone &amp; camera access"
            : video                ? "camera access"
                                   : "microphone access";
        char* host = view_host(view);
        char* who = g_markup_escape_text(host != NULL ? host : "This site", -1);
        /* Screen capture reveals far more than the camera does, so it gets its own
         * session-memory key: allowing the camera on a host must not silently hand
         * that same host the whole screen later. */
        *key = (host != NULL && display) ? g_strdup_printf("display\n%s", host) : host;
        if (host != NULL && display)
            g_free(host); /* key is the prefixed copy; the bare host is spent */
        *msg = g_strdup_printf("<b>%s</b> wants %s%s", who, kind, hint);
        g_free(who);
        return TRUE;
    }
    if (WEBKIT_IS_WEBSITE_DATA_ACCESS_PERMISSION_REQUEST(request)) {
        WebKitWebsiteDataAccessPermissionRequest* wr
            = WEBKIT_WEBSITE_DATA_ACCESS_PERMISSION_REQUEST(request);
        const char* req = webkit_website_data_access_permission_request_get_requesting_domain(wr);
        const char* cur = webkit_website_data_access_permission_request_get_current_domain(wr);
        char* ereq = g_markup_escape_text(req ? req : "A site", -1);
        char* ecur = g_markup_escape_text(cur ? cur : "this site", -1);
        *key = g_strdup_printf("storage\n%s\n%s", req ? req : "", cur ? cur : "");
        *msg = g_strdup_printf("<b>%s</b> wants to use its cookies on <b>%s</b>%s", ereq, ecur, hint);
        g_free(ereq);
        g_free(ecur);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_permission_request(WebKitWebView* view,
    WebKitPermissionRequest* request, gpointer data G_GNUC_UNUSED)
{
    /* Device enumeration (the mic/camera picker Zoom shows) is benign and needed
     * for the media UI once capture is in play; allow it silently. */
    if (WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST(request)) {
        webkit_permission_request_allow(request);
        return TRUE;
    }

    /* Reading an image or video off the clipboard goes through
     * navigator.clipboard.read(), which lands here — denied by default, so media
     * paste silently does nothing. WebKit only asks on a user gesture, so a site
     * can't snoop the clipboard on its own; allowing silently just makes Ctrl+V
     * work, where a modal on every paste would not be worth it. */
    if (WEBKIT_IS_CLIPBOARD_PERMISSION_REQUEST(request)) {
        webkit_permission_request_allow(request);
        return TRUE;
    }

    char* key = NULL;
    char* msg = NULL;
    /* Anything we don't describe keeps WebKit's safe default (denied). */
    if (!perm_describe(view, request, &key, &msg))
        return FALSE;

    if (perm_allowed == NULL)
        perm_allowed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Already granted this session: allow without re-prompting. */
    if (key != NULL && g_hash_table_contains(perm_allowed, key)) {
        webkit_permission_request_allow(request);
        g_free(key);
        g_free(msg);
        return TRUE;
    }

    g_object_set_data(G_OBJECT(request), "perm-win", win_of(view)); /* prompt over the asking page */
    g_object_set_data_full(G_OBJECT(request), "perm-key", key, g_free); /* may be NULL */
    g_object_set_data_full(G_OBJECT(request), "perm-msg", msg, g_free);

    g_queue_push_tail(&perm_queue, g_object_ref(request));
    perm_show_next(win_of(view));
    return TRUE; /* handled asynchronously: we hold a ref until perm_decide */
}

/* Silence xdg-desktop-portal's camera dialog, which is not ours and not asked for.
 *
 * WebKit's capture code asks the portal for camera access
 * (org.freedesktop.portal.Camera.AccessCamera) as soon as a page so much as calls
 * navigator.mediaDevices.enumerateDevices() -- which plenty of sites, claude.ai
 * among them, do on load just to feature-detect. The portal answers that with a
 * *system* dialog ("Allow app to Use the Camera?", drawn by
 * xdg-desktop-portal-gtk), so a page that never wanted the camera puts an OS prompt
 * on screen. It arrives nowhere near the permission-request signal, so it cannot be
 * answered in the app; and the portal records the answer per app id in its
 * permission store, which lives under ~/.local/share/flatpak/db and does not
 * survive a reboot here -- so the dialog keeps coming back.
 *
 * Writing the answer into that store ahead of time is what stops it: the portal
 * replies from the record and never opens a dialog. The recorded answer is "no",
 * not "yes", because "no" is the branch that works. Denied, WebKit enumerates and
 * captures straight off /dev/video* with GStreamer's V4L2 provider -- same camera,
 * and the getUserMedia request still comes through permission-request, so our own
 * modal remains the gate (which is where a browser's camera decision belongs).
 * Granted, WebKit takes the portal's PipeWire route instead, and on this WebKitGTK
 * (2.52) that route hands the web process a remote it immediately crashes on: the
 * tab dies and the camera never works at all.
 *
 * The camera portal is the only one involved: microphone capture never goes through
 * it, and screen sharing keeps its portal picker on purpose (see the PipeWire note
 * in get_shared_web_context) -- handing a page the whole screen is worth a prompt.
 *
 * The app id has to be the one WebKit presents to the portal, which is the default
 * GApplication's -- the same value WebKit itself reads, so it cannot drift. */
static void camera_portal_answer_in_advance(void)
{
    GApplication* application = g_application_get_default();
    GDBusConnection* bus = application != NULL
        ? g_application_get_dbus_connection(application) : NULL;
    const char* app_id = application != NULL
        ? g_application_get_application_id(application) : NULL;
    if (bus == NULL || app_id == NULL)
        return; /* no session bus: no portal asking us anything either */

    /* Rewritten on every launch, not just when unset: an old "yes" (from a dialog
     * answered before this existed) would otherwise keep the crashing path alive. */
    const char* answer[] = { "no", NULL };
    g_dbus_connection_call(bus, "org.freedesktop.impl.portal.PermissionStore",
        "/org/freedesktop/impl/portal/PermissionStore",
        "org.freedesktop.impl.portal.PermissionStore", "SetPermission",
        g_variant_new("(sbss^as)", "devices", TRUE /* create the entry if it's missing */,
            "camera", app_id, answer),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* When `related` is non-NULL the new view is a popup (window.open / target=_blank):
 * constructing it with "related-view" keeps it in the opener's web process and
 * session and, crucially, preserves window.opener. Microsoft Rewards card
 * activities (and many OAuth popups) report completion back through that opener
 * channel, so a detached popup silently fails to credit. A NULL related view is a
 * normal top-level tab using the shared session/context. */
static WebKitWebView* append_tab(Win* w, WebKitWebView* related)
{
    WebKitWebView* view = related
        ? g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "related-view", related, /* shares process + session, keeps window.opener */
            "settings", get_shared_settings(),
            NULL)
        : g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "settings", get_shared_settings(),
            "network-session", get_shared_network_session(),
            "web-context", get_shared_web_context(),
            NULL);
    NULLCHECK(view);
    /* Every handler below is reached from the view, not from a global, so the tab
     * keeps pointing at its own window however many are open. */
    g_object_set_data(G_OBJECT(view), "win", w);
    view_set_dark_chrome(view, TRUE); /* dark until a real page commits (on_load_changed) */

    webkit_settings_set_user_agent(get_shared_settings(), "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15");

    GtkWidget* img = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(img), TAB_ICON_SIZE);
    gtk_image_set_from_icon_name(GTK_IMAGE(img), "folder-earth-symbolic");
    GtkWidget* btn = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(btn), img);
    gtk_widget_add_css_class(btn, "tab");
    gtk_widget_set_focusable(btn, FALSE); /* no focus ring on tab buttons */
    g_object_set_data(G_OBJECT(view), "icon", img);
    g_object_set_data(G_OBJECT(view), "button", btn);
    GtkGesture* click = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_tab_pressed), view);
    gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(click));
    gtk_box_append(w->tabbar, btn);

    g_signal_connect(view, "create", G_CALLBACK(on_create_tab), NULL);
    g_signal_connect(view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), NULL);
    g_signal_connect(view, "load-failed", G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(view, "decide-policy", G_CALLBACK(on_decide_policy), NULL);
    g_signal_connect(view, "permission-request", G_CALLBACK(on_permission_request), NULL);
    g_signal_connect(view, "notify::favicon", G_CALLBACK(on_favicon_notify), NULL);
    g_signal_connect(webkit_web_view_get_find_controller(view), "counted-matches", G_CALLBACK(on_counted_matches), NULL);

    g_signal_connect(view, "notify::estimated-load-progress", G_CALLBACK(on_load_progress), NULL);
    g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(view, "mouse-target-changed", G_CALLBACK(on_mouse_target), NULL);

    WebKitUserContentManager* ucm = webkit_web_view_get_user_content_manager(view);
    webkit_user_content_manager_register_script_message_handler(ucm, "linkFocus", NULL);
    g_signal_connect(ucm, "script-message-received::linkFocus", G_CALLBACK(on_link_focus_message), view);
    WebKitUserScript* script = webkit_user_script_new(LINK_FOCUS_JS,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);

    /* Align navigator.platform with the macOS user agent (see NAVIGATOR_SPOOF_JS). */
    WebKitUserScript* nav = webkit_user_script_new(NAVIGATOR_SPOOF_JS,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, nav);
    webkit_user_script_unref(nav);

    /* Make media paste reach page scripts (see PASTE_SHIM_JS). */
    WebKitUserScript* paste = webkit_user_script_new(PASTE_SHIM_JS,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, paste);
    webkit_user_script_unref(paste);

    /* Attach native ad-block content filters to this tab (adds any that are already
     * compiled, and back-fills the rest as compilation finishes). */
    adblock_apply_to_view(view);

    /* The selection + (dark-only) default-text stylesheets are installed by
     * view_set_dark_chrome (called above), which re-runs them on every load. */

    int n = gtk_notebook_append_page(w->notebook, GTK_WIDGET(view), NULL);
    gtk_notebook_set_current_page(w->notebook, n);
    tab_touch(view); /* seed last-active so the sleep sweep doesn't fire immediately */
    w->num_tabs += 1;
    session_save_queue(); /* a tab appeared: record it without waiting for the exit */
    return view;
}

/* The MAX_NUM_TABS limit is only enforced by the ctrl+t shortcut; tabs opened
 * by a page (window.open / target=_blank) or from another app are never blocked. */
static void notebook_create_new_tab(Win* w, const char* uri)
{
    WebKitWebView* view = append_tab(w, NULL);
    load_uri(view, uri ? uri : "about:blank"); /* about:blank spins up the web process */
}

/* Restore a tab without loading it: the tab appears dimmed with its URL remembered
 * (the same "dead" state a slept tab uses), and revive_if_dead loads it on first
 * switch. Startup then spawns one web process, not one per saved tab. */
static void notebook_create_lazy_tab(Win* w, const char* uri)
{
    WebKitWebView* view = append_tab(w, NULL);
    g_object_set_data_full(G_OBJECT(view), "reload-uri", g_strdup(uri), g_free);
    g_object_set_data(G_OBJECT(view), "dead", GINT_TO_POINTER(1));
    tab_set_asleep(view, TRUE);
}

static GtkWidget* on_create_tab(WebKitWebView* self,
    WebKitNavigationAction* action G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
    /* Return a real related view so WebKit drives the popup itself, preserving
     * window.opener. The old code stopped the opener and reloaded the URL in a
     * detached tab, which broke Rewards/OAuth completion (see append_tab). */
    Win* w = win_of(self);
    modal_hide_for_new_tab(w); /* the popup lands on screen; a stale modal over it doesn't */
    return GTK_WIDGET(append_tab(w, self));
}

/* The page's slot in the MRU list, or -1 if it isn't tracked. */
static int mru_index(Win* w, GtkWidget* page)
{
    for (int i = 0; i < w->mru_len; i++)
        if (w->mru[i] == page)
            return i;
    return -1;
}

/* Move (or insert) a page at the front of the MRU list, capped at MRU_HISTORY
 * (the oldest entry falls off the back once it's full). */
static void mru_promote(Win* w, GtkWidget* page)
{
    int at = mru_index(w, page);
    if (at < 0)
        at = (w->mru_len < MRU_HISTORY) ? w->mru_len++ : MRU_HISTORY - 1; /* drop the oldest */
    for (int i = at; i > 0; i--)
        w->mru[i] = w->mru[i - 1];
    w->mru[0] = page;
}

/* Drop a page from the MRU list (its tab was closed) so it can't linger as an
 * alt+tab target. Reopening it later (ctrl+shift+t) re-promotes it to the front. */
static void mru_remove(Win* w, GtkWidget* page)
{
    int at = mru_index(w, page);
    if (at < 0)
        return;
    for (int i = at; i < w->mru_len - 1; i++)
        w->mru[i] = w->mru[i + 1];
    w->mru[--w->mru_len] = NULL;
    w->alt_walk = -1; /* the list shifted under any walk in progress; restart it */
}

static void push_closed(WebKitWebViewSessionState* st)
{
    if (closed_count == CLOSED_TAB_HISTORY) {
        webkit_web_view_session_state_unref(closed_tabs[0]);
        memmove(closed_tabs, closed_tabs + 1, sizeof(closed_tabs[0]) * (CLOSED_TAB_HISTORY - 1));
        closed_count -= 1;
    }
    closed_tabs[closed_count++] = st;
}

static void close_current_tab(Win* w)
{
    WebKitWebView* view = current_view(w);
    if (view == NULL)
        return;
    push_closed(webkit_web_view_get_session_state(view));

    mru_remove(w, GTK_WIDGET(view));
    GtkWidget* btn = g_object_get_data(G_OBJECT(view), "button");
    if (btn != NULL)
        gtk_box_remove(w->tabbar, btn);
    gtk_notebook_remove_page(w->notebook, gtk_notebook_get_current_page(w->notebook));
    w->num_tabs -= 1;
    if (w->num_tabs <= 0) { /* no homepage to fall back to: this closes the window */
        if (!w->is_main)
            window_destroy_scratch(w);
        else if (resident)
            window_hide_to_resident(w); /* saves first: 0 tabs clears the session */
        else {
            session_save(); /* 0 tabs left -> clears the saved session */
            gtk_window_destroy(w->window);
        }
        return;
    }
    session_save_queue();
}

static void reopen_closed_tab(Win* w)
{
    if (closed_count == 0)
        return;
    WebKitWebViewSessionState* st = closed_tabs[--closed_count];
    WebKitWebView* view = append_tab(w, NULL);
    webkit_web_view_restore_session_state(view, st);
    WebKitBackForwardList* bf = webkit_web_view_get_back_forward_list(view);
    WebKitBackForwardListItem* item = webkit_back_forward_list_get_current_item(bf);
    if (item != NULL)
        webkit_web_view_go_to_back_forward_list_item(view, item);
    else
        load_uri(view, "about:blank");
    webkit_web_view_session_state_unref(st);
}

/* ------------------------------------------------------ session restore */
/* Persist the main window's tabs to SESSION_FILE so the next cold start can bring
 * them back. Called on window close and on SIGTERM (logout / poweroff). With no
 * real tabs open it removes the file, so closing everything starts fresh. */
static void session_save(void)
{
    /* Only the main window is the session: scratch windows are throwaway by
     * design, so their tabs must never end up in (or wipe) the saved list. */
    Win* w = main_win;
    if (w == NULL || w->notebook == NULL)
        return;
    GString* s = g_string_new(NULL);
    int n = gtk_notebook_get_n_pages(w->notebook);
    for (int i = 0; i < n; i++) {
        WebKitWebView* v = WEBKIT_WEB_VIEW(gtk_notebook_get_nth_page(w->notebook, i));
        const char* uri = webkit_web_view_get_uri(v);
        /* A lazily-restored tab the user never switched to has no URI of its own —
         * it was never loaded — so fall back to the URL we're holding for it.
         * Without this, restoring N tabs and reopening only one silently drops the
         * other N-1 from the session the next time it's saved. */
        if (uri == NULL || uri[0] == '\0')
            uri = g_object_get_data(G_OBJECT(v), "reload-uri");
        if (uri == NULL || uri[0] == '\0' || g_str_has_prefix(uri, "about:"))
            continue;
        g_string_append(s, uri);
        g_string_append_c(s, '\n');
    }
    if (s->len > 0)
        g_file_set_contents(SESSION_FILE, s->str, s->len, NULL);
    else
        g_unlink(SESSION_FILE);
    g_string_free(s, TRUE);
}

static guint session_save_source = 0;

static gboolean session_save_now(gpointer data)
{
    session_save_source = 0;
    session_save();
    return G_SOURCE_REMOVE;
}

/* Save the session shortly after the tabs settle, rather than only on the way out.
 * Exit-time saving alone loses everything when the compositor goes first: GDK's
 * Wayland loop `_exit()`s the moment it loses the connection, so neither the
 * close-request handler nor the SIGTERM handler runs — which is why tabs could
 * disappear across a shutdown. Debounced, so a redirect chain writes once. */
static void session_save_queue(void)
{
    if (tearing_down || main_win == NULL)
        return;
    if (session_save_source != 0)
        g_source_remove(session_save_source);
    session_save_source = g_timeout_add_seconds(SESSION_SAVE_SECONDS, session_save_now, NULL);
}

/* Is there a session to restore? A bare stat, so startup can decide whether to
 * show the search modal *before* the window is presented, without doing any of
 * the actual (WebKit-touching) restore work first. */
static gboolean session_exists(void)
{
    return g_file_test(SESSION_FILE, G_FILE_TEST_EXISTS);
}

/* Reload the saved tabs into the freshly-built window. Returns TRUE if at least
 * one tab was restored. `wake` loads the tab that ends up on screen; a launch that
 * is about to append a tab of its own (a link from another app) passes FALSE, so
 * the restored tabs all stay asleep behind the page the user actually asked for. */
static gboolean session_restore(Win* w, gboolean wake)
{
    char* contents = NULL;
    if (!g_file_get_contents(SESSION_FILE, &contents, NULL, NULL))
        return FALSE;
    char** lines = g_strsplit(contents, "\n", -1);
    g_free(contents);

    gboolean any = FALSE;
    for (char** l = lines; *l != NULL; l++) {
        if ((*l)[0] == '\0')
            continue;
        notebook_create_lazy_tab(w, *l);
        any = TRUE;
    }
    g_strfreev(lines);
    /* Every restored tab is lazy; wake only the one on screen (the last appended). */
    if (wake)
        revive_if_dead(current_view(w));
    return any;
}

/* ----------------------------------------------------------------- modal */
/* modal_results holds modal_entry1, then the persistent calc_label, then the
 * bookmark rows -- so the box's spacing only grows when rows exist. Clear the
 * bookmark rows only, keeping the entry and calc_label in place. */
static void clear_results(Win* w)
{
    GtkWidget* c;
    while ((c = gtk_widget_get_last_child(GTK_WIDGET(w->modal_results))) != GTK_WIDGET(w->calc_label))
        gtk_box_remove(w->modal_results, c);
}

/* Hide the live calculation result and forget its value. */
static void calc_clear(Win* w)
{
    w->calc_active = FALSE;
    w->calc_result[0] = '\0';
    gtk_widget_set_visible(GTK_WIDGET(w->calc_label), FALSE);
}

static void modal_hide(Win* w)
{
    w->modal_mode = MODAL_NONE; /* first: it's what stops modal_keep_focus holding on */
    w->modal_focus = NULL;
    gtk_widget_set_visible(w->dim, FALSE);
    gtk_widget_set_visible(w->modal_box, FALSE);
    gtk_entry_set_attributes(w->modal_entry1, NULL);
    clear_results(w);
    calc_clear(w);
    w->fuzzy_count = 0;
    w->fuzzy_sel = -1;
}

/* A tab that appears from outside the modal's own flow — a link handed to us by
 * another app, a popup opened by a page — takes over the window, so a search or
 * bookmark modal left on screen is no longer what the keyboard is for: close it.
 * A pending permission prompt is deliberately left up: it owns the keyboard until
 * it's answered, and hiding it would strand the request (perm_current would never
 * be decided, swallowing every key from then on).
 *
 * Only these outside-driven paths hide it, not append_tab itself: startup shows the
 * search modal *before* the deferred phase creates the first tab, so hiding on every
 * new tab would take the modal back off a blank launch. */
static void modal_hide_for_new_tab(Win* w)
{
    if (w != NULL && w->modal_mode != MODAL_NONE && w->modal_mode != MODAL_PERMISSION)
        modal_hide(w);
}

/* Which modal entry `focus` belongs to, or NULL if it isn't in one. Focus lands on
 * an entry's inner GtkText rather than the GtkEntry itself (that is what
 * gtk_window_get_focus reports), so the widget has to be walked up to its entry. */
static GtkWidget* modal_entry_of(Win* w, GtkWidget* focus)
{
    for (GtkWidget* p = focus; p != NULL; p = gtk_widget_get_parent(p)) {
        if (p == GTK_WIDGET(w->modal_entry1) || p == GTK_WIDGET(w->modal_entry2))
            return p;
    }
    return NULL;
}

/* While a modal is up the keyboard is its own: anything that takes focus away —
 * a click landing on the page through the click-through dim, a page calling
 * focus() as it loads, a new tab handing focus to the notebook — gets it handed
 * straight back. Without this the modal sits there looking focused while what you
 * type goes into the page instead.
 *
 * Connected to the window's focus-widget, so it only ever moves focus *within* our
 * window; alt-tabbing away and back is the compositor's business and is untouched.
 * The permission prompt has nothing focusable (its entries are hidden) and doesn't
 * need it: its keys are handled on the window in the capture phase. */
static void modal_keep_focus(GtkWindow* window, GParamSpec* pspec G_GNUC_UNUSED, gpointer data)
{
    Win* w = data;
    if (w->modal_mode == MODAL_NONE || w->modal_mode == MODAL_PERMISSION)
        return;

    GtkWidget* entry = modal_entry_of(w, gtk_window_get_focus(window));
    if (entry != NULL) {
        w->modal_focus = entry; /* remember it, so a Tab to the url entry sticks */
        return;
    }
    /* Re-grabbing emits focus-widget again, but by then focus is on the entry and
     * the branch above returns — no recursion. */
    gtk_widget_grab_focus(w->modal_focus != NULL && gtk_widget_get_visible(w->modal_focus)
            ? w->modal_focus
            : GTK_WIDGET(w->modal_entry1));
}

static void modal_show(Win* w, ModalMode mode, gboolean open_new_tab)
{
    w->modal_mode = mode;
    w->modal_new_tab = open_new_tab;
    w->modal_blocked = FALSE;
    w->fuzzy_count = 0;
    w->fuzzy_sel = -1;
    clear_results(w);
    calc_clear(w);
    gtk_entry_set_attributes(w->modal_entry1, NULL);
    gtk_widget_set_visible(GTK_WIDGET(w->modal_entry1), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(w->modal_results), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(w->modal_info), FALSE); /* only shown for "Tab limit reached" */
    gtk_widget_set_halign(GTK_WIDGET(w->modal_info), GTK_ALIGN_START); /* undo permission-prompt centering */

    if (mode == MODAL_SEARCH || mode == MODAL_PASSWORD) {
        gtk_widget_set_visible(GTK_WIDGET(w->modal_entry2), FALSE);
        gtk_editable_set_text(GTK_EDITABLE(w->modal_entry1), "");
    } else { /* MODAL_BOOKMARK */
        gtk_widget_set_visible(GTK_WIDGET(w->modal_entry2), TRUE);
        WebKitWebView* v = current_view(w);
        const char* title = v ? webkit_web_view_get_title(v) : NULL;
        const char* uri = v ? webkit_web_view_get_uri(v) : NULL;
        gtk_editable_set_text(GTK_EDITABLE(w->modal_entry1), title ? title : "");
        gtk_editable_set_text(GTK_EDITABLE(w->modal_entry2), uri ? uri : "");
    }

    gtk_widget_set_visible(w->dim, TRUE);
    gtk_widget_set_visible(w->modal_box, TRUE);
    w->modal_focus = GTK_WIDGET(w->modal_entry1);
    gtk_widget_grab_focus(GTK_WIDGET(w->modal_entry1));
    gtk_editable_select_region(GTK_EDITABLE(w->modal_entry1), 0, -1);
}

/* Show a confirm-only prompt (no entry, no results) in the shared modal for a
 * permission request: `markup` is the Pango text, Enter allows / Esc denies
 * (handled in handle_signal_keypress). Reuses the search/bookmark dim + box. */
static void modal_show_permission(Win* w, const char* markup)
{
    w->modal_mode = MODAL_PERMISSION;
    w->modal_blocked = FALSE;
    w->fuzzy_count = 0;
    w->fuzzy_sel = -1;
    clear_results(w);
    calc_clear(w);
    gtk_entry_set_attributes(w->modal_entry1, NULL);
    gtk_widget_set_visible(GTK_WIDGET(w->modal_entry1), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(w->modal_entry2), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(w->modal_results), FALSE);

    gtk_label_set_markup(w->modal_info, markup);
    gtk_label_set_justify(w->modal_info, GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(GTK_WIDGET(w->modal_info), GTK_ALIGN_CENTER);
    gtk_widget_set_visible(GTK_WIDGET(w->modal_info), TRUE);

    gtk_widget_set_visible(w->dim, TRUE);
    gtk_widget_set_visible(w->modal_box, TRUE);
}

static void modal_restyle_results(Win* w)
{
    int i = 0;
    for (GtkWidget* c = gtk_widget_get_next_sibling(GTK_WIDGET(w->modal_entry1)); c != NULL; c = gtk_widget_get_next_sibling(c)) {
        if (c == GTK_WIDGET(w->calc_label))
            continue; /* the calc result is a label, not a selectable row */
        if (i == w->fuzzy_sel)
            gtk_widget_add_css_class(c, "selected");
        else
            gtk_widget_remove_css_class(c, "selected");
        i++;
    }
}

static void modal_move_sel(Win* w, int dir)
{
    if (w->fuzzy_count == 0)
        return;
    w->fuzzy_sel += dir;
    if (w->fuzzy_sel < 0)
        w->fuzzy_sel = 0;
    if (w->fuzzy_sel >= (int)w->fuzzy_count)
        w->fuzzy_sel = w->fuzzy_count - 1;
    modal_restyle_results(w);
}

/* Speculative DNS: as soon as the typed text looks like a host, resolve it so
 * the lookup is already done (or in flight) by the time Enter fires. WebKit dedups
 * repeats, so calling it on each keystroke is cheap. */
static void prefetch_host(const char* text)
{
    const char* p = strstr(text, "://");
    p = p ? p + 3 : text;
    if (strchr(p, ' ') != NULL || strchr(p, '.') == NULL)
        return; /* not a bare host (a search query or a shortcut) */
    const char* end = strchr(p, '/');
    gsize len = end ? (gsize)(end - p) : strlen(p);
    if (len == 0 || len > 253)
        return;
    char* host = g_strndup(p, len);
    webkit_network_session_prefetch_dns(get_shared_network_session(), host);
    g_free(host);
}

/* Rebuild the password picker's result rows for the current host + typed filter. */
static void password_refresh_rows(Win* w, const char* query)
{
    clear_results(w);
    w->fuzzy_sel = -1;
    w->fuzzy_count = passwords_match(w->pass_host, query, w->pass_entries, FUZZY_RESULTS);
    for (guint i = 0; i < w->fuzzy_count; i++) {
        GtkWidget* row = gtk_label_new(w->pass_entries[i]);
        gtk_label_set_xalign(GTK_LABEL(row), 0.0);
        gtk_box_append(w->modal_results, row);
    }
}

static void on_search_changed(GtkEditable* editable, gpointer data)
{
    Win* w = data;
    if (w->modal_mode == MODAL_PASSWORD) {
        password_refresh_rows(w, gtk_editable_get_text(editable));
        return;
    }
    if (w->modal_mode != MODAL_SEARCH)
        return;
    const char* text = gtk_editable_get_text(editable);

    /* Highlight a leader (nx, g, ...) so it's clear the search engine changes. */
    PangoAttrList* attrs = pango_attr_list_new();
    int ll = shortcut_leader_len(text);
    if (ll > 0) {
        PangoAttribute* bg = pango_attr_background_new(0x5e * 257, 0x81 * 257, 0xac * 257);
        bg->start_index = 0;
        bg->end_index = ll;
        pango_attr_list_insert(attrs, bg);
        PangoAttribute* fg = pango_attr_foreground_new(0xffff, 0xffff, 0xffff);
        fg->start_index = 0;
        fg->end_index = ll;
        pango_attr_list_insert(attrs, fg);
    }
    gtk_entry_set_attributes(w->modal_entry1, attrs);
    pango_attr_list_unref(attrs);

    /* Fuzzy-match bookmarks once 2+ chars are typed and there's no leader. */
    clear_results(w);
    w->fuzzy_count = 0;
    w->fuzzy_sel = -1;

    /* Live calculator: show the result under the box when the text is a valid
     * expression (a cheap synchronous parse -- no blocking). Skip when a
     * shortcut leader is active, since that's a search, not a sum. */
    calc_clear(w);
    if (ll == 0 && calc_eval(text, w->calc_result, sizeof w->calc_result)) {
        char* shown = g_strconcat("= ", w->calc_result, NULL);
        gtk_label_set_text(w->calc_label, shown);
        g_free(shown);
        gtk_widget_set_visible(GTK_WIDGET(w->calc_label), TRUE);
        w->calc_active = TRUE;
    }

    if (ll == 0)
        prefetch_host(text); /* warm DNS for a directly-typed host */
    if (ll == 0 && strlen(text) >= 2) {
        const char* names[FUZZY_RESULTS];
        w->fuzzy_count = bookmarks_fuzzy(text, names, w->fuzzy_urls, FUZZY_RESULTS);
        for (guint i = 0; i < w->fuzzy_count; i++) {
            GtkWidget* row = gtk_label_new(names[i]);
            gtk_label_set_xalign(GTK_LABEL(row), 0.0);
            gtk_box_append(w->modal_results, row);
            prefetch_host(w->fuzzy_urls[i]); /* warm DNS for matching bookmarks */
        }
    }
}

/* ------------------------------------------------- reverse image search */
/* Ctrl+V in the search modal with an image (and no text) on the clipboard
 * reverse-searches it on Google Lens. The modal closes at once and the tab does
 * the rest -- the upload happens inside it (see plugins/imagesearch), so there
 * is nothing to wait on here and no progress to show. A clipboard that turns
 * out to be unreadable is reported through the statusbar flash. */
/* The plugin reports failures through a plain callback with no context, so the
 * window that asked for the search is remembered here for the length of it. */
static Win* imagesearch_win = NULL;

static void imagesearch_error(const char* reason)
{
    char* msg = g_strdup_printf("Image search failed: %s", reason);
    status_flash_message(imagesearch_win != NULL ? imagesearch_win : cur(), msg);
    g_free(msg);
}

/* Returns TRUE when the paste was consumed as an image search. */
static gboolean modal_try_image_paste(Win* w)
{
    GdkClipboard* clipboard = gtk_widget_get_clipboard(GTK_WIDGET(w->modal_entry1));
    if (!imagesearch_clipboard_has_image(clipboard))
        return FALSE; /* an ordinary text paste */
    if (w->modal_blocked) { /* tab limit reached: nowhere to put the results */
        modal_hide(w);
        return TRUE;
    }
    gboolean want_new_tab = w->modal_new_tab || current_view(w) == NULL;
    modal_hide(w); /* before the tab appears, so the results are all that is left */
    WebKitWebView* target = want_new_tab ? append_tab(w, NULL) : current_view(w);
    imagesearch_win = w;
    imagesearch_run(clipboard, target, imagesearch_error);
    return TRUE;
}

static void modal_open_search_uri(Win* w, const char* uri)
{
    if (w->modal_new_tab || current_view(w) == NULL) /* no warm tab yet -> make one */
        notebook_create_new_tab(w, uri);
    else
        load_uri(current_view(w), uri);
    modal_hide(w);
}

/* JS injected into the page to fill a login form. `username`/`password` arrive
 * as function arguments (never interpolated into the source) so the secret stays
 * out of the code string and escaping can't break. Values are written through the
 * native value setter + a full event burst so framework-controlled inputs (React,
 * Vue, Angular, ...) pick them up. The form is NOT submitted -- the user presses
 * Enter. Multi-step logins (email page first, password page later) have no password
 * field yet: then only the username is filled; run the picker again on the next step.
 *
 * Field detection mirrors PassFF: every visible, writable input is *scored* against
 * a keyword list by inspecting its id, name, autocomplete, placeholder and its
 * <label>/aria-label text (not just id/name/autocomplete via CSS selectors, which
 * misses fields that only announce themselves through a placeholder or label). id
 * and name weigh 10x; an exact keyword match scores 2, a substring 1. The
 * highest-scoring text/email/tel field is the username; the highest-scoring
 * password field (native type=password always qualifies) is the password. When a
 * password field exists we prefer a username field inside the same <form>, so a
 * newsletter/search box elsewhere on the page can't steal the fill. If nothing
 * scores, we fall back to the visible text/email/tel input just before the
 * password (or the first one, on a password-less step). */
static const char* PASSWORD_FILL_JS =
    "const loginNames = ['login','user','mail','email','tel','username',"
    "'opt_login','log','usr_name'];\n"
    "const pwNames = ['passwd','password','pass'];\n"
    "const vis = el => el.offsetHeight !== 0 && el.offsetParent !== null"
    " && !el.disabled && !el.readOnly;\n"
    /* Gather every <input>, descending into open shadow roots (login widgets). */
    "const collect = (root, out, seen) => {\n"
    "  if (!root || seen.has(root)) return out; seen.add(root);\n"
    "  let all; try { all = root.querySelectorAll('*'); } catch (e) { return out; }\n"
    "  for (const el of all) {\n"
    "    if (el.tagName === 'INPUT') out.push(el);\n"
    "    if (el.shadowRoot) collect(el.shadowRoot, out, seen);\n"
    "  }\n"
    "  return out;\n"
    "};\n"
    "const labelText = el => {\n"
    "  let t = el.getAttribute('aria-label') || '';\n"
    "  if (el.labels) for (const l of el.labels) t += ' ' + (l.textContent || '');\n"
    "  return t;\n"
    "};\n"
    "const rate = (el, names) => {\n"
    "  const attrs = [el.id || '', el.name || '', el.autocomplete || '',"
    " el.placeholder || '', labelText(el)];\n"
    "  let score = 0;\n"
    "  attrs.forEach((a, i) => {\n"
    "    const v = a.toLowerCase(); const mult = i < 2 ? 10 : 1;\n"
    "    for (const n of names) {\n"
    "      if (v === n) score += 2 * mult; else if (v.includes(n)) score += mult;\n"
    "    }\n"
    "  });\n"
    "  return score;\n"
    "};\n"
    "const setVal = (el, val) => {\n"
    "  el.focus();\n"
    "  const d = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(el), 'value')\n"
    "         || Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value');\n"
    "  (d && d.set ? d.set.bind(el) : (v => { el.value = v; }))(val);\n"
    "  for (const t of ['keydown','keypress','keyup','input','change'])\n"
    "    el.dispatchEvent(new Event(t, { bubbles: true }));\n"
    "};\n"
    "const inputs = collect(document, [], new Set()).filter(vis);\n"
    "const isText = el => { const t = (el.type || 'text').toLowerCase();\n"
    "  return t === 'text' || t === 'email' || t === 'tel'; };\n"
    /* Best-scoring password field; a native type=password always qualifies. */
    "let pw = null, pwScore = 0;\n"
    "for (const el of inputs) {\n"
    "  const t = (el.type || '').toLowerCase();\n"
    "  if (t !== 'password' && t !== 'text') continue;\n"
    "  let s = rate(el, pwNames); if (t === 'password' && s === 0) s = 19;\n"
    "  if (s > pwScore) { pwScore = s; pw = el; }\n"
    "}\n"
    /* Best-scoring username field, preferring the password's own form. */
    "const bestLogin = pool => {\n"
    "  let best = null, bs = 0;\n"
    "  for (const el of pool) {\n"
    "    if (el === pw || !isText(el)) continue;\n"
    "    const s = rate(el, loginNames);\n"
    "    if (s > bs) { bs = s; best = el; }\n"
    "  }\n"
    "  return best;\n"
    "};\n"
    "let login = null;\n"
    "if (username) {\n"
    "  if (pw && pw.form) login = bestLogin(inputs.filter(el => el.form === pw.form));\n"
    "  if (!login) login = bestLogin(inputs);\n"
    "  if (!login) {\n"
    "    const texts = inputs.filter(el => el !== pw && isText(el));\n"
    "    const before = pw ? texts.filter(el =>\n"
    "      el.compareDocumentPosition(pw) & Node.DOCUMENT_POSITION_FOLLOWING) : texts;\n"
    "    login = before[before.length - 1] || before[0] || texts[0] || null;\n"
    "  }\n"
    "}\n"
    "if (!pw && !login) return false;\n"
    "if (login) setVal(login, username);\n"
    "if (pw) { setVal(pw, password); pw.focus(); } else login.focus();\n"
    "return true;\n";

/* passwords_show_async callback: got the decrypted credentials, now fill the form. */
static void on_password_ready(const char* username, const char* password, gpointer data)
{
    WebKitWebView* view = data;
    if (view == NULL || !WEBKIT_IS_WEB_VIEW(view) || password == NULL)
        return;

    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "username", g_variant_new_string(username ? username : ""));
    g_variant_builder_add(&b, "{sv}", "password", g_variant_new_string(password));
    GVariant* args = g_variant_ref_sink(g_variant_builder_end(&b));

    webkit_web_view_call_async_javascript_function(view, PASSWORD_FILL_JS, -1,
        args, NULL, NULL, NULL, NULL, NULL);
    g_variant_unref(args);
}

static void on_modal_activate(GtkEntry* entry, gpointer data)
{
    Win* w = data;
    if (w->modal_blocked) {
        modal_hide(w);
        return;
    }
    if (w->modal_mode == MODAL_PASSWORD) {
        /* Enter fills the highlighted entry, or the top match if none is picked. */
        int sel = w->fuzzy_sel >= 0 ? w->fuzzy_sel : (w->fuzzy_count > 0 ? 0 : -1);
        if (sel >= 0) {
            WebKitWebView* target = w->pass_target;
            char* sel_entry = g_strdup(w->pass_entries[sel]);
            modal_hide(w);
            passwords_show_async(sel_entry, on_password_ready, target);
            g_free(sel_entry);
        } else {
            modal_hide(w);
        }
        return;
    }
    if (w->modal_mode == MODAL_SEARCH) {
        if (w->fuzzy_sel >= 0) {
            modal_open_search_uri(w, w->fuzzy_urls[w->fuzzy_sel]);
            return;
        }
        /* No bookmark picked but a calculation is showing: copy the value. */
        if (w->calc_active) {
            gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(w->modal_entry1)), w->calc_result);
            modal_hide(w);
            return;
        }
        modal_open_search_uri(w, gtk_editable_get_text(GTK_EDITABLE(w->modal_entry1)));
        return;
    }
    /* MODAL_BOOKMARK -- trim both fields so a pasted URL/name with surrounding
     * whitespace is stored clean (and later navigates instead of searching). */
    char* name = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(w->modal_entry1))));
    char* url = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(w->modal_entry2))));
    if (name[0] != '\0' && url[0] != '\0')
        bookmarks_save(BOOKMARKS_DIR, name, url);
    g_free(name);
    g_free(url);
    modal_hide(w);
}

/* ------------------------------------------------------------------- find */
static void update_find_label(Win* w)
{
    char* s = g_strdup_printf("%u of %u", w->find_total ? w->find_current : 0, w->find_total);
    gtk_label_set_text(w->find_label, s);
    g_free(s);
}

/* The count lands asynchronously, so resolve the window from the controller's own
 * view rather than trusting anything captured at connect time -- and ignore a count
 * for a tab that is no longer on screen, whose result would otherwise overwrite the
 * label with a total the user isn't looking at. */
static void on_counted_matches(WebKitFindController* fc, guint count, gpointer data)
{
    WebKitWebView* view = webkit_find_controller_get_web_view(fc);
    Win* w = win_of(view);
    if (w == NULL || view != current_view(w))
        return;
    w->find_total = count;
    update_find_label(w);
}

/* (Re)issue a fresh search on the live DOM. WebKit's search() both re-highlights
 * all matches and advances the selection to the next match in document order, so
 * calling it on every step keeps results up to date (content revealed since the
 * last search is picked up) without glitching the order -- it's one step, not two.
 * count_matches refreshes the "N of M" total against the same live DOM. */
static void find_step(Win* w, WebKitFindOptions dir)
{
    WebKitWebView* v = current_view(w);
    if (v == NULL)
        return;
    const char* text = gtk_editable_get_text(GTK_EDITABLE(w->find_entry));
    WebKitFindController* fc = webkit_web_view_get_find_controller(v);
    WebKitFindOptions opts = WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND | dir;
    if (text[0] == '\0') {
        webkit_find_controller_search_finish(fc);
        w->find_total = 0;
        w->find_current = 0;
    } else {
        webkit_find_controller_count_matches(fc, text, opts, G_MAXUINT);
        webkit_find_controller_search(fc, text, opts, G_MAXUINT);
        if (w->find_total > 0)
            w->find_current = (dir & WEBKIT_FIND_OPTIONS_BACKWARDS)
                ? (w->find_current + w->find_total - 2) % w->find_total + 1
                : (w->find_current % w->find_total) + 1;
        else
            w->find_current = 1;
    }
    update_find_label(w);
}

static void do_find(Win* w, const char* text)
{
    WebKitWebView* v = current_view(w);
    if (v == NULL)
        return;
    WebKitFindController* fc = webkit_web_view_get_find_controller(v);
    WebKitFindOptions opts = WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND;
    if (text[0] == '\0') {
        webkit_find_controller_search_finish(fc);
        w->find_total = 0;
        w->find_current = 0;
    } else {
        webkit_find_controller_count_matches(fc, text, opts, G_MAXUINT);
        webkit_find_controller_search(fc, text, opts, G_MAXUINT);
        w->find_current = 1;
    }
    update_find_label(w);
}

static void on_find_changed(GtkEditable* editable, gpointer data)
{
    Win* w = data;
    do_find(w, gtk_editable_get_text(editable));
}

static void find_next(Win* w)
{
    find_step(w, WEBKIT_FIND_OPTIONS_NONE);
}

static void find_prev(Win* w)
{
    find_step(w, WEBKIT_FIND_OPTIONS_BACKWARDS);
}

static void on_find_activate(GtkEntry* entry, gpointer data)
{
    Win* w = data;
    find_next(w); /* Enter = next; Shift+Enter (handled in keypress) = previous */
}

static void find_show(Win* w)
{
    /* If the bar is already open, its matches are still highlighted from the
     * previous search -- Ctrl+F must leave them (and the "N of M" position)
     * exactly as they are and only re-focus the box. When reopening a closed
     * bar there are no highlights left (find_hide cleared them), so re-run the
     * search to restore them. */
    gboolean was_visible = gtk_widget_get_visible(w->findbar);
    gtk_widget_set_visible(w->findbar, TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(w->find_entry));
    gtk_editable_select_region(GTK_EDITABLE(w->find_entry), 0, -1);
    if (!was_visible)
        do_find(w, gtk_editable_get_text(GTK_EDITABLE(w->find_entry)));
}

static void find_hide(Win* w)
{
    WebKitWebView* v = current_view(w);
    if (v != NULL)
        webkit_find_controller_search_finish(webkit_web_view_get_find_controller(v));
    gtk_widget_set_visible(w->findbar, FALSE);
}

/* Fetch the page's serialized HTML, then show it as plain text in a new tab. */
static void on_source_ready(GObject* obj, GAsyncResult* res, gpointer data)
{
    WebKitWebView* view = WEBKIT_WEB_VIEW(obj);
    GError* err = NULL;
    JSCValue* val = webkit_web_view_evaluate_javascript_finish(view, res, &err);
    if (val == NULL) {
        if (err != NULL) g_error_free(err);
        return;
    }
    char* html = jsc_value_to_string(val);
    const char* base = webkit_web_view_get_uri(view);

    WebKitWebView* src = append_tab(win_of(view), NULL);
    GBytes* bytes = g_bytes_new(html, strlen(html));
    webkit_web_view_load_bytes(src, bytes, "text/plain", "utf-8", base);

    g_bytes_unref(bytes);
    g_free(html);
    g_object_unref(val);
}

/* ------------------------------------------------------------- shortcuts */
static void handle_shortcut(Win* w, func id)
{
    static double zoom = 1.0;
    WebKitWebView* view = current_view(w);

    switch (id) {
        case goback:
            if (view) webkit_web_view_go_back(view);
            break;
        case goforward:
            if (view) webkit_web_view_go_forward(view);
            break;
        case refresh:
            if (view) webkit_web_view_reload(view);
            break;
        case refresh_force:
            if (view) webkit_web_view_reload_bypass_cache(view);
            break;
        case zoomin:
            if (view) webkit_web_view_set_zoom_level(view, (zoom += ZOOM_STEPSIZE));
            break;
        case zoomout:
            if (view) webkit_web_view_set_zoom_level(view, (zoom -= ZOOM_STEPSIZE));
            break;
        case zoom_reset:
            if (view) webkit_web_view_set_zoom_level(view, (zoom = 1.0));
            break;
        case tab_up: { /* up the list; do nothing if already at the top */
            int k = gtk_notebook_get_current_page(w->notebook);
            if (k > 0)
                gtk_notebook_set_current_page(w->notebook, k - 1);
            break;
        }
        case tab_down: { /* down the list; do nothing if already at the bottom */
            int n = gtk_notebook_get_n_pages(w->notebook);
            int k = gtk_notebook_get_current_page(w->notebook);
            if (k < n - 1)
                gtk_notebook_set_current_page(w->notebook, k + 1);
            break;
        }
        case last_tab: { /* alt+tab: walk back through the most-recently-used tabs */
            if (w->mru_len < 2)
                break;
            if (w->alt_walk < 0)
                w->alt_walk = 0; /* start the walk at the current (front) tab */
            /* Step to the next entry, wrapping, skipping any whose page has gone. */
            int k = -1;
            for (int tries = 0; tries < w->mru_len; tries++) {
                w->alt_walk = (w->alt_walk + 1) % w->mru_len;
                k = gtk_notebook_page_num(w->notebook, w->mru[w->alt_walk]);
                if (k >= 0)
                    break;
            }
            if (k >= 0) {
                GtkWidget* target = w->mru[w->alt_walk];
                w->alt_switch = TRUE; /* don't let this switch reshuffle the MRU order */
                gtk_notebook_set_current_page(w->notebook, k);
                w->alt_switch = FALSE;
                revive_if_dead(WEBKIT_WEB_VIEW(target)); /* wake it if it was slept */
            }
            break;
        }
        case new_tab: {
            /* If the current tab is already blank (about:blank or nothing
             * loaded), reuse it instead of spawning an empty tab beside it. */
            const char* cur = view ? webkit_web_view_get_uri(view) : NULL;
            gboolean blank = view && (cur == NULL || cur[0] == '\0'
                                      || g_strcmp0(cur, "about:blank") == 0);
            modal_show(w, MODAL_SEARCH, !blank);
            if (MAX_NUM_TABS != 0 && w->num_tabs >= MAX_NUM_TABS) {
                gtk_label_set_text(w->modal_info, "Tab limit reached");
                gtk_widget_set_visible(GTK_WIDGET(w->modal_info), TRUE);
                gtk_widget_set_visible(GTK_WIDGET(w->modal_entry1), FALSE); /* message only */
                gtk_widget_set_visible(GTK_WIDGET(w->modal_results), FALSE); /* drop the empty box's spacing below the label */
                w->modal_blocked = TRUE;
            }
            break;
        }
        case close_tab:
            close_current_tab(w);
            break;
        case reopen_tab:
            reopen_closed_tab(w);
            break;
        case show_finder:
            /* Ctrl+F only opens/focuses the bar and highlights matches; it never
             * moves through results -- only Enter advances to the next match. */
            find_show(w);
            break;
        case find_reset:
            do_find(w, gtk_editable_get_text(GTK_EDITABLE(w->find_entry)));
            break;
        case bookmark_add:
            modal_show(w, MODAL_BOOKMARK, FALSE);
            break;
        case edit_uri: {
            /* Open the search modal pre-filled with the current URL; Enter
             * replaces the current tab (modal_new_tab = FALSE). */
            modal_show(w, MODAL_SEARCH, FALSE);
            const char* uri = view ? webkit_web_view_get_uri(view) : NULL;
            if (uri != NULL) {
                gtk_editable_set_text(GTK_EDITABLE(w->modal_entry1), uri);
                gtk_editable_select_region(GTK_EDITABLE(w->modal_entry1), 0, -1);
            }
            break;
        }
        case toggle_tabs:
            w->tabbar_visible = !w->tabbar_visible;
            gtk_widget_set_visible(GTK_WIDGET(w->tabbar), w->tabbar_visible);
            break;
        case view_source:
            if (view)
                webkit_web_view_evaluate_javascript(view,
                    "document.documentElement.outerHTML", -1, NULL, NULL, NULL,
                    on_source_ready, NULL);
            break;
        case print_page:
            if (view) {
                WebKitPrintOperation* print = webkit_print_operation_new(view);
                webkit_print_operation_run_dialog(print, w->window);
                g_object_unref(print);
            }
            break;
        case reading_mode:
            /* Apply the reader transform (refresh the page to undo it). */
            if (view) {
                char* js = read_readability_js();
                if (js != NULL) {
                    webkit_web_view_evaluate_javascript(view, js, -1, NULL, "lightbrowse-readability", NULL, NULL, NULL);
                    g_free(js);
                }
            }
            break;
        case translate_page: {
            /* Replace the current tab with Google Translate's whole-page proxy
             * of the current URL (auto source -> English). */
            const char* uri = view ? webkit_web_view_get_uri(view) : NULL;
            if (uri != NULL && (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://"))) {
                char* enc = g_uri_escape_string(uri, NULL, TRUE);
                char* translated = g_strdup_printf(
                    "https://translate.google.com/translate?sl=auto&tl=en&u=%s", enc);
                webkit_web_view_load_uri(view, translated);
                g_free(enc);
                g_free(translated);
            }
            break;
        }
        case fill_password: {
            /* Open the password picker for the current page's host. */
            if (view == NULL)
                break;
            w->pass_host[0] = '\0';
            const char* uri = webkit_web_view_get_uri(view);
            if (uri != NULL) {
                GUri* u = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
                if (u != NULL) {
                    const char* host = g_uri_get_host(u);
                    if (host != NULL)
                        g_strlcpy(w->pass_host, host, sizeof w->pass_host);
                    g_uri_unref(u);
                }
            }
            w->pass_target = view;
            modal_show(w, MODAL_PASSWORD, FALSE);
            password_refresh_rows(w, "");
            break;
        }
    }
}

/* ----------------------------------------------------------------- keys */
/* Is the find bar's entry what the keyboard is talking to? Focus lands on the
 * entry's inner GtkText, so walk up the way modal_entry_of does. */
static gboolean find_entry_focused(Win* w)
{
    for (GtkWidget* p = gtk_window_get_focus(w->window); p != NULL; p = gtk_widget_get_parent(p)) {
        if (p == GTK_WIDGET(w->find_entry))
            return TRUE;
    }
    return FALSE;
}

static gboolean handle_signal_keypress(GtkEventControllerKey* self, guint keyval,
    guint keycode, GdkModifierType state, gpointer user_data)
{
    Win* w = user_data;
    /* A pending permission prompt (mic/camera or storage access) owns the keyboard
     * until it's answered: Enter allows, Esc denies, and every other key is swallowed
     * so nothing acts on the page (or closes the tab) out from under the request. */
    if (perm_current != NULL) {
        if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
            perm_decide(TRUE);
        else if (keyval == GDK_KEY_Escape)
            perm_decide(FALSE);
        return TRUE;
    }

    if (keyval == GDK_KEY_Escape) {
        if (w->modal_mode != MODAL_NONE) {
            modal_hide(w);
            return TRUE;
        }
        if (gtk_widget_get_visible(w->findbar)) {
            find_hide(w);
            return TRUE;
        }
        return FALSE;
    }

    /* Shift+Enter in the find bar steps backwards through matches. */
    if (gtk_widget_get_visible(w->findbar) && (state & SFT) && (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)) {
        find_prev(w);
        return TRUE;
    }

    if (w->modal_mode != MODAL_NONE) {
        if (w->modal_mode == MODAL_SEARCH || w->modal_mode == MODAL_PASSWORD) {
            if (keyval == GDK_KEY_Down) {
                modal_move_sel(w, 1);
                return TRUE;
            }
            if (keyval == GDK_KEY_Up) {
                modal_move_sel(w, -1);
                return TRUE;
            }
        }
        if (w->modal_mode == MODAL_SEARCH) {
            /* Pasting an image (rather than text) reverse-searches it on Google
             * Lens -- there's nothing to type into the entry otherwise. */
            if ((state & CTRL) && (keyval == GDK_KEY_v || keyval == GDK_KEY_V)
                && modal_try_image_paste(w))
                return TRUE;
            /* Shift+Enter sends a live calculation to Wolfram|Alpha (as if the
             * user typed "wa <expr>"); otherwise it jumps straight to the top
             * fuzzy bookmark match, skipping the typed-text search. */
            if ((state & SFT) && (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)) {
                if (w->calc_active) {
                    char* tmp = g_strconcat("wa ", gtk_editable_get_text(GTK_EDITABLE(w->modal_entry1)), NULL);
                    char* url = shortcut_expand(tmp);
                    g_free(tmp);
                    if (url != NULL) {
                        modal_open_search_uri(w, url);
                        g_free(url);
                    }
                } else if (w->fuzzy_count > 0) {
                    modal_open_search_uri(w, w->fuzzy_urls[0]);
                }
                return TRUE;
            }
        } else if (keyval == GDK_KEY_Tab) {
            /* Toggle between the bookmark name and url entries. Compared through
             * modal_entry_of because the focus widget is the entry's inner GtkText. */
            GtkWidget* entry = modal_entry_of(w, gtk_window_get_focus(w->window));
            gtk_widget_grab_focus(entry == GTK_WIDGET(w->modal_entry1)
                    ? GTK_WIDGET(w->modal_entry2)
                    : GTK_WIDGET(w->modal_entry1));
            return TRUE;
        }
        return FALSE; /* let the entries handle typing; skip global shortcuts */
    }

    /* Ctrl+A selects everything. WebKit does this itself in a plain input, but not in
     * a JS editor: our user agent claims macOS (see NAVIGATOR_SPOOF_JS, which has to
     * spoof navigator.platform to "MacIntel" to match it), and every editor that reads
     * navigator.platform then switches to Mac key bindings. ProseMirror -- claude.ai's
     * composer, among others -- doesn't merely move select-all to Cmd+A there; its Mac
     * keymap rebinds Ctrl+A to selectTextblockStart, the macOS "go to start of line",
     * and swallows the key. Nothing the page can be told fixes that, so take the key
     * before it: the controller runs in the capture phase, ahead of the web view.
     *
     * Only when the page has the keyboard. The find bar is a GtkEntry whose own
     * select-all is already right, and it reaches here because -- unlike the modal --
     * it doesn't return early above. */
    if ((state & CTRL) && !(state & ALT) && (keyval == GDK_KEY_a || keyval == GDK_KEY_A)
        && !find_entry_focused(w)) {
        WebKitWebView* v = current_view(w);
        if (v != NULL) {
            webkit_web_view_execute_editing_command(v, WEBKIT_EDITING_COMMAND_SELECT_ALL);
            return TRUE;
        }
    }

    for (size_t i = 0; i < sizeof(shortcut) / sizeof(shortcut[0]); i++) {
        if ((state & shortcut[i].mod || shortcut[i].mod == 0x0) && keyval == shortcut[i].key) {
            handle_shortcut(w, shortcut[i].id);
            return TRUE;
        }
    }
    return FALSE;
}

/* Releasing Alt commits the alt+tab walk: the tab the user landed on becomes the
 * new most-recent. Deferring the reshuffle to here is what lets repeated Tab
 * presses step deeper into history rather than just toggling the top two. */
static void handle_signal_keyrelease(GtkEventControllerKey* self G_GNUC_UNUSED,
    guint keyval, guint keycode G_GNUC_UNUSED, GdkModifierType state G_GNUC_UNUSED,
    gpointer user_data)
{
    Win* w = user_data;
    if ((keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R)
        && w->alt_walk > 0 && w->alt_walk < w->mru_len) {
        mru_promote(w, w->mru[w->alt_walk]);
        w->alt_walk = -1;
    }
}

/* --------------------------------------------------------------- theme */
/* The chrome is always dark: every visible chrome widget hardcodes Nord colours
 * in CSS, so the UI never follows the system light/dark. The GTK theme itself is
 * deliberately left to follow the system (gtk-application-prefer-dark-theme),
 * because WebKit maps that to each page's prefers-color-scheme — forcing the GTK
 * theme dark would drag every website dark too. So: chrome dark via CSS, websites
 * dark/light via the (untouched) system theme. */

static void apply_color_scheme(void)
{
    /* The sole purpose of this is to let websites inherit the system light/dark;
     * the chrome is unaffected (its colours are hardcoded in CSS). */
    char* scheme = g_settings_get_string(iface_settings, "color-scheme");
    gboolean dark = g_strcmp0(scheme, "prefer-dark") == 0;
    g_free(scheme);

    g_object_set(gtk_settings_get_default(),
        "gtk-application-prefer-dark-theme", dark, NULL);
}

static void setup_theme(void)
{
    GSettingsSchemaSource* src = g_settings_schema_source_get_default();
    if (src == NULL)
        return;
    GSettingsSchema* schema = g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE);
    if (schema == NULL)
        return; /* this check ONLY needed for development!!!! nix develop */
    g_settings_schema_unref(schema);

    iface_settings = g_settings_new("org.gnome.desktop.interface");
    apply_color_scheme();
    g_signal_connect(iface_settings, "changed::color-scheme", G_CALLBACK(apply_color_scheme), NULL);
}

/* ------------------------------------------------------------------ build */
static void build_modal(Win* w)
{
    w->dim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(w->dim, "dim");
    gtk_widget_set_can_target(w->dim, FALSE);
    gtk_widget_set_visible(w->dim, FALSE);
    gtk_overlay_add_overlay(w->overlay, w->dim);

    w->modal_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); /* gap between name + url entries */
    gtk_widget_add_css_class(w->modal_box, "modal");
    gtk_widget_set_halign(w->modal_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(w->modal_box, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(w->modal_box, FALSE);

    w->modal_info = GTK_LABEL(gtk_label_new(""));
    gtk_widget_set_halign(GTK_WIDGET(w->modal_info), GTK_ALIGN_START);
    w->modal_entry1 = GTK_ENTRY(gtk_entry_new());
    w->modal_entry2 = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_visible(GTK_WIDGET(w->modal_entry2), FALSE);
    /* Entry + bookmark rows share one box (spacing 4) so the gap shows only
     * between present children — no dangling space under the entry when empty. */
    w->modal_results = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
    gtk_box_append(w->modal_results, GTK_WIDGET(w->modal_entry1));
    /* Persistent calc-result label, pinned right under the entry and above any
     * bookmark rows. Hidden until the typed text is a valid expression. */
    w->calc_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->calc_label), "calc");
    gtk_label_set_xalign(w->calc_label, 0.0);
    gtk_widget_set_visible(GTK_WIDGET(w->calc_label), FALSE);
    gtk_box_append(w->modal_results, GTK_WIDGET(w->calc_label));

    gtk_box_append(GTK_BOX(w->modal_box), GTK_WIDGET(w->modal_info));
    gtk_box_append(GTK_BOX(w->modal_box), GTK_WIDGET(w->modal_results));
    gtk_box_append(GTK_BOX(w->modal_box), GTK_WIDGET(w->modal_entry2));

    g_signal_connect(w->modal_entry1, "changed", G_CALLBACK(on_search_changed), w);
    g_signal_connect(w->modal_entry1, "activate", G_CALLBACK(on_modal_activate), w);
    g_signal_connect(w->modal_entry2, "activate", G_CALLBACK(on_modal_activate), w);
    gtk_overlay_add_overlay(w->overlay, w->modal_box);
}

static void build_findbar(Win* w)
{
    w->findbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(w->findbar, "findbar");
    gtk_widget_set_halign(w->findbar, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(w->findbar, GTK_ALIGN_END);
    gtk_widget_set_visible(w->findbar, FALSE);

    w->find_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->find_entry, "Find");
    w->find_label = GTK_LABEL(gtk_label_new(""));
    gtk_box_append(GTK_BOX(w->findbar), GTK_WIDGET(w->find_entry));
    gtk_box_append(GTK_BOX(w->findbar), GTK_WIDGET(w->find_label));

    g_signal_connect(w->find_entry, "changed", G_CALLBACK(on_find_changed), w);
    g_signal_connect(w->find_entry, "activate", G_CALLBACK(on_find_activate), w);
    gtk_overlay_add_overlay(w->overlay, w->findbar);
}

/* ------------------------------------------------------------ application */
static GtkApplication* app;
static gboolean prewarm = FALSE; /* --prewarm: build the window but keep it hidden */
static gboolean shown = FALSE;   /* the window has been presented to the user */

/* Closing the browser in resident mode: save the tabs, drop them so their web
 * processes hand their memory back, and hide the window — still built and still
 * realized, exactly as it sat after --prewarm. Reopening therefore costs a map
 * (~5ms) rather than a fresh start, and the tabs come back asleep from the
 * session file, the same as they would have after a real restart. */
static void window_hide_to_resident(Win* w)
{
    session_save();
    if (session_save_source != 0) { /* nothing queued may overwrite it with the teardown */
        g_source_remove(session_save_source);
        session_save_source = 0;
    }

    tearing_down = TRUE;
    modal_hide(w);
    gtk_widget_set_visible(w->findbar, FALSE);
    for (int n = gtk_notebook_get_n_pages(w->notebook); n > 0; n--) {
        GtkWidget* page = gtk_notebook_get_nth_page(w->notebook, n - 1);
        GtkWidget* btn = g_object_get_data(G_OBJECT(page), "button");
        if (btn != NULL)
            gtk_box_remove(w->tabbar, btn);
        gtk_notebook_remove_page(w->notebook, n - 1);
    }
    tearing_down = FALSE;

    w->num_tabs = 0;
    w->mru_len = 0;
    w->alt_walk = -1;
    w->page_loading = FALSE;
    g_clear_pointer(&w->status_link, g_free);
    update_status(w);

    gtk_widget_set_visible(GTK_WIDGET(w->window), FALSE);
    shown = FALSE; /* the next activation restores the session, like a fresh launch */
}

/* Closing a scratch window really does close it: its tabs were never part of the
 * session, so there is nothing to save and nothing to come back to. */
static void window_destroy_scratch(Win* w)
{
    tearing_down = TRUE;
    g_ptr_array_remove_fast(wins, w);
    /* A view can outlive the window by a turn of the loop (a queued download tidy-up
     * still holds a reference), so cut its link to the Win before that Win is freed:
     * win_of() then reports NULL, which every handler already treats as "gone". */
    for (int n = gtk_notebook_get_n_pages(w->notebook); n > 0; n--)
        g_object_set_data(G_OBJECT(gtk_notebook_get_nth_page(w->notebook, n - 1)), "win", NULL);
    gtk_window_destroy(w->window);
    tearing_down = FALSE;
    g_clear_pointer(&w->status_link, g_free);
    g_clear_pointer(&w->status_flash, g_free);
    if (w->status_flash_source != 0)
        g_source_remove(w->status_flash_source);
    g_free(w);
}

/* Window manager / sway close (the X). The main window in resident mode parks the
 * process rather than ending it; a scratch window just goes away. */
static gboolean on_close_request(GtkWindow* window, gpointer data)
{
    Win* w = data;
    if (!w->is_main) {
        window_destroy_scratch(w);
        return TRUE; /* handled: we did the destroying ourselves */
    }
    if (resident) {
        window_hide_to_resident(w);
        return TRUE; /* handled: don't let GTK destroy the window */
    }
    session_save();
    return FALSE;
}

/* SIGTERM/SIGINT (logout, poweroff, Ctrl-C): save the session and quit cleanly
 * so tabs survive a shutdown. */
static gboolean on_term_signal(gpointer data)
{
    session_save();
    g_application_quit(G_APPLICATION(app));
    return G_SOURCE_REMOVE;
}

/* One-time, process-wide setup: paths, bookmarks, the stylesheet every window
 * shares, the theme watcher and the memory sweep. */
static void app_init_once(void)
{
    static gboolean done = FALSE;
    if (done)
        return;
    done = TRUE;

    g_mkdir_with_parents(DATA_DIR, 0700);
    bookmarks_load(BOOKMARKS_DIR);
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", true, NULL);

    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, CSS);
    /* Above PRIORITY_USER (800) so our custom-class rules beat the theme's
     * ~/.config/gtk-4.0/gtk.css (loaded at USER), e.g. the tab button radius. */
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER + 1);

    setup_theme();
    /* Sleep idle tabs only under real memory pressure, not when RAM is merely full (see sleep_sweep). */
    g_timeout_add_seconds(TAB_SLEEP_SWEEP_SECONDS, sleep_sweep, NULL);
}

/* Build a browser window and its chrome. No tab is created: the caller decides
 * what goes in it (the session, a URL, or nothing at all for a scratch window).
 * `is_main` marks the one window whose tabs are the saved session. */
static Win* window_new(gboolean is_main)
{
    app_init_once();

    Win* w = g_new0(Win, 1);
    w->is_main = is_main;
    w->tabbar_visible = TRUE;
    w->alt_walk = -1;
    w->fuzzy_sel = -1;
    w->modal_mode = MODAL_NONE;
    if (wins == NULL)
        wins = g_ptr_array_new();
    g_ptr_array_add(wins, w);
    if (is_main)
        main_win = w;

    w->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_default_size(w->window, 1100, 700); /* initial size only; never a resize limit */
    /* Callbacks reach their window through the widget they fired on, so several
     * windows can be open without any of them consulting a global. */
    g_object_set_data(G_OBJECT(w->window), "win", w);
    g_signal_connect(w->window, "close-request", G_CALLBACK(on_close_request), w);

    w->notebook = GTK_NOTEBOOK(gtk_notebook_new());
    gtk_notebook_set_show_tabs(w->notebook, FALSE);
    gtk_notebook_set_show_border(w->notebook, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(w->notebook), "webarea");
    gtk_widget_set_hexpand(GTK_WIDGET(w->notebook), TRUE);
    g_signal_connect(w->notebook, "switch-page", G_CALLBACK(on_switch_page), w);

    w->tabbar = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_widget_add_css_class(GTK_WIDGET(w->tabbar), "tabbar");
    gtk_box_set_spacing(w->tabbar, 4);

    /* Status label (hugs its text) over a thin progress bar, floated at the bottom
     * of the webview so only the label's own background shows, not a full-width bar. */
    w->status_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(w->status_label, 0.0);
    gtk_label_set_ellipsize(w->status_label, PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(GTK_WIDGET(w->status_label), GTK_ALIGN_START);

    w->progress = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_add_css_class(GTK_WIDGET(w->progress), "loadbar");

    w->download_progress = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_add_css_class(GTK_WIDGET(w->download_progress), "downloadbar");
    gtk_widget_set_visible(GTK_WIDGET(w->download_progress), FALSE);

    w->statusbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(w->statusbar, "statusbar");
    gtk_widget_set_valign(w->statusbar, GTK_ALIGN_END);
    gtk_widget_set_can_target(w->statusbar, FALSE); /* clicks fall through to the page */
    gtk_widget_set_visible(w->statusbar, FALSE);
    gtk_box_append(GTK_BOX(w->statusbar), GTK_WIDGET(w->status_label));
    gtk_box_append(GTK_BOX(w->statusbar), GTK_WIDGET(w->progress));
    gtk_box_append(GTK_BOX(w->statusbar), GTK_WIDGET(w->download_progress));

    GtkWidget* content = gtk_overlay_new();
    gtk_widget_set_hexpand(content, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(content), GTK_WIDGET(w->notebook));
    gtk_overlay_add_overlay(GTK_OVERLAY(content), w->statusbar);

    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(hbox), GTK_WIDGET(w->tabbar));
    gtk_box_append(GTK_BOX(hbox), content);

    w->overlay = GTK_OVERLAY(gtk_overlay_new());
    gtk_overlay_set_child(w->overlay, hbox);
    gtk_window_set_child(w->window, GTK_WIDGET(w->overlay));

    build_modal(w);
    build_findbar(w);
    g_signal_connect(w->window, "notify::focus-widget", G_CALLBACK(modal_keep_focus), w);

    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(handle_signal_keypress), w);
    g_signal_connect(keys, "key-released", G_CALLBACK(handle_signal_keyrelease), w);
    gtk_widget_add_controller(GTK_WIDGET(w->window), keys);

    /* Neither a tab nor the ad-block store is set up here: everything that touches
     * WebKit is deferred past the first frame (see startup_deferred), so the window
     * is on screen before the slow part of startup even begins. */
    return w;
}

/* A scratch window: empty, instant (the engine is already up), and outside the
 * session — closing it takes its tabs with it. This is what a launch does when
 * the main window is already on screen. */
static void window_new_scratch(void)
{
    Win* w = window_new(FALSE);
    gtk_window_present(w->window);
    notebook_create_new_tab(w, NULL);
    modal_show(w, MODAL_SEARCH, FALSE); /* land in the search box, like a fresh start */
}

/* Everything expensive that startup can survive without for one frame: creating a
 * WebKitWebView drags in the whole engine (web context, network session, GPU
 * process), which is by far the longest step of a cold launch, and the ad-block
 * store does synchronous disk work. Both run from a G_PRIORITY_LOW idle, below
 * GDK's redraw priority, so GTK has already painted the dark chrome + tab bar by
 * the time we get here: the window appears immediately and the page fills in.
 * Ad blocking must be initialised before the first tab exists — a view created
 * earlier is never registered with the filter store and would go unfiltered. */
static gboolean startup_deferred(gpointer data)
{
    char** uris = data; /* URLs to open (from `open`), or NULL for a plain launch */
#if ADBLOCK_ENABLED
    adblock_content_init(ADBLOCK_FILTERS_DIR, ADBLOCK_STORE_DIR);
#endif
    /* Before the first page can touch navigator.mediaDevices and set the portal off. */
    camera_portal_answer_in_advance();
    /* The last session comes back whatever opened us. A link clicked in another app
     * used to replace the saved tabs with that one page — and now that the session
     * is written as you browse rather than only on the way out, that would throw
     * them away for good a few seconds later. */
    Win* w = main_win;
    gboolean restored = session_restore(w, uris == NULL);
    if (uris != NULL) {
        for (char** u = uris; *u != NULL; u++)
            notebook_create_new_tab(w, *u); /* appended last, so it's the tab on screen */
        g_strfreev(uris);
    } else if (!restored) { /* nothing to restore: a blank search */
        if (w->modal_mode == MODAL_NONE)
            modal_show(w, MODAL_SEARCH, FALSE);
        notebook_create_new_tab(w, NULL);
    }
    return G_SOURCE_REMOVE;
}

/* First real launch of the main window: put it on screen, then let everything
 * else follow. `uris` (may be NULL) is handed to the deferred phase, which owns it. */
static void startup_show(char** uris)
{
    if (main_win == NULL)
        window_new(TRUE); /* not prewarmed: build it now */
    /* Decide on the modal from a bare stat, before presenting, so a launch with no
     * session shows its search box in the very first frame rather than a frame later. */
    if (uris == NULL && !session_exists())
        modal_show(main_win, MODAL_SEARCH, FALSE);
    gtk_window_present(main_win->window);
    shown = TRUE;
    g_idle_add_full(G_PRIORITY_LOW, startup_deferred, uris, NULL);
}

/* Launched with no URL, or launched again while we're already running.
 *
 * The first launch opens the main window with the saved session. Once that window
 * is up, launching again means "give me somewhere to browse *now*", so it opens a
 * scratch window instead: empty, on the workspace you're on (sway maps new windows
 * there and focuses them, which also sidesteps an app's inability to focus itself),
 * and outside the session. Closing the main window parks it, so a launch after
 * that reopens it with your tabs rather than making a scratch window. */
static void on_activate(GApplication* application, gpointer data)
{
    /* --prewarm: build and realize the main window without mapping it, so GTK's
     * window setup and the GSK renderer (together the bulk of a cold launch) are
     * already paid for. The launch that follows only has to map an existing surface,
     * which puts it on screen in ~10ms instead of ~400ms. Nothing else is warmed:
     * no tab, and no ad-block filters (loading those costs ~110MB resident, and they
     * are only needed once a page actually loads), so an idle prewarm stays cheap. */
    if (prewarm) {
        prewarm = FALSE; /* one prewarm; any later activation is a real launch */
        gtk_widget_realize(GTK_WIDGET(window_new(TRUE)->window));
        return; /* the (hidden) window keeps GtkApplication alive on its own */
    }

    if (shown)
        window_new_scratch();
    else
        startup_show(NULL);
}

/* Launched/activated with URLs (default-browser link handling): always a tab in
 * the main window, never a new one — a link from Slack or Discord belongs with
 * the tabs you keep, and lands instantly because that window is already warm. */
static void on_open(GApplication* application, GFile** files, gint n_files, const char* hint, gpointer data)
{
    if (shown) {
        modal_hide_for_new_tab(main_win); /* the link is what you want now, not the modal */
        for (gint i = 0; i < n_files; i++) {
            char* uri = g_file_get_uri(files[i]);
            notebook_create_new_tab(main_win, uri);
            g_free(uri);
        }
        gtk_window_present(main_win->window);
        return;
    }

    GPtrArray* uris = g_ptr_array_new();
    for (gint i = 0; i < n_files; i++)
        g_ptr_array_add(uris, g_file_get_uri(files[i]));
    g_ptr_array_add(uris, NULL); /* startup_deferred walks it as a NULL-terminated list */
    startup_show((char**)g_ptr_array_free(uris, FALSE));
}

#define APP_ID "com.amazinaxel.lightbrowse"
#define APP_PATH "/com/amazinaxel/lightbrowse"

/* Raise (or hand URLs to) an instance that is already running, straight over
 * D-Bus. GApplication does this handoff itself, but only after building a
 * GtkApplication and registering it — ~100ms of setup this short-lived process
 * never needs, which is the difference between a launch key that feels instant
 * and one that lags. Returns TRUE if the running instance took the request;
 * FALSE (no instance, or it went away mid-call) means start up normally.
 * With `probe_only` nothing is sent: a --prewarm launch just wants to know
 * whether warming is pointless because an instance is already there. */
static gboolean forward_to_running_instance(int argc, char** argv, gboolean probe_only)
{
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (bus == NULL)
        return FALSE;

    gboolean handled = FALSE;
    if (probe_only) {
        GVariant* owned = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus",
            "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
            g_variant_new("(s)", APP_ID), G_VARIANT_TYPE("(b)"),
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);
        if (owned != NULL) {
            g_variant_get(owned, "(b)", &handled);
            g_variant_unref(owned);
        }
        g_object_unref(bus);
        return handled;
    }

    /* Pass on the compositor's activation token, the way GApplication would have.
     * Without it the running instance asks to be focused with no proof that the
     * request came from something the user just interacted with, and sway is right
     * to ignore that — a link clicked in another app would open in a tab behind
     * whatever window you were looking at. */
    GVariantBuilder platform;
    g_variant_builder_init(&platform, G_VARIANT_TYPE("a{sv}"));
    const char* token = g_getenv("XDG_ACTIVATION_TOKEN");
    if (token == NULL || token[0] == '\0')
        token = g_getenv("DESKTOP_STARTUP_ID"); /* X11 / older launchers */
    if (token != NULL && token[0] != '\0') {
        g_variant_builder_add(&platform, "{sv}", "activation-token", g_variant_new_string(token));
        g_variant_builder_add(&platform, "{sv}", "desktop-startup-id", g_variant_new_string(token));
    }

    const char* method;
    GVariant* args;
    if (argc > 1) { /* org.gtk.Application.Open(as uris, s hint, a{sv}) */
        GVariantBuilder uris;
        g_variant_builder_init(&uris, G_VARIANT_TYPE("as"));
        for (int i = 1; i < argc; i++) {
            /* Same conversion GApplication applies to its own arguments, so a bare
             * path, a relative one and a full URL all arrive as the running
             * instance would have built them. */
            GFile* file = g_file_new_for_commandline_arg(argv[i]);
            char* uri = g_file_get_uri(file);
            g_variant_builder_add(&uris, "s", uri);
            g_free(uri);
            g_object_unref(file);
        }
        GVariant* children[] = { g_variant_builder_end(&uris),
            g_variant_new_string(""), g_variant_builder_end(&platform) };
        method = "Open";
        args = g_variant_new_tuple(children, G_N_ELEMENTS(children));
    } else { /* org.gtk.Application.Activate(a{sv}) */
        GVariant* child = g_variant_builder_end(&platform);
        method = "Activate";
        args = g_variant_new_tuple(&child, 1);
    }

    /* NO_AUTO_START: a missing instance must fall through to a normal launch here,
     * not have the bus try to spawn one. */
    GVariant* reply = g_dbus_connection_call_sync(bus, APP_ID, APP_PATH,
        "org.gtk.Application", method, args, NULL,
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);
    if (reply != NULL) {
        g_variant_unref(reply);
        handled = TRUE;
    }
    g_object_unref(bus);
    return handled;
}

int main(int argc, char** argv)
{
    /* Strip --prewarm before GApplication sees it: with HANDLES_OPEN it would take
     * any leftover argument for a file to open. */
    int keep = 1;
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--prewarm") == 0)
            prewarm = resident = TRUE; /* prewarmed instances also survive their window */
        else
            argv[keep++] = argv[i];
    }
    argv[keep] = NULL;
    argc = keep;

    /* Nothing below this needs to run when an instance is already up: it either
     * took the request or (--prewarm) there is nothing left to warm. */
    if (forward_to_running_instance(argc, argv, prewarm))
        return 0;

    app = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    g_unix_signal_add(SIGTERM, on_term_signal, NULL); /* save tabs on shutdown */
    g_unix_signal_add(SIGINT, on_term_signal, NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}

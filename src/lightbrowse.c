#include <gdk/gdk.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <webkit/webkit.h>

#include "config.h"
#include "plugins/plugins.h"

/* Global variables */
static GtkNotebook* global_notebook;
static GtkWindow* window;
typedef enum { _SEARCH, _FIND, _FILTER, _HIDDEN } Bar_entry_mode;
static struct {
    GtkHeaderBar* widget;
    GtkEntry* line;
    GtkEntryBuffer* line_text;
    Bar_entry_mode entry_mode;
} bar;
static int num_tabs = 0;

/* Forward declarations */
static void toggle_bar(GtkNotebook* notebook, Bar_entry_mode mode);
static void notebook_create_new_tab(GtkNotebook* notebook, const char* uri);
static gboolean handle_signal_keypress(GtkEventControllerKey* self, guint keyval,
    guint keycode, GdkModifierType state, gpointer user_data);

/* Utils */
static WebKitWebView* notebook_get_webview(GtkNotebook* notebook)
{
    WebKitWebView* view = WEBKIT_WEB_VIEW(gtk_notebook_get_nth_page(notebook, gtk_notebook_get_current_page(notebook)));
    NULLCHECK(view);
    return view;
}

/* Load content */
static void load_uri(WebKitWebView* view, const char* uri)
{
    bool is_empty_uri = (uri[0] == '\0');
    if (is_empty_uri) {
        webkit_web_view_load_uri(view, "");
        toggle_bar(global_notebook, _SEARCH);
        return;
    }

    bool has_direct_uri_prefix = g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://") || g_str_has_prefix(uri, "file://") || g_str_has_prefix(uri, "about:") || g_str_has_prefix(uri, "data:");
    if (has_direct_uri_prefix) {
        webkit_web_view_load_uri(view, uri);
        return;
    }

    bool has_common_domain_extension = (strstr(uri, ".com") || strstr(uri, ".org"));
    if (has_common_domain_extension) {
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

    char* search = g_strdup_printf(SEARCH, uri);
    webkit_web_view_load_uri(view, search);
    g_free(search);
}

static void handle_signal_load_changed(WebKitWebView* self, WebKitLoadEvent load_event,
    GtkNotebook* notebook)
{
    switch (load_event) {
        // https://webkitgtk.org/reference/webkit2gtk/2.5.1/WebKitWebView.html
        case WEBKIT_LOAD_STARTED:
        case WEBKIT_LOAD_COMMITTED:
        case WEBKIT_LOAD_REDIRECTED:
            break;
        case WEBKIT_LOAD_FINISHED: {
            /* Add gtk tab title */
            const char* webpage_title = webkit_web_view_get_title(self);
            const int max_length = 25;
            char tab_title[max_length + 1];
            if (webpage_title != NULL) {
                for (int i = 0; i < (max_length); i++) {
                    tab_title[i] = webpage_title[i];
                    if (webpage_title[i] == '\0') {
                        break;
                    }
                }
                tab_title[max_length] = '\0';
            }
            gtk_notebook_set_tab_label_text(notebook, GTK_WIDGET(self),
                webpage_title == NULL ? "—" : tab_title);
        }
    }
}

/* New tabs */
/* Shared web context for all views (needed for web extensions) */
static WebKitWebContext* get_shared_web_context()
{
    static WebKitWebContext* context = NULL;
    if (context == NULL) {
        context = webkit_web_context_new();

        /* Configure web extensions for adblock if enabled */
        if (ADBLOCK_ENABLED) {
            webkit_web_context_set_web_process_extensions_directory(context, ADBLOCK_EXTENSIONS_DIR);

            /* Pass configuration to the extension */
            GVariantBuilder builder;
            g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&builder, "{sv}", "enabled", g_variant_new_boolean(TRUE));
            webkit_web_context_set_web_process_extensions_initialization_user_data(
                context, g_variant_builder_end(&builder));
        }
    }
    return context;
}

/* One network session shared by every tab */
static WebKitNetworkSession* get_shared_network_session()
{
    static WebKitNetworkSession* session = NULL;
    if (session == NULL) {
        session = webkit_network_session_new(DATA_DIR, DATA_DIR);
        WebKitCookieManager* cookiemanager = webkit_network_session_get_cookie_manager(session);
        webkit_cookie_manager_set_persistent_storage(cookiemanager, DATA_DIR "/cookies.sqlite", WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
        webkit_cookie_manager_set_accept_policy(cookiemanager, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
    }
    return session;
}

/* One immutable settings object shared by every tab. */
static WebKitSettings* get_shared_settings()
{
    static WebKitSettings* settings = NULL;
    if (settings == NULL) {
        settings = webkit_settings_new_with_settings(WEBKIT_DEFAULT_SETTINGS, NULL);
        webkit_settings_set_user_agent(
            settings,
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15");
    }
    return settings;
}

static WebKitWebView* create_new_webview()
{
    WebKitWebView* view = g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "settings", get_shared_settings(),
        "network-session", get_shared_network_session(),
        "web-context", get_shared_web_context(),
        NULL);
    NULLCHECK(view);
    return view;
}

static GtkWidget* handle_signal_create_new_tab(WebKitWebView* self,
    WebKitNavigationAction* navigation_action,
    GtkNotebook* notebook)
{
    NULLCHECK(self);
    NULLCHECK(notebook);
    if (num_tabs < MAX_NUM_TABS || num_tabs == 0) {
        WebKitURIRequest* uri_request = webkit_navigation_action_get_request(navigation_action);
        const char* uri = webkit_uri_request_get_uri(uri_request);
        webkit_web_view_stop_loading(self);
        notebook_create_new_tab(notebook, uri);
        gtk_notebook_set_show_tabs(notebook, true);
    } else {
        webkit_web_view_evaluate_javascript(self, "alert('Too many tabs')", -1, NULL, "lightbrowse-alert-numtabs", NULL, NULL, NULL);
    }
    return ABORT_REQUEST_ON_CURRENT_TAB;
    // Could also return GTK_WIDGET(self), in which case the new uri would also be loaded in the current webview. This could be interesting if I wanted to e.g., open an alternative frontend in a new tab
}

static void notebook_create_new_tab(GtkNotebook* notebook, const char* uri)
{
    if (num_tabs < MAX_NUM_TABS || MAX_NUM_TABS == 0) {
        WebKitWebView* view = create_new_webview();
        NULLCHECK(view);

        g_signal_connect(view, "load_changed", G_CALLBACK(handle_signal_load_changed), notebook);
        g_signal_connect(view, "create", G_CALLBACK(handle_signal_create_new_tab), notebook);

        int n = gtk_notebook_append_page(notebook, GTK_WIDGET(view), NULL);
        gtk_notebook_set_tab_reorderable(notebook, GTK_WIDGET(view), true);
        NULLCHECK(window);
        NULLCHECK(bar.widget);
        gtk_widget_set_visible(GTK_WIDGET(window), 1);
        gtk_widget_set_visible(GTK_WIDGET(bar.widget), 0);
        load_uri(view, (uri) ? uri : HOME);

        gtk_notebook_set_current_page(notebook, n);
        gtk_notebook_set_tab_label_text(notebook, GTK_WIDGET(view), "-");
        webkit_web_view_set_zoom_level(view, ZOOM_START_LEVEL);
        num_tabs += 1;
    } else {
        webkit_web_view_evaluate_javascript(notebook_get_webview(notebook), "alert('Too many tabs, not opening a new one')",
            -1, NULL, "lightbrowse-alert-numtabs", NULL, NULL, NULL);
    }
}

/* Top bar */
static void toggle_bar(GtkNotebook* notebook, Bar_entry_mode mode)
{
    bar.entry_mode = mode;
    switch (bar.entry_mode) {
        case _SEARCH: {
            const char* url = webkit_web_view_get_uri(notebook_get_webview(notebook));
            gtk_entry_set_placeholder_text(bar.line, "Search");
            gtk_entry_buffer_set_text(bar.line_text, url, strlen(url));
            gtk_widget_set_visible(GTK_WIDGET(bar.widget), 1);
            gtk_window_set_focus(window, GTK_WIDGET(bar.line));
            break;
        }
        case _FIND: {
            const char* search_text = webkit_find_controller_get_search_text(
                webkit_web_view_get_find_controller(notebook_get_webview(notebook)));

            if (search_text != NULL)
                gtk_entry_buffer_set_text(bar.line_text, search_text, strlen(search_text));

            gtk_entry_set_placeholder_text(bar.line, "Find");
            gtk_widget_set_visible(GTK_WIDGET(bar.widget), 1);
            gtk_window_set_focus(window, GTK_WIDGET(bar.line));
            break;
        }
        case _FILTER: {
            gtk_entry_set_placeholder_text(bar.line, "Filter");
            gtk_entry_buffer_set_text(bar.line_text, "", strlen(""));
            gtk_widget_set_visible(GTK_WIDGET(bar.widget), 1);
            gtk_window_set_focus(window, GTK_WIDGET(bar.line));
            break;
        }
        case _HIDDEN:
            gtk_widget_set_visible(GTK_WIDGET(bar.widget), 0);
    }
}

// Handle what happens when the user is on the bar and presses enter
static void handle_signal_bar_press_enter(GtkEntry* self, GtkNotebook* notebook)
{
    WebKitWebView* view = notebook_get_webview(notebook);
    const char* bar_line_text = gtk_entry_buffer_get_text(bar.line_text);
    switch (bar.entry_mode) {
        case _SEARCH: {
            load_uri(view, bar_line_text);
            gtk_widget_set_visible(GTK_WIDGET(bar.widget), 0);
            break;
        }
        case _FIND: {
            webkit_find_controller_search(
                webkit_web_view_get_find_controller(view),
                bar_line_text,
                WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND,
                G_MAXUINT);
            gtk_widget_set_visible(GTK_WIDGET(bar.widget), 0);
            break;
        }
        case _FILTER: {
            char* js_command = g_strdup_printf("filterByKeyword(\"%s\")", bar_line_text);
            webkit_web_view_evaluate_javascript(view, js_command, -1, NULL, "lightbrowse-filter-plugin", NULL, NULL, NULL);
            g_free(js_command);
            gtk_widget_set_visible(GTK_WIDGET(bar.widget), 0);

            break;
        }
        case _HIDDEN:
        // no op
    }

}

/* Shortcuts */
static gboolean handle_shortcut(func id)
{
    static double zoom = ZOOM_START_LEVEL;
    static bool is_fullscreen = 0;

    WebKitWebView* view = notebook_get_webview(global_notebook);
    NULLCHECK(global_notebook);
    NULLCHECK(view);

    switch (id) {
        case goback:
            webkit_web_view_go_back(view);
            break;
        case goforward:
            webkit_web_view_go_forward(view);
            break;

        case refresh:
            webkit_web_view_reload(view);
            break;
        case refresh_force:
            webkit_web_view_reload_bypass_cache(view);
            break;

        case zoomin:
            webkit_web_view_set_zoom_level(view,
                (zoom += ZOOM_STEPSIZE));
            break;
        case zoomout:
            webkit_web_view_set_zoom_level(view,
                (zoom -= ZOOM_STEPSIZE));
            break;
        case zoom_reset:
            webkit_web_view_set_zoom_level(view,
                (zoom = ZOOM_START_LEVEL));
            break;

        case prev_tab:; // declarations aren't statements
            // https://stackoverflow.com/questions/92396/why-cant-variables-be-declared-in-a-switch-statement
            int n = gtk_notebook_get_n_pages(global_notebook);
            int k = gtk_notebook_get_current_page(global_notebook);
            int o = (n + k - 1) % n;
            gtk_notebook_set_current_page(global_notebook, o);
            break;
        case next_tab:;
            int m = gtk_notebook_get_n_pages(global_notebook);
            int l = gtk_notebook_get_current_page(global_notebook);
            int p = (l + 1) % m;
            gtk_notebook_set_current_page(global_notebook, p);
            break;
        case close_tab:
            num_tabs -= 1;
            switch(num_tabs){
                case 0:
                    exit(0);
                    break;
                case 1:
                    gtk_notebook_set_show_tabs(global_notebook, false);
                    // fallthrough
                default:
                    gtk_notebook_remove_page(global_notebook, gtk_notebook_get_current_page(global_notebook));
            }
            break;
        case toggle_fullscreen:
            if (is_fullscreen)
                gtk_window_unfullscreen(window);
            else
                gtk_window_fullscreen(window);
            is_fullscreen = !is_fullscreen;
            break;
        case show_searchbar:
            toggle_bar(global_notebook, _SEARCH);
            break;
        case show_finder:
            toggle_bar(global_notebook, _FIND);
            break;
        case filter:
            toggle_bar(global_notebook, _FILTER);
            break;

        case finder_next:
            webkit_find_controller_search_next(webkit_web_view_get_find_controller(view));
            break;
        case finder_prev:
            webkit_find_controller_search_previous(webkit_web_view_get_find_controller(view));
            break;

        case new_tab:
            notebook_create_new_tab(global_notebook, NULL);
            gtk_notebook_set_show_tabs(global_notebook, true);
            toggle_bar(global_notebook, _SEARCH);
            break;

        case hide_bar:
            gtk_widget_set_visible(GTK_WIDGET(bar.widget), 0);
            toggle_bar(global_notebook, _HIDDEN);
            break;

        case prettify: {
            if (READABILITY_ENABLED) {
                char* readability_js = read_readability_js();
                if (readability_js != NULL) {
                    webkit_web_view_evaluate_javascript(view, readability_js, -1, NULL, "lightbrowse-readability-plugin", NULL, NULL, NULL);
                    g_free(readability_js);
                }
            }
            break;
        }
    }

    return TRUE;
}

/* Listen to keypresses */
static gboolean handle_signal_keypress(GtkEventControllerKey* self, guint keyval,
    guint keycode, GdkModifierType state, gpointer user_data)
{
    for (size_t i = 0; i < sizeof(shortcut) / sizeof(shortcut[0]); i++) {
        if ((state & shortcut[i].mod || shortcut[i].mod == 0x0) && keyval == shortcut[i].key) {
            return handle_shortcut(shortcut[i].id);
        }
    }

    return FALSE;
}

int main(int argc, char** argv)
{
    // Ensure the cache/data directory exists before WebKit needs it
    g_mkdir_with_parents(DATA_DIR, 0700);

    // Initialize GTK in general
    gtk_init();
    g_object_set(gtk_settings_get_default(), GTK_SETTINGS_CONFIG_H, NULL);
    // https://docs.gtk.org/gobject/method.Object.set.html

    // Create the main window
    window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_default_size(window, WIDTH, HEIGHT);

    // Set up notebook
    global_notebook = GTK_NOTEBOOK(gtk_notebook_new());
    gtk_notebook_set_show_tabs(global_notebook, false);
    gtk_notebook_set_show_border(global_notebook, false);
    gtk_window_set_child(window, GTK_WIDGET(global_notebook));

    // Set up top bar
    bar.line_text = GTK_ENTRY_BUFFER(gtk_entry_buffer_new("", 0));
    bar.line = GTK_ENTRY(gtk_entry_new_with_buffer(bar.line_text));
    gtk_entry_set_alignment(bar.line, 0.5);
    gtk_widget_set_size_request(GTK_WIDGET(bar.line), BAR_WIDTH, -1);

    bar.widget = GTK_HEADER_BAR(gtk_header_bar_new());
    gtk_header_bar_set_title_widget(bar.widget, GTK_WIDGET(bar.line));
    gtk_window_set_titlebar(window, GTK_WIDGET(bar.widget));

    // One global key controller in the capture phase, so shortcuts are caught
    // before the focused webview sees them — no need for a controller per tab.
    GtkEventController* event_controller = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(event_controller, GTK_PHASE_CAPTURE);
    g_signal_connect(event_controller, "key-pressed", G_CALLBACK(handle_signal_keypress), NULL);
    gtk_widget_add_controller(GTK_WIDGET(window), event_controller);

    g_signal_connect(bar.line, "activate", G_CALLBACK(handle_signal_bar_press_enter), global_notebook);
    g_signal_connect(GTK_WIDGET(window), "destroy", G_CALLBACK(exit), global_notebook);

    // Load first tab
    char* first_uri = argc > 1 ? argv[1] : HOME;
    notebook_create_new_tab(global_notebook, first_uri);

    // Show to user (notebook_create_new_tab already made the window visible and
    // hid the bar; present() raises and focuses it).
    gtk_window_present(window);

    // Deal with more tabs, if any
    if (argc > 2) {
        gtk_notebook_set_show_tabs(global_notebook, true);
        for (int i = 2; i < argc; i++) {
            notebook_create_new_tab(global_notebook, argv[i]);
        }
    }

    // Enter the main event loop, and wait for user interaction
    while (g_list_model_get_n_items(gtk_window_get_toplevels()) > 0 && num_tabs > 0)
        g_main_context_iteration(NULL, TRUE);

    return 0;
}

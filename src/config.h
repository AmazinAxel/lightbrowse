#include <stdbool.h>
#include <gtk/gtk.h>

#define ZOOM_STEPSIZE .1
#define MAX_NUM_TABS 16 // 0 for inf tabs
#define CLOSED_TAB_HISTORY 3 // how many closed tabs can be reopened (ctrl+shift+t)
/* Tab sleeping is driven by *system* memory pressure, not a fixed timer: tabs only
 * sleep when the machine is actually low on RAM. A slept tab's web process is freed
 * (RAM reclaimed), its button dims to 50%, and reselecting it reloads the page. */
#define TAB_SLEEP_LOW_MEM_MB 640      // below this much available system RAM, sleep the least-recently-used background tab
#define TAB_SLEEP_CRITICAL_MEM_MB 320 // below this, sleep every background tab at once to head off an imminent system OOM
#define TAB_SLEEP_SWEEP_SECONDS 8     // how often to check available memory
#define FUZZY_RESULTS 3 // max bookmark suggestions shown in the search modal
#define TAB_ICON_SIZE 24 // px, favicon size in the vertical tab strip
#define SEARCH "https://bing.com/search?q=%s"
#define THEME_NAME "Graphite-nord-dark" // GTK UI theme; the UI is always dark

#define DATA_DIR "/home/alec/.config/lightbrowse"
#define BOOKMARKS_DIR DATA_DIR "/bookmarks"
#define SESSION_FILE DATA_DIR "/session" // open tab URLs, restored on next launch
#define DOWNLOADS_DIR "/home/alec/Downloads" // saved downloads (assumed to exist)

/* Runtime asset directory.
 * The Nix package overrides this at build time (-DLIGHTBROWSE_SHARE_DIR=...)
 * to point into the store ($out/share/lightbrowse); the makefile points it at
 * the in-repo dev tree (out/share/lightbrowse). */
#ifndef LIGHTBROWSE_SHARE_DIR
#define LIGHTBROWSE_SHARE_DIR "/opt/lightbrowse"
#endif

/* Plugins */
#define ADBLOCK_EXTENSIONS_DIR LIGHTBROWSE_SHARE_DIR "/extensions"
#define ADBLOCK_FILTERLIST_PATH LIGHTBROWSE_SHARE_DIR "/filterlist.txt"

#define WEBKIT_DEFAULT_SETTINGS \
    "enable-developer-extras", true, \
    "allow-modal-dialogs", true, \
    "enable-encrypted-media", true, \
    "enable-media-capabilities", true, \
    "enable-media-stream", true, \
    "enable-smooth-scrolling", true, \
    "default-charset", "utf-8"

#define KEY(x) GDK_KEY_##x
#define SFT  1 << 0
#define CTRL 1 << 2
#define ALT  1 << 3

/* Misc helpers */
#define NULLCHECK(x)                                   \
    do {                                               \
        if (x == NULL) {                               \
            printf("\nNULL check not passed");         \
            printf("@ %s (%d): ", __FILE__, __LINE__); \
            exit(0);                                   \
        }                                              \
    } while (0)

/* keybinds */
typedef enum {
    goback,
    goforward,

    refresh,
    refresh_force,

    zoomin,
    zoomout,
    zoom_reset,

    new_tab,
    tab_up,
    tab_down,
    last_tab,
    close_tab,
    reopen_tab,

    show_finder,
    find_reset,

    bookmark_add,
    edit_uri,
    toggle_tabs,
    view_source,

    print_page,

    reading_mode
} func;

static struct {
    unsigned mod;
    unsigned key;
    func id;
} shortcut[] = {
    { CTRL,        KEY(h),             goback               },
    { ALT,         KEY(Left),          goback               },
    { CTRL,        KEY(j),             goforward            },
    { ALT,         KEY(Right),         goforward            },

    { CTRL,        KEY(r),             refresh              },
    { CTRL,        KEY(R),             refresh_force        },

    { CTRL,        KEY(p),             reading_mode         },
    { CTRL,        KEY(m),             edit_uri             },

    { CTRL,        KEY(equal),         zoomin               },
    { CTRL,        KEY(minus),         zoomout              },
    { CTRL,        KEY(0),             zoom_reset           },

    { ALT,         KEY(Tab),           last_tab             },
    { ALT,         KEY(Up),            tab_up               },
    { ALT,         KEY(Down),          tab_down             },
    { CTRL,        KEY(t),             new_tab              },
    { CTRL,        KEY(w),             close_tab            },
    { CTRL,        KEY(T),             reopen_tab           },

    { CTRL,        KEY(f),             show_finder          },
    { CTRL,        KEY(F),             find_reset           },

    { CTRL,        KEY(b),             bookmark_add         },
    { CTRL,        KEY(g),             toggle_tabs          },
    { CTRL,        KEY(e),             view_source          },

    { CTRL,        KEY(P),             print_page           },
};

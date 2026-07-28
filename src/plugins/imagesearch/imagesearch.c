/* Reverse image search from the clipboard.
 *
 * Lens only takes an image as a multipart POST and answers with a redirect to
 * the results, so there is no URL we could simply navigate to. Uploading it
 * ourselves (libsoup, curl, anything outside the browser) does not work: Google
 * ties the resulting session to the connection that made it, and a second
 * network stack reaches Google as a different client -- most visibly over a
 * different IP, since WebKit and a fresh socket need not pick the same address
 * family. The results page then rejects the session ("unusual traffic", or a
 * broken thumbnail that never loads).
 *
 * So the upload is done *by the tab itself*: give the tab a small generated
 * document whose script builds a form, fills its file input from a DataTransfer
 * and submits it. A form POST is an ordinary navigation -- no CORS involved --
 * so the tab follows the redirect and lands on the results with the browser's
 * own cookies, IP and user agent.
 *
 * The document is loaded with a lens.google.com base URI rather than fetched
 * from the network. That keeps the POST same-site, so the user's Google session
 * cookies ride along and the results are personalised as in any other browser --
 * and it costs no round trip, so no intermediate page is ever shown. (Fetching a
 * real page to bootstrap from meant staring at whatever Google served, e.g. its
 * 404, until the POST went through.) */

#include "imagesearch.h"

/* Origin the generated document claims, so the POST to /upload is same-site.
 * Only its origin matters -- nothing is ever fetched from this URI. */
#define LENS_BASE_URI "https://lens.google.com/"

/* Image types we hand to Lens, best first. The clipboard is read as raw bytes in
 * one of these and passed through untouched -- deliberately *not* via
 * gdk_clipboard_read_texture_async(), which decodes to a texture we would only
 * have to re-encode. Skipping the round trip is both cheaper and lossless, and
 * it sidesteps the clipboard image decoding being broken here: that call fails
 * with "Unrecognized image file format" on a PNG that gdk_texture_new_from_bytes
 * decodes happily, which looks like the same breakage that stops images pasting
 * into pages. */
static const char* IMAGE_MIME_TYPES[] = {
    "image/png", "image/jpeg", "image/webp", "image/gif", "image/bmp", NULL
};
/* The image is handed to the page as base64 inside the injected script, which
 * costs ~4/3 its size in JS source; Lens rejects oversized uploads anyway. */
#define MAX_IMAGE_BYTES (8 * 1024 * 1024)

typedef struct {
    WebKitWebView* view;
    ImageSearchFailed on_error;
    char* mime; /* whichever of IMAGE_MIME_TYPES the clipboard gave us */
} Upload;

static void upload_free(Upload* up)
{
    g_object_unref(up->view);
    g_free(up->mime);
    g_free(up);
}

/* Hand the tab a document that rebuilds the image, hangs it off a file input via
 * DataTransfer (the one way script may populate one) and submits. It paints the
 * chrome's own background so the moment before the POST navigates away is not a
 * white flash. */
static void submit_form(Upload* up, const char* base64)
{
    char* html = g_strdup_printf(
        "<!doctype html><meta charset=\"utf-8\">"
        "<body style=\"margin:0;background:#2E3440\"><script>"
        "var bin=atob('%s');var a=new Uint8Array(bin.length);"
        "for(var i=0;i<bin.length;i++)a[i]=bin.charCodeAt(i);"
        "var dt=new DataTransfer();"
        "dt.items.add(new File([a],'image',{type:'%s'}));"
        "var f=document.createElement('form');"
        "f.method='POST';f.enctype='multipart/form-data';"
        "f.action='https://lens.google.com/upload';"
        "var inp=document.createElement('input');"
        "inp.type='file';inp.name='encoded_image';"
        "f.appendChild(inp);document.body.appendChild(f);"
        "inp.files=dt.files;f.submit();"
        "</script>",
        base64, up->mime);
    webkit_web_view_load_html(up->view, html, LENS_BASE_URI);
    g_free(html);
}

static void fail(Upload* up, const char* reason)
{
    if (up->on_error != NULL)
        up->on_error(reason);
    upload_free(up);
}

/* The whole image has been drained from the clipboard: encode it and hand it to
 * the tab. */
static void on_spliced(GObject* source, GAsyncResult* res, gpointer data)
{
    Upload* up = data;
    GError* err = NULL;
    if (g_output_stream_splice_finish(G_OUTPUT_STREAM(source), res, &err) < 0) {
        char* msg = g_strdup(err != NULL ? err->message : "could not read the image");
        g_clear_error(&err);
        fail(up, msg);
        g_free(msg);
        return;
    }

    GBytes* image = g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(source));
    gsize len;
    const guchar* raw = g_bytes_get_data(image, &len);
    if (len == 0) {
        g_bytes_unref(image);
        fail(up, "the clipboard image was empty");
        return;
    }
    if (len > MAX_IMAGE_BYTES) {
        g_bytes_unref(image);
        fail(up, "the image is too large to search");
        return;
    }
    char* base64 = g_base64_encode(raw, len);
    g_bytes_unref(image);
    submit_form(up, base64);
    g_free(base64);
    upload_free(up);
}

static void on_clipboard_read(GObject* source, GAsyncResult* res, gpointer data)
{
    Upload* up = data;
    const char* mime = NULL;
    GError* err = NULL;
    GInputStream* stream = gdk_clipboard_read_finish(GDK_CLIPBOARD(source), res, &mime, &err);
    if (stream == NULL) {
        char* msg = g_strdup(err != NULL ? err->message : "the clipboard held no image");
        g_clear_error(&err);
        fail(up, msg);
        g_free(msg);
        return;
    }
    up->mime = g_strdup(mime);
    /* The clipboard hands over a pipe, so a single read would return whatever
     * chunk has arrived so far; splice drains it to the end. */
    GOutputStream* buffer = g_memory_output_stream_new_resizable();
    g_output_stream_splice_async(buffer, stream,
        G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
        G_PRIORITY_DEFAULT, NULL, on_spliced, up);
    g_object_unref(buffer);
    g_object_unref(stream);
}

gboolean imagesearch_clipboard_has_image(GdkClipboard* clipboard)
{
    GdkContentFormats* formats = gdk_clipboard_get_formats(clipboard);
    if (formats == NULL)
        return FALSE;
    /* Text wins when both are offered: that paste has something to type, and
     * silently searching it as an image would be a surprise. */
    if (gdk_content_formats_contain_gtype(formats, G_TYPE_STRING)
        || gdk_content_formats_contain_mime_type(formats, "text/plain;charset=utf-8"))
        return FALSE;
    for (const char** m = IMAGE_MIME_TYPES; *m != NULL; m++)
        if (gdk_content_formats_contain_mime_type(formats, *m))
            return TRUE;
    return FALSE;
}

void imagesearch_run(GdkClipboard* clipboard, WebKitWebView* view, ImageSearchFailed on_error)
{
    Upload* up = g_new0(Upload, 1);
    up->view = g_object_ref(view);
    up->on_error = on_error;
    gdk_clipboard_read_async(clipboard, IMAGE_MIME_TYPES, G_PRIORITY_DEFAULT, NULL, on_clipboard_read, up);
}

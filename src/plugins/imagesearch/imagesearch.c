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
 * So the upload is done *by the tab itself*: load a page on lens.google.com,
 * then build a form there whose file input is filled from a DataTransfer and
 * submit it. A form POST is an ordinary navigation -- no CORS involved -- so the
 * tab follows the redirect and lands on the results with the browser's own
 * cookies, IP and user agent. Bootstrapping from a lens.google.com page (rather
 * than about:blank) keeps the POST same-site, so the user's Google session
 * cookies ride along and the results are personalised as they would be in any
 * other browser. */

#include "imagesearch.h"

/* Cheapest same-origin page to bootstrap from: it is small, cacheable, and
 * exists purely to give the form a lens.google.com document to submit from. */
#define LENS_BOOTSTRAP_URI "https://lens.google.com/robots.txt"

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
    char* base64;
    char* mime; /* whichever of IMAGE_MIME_TYPES the clipboard gave us */
    gulong load_changed;
    gulong load_failed;
} Upload;

static void upload_free(Upload* up)
{
    if (up->load_changed != 0)
        g_signal_handler_disconnect(up->view, up->load_changed);
    if (up->load_failed != 0)
        g_signal_handler_disconnect(up->view, up->load_failed);
    g_object_unref(up->view);
    g_free(up->base64);
    g_free(up->mime);
    g_free(up);
}

/* Rebuild the image inside the page, hang it off a file input via DataTransfer
 * (the one way script may populate one), and submit. */
static void submit_form(Upload* up)
{
    char* js = g_strdup_printf(
        "(function(){try{"
        "var bin=atob('%s');var a=new Uint8Array(bin.length);"
        "for(var i=0;i<bin.length;i++)a[i]=bin.charCodeAt(i);"
        "var dt=new DataTransfer();"
        "dt.items.add(new File([a],'image',{type:'%s'}));"
        "var f=document.createElement('form');"
        "f.method='POST';f.enctype='multipart/form-data';"
        "f.action='https://lens.google.com/upload';"
        "var i=document.createElement('input');"
        "i.type='file';i.name='encoded_image';"
        "f.appendChild(i);document.body.appendChild(f);"
        "i.files=dt.files;"
        "if(i.files.length!==1)return 'file input rejected the image';"
        "f.submit();return '';"
        "}catch(e){return ''+e;}})()",
        up->base64, up->mime);
    webkit_web_view_evaluate_javascript(up->view, js, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(js);
}

static void on_load_changed(WebKitWebView* view, WebKitLoadEvent event, gpointer data)
{
    Upload* up = data;
    if (event != WEBKIT_LOAD_FINISHED)
        return;
    /* Only ever inject into the page we asked for: if the user typed elsewhere
     * while it loaded, drop the upload rather than scripting their page. */
    const char* uri = webkit_web_view_get_uri(view);
    if (g_strcmp0(uri, LENS_BOOTSTRAP_URI) == 0)
        submit_form(up);
    upload_free(up);
}

static gboolean on_load_failed(WebKitWebView* view, WebKitLoadEvent event,
    const char* failing_uri, GError* error, gpointer data)
{
    upload_free(data); /* WebKit shows its own error page */
    return FALSE;
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
    up->base64 = g_base64_encode(raw, len);
    g_bytes_unref(image);

    up->load_changed = g_signal_connect(up->view, "load-changed", G_CALLBACK(on_load_changed), up);
    up->load_failed = g_signal_connect(up->view, "load-failed", G_CALLBACK(on_load_failed), up);
    webkit_web_view_load_uri(up->view, LENS_BOOTSTRAP_URI);
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

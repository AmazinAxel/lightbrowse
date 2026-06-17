#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define READABILITY_N 88212 + 1000

/* Overridden at build time (-DLIGHTBROWSE_SHARE_DIR=...) by the Nix package
 * and the makefile; falls back to a system path otherwise. */
#ifndef LIGHTBROWSE_SHARE_DIR
#define LIGHTBROWSE_SHARE_DIR "/opt/lightbrowse"
#endif

void read_readability_js(char* string)
{
    gchar* file_contents = NULL;
    gsize length = 0;
    GError* error = NULL;

    if (!g_file_get_contents(LIGHTBROWSE_SHARE_DIR "/readability.js", &file_contents, &length, &error)) {
        fprintf(stderr, "Failed to open file: %s\n", error ? error->message : "unknown error");
        if (error) g_error_free(error);
        string[0] = '\0';
        return;
    }

    if (length > READABILITY_N) {
        fprintf(stderr, "readability.js file is too large (%zu bytes, max %d)\n", length, READABILITY_N);
        fprintf(stderr, "Consider increasing READABILITY_N or running recompute_READABILITY_N.sh\n");
        g_free(file_contents);
        string[0] = '\0';
        return;
    }

    memcpy(string, file_contents, length);
    string[length] = '\0';
    g_free(file_contents);
}

/*
int main(){
    char* readability_js = malloc(READABILITY_N+1);
    read_readability_js(readability_js);
    printf("%s", readability_js);
    free(readability_js);
}
*/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../strings/strings.h"

#define SHORTCUT_N 41
#define DEBUG false

/* Inspired by https://duckduckgo.com/bangs */
int shortcut_expand(const char* uri, char* output)
{
    int len_uri = strlen(uri);
    int len_output = strlen(output);

    if ((len_output - len_uri) < SHORTCUT_N) {
        fprintf(stderr, "Not enough memory\n");
        return 1; // not enough memory.
    } else {
        char* shortcuts[] = {
            "nx ",
            "gh ",
            "g ",
            "yt "
        };

        char* expansions[] = {
            "https://search.nixos.org/packages?channel=unstable&query=",
            "https://github.com/search?q=",
            "https://google.com/search?q=",
            "https://youtube.com/search?q="
        };

        // len = sizeof(shortcuts) / sizeof(shortcuts[0]);
        int len = sizeof(shortcuts) / sizeof(char*);

        for (int i = 0; i < len; i++) {
            str_init(output, len_output);
            int replace_check = str_replace_start(uri, shortcuts[i],
                expansions[i], output);
            switch (replace_check) {
                case 0: // no match found
                    break;
                case 1: // str_replace_start somehow failed
                    fprintf(stderr, "str_replace_start failed\n");
                    return 1;
                    break;
                case 2: // match succeeded
                    return 2;
                    break;
                default:
                    fprintf(stderr, "Unreachable state\n");
            }
        }
        // Use strncpy with explicit null termination for safety
        strncpy(output, uri, len_output - 1);
        output[len_output - 1] = '\0';
    }
    if (DEBUG) printf("No match found\n\n");
    return 0;
}

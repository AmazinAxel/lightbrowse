## Shared
COMMON_CODE=./src/plugins/strings/strings.c
STAND_IN=./src/plugins/stand_in/stand_in.c # gives function definitions for the above, which do nothing

## Plugins
SHORTCUTS=./src/plugins/shortcuts/shortcuts.c
READABILITY=./src/plugins/readability/readability.c 

PLUGINS=$(COMMON_CODE) $(CUSTOM_STYLES) $(SHORTCUTS) $(READABILITY)


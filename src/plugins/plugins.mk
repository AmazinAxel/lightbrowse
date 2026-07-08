## Plugins
SHORTCUTS=./src/plugins/shortcuts/shortcuts.c
READABILITY=./src/plugins/readability/readability.c
BOOKMARKS=./src/plugins/bookmarks/bookmarks.c
CALCULATOR=./src/plugins/calculator/tinyexpr.c ./src/plugins/calculator/calculator.c
ADBLOCK=./src/plugins/adblock/content_filters.c
PASSWORDS=./src/plugins/passwords/passwords.c

PLUGINS=$(SHORTCUTS) $(READABILITY) $(BOOKMARKS) $(CALCULATOR) $(ADBLOCK) $(PASSWORDS)

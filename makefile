# Lightbrowse — development makefile
#
# Intended to be run inside `nix develop`, which provides the C toolchain and
# all GTK4 / WebKitGTK dependencies. The Nix package (flake.nix) builds the
# release artifacts; this makefile is for fast local iteration.

CC ?= gcc
WARNINGS=-Wall -Wextra -Wshadow -Wformat=2 -Wno-unused-parameter
OPTIMIZED=-O2
DEBUG=-g
STD=-std=c23
SECURITY=-fstack-protector-strong

# Dependencies for WebKitGTK 6 / GTK4
SRC=src/lightbrowse.c
DEPS=webkitgtk-6.0 gtk4
INCS=`pkg-config --cflags $(DEPS)`
LIBS=`pkg-config --libs $(DEPS)` -lm

CONFIG=src/config.h

# Plugins
include src/plugins/plugins.mk

# Runtime assets live alongside the binary in the dev tree. The binary is told to
# look here via -DLIGHTBROWSE_SHARE_DIR.
SHARE_DIR=$(CURDIR)/out/share/lightbrowse
ADBLOCK_CONFIG=adblock/filter_lists.toml

.PHONY: build assets adblock format lint clean run inspect

build: assets $(SRC) $(PLUGINS) $(CONFIG)
	$(CC) $(STD) $(WARNINGS) $(SECURITY) $(OPTIMIZED) $(DEBUG) \
	  -DLIGHTBROWSE_SHARE_DIR='"$(SHARE_DIR)"' \
	  $(INCS) $(PLUGINS) $(SRC) -o out/lightbrowse $(LIBS)

# Assemble the dev share dir: readability.js + the adblock filter JSON.
assets: adblock
	mkdir -p $(SHARE_DIR)
	cp -f src/plugins/readability/readability.js $(SHARE_DIR)/readability.js

# Generate WebKit content-blocker JSON from the shipped list set. Fetches upstream
# lists, so it's done once and skipped if present (`make clean` forces a refresh).
# Needs the ublock-webkit-filters converter, provided by `nix develop`.
adblock:
	@if [ ! -f $(SHARE_DIR)/adblock/combined-part1.json ]; then \
	  echo "generating adblock filters..."; \
	  mkdir -p $(SHARE_DIR)/adblock; \
	  ublock-webkit-filters convert -c $(ADBLOCK_CONFIG) --output $(SHARE_DIR)/adblock; \
	  printf 'dev' > $(SHARE_DIR)/adblock/version; \
	else echo "adblock filters present (make clean to refresh)"; fi

format:
	clang-format -i -style="{BasedOnStyle: webkit, AllowShortIfStatementsOnASingleLine: true, IndentCaseLabels: true, AllowShortEnumsOnASingleLine: true}" \
	  $(SRC) $(PLUGINS) $(CONFIG)

lint:
	clang-tidy $(SRC) $(PLUGINS) -- $(STD) $(WARNINGS) $(OPTIMIZED) \
	  -DLIGHTBROWSE_SHARE_DIR='"$(SHARE_DIR)"' $(INCS)

clean:
	rm -rf out

run: build
	./out/lightbrowse

inspect: build
	GTK_DEBUG=interactive ./out/lightbrowse

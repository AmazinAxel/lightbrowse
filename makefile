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

# Runtime assets live alongside the binary in the dev tree. Both the binary and
# the adblock extension are told to look here via -DLIGHTBROWSE_SHARE_DIR.
SHARE_DIR=$(CURDIR)/out/share/lightbrowse
ADBLOCK_DIR=src/plugins/adblock

.PHONY: build assets adblock format lint clean run inspect

build: assets $(SRC) $(PLUGINS) $(CONFIG)
	$(CC) $(STD) $(WARNINGS) $(SECURITY) $(OPTIMIZED) $(DEBUG) \
	  -DLIGHTBROWSE_SHARE_DIR='"$(SHARE_DIR)"' \
	  $(INCS) $(PLUGINS) $(SRC) -o out/lightbrowse $(LIBS)

# Assemble the dev share dir: readability.js + the adblock extension.
assets: adblock
	mkdir -p $(SHARE_DIR)
	cp -f src/plugins/readability/readability.js $(SHARE_DIR)/readability.js

adblock:
	mkdir -p $(SHARE_DIR)/extensions
	$(MAKE) -C $(ADBLOCK_DIR) \
	  ADBLOCK_FILTERLIST_PATH=$(SHARE_DIR)/filterlist.txt
	cp -f $(ADBLOCK_DIR)/liblightbrowse-adblock.so $(SHARE_DIR)/extensions/

format:
	clang-format -i -style="{BasedOnStyle: webkit, AllowShortIfStatementsOnASingleLine: true, IndentCaseLabels: true, AllowShortEnumsOnASingleLine: true}" \
	  $(SRC) $(PLUGINS) $(CONFIG)

lint:
	clang-tidy $(SRC) $(PLUGINS) -- $(STD) $(WARNINGS) $(OPTIMIZED) \
	  -DLIGHTBROWSE_SHARE_DIR='"$(SHARE_DIR)"' $(INCS)

clean:
	rm -rf out
	$(MAKE) -C $(ADBLOCK_DIR) clean

run: build
	./out/lightbrowse

inspect: build
	GTK_DEBUG=interactive ./out/lightbrowse

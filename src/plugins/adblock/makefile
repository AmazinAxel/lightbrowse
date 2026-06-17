# Lightbrowse Adblock Extension Makefile
#
# This builds the adblock web extension as a shared library (.so)
# that WebKit loads into the web process.

CC ?= gcc
CFLAGS ?= -std=c23 -Wall -Wextra -O2 -fPIC
LDFLAGS ?= -shared

# WebKit dependencies
# Note: webkitgtk-web-process-extension-6.0 provides headers for web extensions in webkitgtk 6.0
WEBKIT_EXT_DEPS = webkitgtk-web-process-extension-6.0
WEBKIT_EXT_CFLAGS = $(shell pkg-config --cflags $(WEBKIT_EXT_DEPS) glib-2.0 gio-2.0)
WEBKIT_EXT_LIBS = $(shell pkg-config --libs $(WEBKIT_EXT_DEPS) glib-2.0 gio-2.0)

# Configuration (overridable; the Nix package points these into the store)
ADBLOCK_FILTERLIST_PATH ?= /opt/lightbrowse/filterlist.txt
EXTENSION_DIR ?= /opt/lightbrowse/extensions

# Source files
SRCS = adblock_extension.c uri_tester.c
TARGET = liblightbrowse-adblock.so

# Build flags
ALL_CFLAGS = $(CFLAGS) $(WEBKIT_EXT_CFLAGS) -DADBLOCK_FILTERLIST_PATH=\"$(ADBLOCK_FILTERLIST_PATH)\"
ALL_LDFLAGS = $(LDFLAGS) $(WEBKIT_EXT_LIBS)

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(ALL_CFLAGS) $(SRCS) -o $@ $(ALL_LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(EXTENSION_DIR)
	install -m 755 $(TARGET) $(DESTDIR)$(EXTENSION_DIR)/

uninstall:
	rm -f $(DESTDIR)$(EXTENSION_DIR)/$(TARGET)

# Debug build
debug: CFLAGS += -g -DDEBUG
debug: all

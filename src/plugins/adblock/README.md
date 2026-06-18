# Lightbrowse Adblock Plugin

A WebKit web extension that blocks ads using EasyList-format filter rules.

## How It Works

This plugin consists of two parts:

1. **Web Extension** (`liblightbrowse-adblock.so`) - A shared library loaded by WebKit into the web process. It intercepts all HTTP requests and checks them against the filter rules.

2. **URI Tester** (`uri_tester.c`) - The filter matching engine, adapted from Epiphany/GNOME Web browser. It parses EasyList-format filter rules and matches URLs against them. Comment and cosmetic lines (`!`, `#`, `##`) are skipped, so uBlock Origin lists degrade gracefully to their network rules.

## Packaging

The Nix flake at the repository root builds and wires this up automatically:

- It compiles `liblightbrowse-adblock.so` and installs it to
  `$out/share/lightbrowse/extensions/`.
- It fetches EasyList, EasyPrivacy and the uBlock Origin filter lists (with no
  pinned hash, hence `nix build --impure`), concatenates them into
  `$out/share/lightbrowse/filterlist.txt`, and bakes that path into the
  extension via `-DADBLOCK_FILTERLIST_PATH`.

The browser configures adblock through `config.h`:

- `ADBLOCK_FILTERLIST_PATH` — derived from `LIGHTBROWSE_SHARE_DIR`.
- `ADBLOCK_EXTENSIONS_DIR` — derived from `LIGHTBROWSE_SHARE_DIR`.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    LIGHTBROWSE PROCESS                       │
│  ┌────────────────────────────────────────────────────────┐ │
│  │                    WebKit Web Process                   │ │
│  │  ┌──────────────────────────────────────────────────┐  │ │
│  │  │       liblightbrowse-adblock.so (extension)      │  │ │
│  │  │                                                  │  │ │
│  │  │  on_send_request() ──► adblock_uri_tester_test() │  │ │
│  │  │         │                       │                │  │ │
│  │  │         ▼                       ▼                │  │ │
│  │  │  Return TRUE (block)    Check against filters    │  │ │
│  │  │  or FALSE (allow)       loaded from filterlist   │  │ │
│  │  └──────────────────────────────────────────────────┘  │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

## Credits

- URI tester code adapted from [Epiphany/GNOME Web](https://gitlab.gnome.org/GNOME/epiphany)
- Original wyebadblock by [jun7](https://github.com/jun7/wyebadblock)
- Filter format by [EasyList](https://easylist.to/)

## License

GPL-3.0 (same as Epiphany source code)

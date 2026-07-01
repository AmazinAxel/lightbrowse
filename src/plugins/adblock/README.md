# Lightbrowse Adblock Plugin

Ad blocking via WebKit's **native content-blocker** engine
(`WebKitUserContentFilterStore`), fed by JSON generated from EasyList/uBlock
Origin lists. This replaces the old web-process extension + GRegex matcher: WebKit
now compiles the rules once into a serialized bytecode matcher (cached on disk) and
does all matching itself — network blocking **and** cosmetic element hiding.

## How It Works

- **`content_filters.c` / `.h`** — the whole runtime. `adblock_content_init()`
  loads each `combined-part*.json` through a `WebKitUserContentFilterStore` (loading
  from the cache when possible, compiling from source the first time), and
  `adblock_apply_to_view()` adds the compiled filters to each tab's
  `WebKitUserContentManager`. Compilation is async; views opened before it finishes
  are back-filled when it lands.
- **Version keying** — the store identifiers embed a shipped `version` marker, so a
  new list release recompiles exactly once and then reloads from cache. When the
  version changes, the old compiled blobs are wiped for disk hygiene.

## Filter generation

The JSON is produced by [`bnema/ublock-webkit-filters`](https://github.com/bnema/ublock-webkit-filters),
a Go converter that turns EasyList/uBO syntax into WebKit content-blocker rules. It
**skips** anything WebKit can't faithfully express (scriptlets, procedural
cosmetics, redirects/CSP) rather than broadening it — so YouTube/Spotify inline
video ads and most anti-adblock (which uBO handles with JS scriptlets) are **not**
blocked. What is covered: network requests + `css-display-none` cosmetic hiding,
including cookie-consent and annoyance banners.

The list set is defined once in [`adblock/filter_lists.toml`](../../../adblock/filter_lists.toml)
(repo root) and includes EasyList, EasyPrivacy, the uBO filter/privacy/badware/
unbreak/quick-fixes lists, Peter Lowe's list, uBO Annoyances, and Fanboy's cookie
list.

## Packaging

- **Nix (`flake.nix`)** builds the converter from source (`buildGoModule`), fetches
  the lists via `builtins.fetchurl` (no pinned hash → `nix build --impure`), serves
  them over the sandbox loopback interface (the converter is HTTP-only), runs the
  conversion, and installs `combined-part*.json` + `version` into
  `$out/share/lightbrowse/adblock/`.
- **Dev (`make adblock`)** runs the converter (provided by `nix develop`) directly
  against the TOML into `out/share/lightbrowse/adblock/`; done once, `make clean`
  refreshes.

The browser finds the filters through `config.h`:

- `ADBLOCK_FILTERS_DIR` — shipped JSON + `version`, from `LIGHTBROWSE_SHARE_DIR`.
- `ADBLOCK_STORE_DIR` — WebKit's writable compiled-filter cache, under `DATA_DIR`.
- `ADBLOCK_ENABLED` — compile-time on/off.

## License

GPL-3.0

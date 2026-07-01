## Lightbrowse

A very lightweight GTK4 & Webkit browser for very fast browsing, forked from [Rosenrot browser](https://github.com/NunoSempere/rosenrot-browser).

### Building / installing (Nix)

The package builds the ad-block filter JSON at build time (fetching EasyList,
EasyPrivacy and the uBlock Origin lists *without a pinned hash*), so it must be
built impurely:

```sh
nix build --impure
```

Add it to your own flake by referencing `packages.<system>.lightbrowse` (or
`default`). The build is fully self-contained — it ships its own homepage,
readability script and WebKit content-blocker filters inside the store output;
nothing is written to your home or `/etc`. Ad blocking uses WebKit's native
content-blocker engine — see [`src/plugins/adblock`](src/plugins/adblock).

### Development

```sh
nix develop      # drops you into a shell with the toolchain + GTK/WebKit deps
make run         # build out/lightbrowse and launch it
```

In the dev tree the runtime assets live under `out/share/lightbrowse/`. `make`
generates the ad-block filter JSON once (via the `ublock-webkit-filters`
converter from the dev shell); run `make clean` to force a refresh.

## Lightbrowse

A very lightweight GTK4 & Webkit browser for very fast browsing, forked from [Rosenrot browser](https://github.com/NunoSempere/rosenrot-browser).

### Building / installing (Nix)

The package fetches the adblock filter lists (EasyList, EasyPrivacy and the
uBlock Origin lists) at build time *without a pinned hash*, so it must be built
impurely:

```sh
nix build --impure
```

Add it to your own flake by referencing `packages.<system>.lightbrowse` (or
`default`). The build is fully self-contained — it ships its own homepage,
readability script, adblock extension and combined filter list inside the
store output; nothing is written to your home or `/etc`.

### Development

```sh
nix develop      # drops you into a shell with the toolchain + GTK/WebKit deps
make run         # build out/lightbrowse and launch it
```

In the dev tree the runtime assets live under `out/share/lightbrowse/`. The
adblock filter list is only assembled by the Nix package, so adblock stays
disabled under `make` unless you drop a `filterlist.txt` there yourself.

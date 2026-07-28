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

### Instant launches (`--prewarm`)

A cold GTK4 start costs ~400ms before anything is on screen, and roughly 250ms of
that is GTK itself (window setup + GSK renderer), not lightbrowse. `--prewarm`
pays it up front: the process builds and *realizes* its window without mapping
it, then sits resident until something activates it — at which point the window
maps in ~10ms. No tab is created, so a prewarmed instance is just the GTK chrome;
WebKit is only spun up on first use.

Idle cost of a prewarmed instance: ~120MB RSS / ~89MB PSS, of which only ~22MB is
private dirty — the rest is file-backed library and GPU-driver mapping the kernel
can drop under pressure. (A bare GTK4 window with a realized surface is ~59MB PSS
on its own, so lightbrowse adds ~30MB to it.) Ad-block filters are deliberately
*not* loaded during prewarm: they cost ~110MB resident and are only needed once a
page actually loads, so they come up with the first tab instead.

Start it once at login (sway), and bind the launch key to a D-Bus activation
rather than re-running the binary — that spends ~140ms dynamically linking WebKit
before it can hand off, where the bus call is ~10ms. `busctl` comes with systemd,
so it needs nothing installed:

```
exec lightbrowse --prewarm
bindsym $mod+e exec busctl --user call com.amazinaxel.lightbrowse \
    /com/amazinaxel/lightbrowse org.gtk.Application Activate 'a{sv}' 0
```

(`gapplication launch com.amazinaxel.lightbrowse` does the same thing in one short
line, but pulls in `pkgs.glib`.)

Running `lightbrowse` normally still works and reaches the same instance — it now
hands off over D-Bus before building any GTK application state. `--prewarm` is a
no-op when an instance is already running. The prewarm ends when the window is
closed (the process exits with it), so re-run the `exec` line if you want it back.

### Development

```sh
nix develop      # drops you into a shell with the toolchain + GTK/WebKit deps
make run         # build out/lightbrowse and launch it
```

In the dev tree the runtime assets live under `out/share/lightbrowse/`. `make`
generates the ad-block filter JSON once (via the `ublock-webkit-filters`
converter from the dev shell); run `make clean` to force a refresh.

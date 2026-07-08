{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f:
        nixpkgs.lib.genAttrs systems
          (system: f system nixpkgs.legacyPackages.${system});

      # Version of bnema/ublock-webkit-filters we build the converter from.
      # Baked into the shipped `version` marker: bumping it makes the browser
      # recompile the content-blocker JSON exactly once (see content_filters.c).
      converterVersion = "v2026.06.30";

      # The converter turns EasyList/uBO lists into WebKit content-blocker JSON
      # (network blocks + cosmetic hiding). Built from source so we can feed it our
      # own list set (adblock/filter_lists.toml — adds cookie/annoyance lists).
      converterFor = pkgs: pkgs.buildGoModule {
        pname = "ublock-webkit-filters";
        version = converterVersion;
        src = pkgs.fetchFromGitHub {
          owner = "bnema";
          repo = "ublock-webkit-filters";
          rev = converterVersion;
          # nix build --impure once with lib.fakeHash, then paste the reported hash.
          hash = "sha256-/V/auyEcfiCYvxNrY64yTR4lUesbcc4JG8XUhPF1PWk=";
        };
        # buildGoModule needs the vendored-deps hash; same fill-in-from-error dance.
        vendorHash = "sha256-I3Dnf6EADqyYTDlk881xIIPGZbxvkbBxO1l+U1Cbbgg=";
        subPackages = [ "cmd/ublock-webkit-filters" ];
      };
    in {
      packages = forAllSystems (system: pkgs:
        let
          lib = pkgs.lib;
          ublockConverter = converterFor pkgs;

          # Single source of truth for which lists we ship: parse the same TOML the
          # dev `make adblock` target uses.
          adblockConfig = builtins.fromTOML (builtins.readFile ./adblock/filter_lists.toml);
          enabledLists = builtins.filter (l: l.enabled) adblockConfig.lists;
          # Fetch each list impurely (NEEDS --impure; no hash, like the old flake).
          # The converter only speaks HTTP, and the build sandbox has no outbound
          # network — so we serve the fetched files back over loopback (which the
          # sandbox does provide) and point the converter at localhost.
          # Explicit name: some list URLs carry query strings that aren't valid
          # store-path names (e.g. Peter Lowe's serverlist.php?...).
          fetchedLists = map
            (l: l // { path = builtins.fetchurl { inherit (l) url name; }; })
            enabledLists;
          adblockPort = "8765";
          localToml = pkgs.writeText "filter_lists_local.toml" ''
            [http]
            timeout = "30s"
            retries = 3
            [output]
            max_rules_per_file = ${toString adblockConfig.output.max_rules_per_file}
            generate_combined = true
            generate_manifest = true
            ${lib.concatMapStringsSep "\n" (l: ''
              [[lists]]
              name = "${l.name}"
              url = "http://127.0.0.1:${adblockPort}/${l.name}"
              enabled = true
            '') fetchedLists}
          '';

          adblockFilters = pkgs.runCommand "lightbrowse-adblock-filters"
            { nativeBuildInputs = [ ublockConverter pkgs.python3 ]; } ''
            mkdir -p docroot "$out"
            ${lib.concatMapStringsSep "\n"
              (l: "cp ${l.path} docroot/${l.name}") fetchedLists}

            python3 -m http.server ${adblockPort} --directory docroot &
            server=$!
            trap "kill $server 2>/dev/null" EXIT
            # Wait for the loopback server to accept connections before converting.
            for _ in $(seq 1 50); do
              if python3 -c 'import socket,sys; sys.exit(0 if socket.socket().connect_ex(("127.0.0.1",${adblockPort}))==0 else 1)'; then
                break
              fi
              sleep 0.1
            done

            ublock-webkit-filters convert -c ${localToml} --output "$out"
            # Version marker the browser reads to key WebKit's compiled-filter cache.
            printf '%s' "${converterVersion}" > "$out/version"
          '';

          gstPlugins = with pkgs.gst_all_1; [
            gstreamer
            gst-plugins-base
            gst-plugins-good # v4l2 device provider (USB/UVC webcams)
            gst-plugins-bad
            gst-plugins-ugly
            gst-libav
            gst-plugin-pipewire # PipeWire camera/mic device provider — without it WebKit
                                # enumerates 0 capture devices on PipeWire-routed systems
          ];

          buildInputs = (with pkgs; [
            glib
            glib-networking
            gsettings-desktop-schemas # color-scheme (website light/dark follows system)
            gtk4
            webkitgtk_6_0
          ]) ++ gstPlugins;

          lightbrowse = pkgs.stdenv.mkDerivation {
            pname = "lightbrowse";
            version = "1.0.0";

            src = ./.;

            nativeBuildInputs = with pkgs; [ pkg-config wrapGAppsHook4 ];

            inherit buildInputs;

            buildPhase = ''
              runHook preBuild
              mkdir -p out

              # Main browser binary. LIGHTBROWSE_SHARE_DIR is baked in so the
              # binary finds readability.js + the adblock filter JSON in the
              # store at runtime.
              gcc -std=c23 -O2 -flto -Wall -Wextra -Wno-unused-parameter -fstack-protector-strong \
                -DLIGHTBROWSE_SHARE_DIR="\"$out/share/lightbrowse\"" \
                $(pkg-config --cflags webkitgtk-6.0 gtk4) \
                src/plugins/shortcuts/shortcuts.c \
                src/plugins/readability/readability.c \
                src/plugins/bookmarks/bookmarks.c \
                src/plugins/calculator/tinyexpr.c \
                src/plugins/calculator/calculator.c \
                src/plugins/adblock/content_filters.c \
                src/plugins/passwords/passwords.c \
                src/lightbrowse.c \
                -o out/lightbrowse \
                $(pkg-config --libs webkitgtk-6.0 gtk4) -lm

              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              install -Dm755 out/lightbrowse $out/bin/lightbrowse
              install -Dm644 src/plugins/readability/readability.js \
                $out/share/lightbrowse/readability.js

              # WebKit content-blocker JSON (+ version marker) the browser compiles
              # and attaches at runtime via WebKitUserContentFilterStore.
              install -d $out/share/lightbrowse/adblock
              install -Dm644 ${adblockFilters}/combined-part*.json \
                -t $out/share/lightbrowse/adblock
              install -Dm644 ${adblockFilters}/version \
                $out/share/lightbrowse/adblock/version

              # Desktop entry: required so the system can route http(s) links to
              install -d $out/share/applications
              printf '%s\n' \
                '[Desktop Entry]' \
                'Type=Application' \
                'Name=Lightbrowse' \
                "Exec=$out/bin/lightbrowse %U" \
                'Terminal=false' \
                'NoDisplay=true' \
                'StartupNotify=false' \
                'MimeType=x-scheme-handler/http;x-scheme-handler/https;text/html;' \
                > $out/share/applications/com.amazinaxel.lightbrowse.desktop
              runHook postInstall
            '';
          };
        in {
          default = lightbrowse;
          inherit lightbrowse;
        });

      devShells = forAllSystems (system: pkgs:
        let
          # Same converter the package builds, exposed in the dev shell so `make
          # adblock` can regenerate the content-blocker JSON locally.
          ublockConverter = converterFor pkgs;
          gstPlugins = with pkgs.gst_all_1; [
            gstreamer
            gst-plugins-base
            gst-plugins-good # v4l2 device provider (USB/UVC webcams)
            gst-plugins-bad
            gst-plugins-ugly
            gst-libav
            gst-plugin-pipewire # PipeWire camera/mic device provider — without it WebKit
                                # enumerates 0 capture devices on PipeWire-routed systems
          ];
        in {
          default = pkgs.mkShell {
            nativeBuildInputs = (with pkgs; [
              pkg-config
              wrapGAppsHook4
              gcc
              gnumake
              clang-tools # make format
            ]) ++ [ ublockConverter ]; # `make adblock` regenerates the filter JSON

            buildInputs = (with pkgs; [
              glib
              glib-networking
              gsettings-desktop-schemas
              gtk4
              webkitgtk_6_0
            ]) ++ gstPlugins;

            # needed for networking
            shellHook = ''
              export GIO_EXTRA_MODULES="${pkgs.glib-networking}/lib/gio/modules''${GIO_EXTRA_MODULES:+:$GIO_EXTRA_MODULES}"
              export GST_PLUGIN_SYSTEM_PATH_1_0="${nixpkgs.lib.makeSearchPath "lib/gstreamer-1.0" gstPlugins}''${GST_PLUGIN_SYSTEM_PATH_1_0:+:$GST_PLUGIN_SYSTEM_PATH_1_0}"
            '';
          };
        });
    };
}

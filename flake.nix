{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f:
        nixpkgs.lib.genAttrs systems
          (system: f system nixpkgs.legacyPackages.${system});
    in {
      packages = forAllSystems (system: pkgs:
        let
          # NEEDS IMPURITY TO BUILD LISTS (use --impure)
          fetch = url: builtins.fetchurl url;

          filterSources = [
            # EasyList family
            (fetch "https://easylist.to/easylist/easylist.txt")
            (fetch "https://easylist.to/easylist/easyprivacy.txt")
            # uBlock Origin's own filter lists
            (fetch "https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/filters.txt")
            (fetch "https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/badware.txt")
            (fetch "https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/privacy.txt")
            (fetch "https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/quick-fixes.txt")
            (fetch "https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/resource-abuse.txt")
            (fetch "https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/unbreak.txt")
          ];
          # combined lists
          filterList = pkgs.runCommand "lightbrowse-filterlist.txt" { } ''
            cat ${nixpkgs.lib.concatStringsSep " " filterSources} > $out
          '';

          gstPlugins = with pkgs.gst_all_1; [
            gstreamer
            gst-plugins-base
            gst-plugins-good
            gst-plugins-bad
            gst-plugins-ugly
            gst-libav
          ];

          buildInputs = (with pkgs; [
            glib
            glib-networking
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
              # binary finds readability.js + the adblock extension dir in the
              # store at runtime.
              gcc -std=c23 -O2 -flto -Wall -Wextra -Wno-unused-parameter -fstack-protector-strong \
                -DLIGHTBROWSE_SHARE_DIR="\"$out/share/lightbrowse\"" \
                $(pkg-config --cflags webkitgtk-6.0 gtk4) \
                src/plugins/shortcuts/shortcuts.c \
                src/plugins/readability/readability.c \
                src/lightbrowse.c \
                -o out/lightbrowse \
                $(pkg-config --libs webkitgtk-6.0 gtk4)

              # Adblock web-process extension: a shared library WebKit loads
              # into the web process to block requests against the filter list.
              # The filter list path is compiled in via -D.
              gcc -std=c23 -O2 -flto -Wall -Wextra -Wno-unused-parameter -fPIC -shared \
                $(pkg-config --cflags webkitgtk-web-process-extension-6.0 glib-2.0 gio-2.0) \
                -DADBLOCK_FILTERLIST_PATH="\"$out/share/lightbrowse/filterlist.txt\"" \
                src/plugins/adblock/adblock_extension.c \
                src/plugins/adblock/uri_tester.c \
                -o out/liblightbrowse-adblock.so \
                $(pkg-config --libs webkitgtk-web-process-extension-6.0 glib-2.0 gio-2.0)

              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              install -Dm755 out/lightbrowse $out/bin/lightbrowse
              install -Dm644 src/plugins/readability/readability.js \
                $out/share/lightbrowse/readability.js

              # Adblock extension + combined filter list
              install -Dm755 out/liblightbrowse-adblock.so \
                $out/share/lightbrowse/extensions/liblightbrowse-adblock.so
              install -Dm644 ${filterList} $out/share/lightbrowse/filterlist.txt
              runHook postInstall
            '';
          };
        in {
          default = lightbrowse;
          inherit lightbrowse;
        });

      devShells = forAllSystems (system: pkgs:
        let
          gstPlugins = with pkgs.gst_all_1; [
            gstreamer
            gst-plugins-base
            gst-plugins-good
            gst-plugins-bad
            gst-plugins-ugly
            gst-libav
          ];
        in {
          default = pkgs.mkShell {
            nativeBuildInputs = with pkgs; [
              pkg-config
              wrapGAppsHook4
              gcc
              gnumake
              clang-tools # make format
            ];

            buildInputs = (with pkgs; [
              glib
              glib-networking
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

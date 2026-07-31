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

      # WebKitGTK with the features nixpkgs leaves compiled out.
      #
      # nixpkgs builds webkitgtk with `enableExperimental = false`, and the GTK
      # port maps that single cmake switch (ENABLE_EXPERIMENTAL_FEATURES) onto a
      # batch of whole subsystems -- see Source/cmake/OptionsGTK.cmake:124-148.
      # With it off, these are absent from the binary no matter what
      # WebKitSettings says at runtime:
      #
      #   ENABLE_WEB_RTC          RTCPeerConnection (our "enable-webrtc": true
      #                           setting was inert without this)
      #   ENABLE_ENCRYPTED_MEDIA  EME/DRM (likewise "enable-encrypted-media")
      #   ENABLE_WEBXR (+_HIT_TEST, _LAYERS)
      #   ENABLE_WK_WEB_EXTENSIONS
      #   ENABLE_WEBDRIVER_BIDI
      #
      # It also pulls openssl + librice (WebRTC's ICE stack) and openxr-loader
      # into the build inputs, which is why it has to be an override rather than
      # extra cmakeFlags bolted on with overrideAttrs.
      #
      # Two things asked for are NOT here because the 2.52 release tarball cannot
      # build them on the GTK port, regardless of flags:
      #   * WebGPU  -- Source/WebGPU/WebGPU ships headers only (the Metal backend
      #     is Cocoa-only) and OptionsGTK.cmake never mentions ENABLE_WEBGPU.
      #   * WebAuthn -- UIProcess/WebAuthentication/AuthenticatorManager.cpp and
      #     the CTAP/HID transports appear in no port's Sources*.txt, so
      #     ENABLE_WEB_AUTHN=ON compiles the API and then fails to link.
      #
      # NOTE: no binary cache has this build (an override is a fresh derivation
      # hash; Hydra only ever builds nixpkgs' default one). Expect one long local
      # WebKit compile, then it is cached in /nix/store until nixpkgs bumps
      # webkitgtk. `nix build .#webkitgtk` builds just this, so it can be pushed
      # to a personal Cachix and shared across machines.
      # The overrideAttrs on top only trims build cost -- none of it changes the
      # runtime library. WebKit is already configured for speed upstream (Skia
      # rather than Cairo, JIT+FTL, bmalloc), so there is nothing to add there;
      # what is worth removing is work we never consume:
      #
      #   separateDebugInfo  nixpkgs sets this, which puts -g on every WebKit
      #                      translation unit. Debug info dominates compile
      #                      memory and disk here and lands in a `debug` output
      #                      nothing reads. Dropping it is the single biggest
      #                      build-time saving, and it lowers the peak memory
      #                      that makes this build risky on a small machine.
      #   ENABLE_MINIBROWSER WebKit's own sample browser (GTK default ON);
      #                      nixpkgs already turns it off on Darwin, so the
      #                      no-MiniBrowser config is upstream-supported.
      #   ENABLE_DOCUMENTATION  runs gi-docgen over the whole API to fill the
      #                      devdoc output. Safe to skip: the derivation's
      #                      postFixup moveToOutput no-ops when share/doc is
      #                      absent (nixpkgs multiple-outputs.sh:117), so devdoc
      #                      just comes out empty instead of failing.
      #
      # Deliberately NOT set: LTO_MODE=thin. It is the only remaining change that
      # would actually speed up *browsing* (~5%), but ThinLTO linking
      # libwebkitgtk wants 8-16GB and this machine has 7GB, so the link would run
      # out of swap at the very end of a multi-hour build.
      #
      # ninjaFlags caps parallelism regardless of how many cores nix hands the
      # build. WebCore's unified sources are the fattest translation units here
      # (~2GB of clang each at -O3), so ninja's default -j$NIX_BUILD_CORES = 8
      # drives a 7GB machine into swap; systemd-oomd then kills the whole build
      # cgroup with SIGTERM (nix reports "exit code 143") around 85% done.
      # ninja takes the last -j on the line, and the setup hook puts $ninjaFlags
      # after its own, so this wins.
      # webkitgtkFor = pkgs:
      #   (pkgs.webkitgtk_6_0.override { enableExperimental = true; })
      #   .overrideAttrs (old: {
      #     separateDebugInfo = false;
      #     ninjaFlags = (old.ninjaFlags or [ ]) ++ [ "-j3" ];
      #     cmakeFlags = old.cmakeFlags ++ [
      #       "-DENABLE_MINIBROWSER=OFF"
      #       "-DENABLE_DOCUMENTATION=OFF"
      #     ];
      #   });

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
            gst-plugins-good
            gst-plugins-bad
            gst-plugins-ugly
            gst-libav
          ];
          # PipeWire ships a GStreamer plugin (pipewiredeviceprovider) that WebKit's
          # capture process needs to enumerate the camera/mic on a PipeWire-routed
          # system — without it it sees 0 devices. It lives in pipewire's default
          # output but, unlike gst_all_1.*, carries no setup hook to register itself
          # on GST_PLUGIN_SYSTEM_PATH_1_0, so it's added to the wrapper by hand below.
          pipewireGstPath = "${pkgs.pipewire}/lib/gstreamer-1.0";

          # webkitgtk = webkitgtkFor pkgs;

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

            # wrapGAppsHook4 auto-adds the gst_all_1.* plugins (they set
            # GST_PLUGIN_SYSTEM_PATH_1_0 via a setup hook) but not PipeWire's, so
            # append it to the generated wrapper explicitly (see pipewireGstPath).
            preFixup = ''
              gappsWrapperArgs+=(--prefix GST_PLUGIN_SYSTEM_PATH_1_0 : "${pipewireGstPath}")
            '';

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
                src/plugins/imagesearch/imagesearch.c \
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

              # Desktop entry: required so the system can route http(s) links to us.
              #
              # StartupNotify must stay true. GLib only asks the launching app for an
              # xdg-activation token when this key is set (gdesktopappinfo.c guards the
              # whole block on it), and without that token in XDG_ACTIVATION_TOKEN the
              # running instance has no proof the request came from something the user
              # just clicked -- so sway refuses to raise us and the link opens in a tab
              # on a workspace you cannot see. Nothing is lost by claiming startup
              # notification: the token is consumed by gtk_window_present(), and a
              # compositor simply expires it if we never do.
              install -d $out/share/applications
              printf '%s\n' \
                '[Desktop Entry]' \
                'Type=Application' \
                'Name=Lightbrowse' \
                "Exec=$out/bin/lightbrowse %U" \
                'Terminal=false' \
                'NoDisplay=true' \
                'StartupNotify=true' \
                'MimeType=x-scheme-handler/http;x-scheme-handler/https;text/html;' \
                > $out/share/applications/com.amazinaxel.lightbrowse.desktop
              runHook postInstall
            '';
          };
        in {
          default = lightbrowse;
          # Exposed so the long WebKit compile can be built (and pushed to a
          # personal binary cache) on its own: `nix build .#webkitgtk`.
          inherit lightbrowse ;
        });

      devShells = forAllSystems (system: pkgs:
        let
          # Same converter the package builds, exposed in the dev shell so `make
          # adblock` can regenerate the content-blocker JSON locally.
          ublockConverter = converterFor pkgs;
          # Mirrors the package's runtime GStreamer plugins so `make run` has the same
          # media stack. pkgs.pipewire supplies pipewiredeviceprovider (camera/mic
          # enumeration); makeSearchPath in the shellHook adds each to GST_PLUGIN_SYSTEM_PATH_1_0.
          gstPlugins = (with pkgs.gst_all_1; [
            gstreamer
            gst-plugins-base
            gst-plugins-good
            gst-plugins-bad
            gst-plugins-ugly
            gst-libav
          ]) ++ [ pkgs.pipewire ];
        in {
          default = pkgs.mkShell {
            nativeBuildInputs = (with pkgs; [
              pkg-config
              wrapGAppsHook4
              gcc
              gnumake
              clang-tools # make format
            ]) ++ [ ublockConverter ]; # `make adblock` regenerates the filter JSON

            # Same WebKit build the package links against, so `make run` and
            # `nix build` behave identically (and share one compile).
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

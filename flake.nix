{
  description = "VST for Resolve Linux - run VST2/VST3/CLAP plugins inside DaVinci Resolve (BMDAudioPlugins bridge)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      lib = nixpkgs.lib;
      # x86_64 only, and not by preference.
      #
      # src/proxy.cpp contains hand-written x86-64 assembly - the trace and probe thunks are
      # written in it - so aarch64 does not compile. Resolve's own vtable offsets, which this
      # bridge reads and patches, are x86-64 layout as well.
      systems = [ "x86_64-linux" ];
      # The build references davinci-resolve-studio, which is unfree, so the
      # nixpkgs used here must allow unfree. Importing with that config (rather
      # than legacyPackages, which defaults allowUnfree = false) also matches how
      # a consuming flake like nix-configs builds its pkgs, and it keeps following
      # the parent's nixpkgs input when this flake is folded into one.
      mkPkgs = system: import nixpkgs {
        inherit system;
        config.allowUnfree = true;
      };
      forAllSystems = f: lib.genAttrs systems (sys: f (mkPkgs sys));

      # The two vendored third_party/ submodules are not checked out in a
      # bare clone, so they are fetched explicitly (and pinned) and overlaid
      # onto the source tree at build time. This keeps the flake reproducible
      # whether the main repo is referenced by path, by github, or in CI.
      #
      # Hashes are computed by nix from the bytes it actually downloads; a
      # lib.fakeHash placeholder fails the build and prints the real value.
      fetchSubmodule = owner: repo: rev: hash:
        (mkPkgs (builtins.head systems)).fetchFromGitHub {
          inherit owner repo rev hash;
          fetchSubmodules = false;
        };

      # The build source: this repo's files, with the pinned submodules laid
      # into third_party/. Any developer-checked-out copies are discarded first
      # so the result never depends on local checkout state.
      buildSrc = pkgs:
        let
          src = self;
          clap = fetchSubmodule "free-audio" "clap" "a47f6badb49d948fd009998f28309cdab78979c9"
            "sha256-CHhQO/RdNWuhOeoY/xUZg/rvpMy4g0gu1JwkspOOqG4=";
          dpf  = fetchSubmodule "DISTRHO" "DPF" "4238e1c7f0351bbe488d79f0899c540543ac7583"
            "sha256-7xuxcT6W8vhJY91WPPqnGNkzX4PZKkm60bpNMGZrE1E=";
        in
        pkgs.runCommand "vstforresolve-linux-src" { } ''
          # Copying a store path makes the new dir mode 555 (read-only), so make
          # it writable immediately - any later mkdir/copy would otherwise fail.
          cp -r ${src} $out
          chmod -R u+rwX $out
          rm -rf $out/.git
          rm -rf $out/third_party
          mkdir -p $out/third_party
          cp -r ${clap} $out/third_party/clap
          cp -r ${dpf}  $out/third_party/dpf
          chmod -R u+rwX $out
        '';

      # Bumped by hand with the tag and CHANGELOG.md. Nix cannot read it from git here without
      # making the build impure, so this is the one place that has to be kept in step.
      version = "0.2.11";

      # Common compiler flags. carla is headers-only here (CarlaNative.h /
      # CarlaDefines.h); the library is dlopen()'d at run time, never linked.
      # Single line so it slots into a `\`-continued g++ command intact.
      includeFlags = carla:
        "-I third_party/clap/include -I third_party/dpf/distrho/src/travesty -I ${carla}/include/carla/includes";

      # The actual bridge: the shared library Resolve dlopen()'s, plus the
      # standalone scanner. Everything lands under $out/BMDAudioPlugins so the
      # consumer can point BMDPlugins.Path at $out/BMDAudioPlugins/libfxbridge.so.
      bmdaudioplugins = pkgs:
        let
          carla   = pkgs.carla;
          # davinci-resolve-studio is an FHS-env *wrapper*; the real app tree
          # (bin/, libs/, BlackmagicRAWPlayer/, ...) lives in its inner `davinci`
          # derivation. We point at that inner one, not the outer wrapper.
          davinci = pkgs.davinci-resolve-studio.davinci;
        in
        pkgs.stdenv.mkDerivation {
          pname = "bmdaudioplugins";
          inherit version;
          src = buildSrc pkgs;

          # carla is only read for its headers during compilation; libX11 and
          # zlib are linked. dl/pthread come from the toolchain's glibc.
          # davinci (the inner Resolve derivation) is a build-time reference
          # only: its libs/ path is baked into the .so, so the store path must be
          # present for that dlopen to resolve at run time.
          nativeBuildInputs = [ carla davinci ];
          buildInputs = [ pkgs.libX11 pkgs.zlib ];

          # No autotools here; the two g++ invocations are hand-rolled,
          # mirroring build.sh.
          dontConfigure = true;
          dontMakeBuildWrapper = true;

          # The bridge dlopens Resolve's stock BMD library (kStockLibrary) to
          # clone it. That constant is hardcoded to /opt/resolve/libs/..., which
          # does not exist under Nix. Point it at the inner davinci store path (a
          # sibling of the FHS wrapper, whose libs/ really holds the .so) so the
          # dlopen resolves at run time. The stock lib file is unchanged; only its
          # location changes.
          patchPhase = ''
            sed -i "s|/opt/resolve|${davinci}|g" src/proxy.cpp
          '';

          buildPhase = ''
            mkdir -p build

            # The plugin (shared object).
            g++ -std=c++17 -shared -fPIC -O2 -Wall -Wextra \
              ${includeFlags carla} \
              -o build/libfxbridge.so \
              src/proxy.cpp src/carla_host.cpp src/vst2_plugin.cpp \
              src/clap_plugin.cpp src/host_thread.cpp src/vst3_plugin.cpp \
              src/plugin_scan.cpp src/plugin_state.cpp src/fx_categories.cpp \
              src/plugin_window.cpp \
              -ldl -lX11 -lz -lpthread

            # The pre-launch scanner (standalone executable).
            g++ -std=c++17 -O2 -Wall -Wextra \
              ${includeFlags carla} \
              -o build/fxbridge-scan \
              src/scan_main.cpp src/plugin_scan.cpp src/vst3_plugin.cpp \
              src/host_thread.cpp src/plugin_window.cpp src/fx_categories.cpp \
              -ldl -lX11 -lz -lpthread
          '';

          # Without this the checkPhase below never runs: stdenv only runs it when doCheck is
          # set, so the guard was present and dead.
          doCheck = true;

          # A library that dlopen()s against a private ABI can link cleanly
          # yet still carry unresolved symbols that only fail at load time.
          # Fail the build in that case rather than ship a silent no-op.
          checkPhase = ''
            if ldd -r build/libfxbridge.so 2>&1 | grep -q "undefined symbol"; then
              echo "REFUSING TO INSTALL - libfxbridge.so has undefined symbols:" >&2
              ldd -r build/libfxbridge.so 2>&1 | grep "undefined symbol" >&2
              exit 1
            fi
          '';

          installPhase = ''
            mkdir -p $out/BMDAudioPlugins $out/bin
            install -m 755 build/libfxbridge.so $out/BMDAudioPlugins/libfxbridge.so
            # The scanner lives in $out/bin so it lands on PATH when the package
            # is added to environment.systemPackages (a non-bin directory would not).
            install -m 755 build/fxbridge-scan  $out/bin/fxbridge-scan
          '';

          meta = with lib; {
            description = "Bridge that lets VST2/VST3/CLAP plugins load in DaVinci Resolve on Linux";
            longDescription = ''
              Registers a private Blackmagic plugin path into Resolve and hosts a real
              CLAP / native-VST3 / (Windows VST via yabridge) effect behind a cloned
              "Stereo Delay" slot, patching Resolve's own structures at run time.
            '';
            homepage = "https://github.com/JaySNL/VSTForResolveLinux";
            license = licenses.mit;
            platforms = [ "x86_64-linux" ];
            maintainers = [ ];
          };
        };

      # A tiny installer that points Resolve at the library above. Nix cannot
      # touch $HOME at build time, so this is a script the user runs once:
      #   nix run .#bmdaudioplugins-install
      # It resolves its own store path (bmdaudioplugins is a build input here)
      # and writes BMDPlugins.Path into config-fairlight.dat.
      bmdaudioplugins-install = pkgs:
        let bmd = (bmdaudioplugins pkgs); in
        pkgs.stdenvNoCC.mkDerivation {
          name = "bmdaudioplugins-install";
          dontUnpack = true;
          nativeBuildInputs = [ bmd ];
          dontFixup = true;
          buildPhase = ''
            mkdir -p $out/bin
            cat > $out/bin/bmdaudioplugins-install <<'EOF'
            #!/bin/sh
            # Resolve points BMDPlugins.Path at an absolute path to libfxbridge.so.
            bmd='${bmd}'
            target="$bmd/BMDAudioPlugins/libfxbridge.so"

            config_dir="$HOME/.local/share/DaVinciResolve/configs"
            config="$config_dir/config-fairlight.dat"
            line="BMDPlugins.Path = $target"

            # Resolve owns this file and rewrites it on quit; editing it under a
            # running Resolve loses the change.
            # Matched on the path-agnostic part, because on NixOS Resolve is not in /opt at all -
            # which is the whole reason this flake rewrites that path during the build. Looking for
            # /opt/resolve here meant the guard could never fire on the platform it was written
            # for, and Resolve rewrites this file on quit, so the edit would be silently lost.
            # Its process is called GUI rather than resolve, which is why this greps comm.
            if ps -eo comm,args 2>/dev/null | awk '$1=="GUI" && /\/bin\/resolve/ { found=1 } END { exit !found }'; then
              echo "Resolve is RUNNING; close it first, or add by hand:" >&2
              echo "    $line" >&2
              exit 1
            fi

            mkdir -p "$config_dir"
            [ -f "$config" ] || : > "$config"

            current=$(sed -n 's/^[[:space:]]*BMDPlugins\.Path[[:space:]]*=\{0,1\}[[:space:]]*//p' "$config" | tail -1)
            if [ "$current" = "$target" ]; then
              echo "config already points at this library"
              exit 0
            fi

            backup="$config.before-fxbridge-$(date +%Y%m%d-%H%M%S)"
            [ -f "$config" ] && cp "$config" "$backup"

            tmp="$config.fxbridge-tmp"
            awk -v repl="$line" '
              /^[[:space:]]*BMDPlugins\.Path([[:space:]]|=)/ { if (!done) { print repl; done = 1 } next }
              { print }
              END { if (!done) print repl }
            ' "$config" > "$tmp"
            mv "$tmp" "$config"

            echo "wrote $config"
            echo "  $line"
            [ -f "$backup" ] && echo "  previous kept at $backup"
            echo "Start Resolve. The plugins are in the Audio FX panel."
            EOF
            chmod +x $out/bin/bmdaudioplugins-install
          '';
        };
    in
    {
      packages = forAllSystems (pkgs: {
        bmdaudioplugins = bmdaudioplugins pkgs;
        bmdaudioplugins-install = bmdaudioplugins-install pkgs;
        default = (bmdaudioplugins pkgs);
      });

      apps = forAllSystems (pkgs: {
        # nix run .#bmdaudioplugins-install
        bmdaudioplugins-install = {
          type = "app";
          program = "${(bmdaudioplugins-install pkgs)}/bin/bmdaudioplugins-install";
        };
      });
    };
}

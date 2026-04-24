{
  description = "Yetty - WebGPU Terminal Emulator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config = {
            allowUnfree = true;
            android_sdk.accept_license = true;
          };
        };

        # Use Clang with libstdc++ (GCC's C++ library) for Dawn ABI compatibility
        clangStdenv = pkgs.clangStdenv;

        # Yetty package (nix build .#yetty)
        yetty = pkgs.callPackage ./build-tools/nix/default.nix {
          inherit clangStdenv;
        };

        # Android SDK/NDK setup
        androidComposition = pkgs.androidenv.composeAndroidPackages {
          platformVersions = [ "34" ];
          buildToolsVersions = [ "34.0.0" ];
          ndkVersions = [ "26.1.10909125" ];
          cmakeVersions = [ "3.22.1" ];
          includeNDK = true;
          # Emulator support
          includeEmulator = true;
          includeSystemImages = true;
          systemImageTypes = [ "google_apis" ];
          abiVersions = [ "arm64-v8a" "x86_64" ];
        };

        androidSdk = androidComposition.androidsdk;
        androidNdk = "${androidSdk}/libexec/android-sdk/ndk/26.1.10909125";

        # Common build dependencies
        commonDeps = with pkgs; [
          cmake
          ninja
          pkg-config
          git
          llvmPackages_21.clang
          llvmPackages_21.clang-unwrapped.dev
          llvmPackages_21.lld
          llvmPackages_21.llvm
          llvmPackages_21.libclang
          meson  # For dav1d (AV1 decoder)
          nasm   # For dav1d assembly optimizations
        ];

        # Static library packages (for static linking)
        fontconfigStatic = pkgs.fontconfig.overrideAttrs (old: {
          configureFlags = (old.configureFlags or []) ++ [ "--enable-static" "--disable-shared" ];
          postInstall = (old.postInstall or "") + ''
            rm -f $out/lib/*.so*
          '';
        });
        expatStatic = pkgs.expat.overrideAttrs (old: {
          configureFlags = (old.configureFlags or []) ++ [ "--enable-static" "--disable-shared" ];
          postInstall = (old.postInstall or "") + ''
            rm -f $out/lib/*.so*
          '';
        });
        libuuidStatic = pkgs.libuuid.overrideAttrs (old: {
          configureFlags = (old.configureFlags or []) ++ [ "--enable-static" ];
        });

        # Desktop build dependencies
        desktopDeps = with pkgs; [
          # Graphics
          xorg.libX11
          xorg.libXcursor
          xorg.libXrandr
          xorg.libXi
          xorg.libXinerama
          libxkbcommon
          wayland
          libGL
          vulkan-loader
          vulkan-headers

          # Fonts
          freetype
          fontconfig
          expat  # Required by fontconfig

          # Other
          glfw
          zlib
          openssl
          brotli  # For asset compression in incbin

          # Static libraries (referenced via *_STATIC_LIB env vars, not linked globally)
          zlib.static
          fontconfigStatic
          expatStatic
          libuuidStatic
        ];

        # Android build dependencies
        androidDeps = [
          androidSdk
          pkgs.llvmPackages.libclang
          pkgs.llvmPackages.clang
          pkgs.qemu  # For ARM emulation
        ];

        # ARM emulator script using QEMU
        armEmulatorScript = pkgs.writeShellScriptBin "run-arm-emulator" ''
          SYSTEM_IMG="${androidSdk}/libexec/android-sdk/system-images/android-34/google_apis/arm64-v8a"

          echo "Starting ARM64 Android emulator (software emulation - will be slow)"
          echo "System image: $SYSTEM_IMG"

          # Create temp directory for runtime files
          WORKDIR=$(mktemp -d)
          trap "rm -rf $WORKDIR" EXIT

          echo "Copying images to writable location..."
          cp $SYSTEM_IMG/system.img $WORKDIR/system.img
          cp $SYSTEM_IMG/vendor.img $WORKDIR/vendor.img
          cp $SYSTEM_IMG/userdata.img $WORKDIR/userdata.img
          chmod 644 $WORKDIR/*.img

          # Resize userdata for more space
          ${pkgs.qemu}/bin/qemu-img resize -f raw $WORKDIR/userdata.img 2G

          echo "Starting QEMU with GUI..."
          # Run QEMU with Android system image and graphical display
          ${pkgs.qemu}/bin/qemu-system-aarch64 \
            -machine virt,gic-version=2 \
            -cpu cortex-a57 \
            -smp 4 \
            -m 3072 \
            -kernel $SYSTEM_IMG/kernel-ranchu \
            -initrd $SYSTEM_IMG/ramdisk.img \
            -drive file=$WORKDIR/system.img,format=raw,if=none,id=system,readonly=off \
            -device virtio-blk-device,drive=system \
            -drive file=$WORKDIR/vendor.img,format=raw,if=none,id=vendor,readonly=off \
            -device virtio-blk-device,drive=vendor \
            -drive file=$WORKDIR/userdata.img,format=raw,if=none,id=userdata \
            -device virtio-blk-device,drive=userdata \
            -append "androidboot.hardware=ranchu androidboot.selinux=permissive console=ttyAMA0 androidboot.console=ttyAMA0" \
            -netdev user,id=net0,hostfwd=tcp::5555-:5555 \
            -device virtio-net-device,netdev=net0 \
            -device virtio-gpu-pci,xres=720,yres=1280 \
            -display sdl \
            -device qemu-xhci \
            -device usb-kbd \
            -device usb-tablet
        '';

      in {
        devShells = {
          # Default shell - desktop development
          default = pkgs.mkShell {
            buildInputs = commonDeps ++ desktopDeps;

            LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath desktopDeps;

            # Static library paths for cmake
            FONTCONFIG_STATIC_LIB = "${fontconfigStatic.lib}/lib/libfontconfig.a";
            EXPAT_STATIC_LIB = "${expatStatic}/lib/libexpat.a";
            ZLIB_STATIC_LIB = "${pkgs.zlib.static}/lib/libz.a";
            UUID_STATIC_LIB = "${libuuidStatic.lib}/lib/libuuid.a";

            # LLVM/Clang paths for QA tools
            LLVM_DIR = "${pkgs.llvmPackages_21.llvm.dev}/lib/cmake/llvm";
            Clang_DIR = "${pkgs.llvmPackages_21.clang-unwrapped.dev}/lib/cmake/clang";

            shellHook = ''
              echo "Yetty development environment (desktop)"
              echo "  Run: make release"
            '';
          };

          # Android build shell
          android = pkgs.mkShell {
            buildInputs = commonDeps ++ androidDeps ++ [ pkgs.zlib pkgs.openssl pkgs.brotli armEmulatorScript ];

            ANDROID_HOME = "${androidSdk}/libexec/android-sdk";
            ANDROID_SDK_ROOT = "${androidSdk}/libexec/android-sdk";
            ANDROID_NDK_HOME = androidNdk;
            NDK_HOME = androidNdk;
            JAVA_HOME = "${pkgs.jdk17}";

            shellHook = ''
              # Add NDK toolchain to PATH for cross-compilation
              export PATH="${androidNdk}/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"

              echo "Yetty Android build environment"
              echo "  Android SDK: $ANDROID_HOME"
              echo "  Android NDK: $ANDROID_NDK_HOME"
              echo "  NDK clang: $(which aarch64-linux-android26-clang 2>/dev/null || echo 'not in PATH')"
              echo ""
              echo "Run: make android"
              echo "Run ARM emulator: run-arm-emulator"
            '';
          };

          # Asset-build shell: RISC-V cross-toolchain for OpenSBI + Linux kernel.
          # Used by build-tools/assets/{opensbi,linux}/build.sh.
          # Override CROSS_COMPILE when invoking scripts outside nix if needed.
          assets-riscv = pkgs.mkShell {
            buildInputs = with pkgs; [
              # RISC-V cross-toolchain
              pkgsCross.riscv64.buildPackages.gcc
              pkgsCross.riscv64.buildPackages.binutils

              # Build drivers + fetch tools
              gnumake
              bc
              bison
              flex
              cpio
              rsync
              curl
              gnutar
              gzip

              # Kernel deps (host-side, for scripts/)
              openssl
              elfutils
              perl
              python3
              pkg-config
            ];

            shellHook = ''
              export CROSS_COMPILE="riscv64-unknown-linux-gnu-"
              echo "Yetty asset-build environment (RISC-V cross)"
              echo "  CROSS_COMPILE=$CROSS_COMPILE"
              echo "  gcc:    $(''${CROSS_COMPILE}gcc --version | head -1)"
              echo ""
              echo "Run: bash build-tools/assets/opensbi/build.sh"
              echo "Run: bash build-tools/assets/linux/build.sh"
            '';
          };

          # Asset-build shell: host toolchain for building yetty-ymsdf-gen
          # and generating MSDF CDB font databases from TTFs.
          # Used by build-tools/assets/cdb/build.sh.
          assets-cdb = pkgs.mkShell {
            buildInputs = with pkgs; [
              cmake
              ninja
              pkg-config
              git
              gcc
              curl
              gnutar
              gzip
              zlib
              zlib.static
              brotli
            ];
            shellHook = ''
              echo "Yetty asset-build environment (cdb / host tools)"
            '';
          };

          # Asset-build shells: one per QEMU target platform.
          # Each carries the target-arch glib/pixman/pkg-config so QEMU's
          # meson configure can find them via the cross pkg-config wrapper.

          assets-qemu-linux-x86_64 = pkgs.mkShell {
            buildInputs = with pkgs; [
              meson ninja pkg-config python3 bison flex gnumake perl
              curl gnutar xz gzip
              gcc glib glib.dev pixman zlib zlib.static
            ];
            shellHook = "echo 'Yetty asset-build (qemu linux-x86_64)'";
          };

          # Use pkgsCross.*.mkShell so nativeBuildInputs (host-side build
          # tools) stay out of PKG_CONFIG_PATH_FOR_TARGET. Otherwise native
          # zlib from curl/python leaks into the cross link and the
          # aarch64 linker gets fed x86_64 libz.
          assets-qemu-linux-aarch64 = pkgs.pkgsCross.aarch64-multiplatform.mkShell {
            nativeBuildInputs = with pkgs; [
              meson ninja python3 bison flex gnumake perl
              curl gnutar xz gzip
              pkgsCross.aarch64-multiplatform.buildPackages.pkg-config
            ];
            buildInputs = with pkgs.pkgsCross.aarch64-multiplatform; [
              glib pixman zlib
            ];
            shellHook = ''
              # QEMU configure defers pkg-config lookups to $PKG_CONFIG
              # when set; make it the aarch64 cross wrapper.
              export PKG_CONFIG=aarch64-unknown-linux-gnu-pkg-config
              echo "Yetty asset-build (qemu linux-aarch64)"
            '';
          };

          # windows-x86_64: glib is not available in nixpkgs' mingwW64 cross.
          # Shell provides just the toolchain + zlib so the configure
          # failure lands deterministically; CI can supply glib via a
          # downloaded binary package when we're ready to support it.
          assets-qemu-windows-x86_64 = pkgs.mkShell {
            buildInputs = with pkgs; [
              meson ninja python3 bison flex gnumake perl
              curl gnutar xz gzip
              pkgsCross.mingwW64.buildPackages.gcc
              pkgsCross.mingwW64.buildPackages.pkg-config
              pkgsCross.mingwW64.zlib
            ];
            shellHook = "echo 'Yetty asset-build (qemu windows-x86_64)'";
          };

          # android targets: use the Android NDK directly (same toolchain
          # yetty's own Android build uses via gradle). Sidesteps
          # pkgsCross.*-android, whose compiler-rt rebuild has been broken
          # across clang-19/20/21. We build glib + pixman from source into
          # a sysroot with NDK clang — see build-tools/assets/qemu/
          # platforms/android-*.sh.
          #
          # This shell only supplies the build-side tooling; the NDK comes
          # in via the `ANDROID_NDK_HOME` env and PATH additions below.
          assets-qemu-android-arm64-v8a = pkgs.mkShell {
            buildInputs = commonDeps ++ androidDeps ++ (with pkgs; [
              meson ninja python3 bison flex gnumake perl
              curl gnutar xz gzip pkg-config
            ]);
            ANDROID_NDK_HOME = androidNdk;
            ANDROID_API = "26";
            ANDROID_TARGET_ABI = "arm64-v8a";
            shellHook = ''
              export PATH="${androidNdk}/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
              echo "Yetty asset-build (qemu android-arm64-v8a) — NDK direct"
            '';
          };

          assets-qemu-android-x86_64 = pkgs.mkShell {
            buildInputs = commonDeps ++ androidDeps ++ (with pkgs; [
              meson ninja python3 bison flex gnumake perl
              curl gnutar xz gzip pkg-config
            ]);
            ANDROID_NDK_HOME = androidNdk;
            ANDROID_API = "26";
            ANDROID_TARGET_ABI = "x86_64";
            shellHook = ''
              export PATH="${androidNdk}/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
              echo "Yetty asset-build (qemu android-x86_64) — NDK direct"
            '';
          };

          # Darwin targets (macos/ios/tvos): run on a macOS host with nix
          # installed. Build tools come from nix; the Xcode SDKs for
          # iOS/tvOS still come from xcrun at script time.
          # macos native uses the host's clang + nix-provided glib/pixman.
          # iOS/tvOS use xcrun -sdk switches — they don't need glib/pixman
          # cross-builds because the resulting binary is embedded (no
          # dynamic glib dep) — but QEMU's configure still wants glib at
          # build time. Supplying a native glib and pointing configure at
          # it works because meson uses the same compiler for both host
          # and target on darwin cross (the kludge path used in poc/qemu
          # ios scripts).
          assets-qemu-macos-x86_64 = pkgs.mkShell {
            buildInputs = with pkgs; [
              meson ninja pkg-config python3 bison flex gnumake perl
              curl gnutar xz gzip
              glib pixman
            ];
            shellHook = "echo 'Yetty asset-build (qemu macos-x86_64)'";
          };

          assets-qemu-macos-arm64 = pkgs.mkShell {
            buildInputs = with pkgs; [
              meson ninja pkg-config python3 bison flex gnumake perl
              curl gnutar xz gzip
              glib pixman
            ];
            shellHook = "echo 'Yetty asset-build (qemu macos-arm64)'";
          };

          assets-qemu-ios-arm64 = pkgs.mkShell {
            buildInputs = with pkgs; [
              meson ninja pkg-config python3 bison flex gnumake perl
              curl gnutar xz gzip
              glib pixman
            ];
            shellHook = "echo 'Yetty asset-build (qemu ios-arm64)'";
          };

          assets-qemu-ios-x86_64 = pkgs.mkShell {
            buildInputs = with pkgs; [
              meson ninja pkg-config python3 bison flex gnumake perl
              curl gnutar xz gzip
              glib pixman
            ];
            shellHook = "echo 'Yetty asset-build (qemu ios-x86_64 simulator)'";
          };

          assets-qemu-tvos-arm64 = pkgs.mkShell {
            buildInputs = with pkgs; [
              meson ninja pkg-config python3 bison flex gnumake perl
              curl gnutar xz gzip
              glib pixman
            ];
            shellHook = "echo 'Yetty asset-build (qemu tvos-arm64)'";
          };

          # Web/Emscripten build shell
          web = pkgs.mkShell {
            buildInputs = commonDeps ++ [
              pkgs.emscripten
              pkgs.python3
              pkgs.nodejs
              pkgs.gcc  # For building host tools (yecho-static for VM)
              pkgs.brotli  # For asset compression in incbin
            ];

            # Emscripten environment
            EMSDK = "${pkgs.emscripten}/share/emscripten";
            EM_CONFIG = "${pkgs.emscripten}/share/emscripten/.emscripten";
            EM_CACHE = "/tmp/emscripten-cache";

            shellHook = ''
              echo "Yetty Web/Emscripten build environment"
              echo "  Emscripten: $(emcc --version | head -1)"
              echo "  EMSDK: $EMSDK"
              echo ""
              echo "Run: make web"
            '';
          };
        };

        # Apps - runnable outputs
        apps = {
          emulator = {
            type = "app";
            program = "${armEmulatorScript}/bin/run-arm-emulator";
          };
        };

        # Packages
        packages = {
          inherit yetty;
          default = yetty;
          arm-emulator = armEmulatorScript;
        };
      }
    );
}

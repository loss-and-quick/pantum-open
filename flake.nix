{
  description = "Open-source SANE backend and CUPS filter for Pantum M6500/M6507";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
      ...
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        # Backend and filter are one derivation by default: they are the same
        # device, the same repository and the same version, and a NixOS user
        # normally wants both (hardware.sane.extraBackends and
        # services.printing.drivers point at the same package).  The two
        # single-purpose variants exist for people who only need one half and
        # do not want the other's closure.
        mkPantum =
          {
            backend ? true,
            filter ? true,
            pname ? "pantum-open",
          }:
          pkgs.stdenv.mkDerivation {
            inherit pname;
            version = "0.1.0";
            src = self;

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.pkg-config
            ];

            buildInputs =
              pkgs.lib.optionals backend [
                pkgs.libusb1
                # Headers only: the backend is dlopened by SANE's dll meta
                # backend and must not link against libsane.
                pkgs.sane-backends
              ]
              ++ pkgs.lib.optionals filter [
                pkgs.cups
                pkgs.jbigkit
              ];

            cmakeFlags = [
              # The backend looks for pantum.conf in SANE_CONFIG_DIR first;
              # this is the fallback for a plain /etc/sane.d installation.
              "-DPANTUM_CONFIG_DIR=/etc/sane.d"
              "-DPANTUM_BUILD_BACKEND=${if backend then "ON" else "OFF"}"
              "-DPANTUM_BUILD_FILTER=${if filter then "ON" else "OFF"}"
            ];

            doInstallCheck = filter;
            installCheckPhase = ''
              # cupstestppd wants to resolve *cupsFilter against the CUPS
              # server directory, which does not exist inside the sandbox, so
              # only the structural errors are interesting here.
              ${pkgs.cups}/bin/cupstestppd -W filters \
                $out/share/cups/model/Pantum-M6500-open.ppd
            '';

            meta = with pkgs.lib; {
              description =
                "Free SANE backend and CUPS filter for Pantum M6500/M6507 devices";
              homepage = "https://github.com/";
              platforms = platforms.linux;
              license = licenses.gpl2Plus;
            };
          };

        pantum-open = mkPantum { };
      in
      {
        # Layout matches nixpkgs' pantum-driver, so the package can be dropped
        # into hardware.sane.extraBackends and services.printing.drivers:
        #   lib/sane/libsane-pantum.so.1
        #   etc/sane.d/pantum.conf
        #   etc/sane.d/dll.d/pantum
        #   lib/cups/filter/rastertopantum
        #   share/cups/model/Pantum-M6500-open.ppd
        packages.default = pantum-open;
        packages.pantum-open = pantum-open;
        packages.sane-pantum = mkPantum {
          filter = false;
          pname = "sane-pantum";
        };
        packages.rastertopantum = mkPantum {
          backend = false;
          pname = "rastertopantum";
        };

        devShells.default = pkgs.mkShell {
          packages = [
            # Protocol capture: the LD_PRELOAD shim wraps libusb, so the
            # capture tools need the same headers the proprietary backend
            # links against.
            pkgs.libusb1
            pkgs.pkg-config
            pkgs.gcc
            pkgs.gdb
            pkgs.binutils

            # Building the backend and the filter.
            pkgs.cmake
            pkgs.cups
            pkgs.jbigkit

            # Disassembly of the proprietary driver.
            pkgs.radare2

            # Prototyping the protocols before they are rewritten in C.
            (pkgs.python3.withPackages (ps: [
              ps.pyusb
              ps.pillow
              ps.numpy
            ]))

            # Frontends and regression helpers.
            pkgs.sane-backends
            pkgs.netpbm
            pkgs.cups-filters
            pkgs.ghostscript
            pkgs.foo2zjs
          ];

          shellHook = ''
            export SANE_CONFIG_DIR=/etc/sane-config
            export LD_LIBRARY_PATH=/etc/sane-libs
            echo "pantum-open dev shell — device: 232b:0e20 (bus 003)"
          '';
        };

        formatter = pkgs.nixfmt-tree;
      }
    );
}

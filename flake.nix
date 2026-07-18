{
  description = "kabai - MCP Server for Database-Driven Kanban (PostgreSQL Backend)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          name = "kabai";
          version = "0.6.0";
          src = ./.;

          nativeBuildInputs = with pkgs; [ gcc pkg-config ];
          buildInputs       = with pkgs; [ postgresql cjson ];

          buildPhase = ''
            gcc -o kabai \
              -I./src \
              -I${pkgs.postgresql.dev}/include \
              -I${pkgs.cjson}/include \
              src/main.c \
              src/db/connection.c \
              src/db/transaction.c \
              src/mcp/mcp.c \
              src/mcp/schema.c \
              src/kanban/projects.c \
              src/kanban/tickets.c \
              src/kanban/comments.c \
              src/kanban/board_statuses.c \
              src/kanban/kanban_tools.c \
              src/docs/docs_tools.c \
              src/attachments/attachment_tools.c \
              -L${pkgs.postgresql.lib}/lib \
              -L${pkgs.cjson}/lib \
              -lpq -lcjson -lm
          '';

          installPhase = ''
            mkdir -p $out/bin
            cp kabai $out/bin/
          '';
        };

        # Statische Windows-x86_64-Binary (kabai.exe) ohne DLL-Abhängigkeiten.
        # Kein pkgsStatic: dessen mingw-Triple (x86_64-w64-windows-gnu) scheitert
        # an config.sub beim Toolchain-Bootstrap. Stattdessen plain mingwW64 und
        # die Bibliotheken einzeln statisch (libpq baut .a immer mit und behält
        # sie bei dontDisableStatic im dev-Output).
        # allowUnsupportedSystem: nixpkgs' libpq deklariert Windows nicht in
        # meta.platforms, baut aber via mingw (siehe Ticket #520).
        packages.windows =
          let
            pkgsWin = import nixpkgs {
              inherit system;
              crossSystem = nixpkgs.lib.systems.examples.mingwW64;
              config.allowUnsupportedSystem = true;
            };
            opensslWin = pkgsWin.openssl.override { static = true; };
            zlibWin    = pkgsWin.zlib.override { shared = false; };
            libpqWin   = (pkgsWin.libpq.override {
              openssl     = opensslWin;
              zlib        = zlibWin;
              curlSupport = false;
            }).overrideAttrs (old: {
              dontDisableStatic = true;
              # makeWrappers Hook verlangt eine Host-Bash — die baut nicht für
              # mingw, und libpq ruft wrapProgram ohnehin nie auf.
              nativeBuildInputs = builtins.filter
                (d: !(nixpkgs.lib.hasInfix "wrapper-hook" (d.name or "")))
                old.nativeBuildInputs;
              # Statisches libcrypto referenziert Windows-Systemlibs; ohne sie
              # scheitert configures Link-Test ("library 'crypto' is required").
              env = (old.env or { }) // {
                LIBS = "-lws2_32 -lgdi32 -lcrypt32 -lbcrypt";
              };
              # Die mingw-Toolchain in nixpkgs nutzt mcfgthread ohne pthread.h;
              # libpgports pthread-Ersatz braucht winpthreads.
              buildInputs = (old.buildInputs or [ ]) ++ [ pkgsWin.windows.pthreads ];
              # Beim win32-Port ist "libpq.a" die Import-Library der DLL.
              # Wie nixpkgs' isStatic-Zweig: echte statische Lib bauen lassen.
              postPatch = (old.postPatch or "") + ''
                substituteInPlace src/interfaces/libpq/Makefile \
                  --replace-fail "all: all-lib libpq-refs-stamp" "all: all-lib"
                substituteInPlace src/Makefile.shlib \
                  --replace-fail "all-lib: all-shared-lib" "all-lib: all-static-lib" \
                  --replace-fail "install-lib: install-lib-shared" "install-lib: install-lib-static"
              '';
              # Auf win32 ist $(stlib) trotzdem nur die per --out-implib
              # erzeugte Import-Lib der DLL (Makefile.shlib, haslibarule).
              # Echtes statisches Archiv aus den Objekten überschreibt sie;
              # install-lib-static installiert es dann unverändert.
              postBuild = ''
                rm -f src/interfaces/libpq/libpq.a
                $AR crs src/interfaces/libpq/libpq.a \
                  $(find src/interfaces/libpq -maxdepth 1 -name '*.o' ! -name 'win32ver*')
              '';
            });
            cjsonWin = pkgsWin.cjson.overrideAttrs (old: {
              cmakeFlags = (old.cmakeFlags or [ ]) ++ [
                "-DBUILD_SHARED_LIBS=OFF"
                # mingw's isnan/isinf stolpert über cJSONs -Werror=float-conversion
                "-DENABLE_CUSTOM_COMPILER_FLAGS=OFF"
              ];
            });
          in
          pkgsWin.stdenv.mkDerivation {
            name = "kabai-windows";
            version = "0.6.0";
            src = ./.;

            nativeBuildInputs = [ pkgsWin.buildPackages.pkg-config ];
            buildInputs       = [ libpqWin cjsonWin opensslWin zlibWin pkgsWin.windows.pthreads ];

            buildPhase = ''
              $CC -o kabai.exe \
                -I./src \
                $(''${PKG_CONFIG:-pkg-config} --cflags libpq libcjson) \
                src/main.c \
                src/db/connection.c \
                src/db/transaction.c \
                src/mcp/mcp.c \
                src/mcp/schema.c \
                src/kanban/projects.c \
                src/kanban/tickets.c \
                src/kanban/comments.c \
                src/kanban/board_statuses.c \
                src/kanban/kanban_tools.c \
                src/docs/docs_tools.c \
                src/attachments/attachment_tools.c \
                -static \
                ${libpqWin.dev}/lib/libpq.a \
                ${libpqWin.dev}/lib/libpgcommon.a \
                ${libpqWin.dev}/lib/libpgport.a \
                $(''${PKG_CONFIG:-pkg-config} --libs --static libcjson) \
                -lssl -lcrypto -lz -lwinpthread \
                -lshell32 -lws2_32 -lgdi32 -lcrypt32 -lbcrypt -lsecur32
            '';

            installPhase = ''
              mkdir -p $out/bin
              cp kabai.exe $out/bin/
            '';
          };

        apps.default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/kabai";
        };

        devShells.default = pkgs.mkShell {
          buildInputs = with pkgs; [
            gcc
            gdb
            pkg-config
            postgresql
            cjson
          ];

          shellHook = ''
            echo "kabai MCP Server Development Shell"
            echo "======================================"
            echo ""
            echo "Build commands:"
            echo "  nix build   - Build binary (Linux)"
            echo "  nix run .   - Run MCP server"
            echo ""
            echo "Environment variables for DB connection:"
            echo "  KABAI_DB_HOST      (default: localhost)"
            echo "  KABAI_DB_PORT      (default: 5432)"
            echo "  KABAI_DB_NAME      (default: kabai)"
            echo "  KABAI_DB_USER      (default: postgres)"
            echo "  KABAI_DB_PASSWORD  (default: )"
            echo ""
            echo "Agent identity:"
            echo "  KABAI_AGENT_NAME   (default: none)"
            echo "  KABAI_AGENT_MODEL  (default: none)"
            echo ""
            export CFLAGS="-I./src -I${pkgs.postgresql.dev}/include -I${pkgs.cjson}/include"
            export LDFLAGS="-L${pkgs.postgresql.lib}/lib -L${pkgs.cjson}/lib -lpq -lcjson"
          '';
        };
      }
    );
}

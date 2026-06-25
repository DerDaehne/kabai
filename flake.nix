{
  description = "kb.ai - MCP Server for Database-Driven Kanban (PostgreSQL Backend)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs       = import nixpkgs { inherit system; };
        pkgsWin    = import nixpkgs {
          inherit system;
          crossSystem = nixpkgs.lib.systems.examples.mingwW64;
        };

        srcFiles = ''
          src/main.c \
          src/db/connection.c \
          src/db/transaction.c \
          src/kanban/projects.c \
          src/kanban/tickets.c \
          src/kanban/comments.c \
          src/kanban/board_statuses.c
        '';
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          name = "kbai";
          version = "0.3.0";
          src = ./.;

          nativeBuildInputs = with pkgs; [ gcc pkg-config ];
          buildInputs      = with pkgs; [ postgresql cjson ];

          buildPhase = ''
            gcc -o kbai \
              -I./src \
              -I${pkgs.postgresql.dev}/include \
              -I${pkgs.cjson}/include \
              ${srcFiles} \
              -L${pkgs.postgresql.lib}/lib \
              -L${pkgs.cjson}/lib \
              -lpq -lcjson -lm
          '';

          installPhase = ''
            mkdir -p $out/bin
            cp kbai $out/bin/
          '';
        };

        packages.windows = pkgsWin.stdenv.mkDerivation {
          name = "kbai-windows";
          version = "0.3.0";
          src = ./.;

          nativeBuildInputs = [ pkgs.pkg-config ];
          buildInputs = with pkgsWin; [
            postgresql
            cjson
            openssl
            windows.pthreads
          ];

          buildPhase = ''
            ${pkgsWin.stdenv.cc.targetPrefix}cc -o kbai.exe \
              -I./src \
              -I${pkgsWin.postgresql.dev}/include \
              -I${pkgsWin.cjson}/include \
              -I${pkgsWin.openssl.dev}/include \
              ${srcFiles} \
              -L${pkgsWin.postgresql.lib}/lib \
              -L${pkgsWin.cjson}/lib \
              -L${pkgsWin.openssl.out}/lib \
              -lpq -lssl -lcrypto -lcjson \
              -lws2_32 -lgdi32 -lcrypt32 -lsecur32 \
              -static -static-libgcc
          '';

          installPhase = ''
            mkdir -p $out/bin
            cp kbai.exe $out/bin/
          '';
        };

        apps.default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/kbai";
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
            echo "kb.ai MCP Server Development Shell"
            echo "======================================"
            echo ""
            echo "Build commands:"
            echo "  nix build           - Build binary"
            echo "  nix run .           - Run MCP server"
            echo ""
            echo "Environment variables for DB connection:"
            echo "  KB_AI_DB_HOST      (default: localhost)"
            echo "  KB_AI_DB_PORT      (default: 5432)"
            echo "  KB_AI_DB_NAME      (default: kb_ai)"
            echo "  KB_AI_DB_USER      (default: postgres)"
            echo "  KB_AI_DB_PASSWORD  (default: )"
            echo ""
            export CFLAGS="-I./src -I${pkgs.postgresql.dev}/include -I${pkgs.cjson}/include"
            export LDFLAGS="-L${pkgs.postgresql.lib}/lib -L${pkgs.cjson.lib}/lib -lpq -lcjson"
          '';
        };
      }
    );
}

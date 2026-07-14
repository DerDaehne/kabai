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
          version = "0.5.0";
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
              -L${pkgs.postgresql.lib}/lib \
              -L${pkgs.cjson}/lib \
              -lpq -lcjson -lm
          '';

          installPhase = ''
            mkdir -p $out/bin
            cp kabai $out/bin/
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

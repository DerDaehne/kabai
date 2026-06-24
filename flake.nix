{
  description = "kb.ai - Database-Driven Kanban Engine for Agentic AI Workflows";

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
          name = "kbai";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = with pkgs; [
            gcc
            pkg-config
          ];

          buildInputs = with pkgs; [
            postgresql
          ];

          CFLAGS = "-I. -I./include -I${pkgs.postgresql.dev}/include";
          LDFLAGS = "-L${pkgs.postgresql.lib}/lib -lpq";

          buildPhase = ''
            gcc -o kbai ${CFLAGS} ${LDFLAGS} \
              src/main.c \
              src/db/connection.c \
              src/kanban/projects.c \
              src/kanban/tickets.c
          '';

          installPhase = ''
            mkdir -p $out/bin
            cp kbai $out/bin/
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
          ];

          shellHook = ''
            echo "kb.ai Development Shell"
            echo "========================"
            echo "Commands: nix build, nix run . -- start"
            export CFLAGS="-I${pkgs.postgresql.dev}/include"
            export LDFLAGS="-L${pkgs.postgresql.lib}/lib -lpq"
          '';
        };
      }
    );
}

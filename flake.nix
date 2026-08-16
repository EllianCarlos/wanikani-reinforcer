{
  description = "wkr - WaniKani Reinforcer";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
    in
    {
      devShells.${system}.default = pkgs.mkShell {
        packages = [
          pkgs.cmake
          pkgs.ninja
          pkgs.pkg-config
          pkgs.curl.dev
          pkgs.sqlite.dev
          pkgs.nlohmann_json
          pkgs.catch2_3
          pkgs.gcc
        ];
      };
    };
}

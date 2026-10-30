{
  description = "wl-find: Ctrl+F for the whole screen on Wayland. Type what to find, then click it";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      forAllSystems =
        f:
        nixpkgs.lib.genAttrs [ "x86_64-linux" "aarch64-linux" ] (
          system: f nixpkgs.legacyPackages.${system}
        );
    in
    {
      packages = forAllSystems (pkgs: {
        default = pkgs.callPackage ./package.nix { };
      });

      # wl-find on PATH, and the find bar kept loaded as a user service.
      homeManagerModules.default = import ./module.nix self;

      checks = forAllSystems (pkgs: {
        package = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
        # writeNu does not parse the script; a syntax error would only show
        # on the first Super+/.
        script = pkgs.runCommand "wl-find-nu-check" { nativeBuildInputs = [ pkgs.nushell ]; } ''
          nu -n -c 'if not (nu-check ${./wl-find.nu}) { nu-check --debug ${./wl-find.nu} }'
          touch $out
        '';
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}

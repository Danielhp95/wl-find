# Home Manager: wl-find on PATH, and its find bar kept loaded as
# wl-find-bar.service, so the bar is up the moment wl-find opens it. Without
# the service wl-find starts the bar itself on first use, and that find waits
# ~0.4 s for it. The key that runs wl-find goes in the compositor's config.
self:
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.programs.wl-find;
in
{
  options.programs.wl-find = {
    enable = lib.mkEnableOption "wl-find, Ctrl+F for the whole screen";
    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "wl-find.packages.\${system}.default";
      example = lib.literalExpression ''
        wl-find.packages.''${system}.default.override {
          colors.accent = "#7aa2f7";
          font = "JetBrains Mono";
        }
      '';
      description = "The wl-find package; its `colors` and `font` restyle the bar.";
    };
  };

  config = lib.mkIf cfg.enable {
    home.packages = [ cfg.package ];

    # wl-find starts it by this name when the bar does not answer.
    systemd.user.services.wl-find-bar = {
      Unit = {
        Description = "wl-find's find bar (Quickshell)";
        PartOf = [ "graphical-session.target" ];
        After = [ "graphical-session.target" ];
      };
      Service = {
        ExecStart = lib.getExe' cfg.package "wl-find-bar";
        Restart = "always";
        RestartSec = 1;
      };
      Install.WantedBy = [ "graphical-session.target" ];
    };
  };
}

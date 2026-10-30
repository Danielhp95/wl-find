# wl-find, and wl-find-bar: its find bar, a Quickshell config that wl-find
# opens over IPC. `colors` and `font` restyle the bar; a colour is #RRGGBB,
# or #AARRGGBB with the alpha first, as QML takes them.
{
  lib,
  stdenv,
  writers,
  writeShellScriptBin,
  symlinkJoin,
  replaceVars,
  runtimeShell,
  meson,
  ninja,
  pkg-config,
  wayland-scanner,
  wayland,
  wayland-protocols,
  wlr-protocols,
  libportal,
  libei,
  coreutils,
  quickshell,
  grim,
  imagemagick,
  tesseract5,
  wl-kbptr,
  colors ? { },
  font ? "monospace",
}:
let
  bar = replaceVars ./wl-find.qml (
    {
      # wl-kbptr's own dim (mode_floating.unselectable_bg_color), so its
      # hints take over without a change of shade. Set it to yours if you
      # changed that.
      dim = "#66222222";
      bg = "#1c1b19";
      border = "#3a342d";
      accent = "#e08060";
      accentDim = "#b8654c";
      fg = "#d8d0c0";
      fgDim = "#9a9288";
      error = "#e05252";
    }
    // colors
    // {
      inherit font;
      # A service's PATH is bare: what the bar runs, it runs by store path.
      sh = runtimeShell;
      coreutils = "${coreutils}/bin";
    }
  );

  # Finds the focused output and clicks, the same way on every compositor
  # (helper/wl-find-helper.c).
  helper = stdenv.mkDerivation {
    pname = "wl-find-helper";
    version = "0.1.0";
    src = ./helper;
    strictDeps = true;
    nativeBuildInputs = [
      meson
      ninja
      pkg-config
      wayland-scanner
    ];
    buildInputs = [
      wayland
      wayland-protocols
      wlr-protocols
      libportal
      libei
    ];
  };

  find = writers.writeNuBin "wl-find" {
    makeWrapperArgs = [
      "--set"
      "WL_FIND_QML"
      bar
      "--prefix"
      "PATH"
      ":"
      (lib.makeBinPath [
        coreutils
        helper
        quickshell
        grim
        imagemagick # the dark-on-light pass before tesseract
        tesseract5
        wl-kbptr
      ])
    ];
  } (builtins.readFile ./wl-find.nu);

  # wl-find finds the bar it runs by this path (wl-find.qml's socket).
  findBar = writeShellScriptBin "wl-find-bar" ''
    export WL_FIND_QML=${bar}
    exec ${lib.getExe' quickshell "qs"} --path ${bar} "$@"
  '';
in
symlinkJoin {
  name = "wl-find";
  paths = [
    find
    findBar
  ];
  passthru = { inherit bar helper; };
  meta = {
    description = "Ctrl+F for the whole screen on Wayland: type what to find, then click it";
    homepage = "https://github.com/Danielhp95/wl-find";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "wl-find";
  };
}

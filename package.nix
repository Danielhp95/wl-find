# wl-find, and wl-find-bar: its find bar, a Quickshell config that wl-find
# opens over IPC. `colors` and `font` restyle the bar; a colour is #RRGGBB,
# or #AARRGGBB with the alpha first, as QML takes them.
{
  lib,
  stdenv,
  fetchurl,
  writers,
  writeShellScriptBin,
  symlinkJoin,
  replaceVars,
  runCommandLocal,
  removeReferencesTo,
  patchelf,
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
  cpptrace,
  libdrm,
  libgbm,
  libxcb,
  pam,
  pipewire,
  polkit,
  glib,
  grim,
  onnxruntime,
  wl-kbptr,
  wl-clipboard,
  opencv,
  colors ? { },
  font ? "monospace",
  # The two models wl-find-ocr reads with, as ONNX files: the one that finds
  # the text on a screen, and the one that reads a line of it, which carries
  # the list of its characters with it. PaddleOCR's (Apache 2.0), as RapidOCR
  # publishes them: PP-OCRv6's smallest detector, and PP-OCRv5's recogniser
  # for English and the accented letters of western Europe. Any other of
  # RapidOCR's recognisers reads other scripts, slower or less surely.
  detector ? fetchurl {
    url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv6/det/PP-OCRv6_det_tiny.onnx";
    hash = "sha256-9CwPvSlNlerBpVDhMbJ32sl0YsgCX6S2w87Bt4lL09U=";
  },
  recogniser ? fetchurl {
    url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv5/rec/en_PP-OCRv5_rec_mobile.onnx";
    hash = "sha256-w0Ya3Vm7QyPsupakkqt14G3aQkZ8nj0MGNtdHSGSS+g=";
  },
}:
let
  # Only what the bar uses of Quickshell: the rest (PipeWire and through it
  # GStreamer and Python, Bluetooth, PAM, polkit...) is ~400 MiB. Qt stays
  # the stock one, which is in the binary cache.
  quickshell' = quickshell.overrideAttrs (old: {
    buildInputs = lib.subtractLists [
      cpptrace
      libdrm
      libgbm
      libxcb
      pam
      pipewire
      polkit
      glib
    ] old.buildInputs;
    cmakeFlags =
      old.cmakeFlags
      ++ map (feature: lib.cmakeBool feature false) [
        "CRASH_HANDLER"
        "X11"
        "I3"
        "HYPRLAND"
        "SCREENCOPY"
        "WAYLAND_SESSION_LOCK"
        "WAYLAND_TOPLEVEL_MANAGEMENT"
        "SERVICE_STATUS_NOTIFIER"
        "SERVICE_PIPEWIRE"
        "SERVICE_MPRIS"
        "SERVICE_PAM"
        "SERVICE_POLKIT"
        "SERVICE_GREETD"
        "SERVICE_UPOWER"
        "SERVICE_NOTIFICATIONS"
        "BLUETOOTH"
        "NETWORK"
      ];
  });

  # wl-find hands wl-kbptr its targets, so it never detects them with OpenCV.
  # Without it wl-kbptr refuses a config with source=detect, the setting for
  # using it on its own, before reading the source=stdin wl-find passes.
  wl-kbptr' = wl-kbptr.overrideAttrs (old: {
    buildInputs = lib.remove opencv old.buildInputs;
    mesonFlags = [ "-Dopencv=disabled" ];
    postPatch = (old.postPatch or "") + ''
      substituteInPlace src/config.c --replace-fail \
        "LOG_ERR(\"Binary not build with OpenCV. 'detect' source not supported.\");" \
        "*out = FLOATING_MODE_SOURCE_STDIN; return 0;"
    '';
  });

  models = runCommandLocal "wl-find-models" { } ''
    mkdir $out
    ln -s ${detector} $out/det.onnx
    ln -s ${recogniser} $out/rec.onnx
  '';

  # ONNX Runtime's own library and nothing else of it. nixpkgs builds it with
  # the OpenVINO provider, a file beside the library that wl-find never
  # loads: with it would come OpenVINO, 165 MiB. The library names its own
  # directory, to look for such providers in, and that name goes too.
  onnxruntime' =
    runCommandLocal "onnxruntime-${onnxruntime.version}-lib"
      { nativeBuildInputs = [ removeReferencesTo ]; }
      ''
        mkdir -p $out/lib
        cp -P ${lib.getLib onnxruntime}/lib/libonnxruntime.so* $out/lib/
        chmod u+w $out/lib/*
        remove-references-to -t ${lib.getLib onnxruntime} $out/lib/libonnxruntime.so.*.*
      '';

  # Reads the text off a screenshot (ocr/), with the models above unless it
  # is told others. Its own parts are tested as it is built.
  ocr = stdenv.mkDerivation {
    pname = "wl-find-ocr";
    version = "0.1.0";
    src = ./ocr;
    strictDeps = true;
    nativeBuildInputs = [
      meson
      ninja
      pkg-config
      patchelf
    ];
    buildInputs = [ onnxruntime ];
    mesonFlags = [ (lib.mesonOption "models" "${models}") ];
    doCheck = true;
    # Built against all of ONNX Runtime, and run with its library alone.
    postFixup = ''
      patchelf --set-rpath "$(patchelf --print-rpath $out/bin/wl-find-ocr | sed "s|${lib.getLib onnxruntime}/lib|${onnxruntime'}/lib|")" $out/bin/wl-find-ocr
    '';
    disallowedReferences = [ (lib.getLib onnxruntime) ];
  };

  qml = replaceVars ./wl-find.qml (
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

  # A directory, as the QML imports the JS beside it.
  bar = runCommandLocal "wl-find-bar-qml" { } ''
    mkdir $out
    cp ${qml} $out/wl-find.qml
    cp ${./vim-regex.js} $out/vim-regex.js
  '';

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
      "${bar}/wl-find.qml"
      "--prefix"
      "PATH"
      ":"
      (lib.makeBinPath [
        coreutils
        helper
        quickshell'
        grim
        ocr
        wl-kbptr'
        wl-clipboard # Ctrl+Y
      ])
    ];
  } (builtins.readFile ./wl-find.nu);

  # wl-find finds the bar it runs by this path (wl-find.qml's socket).
  findBar = writeShellScriptBin "wl-find-bar" ''
    export WL_FIND_QML=${bar}/wl-find.qml
    exec ${lib.getExe' quickshell' "qs"} --path ${bar}/wl-find.qml "$@"
  '';
in
symlinkJoin {
  name = "wl-find";
  paths = [
    find
    findBar
  ];
  passthru = { inherit bar helper ocr; };
  meta = {
    description = "Ctrl+F for the whole screen on Wayland: type what to find, then click it";
    homepage = "https://github.com/Danielhp95/wl-find";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "wl-find";
  };
}

<div align=center>

<img src="extras/banner.png" alt="Banner" width="50%">

</div>
<h1 align=center>GTA: Liberty City Stories - Nintendo Switch port</h1>

This is a wrapper/port of the Android version of Grand Theft Auto: Liberty City Stories (v2.4.379).
It loads the original game binary, patches it and runs it.
It's basically as if we emulate a minimalist Android environment in which we natively run the original Android binary as is.

### How to install

You're going to need:
* the `.apk` for version **2.4.379**

To install:
1. Create a folder called `gtalcs` in the `switch` folder on your SD card.
2. Extract `assets/data_main.wad` and `assets/data_music.wad` to `/switch/gtalcs/`.
3. Extract `lib/arm64-v8a/libGame.so` **and** `lib/arm64-v8a/libopenal.so` from the `arm64-v8a`
   APK to `/switch/gtalcs/`.
4. Extract `res\raw\intro.m4v` to `/switch/gtalcs/`.
5. Copy `gtalcs_nx.nro` into `/switch/gtalcs/`.

Your SD card should contain: `/switch/gtalcs/gtalcs_nx.nro`, `/switch/gtalcs/libGame.so`,
`/switch/gtalcs/libopenal.so`, `/switch/gtalcs/data_main.wad` `/switch/gtalcs/data_music.wad`
and `/switch/gtalcs/intro.m4v`

### Notes

This will not work in applet/album mode. Use a game override (hold R on a title) or a forwarder.

Save games and settings are stored in `/switch/gtalcs/`.

The port has an extra config file, located at `/switch/gtalcs/config.txt`. It is created when you
first run the game:
* `screen_width` / `screen_height` — render resolution; `-1` picks 1280x720 in handheld and 1920x1080 docked
* `trilinear_filter` — `1` forces trilinear texture filtering
* `show_fps` — `1` draws a small FPS counter in the top left corner
* `xbox_layout` — `1` original Xbox layout

### How to build

You're going to need devkitA64 and the following packages/libraries:
* `switch-mesa`
* `switch-libdrm_nouveau`
* `switch-sdl2`
* `switch-mpg123`
* `switch-ffmpeg`
* `switch-openal-soft`
* `devkitpro-pkgbuild-helpers`

### Credits

* TheOfficialFloW for the method and the original PS Vita work;
* fgsfds for max_nx, which this port is based on;

### Support

If you enjoy my work and want to support me :

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project has no direct affiliation with Take-Two Interactive Software, Inc. or Rockstar Games, Inc.
"Grand Theft Auto" and "Grand Theft Auto: Liberty City Stories" are trademarks of their respective owners.
All Rights Reserved.

No assets or program code from the original game or its Android port are included in this project.
We do not condone piracy in any way, shape or form and encourage users to legally own the original game.

Unless specified otherwise, the source code provided in this repository is licenced under the MIT License.
Please see the accompanying LICENSE file.

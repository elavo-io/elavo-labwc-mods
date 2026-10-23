# elavo-labwc-mods

Elavo's labwc features, compiled into an untouched upstream tree.

[`elavo-labwc`](https://github.com/elavo-io/elavo-labwc) is a mirror of upstream labwc. Nothing in this repository is committed there. `scripts/assemble.sh` checks the mirror out at `LABWC_REF`, copies `mods/` into `src/elavo-mods/`, and applies the files in `patches/` in order. These are the only edits to labwc source. `0001-elavo-mods-hooks.patch` adds the hooks: init and teardown, the server-side decoration and shadow updates, view geometry changes, theme load, and the output frame. `0002-session-lock-locked-after-cover.patch` makes labwc cover each output in black as soon as a lock starts, and tell the lock client "locked" only after that frame has been presented. Upstream sends it at once, so a laptop could suspend with the unlocked desktop still on the panel and show it again on wake.

GPL-2.0-only, the same license as labwc.

## What the mods do

- **Theme keys**, read from `themerc` before labwc clamps the radius. A key that is present wins over `rc.xml`.
  - `window.cornerRadius`
  - `window.maximizedCorners` (`yes` keeps the top corners when a window is maximized)
  - `window.dropShadows`
  - `window.dropShadowsOnTiled` (unlike stock labwc, no gap is required)
  - `window.dropShadowsOnMaximized`
  - `window.blur.strength` (1 to 5 downsample steps, default 3)
- **Shadows on maximized and tiled windows.** labwc's shadow stays on windows it decorates when they are maximized or tiled, if the theme asks. A maximized window that draws its own title bar gets the same shadow, built from the buffers labwc renders from the theme's `window.*.shadow` keys. Tiled client-decorated windows draw their own shadow and get none here. labwc's shadow corner pieces are redrawn to follow `window.cornerRadius`, on every window, so the shadow is round where the window is.
- **Maximized top corners.** Bottom corners and clipping the client's own pixels are not done here. Those need a shader. A maximized window still drops its border; the corner pieces are the title bar's, and that join has to be looked at on a real window.
- **Blur** for surfaces that ask, via `ext-background-effect-v1`. The backdrop is captured the way labwc's window-switcher thumbnails are, blurred with scaled texture draws (no scenefx, no custom shader), and placed in the scene just below the surface. The first eight re-blurs log their time. Set `<blur>no</blur>` in `rc.xml` to turn it off. On the pixman renderer it stays off unless `ELAVO_BLUR_PIXMAN=1`.

The panel does not ask for blur. The dock and Lens do.

## Build

```bash
./scripts/assemble.sh
./build/labwc/build/labwc --version
```

`ELAVO_LABWC_MIRROR` overrides the mirror. Next to a checkout of `elavo-labwc`, that checkout is used. Otherwise the script clones `elavo-io/elavo-labwc`.

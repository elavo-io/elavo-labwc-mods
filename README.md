# elavo-labwc-mods

Elavo's labwc features, compiled into an untouched upstream tree.

[`elavo-labwc`](https://github.com/elavo-io/elavo-labwc) is a mirror of upstream labwc. Nothing in this repository is committed there. `scripts/assemble.sh` checks the mirror out at `LABWC_REF`, copies `mods/` into `src/elavo-mods/`, and applies `patches/0001-elavo-mods-hooks.patch`. That patch is the only edit to labwc source: init and teardown, the server-side decoration update, theme load, and the output frame.

GPL-2.0-only, the same license as labwc.

## What the mods do

- **Theme keys**, read from `themerc` before labwc clamps the radius. A key that is present wins over `rc.xml`.
  - `window.cornerRadius`
  - `window.maximizedCorners` (`yes` keeps the top corners when a window is maximized)
  - `window.dropShadows`
  - `window.dropShadowsOnTiled`
  - `window.blur.strength` (1 to 5 downsample steps, default 3)
- **Maximized top corners.** Bottom corners and clipping the client's own pixels are not done here. Those need a shader. A maximized window still drops its border; the corner pieces are the title bar's, and that join has to be looked at on a real window.
- **Blur** for surfaces that ask, via `ext-background-effect-v1`. The backdrop is captured the way labwc's window-switcher thumbnails are, blurred with scaled texture draws (no scenefx, no custom shader), and placed in the scene just below the surface. The first eight re-blurs log their time. Set `<blur>no</blur>` in `rc.xml` to turn it off. On the pixman renderer it stays off unless `ELAVO_BLUR_PIXMAN=1`.

The panel does not ask for blur. The dock and Lens do.

## Build

```bash
./scripts/assemble.sh
./build/labwc/build/labwc --version
```

`ELAVO_LABWC_MIRROR` overrides the mirror. Next to a checkout of `elavo-labwc`, that checkout is used. Otherwise the script clones `elavo-io/elavo-labwc`.

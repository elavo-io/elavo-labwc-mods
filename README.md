# elavo-labwc-mods

Elavo's labwc features, compiled into an untouched upstream tree.

[`elavo-labwc`](https://github.com/elavo-io/elavo-labwc) is a mirror of upstream labwc. Nothing in this repository is committed there. `scripts/assemble.sh` checks the mirror out at `LABWC_REF`, and runs `scripts/apply-mods.sh`, which copies `mods/` into `src/elavo-mods/`, applies the files in `patches/` in order, and then the SceneFX layer (see below). These are the only edits to labwc source. `0001-elavo-mods-hooks.patch` adds the hooks: init and teardown, the server-side decoration and shadow updates, view geometry changes, theme load, and the output frame. `0002-session-lock-locked-after-cover.patch` makes labwc cover each output in black as soon as a lock starts, and tell the lock client "locked" only after that frame has been presented. Upstream sends it at once, so a laptop could suspend with the unlocked desktop still on the panel and show it again on wake. `0003-desktop-focus-deactivates-windows.patch` deactivates the focused window when a bottom or background layer surface, such as Elavo's desktop icons, takes the keyboard. Upstream leaves that window drawn as active, so clicking the desktop did not look like leaving the window. `0004-touch-csd-move-and-builtin-panel-map.patch` lets GTK4 CSD apps start interactive move/resize from touch (seed grab state on touch-down, drive the grab from touch motion, finish on up/cancel), and maps unconfigured touch devices to the first usable eDP/LVDS/DSI panel so an external monitor does not steal the digitizer. `0005-touch-window-button-pill.patch` takes a finger on the close, minimize, and maximize buttons, in labwc's title bar or a client-side one, and opens a larger pill of the hover icons over that cluster. The pill shifts to stay on the screen. The next tap on a large button runs the action. A pointer click is unchanged. `0006-hover-all-window-buttons.patch` shows the hover image on close, minimize, and maximize together when the pointer is on any one of them, as the GTK theme does on client-side title bars. Upstream lights only the button under the pointer.

`patches/scenefx/0007-scenefx.patch` moves labwc onto [SceneFX](https://github.com/wlrfx/scenefx) 0.5, a drop-in replacement for wlroots' scene graph and renderer with per-buffer corner radii, shadow nodes, and blur nodes. It links `scenefx-0.5` ahead of wlroots (falling back to building the 0.5 release tarball through `subprojects/scenefx.wrap` when the system has no `scenefx-devel`), creates the renderer with `fx_renderer_create()`, and names and skips the new node types in the scene dump and window-switcher thumbnails. `scripts/swap-scenefx-includes.sh` then points every `<wlr/types/wlr_scene.h>` include in `src/` and `include/` at `<scenefx/types/wlr_scene.h>`, since both headers use the same include guard. SceneFX renders with GLES2 only: no pixman and no Vulkan renderer, and it needs a DRM render node. Set `ELAVO_SCENEFX=0` when assembling to build without it; every mod keeps its earlier wlr_scene code path for that build.

GPL-2.0-only, the same license as labwc. The include swap script is adapted from the [grigio/labwc labwc-blur fork](https://github.com/grigio/labwc) (labwc PR 3751, GPL-2.0-only).

## What the mods do

- **Theme keys**, read from `themerc` before labwc clamps the radius. A key that is present wins over `rc.xml`.
  - `window.cornerRadius`
  - `window.maximizedCorners` (`yes` keeps rounded corners on maximized and tiled windows; labwc squares both)
  - `window.dropShadows`
  - `window.clipApps` (app ids of client-decorated apps to clip anyway, such as `org.telegram.desktop`)
  - `window.dropShadowsOnTiled` (unlike stock labwc, no gap is required)
  - `window.dropShadowsOnMaximized`
  - `window.blur.strength` (1 to 5 downsample steps, default 3)
  - `window.clipCorners` (`yes` rounds the bottom corners of windows labwc decorates; default off)
  - `window.fx.corners` (SceneFX build: `no` goes back to the strip clipping below; default `yes`)
  - `window.fx.shadows` (SceneFX build: `no` goes back to labwc's buffer shadows; default `yes`)
- **Shadows on maximized and tiled windows.** labwc's shadow stays on windows it decorates when they are maximized or tiled, if the theme asks. A maximized window that draws its own title bar gets the same shadow, built from the buffers labwc renders from the theme's `window.*.shadow` keys. Tiled client-decorated windows draw their own shadow and get none here. labwc's shadow corner pieces are redrawn to follow `window.cornerRadius`, on every window, so the shadow is round where the window is.
- **Rounded maximized and tiled windows.** With `window.maximizedCorners: yes`, the title bar keeps its round corners when a window is maximized or tiled. Only a window too narrow for two corner pieces is squared. Fullscreen stays square.
- **Bottom corners.** With `window.clipCorners: yes`, a window labwc decorates gets its bottom corners rounded to `window.cornerRadius` when floating, and also when maximized or tiled if `window.maximizedCorners` is on (not fullscreen or shaded). In the SceneFX build each buffer in the window's surface tree (popups excluded) gets a corner radius on the window corners it covers, clipped to the window geometry, so the curve is antialiased by the shader and costs no extra draws. The border's bottom becomes one rounded plate with the window cut out of it. Without SceneFX, or with `window.fx.corners: no`, wlr_scene clips a surface only to one rectangle, so the client is drawn once clipped above the corner band, plus one copy per run of rows in the band, each clipped to the width the curve leaves (about 10 extra textured draws per window at radius 16, no extra memory). The edge is a one-pixel staircase without antialiasing; a border arc drawn over it hides most of that. A tiled window's border is laid out as a floating one's. Apps that draw their own decorations are not touched unless `window.clipApps` names their app id (space separated); those are clipped on all four corners of their window geometry and get labwc's shadow in place of their own.
- **SceneFX shadows.** In the SceneFX build, labwc's shadow pieces and the buffer shadow behind maximized client-decorated windows are replaced by one SceneFX shadow node per window, under the same rules (`window.dropShadows*`), sized from the theme's `window.*.shadow.size` and colored from `window.*.shadow.color`, rounded to `window.cornerRadius`, with the window cut out of it.
- **Blur** for surfaces that ask, via `ext-background-effect-v1`. In the SceneFX build the requested region becomes SceneFX blur nodes just below the surface: one rounded node when the region is a rounded rectangle (the dock's), otherwise one per rectangle. `window.blur.strength` sets the number of blur passes. Without SceneFX the backdrop is captured the way labwc's window-switcher thumbnails are, blurred with scaled texture draws (no scenefx, no custom shader), and placed in the scene just below the surface. The first eight re-blurs log their time. Set `<blur>no</blur>` in `rc.xml` to turn it off. On the pixman renderer it stays off unless `ELAVO_BLUR_PIXMAN=1`.

The panel does not ask for blur. The dock and Lens do.

- **Touch window buttons.** A finger tap on close, minimize, or maximize opens a larger pill of the hover icons, filled with the title bar color, centered over the cluster and shifted fully on screen. It grows from half size while fading in, over 180 ms, and shrinks and fades out the same way when dismissed or timed out. The next tap on a large button runs that action. Pointer clicks stay on the small buttons. One check in the touch path covers labwc's title bar and client-side title bars. `~/.config/elavo/touch.conf` can turn the pill off (`[Touchscreen] EnlargedWindowButtons=false`). Missing means on.

## Build

```bash
./scripts/assemble.sh
./build/labwc/build/labwc --version
ELAVO_SCENEFX=0 ./scripts/assemble.sh   # without SceneFX
```

The SceneFX build wants `scenefx-devel` 0.5 (Fedora 44 packages it). Without it, meson downloads the 0.5 release tarball and builds it as a static subproject.

`ELAVO_LABWC_MIRROR` overrides the mirror. Next to a checkout of `elavo-labwc`, that checkout is used. Otherwise the script clones `elavo-io/elavo-labwc`.

# Frame rate and stutter

How to measure the game's frame pacing, what the numbers mean, and what we measured.
Reach runs at 30 fps on the Xbox 360 (its menus at 30, the pre-title screens at 60).

## Measuring

`RECOMP_PERF=1` turns on the frame statistics (`reach-recomp/src/debug/frame_stats.cpp`; the
older `REACH_FPSLOG=1` does the same). They are off by default and cost nothing then; when on,
they keep the frame times in memory and log two lines every 10 seconds and two at exit:

```
PERF: scene=menu-warm src=guest t=50.9 fps=30.0 p50=33.6 p95=33.7 p99=33.7 max=34.0 low1=29.5 stutters=0 hitches=0 ontarget=100.0 pipelines=0
PERF: scene=menu-warm src=present t=60.5 fps=144.0 p50=6.9 p95=7.5 p99=7.9 max=10.8 low1=116.8 stutters=0 hitches=0 ontarget=0.0 pipelines=0 final=1 from=28.8
```

- `RECOMP_PERF_SCENE=<name>` labels the lines.
- `RECOMP_PERF_CSV=<path>` writes every frame time (`scene,src,t,ms`).
- `RECOMP_PERF_RESET=<path>`: when that file appears (it is deleted again), the summary
  restarts, so it covers only what follows, e.g. from the moment a scene is reached. The
  summary line ends in `final=1 from=<s>`.

`tools/bench.sh menu|gameplay [--cold]` does all of this with a scripted run (input FIFO,
`tools/live_session.sh`): START skips the intro at 9 s, then it goes to the main menu
(`menu`), or into the Custom Game lobby and START GAME with the profile's last game type and
map, SWAT on Sword Base here (`gameplay`, turning in place with the right stick). Once the
scene is on screen it restarts the summary, measures 30 s (`--seconds`), quits the game
and prints the two summary lines, the game's CPU use, the GPU's load and the swapchain's
present mode. Each step leaves a screenshot in the run's directory. `--cold` starts with an
empty shader storage (a temporary `cache_root` sharing the game's `cache0`/`cache1`) and
Mesa's shader cache off, so every pipeline is compiled during the run; the default (warm)
uses the caches as they are. Other arguments go to the game, e.g.
`--vulkan_allow_present_mode_immediate=true`. Runs are windowed unless `--fullscreen`.

Other programs skew the numbers, other games most of all. `bench.sh` records other `reach`
and `forza` processes, the load average, available memory and the GPU's load (AMD) before
the launch and at the start and end of the measurement, and checks for other games every
second. A run with another game running, a 1-minute load average above 2 before the launch
or the GPU more than 20% busy prints `CONTENDED` and is not a baseline: such runs do not go
in the table below. On a shared machine, `BENCH_LOCK=<file> BENCH_OWNER=<name>` makes it
refuse to start while someone else holds the lock in that file (`owner expiry-epoch
purpose`).

## Metrics

Two streams are timed:

- **guest**: the game handing a frame to `VdSwap`. This is the game's own frame rate.
- **present**: the host presenter calling `vkQueuePresentKHR`, hooked in the SDK's Vulkan
  device function table (`reach-recomp/src/platform/vulkan_hooks.cpp`). How often the window
  is updated; with FIFO in a window, about how often the compositor takes a new image.

Per 10 s window, and over the whole measurement in the summary:

| Field | Meaning |
| --- | --- |
| `fps` | frames / total frame time |
| `p50`, `p95`, `p99`, `max` | frame time percentiles and maximum, in ms (nearest rank) |
| `low1` | 1000 / mean of the slowest 1% of frame times ("1% low", fps) |
| `stutters` | frames longer than 2 × that window's p50 |
| `hitches` | frames longer than 100 ms |
| `ontarget` | % of frames within ±20% of 33.3 ms (the 30 fps target); meaningless for `present` |
| `pipelines` | graphics and compute pipelines the GPU plugin created in the window (guest shaders, render-target helpers): stutter from shader compilation shows here |

The pipeline count leaves out the SDK presenter's own pipeline, which it rebuilds on every
present (see below).

## Present modes

Every swapchain creation logs its present mode and the modes the surface offers:

```
Vulkan swapchain 1920x1080: present mode FIFO (the surface offers MAILBOX FIFO IMMEDIATE; ...)
```

On KDE Plasma 6.7 (Wayland) with Mesa 26.2 RADV, a Wayland surface offers MAILBOX, FIFO and
IMMEDIATE (no FIFO_RELAXED). The SDK takes the first allowed of IMMEDIATE, MAILBOX,
FIFO_RELAXED, FIFO. The game now allows only FIFO by default (`src/main.cpp`), because:

- **IMMEDIATE tears.** KWin lets fullscreen windows that ask for it tear. Reach's menus pan
  slowly, and a 30 fps image on a 144 Hz display puts the tear line at a different height
  every frame: a band of light that seems to crawl down the screen. The guest frames have no
  such band (frame dumps of the title and main menus three times a second: row brightness
  changes by at most 4/255, with no moving peak; only the clouds and the selection pulse
  move), so it is the presentation.
- **FIFO_RELAXED tears too** at 30 fps: every image arrives after the vblank it was due for.
- **MAILBOX does not tear, but spins.** The SDK's UI thread repaints nonstop whenever an
  ImGui dialog exists, and the achievement notification dialog always does. On Windows the
  SDK paces that with DXGI vblank waits; on Linux only a FIFO swapchain does. With MAILBOX
  (and IMMEDIATE) the window is repainted thousands of times a second (about 4000 on the
  main menu, 3300 in a game, on an idle machine), each time also rebuilding the output
  pipeline and waiting for the previous repaint on the GPU: the presenter never stores the
  swapchain format it built the pipeline for (`swapchain_effect_pipeline.swapchain_format`
  in `vulkan_presenter.cpp`), so every paint sees a mismatch. That costs GPU time the game
  and the compositor need. FIFO caps it at the display's refresh rate.

FIFO's cost is up to one more refresh of latency (7 ms at 144 Hz, 17 ms at 60 Hz). To change
it, in `reach.toml`:

```toml
vulkan_allow_present_mode_mailbox = true     # no tearing, lowest latency, repaints nonstop
vulkan_allow_present_mode_immediate = true   # the SDK's default: tears
```

The command line works too (`tools/play.sh --vulkan_allow_present_mode_immediate=true`).

## Results

Machine: Ryzen 7 7800X3D, Radeon RX 9060 XT (Mesa 26.2.1 RADV), KDE Plasma 6.7.4 on Wayland,
1920x1080 at 144 Hz. `tools/bench.sh`, 30 s measured, windowed (1280x720), no other game
running and the load average under 2 at launch (a reserved, idle machine). IMMEDIATE was the
default before commit `c7cccb2`; FIFO is the default since. Times in ms; `present fps` is
vkQueuePresentKHR calls per second; CPU is the game's, in % of one core; GPU is the whole
GPU's busy percentage.

| Date | Commit | Scenario | Cache | Present mode | fps | p50 | p95 | p99 | max | 1% low | stutters | hitches | on target % | pipelines | present fps | CPU % | GPU % |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-10-10 | c7cccb2 | menu | warm | IMMEDIATE | 29.9 | 33.6 | 33.8 | 34.1 | 67.3 | 24.0 | 1 | 0 | 99.8 | 1 | 3937 | 240 | 44 |
| 2026-10-10 | c7cccb2 | menu | warm | MAILBOX | 29.9 | 33.6 | 33.7 | 33.8 | 66.4 | 24.3 | 0 | 0 | 99.8 | 1 | 4072 | 241 | 44 |
| 2026-10-10 | c7cccb2 | menu | warm | FIFO | 30.0 | 33.6 | 33.7 | 33.9 | 34.2 | 29.4 | 0 | 0 | 100.0 | 0 | 144 | 178 | 21 |
| 2026-10-10 | c7cccb2 | menu | cold | FIFO | 30.0 | 33.6 | 33.7 | 33.8 | 34.0 | 29.6 | 0 | 0 | 100.0 | 0 | 144 | 184 | 21 |
| 2026-10-10 | c7cccb2 | gameplay (SWAT, Sword Base) | warm | IMMEDIATE | 30.0 | 33.6 | 33.7 | 33.8 | 34.2 | 29.5 | 0 | 0 | 100.0 | 6 | 3348 | 206 | 55 |
| 2026-10-10 | c7cccb2 | gameplay (SWAT, Sword Base) | warm | FIFO | 30.0 | 33.6 | 33.7 | 33.8 | 33.8 | 29.6 | 0 | 0 | 100.0 | 2 | 144 | 152 | 32 |

- The game itself keeps 30.0 fps in every mode on an idle machine. Vsync costs it nothing;
  dropping the nonstop repaints frees about a quarter of the GPU and 0.5-0.6 of a CPU core.
- FIFO presents at exactly the display's 144 Hz.
- Cold vs warm: the warm run restores 1797 pipelines from the shader storage before the
  first frame. The cold run compiles 147 pipelines in the first 10 s of frames (the intro
  and title screen; the worst frame took 642 ms) and 13 more in the next 10 s (67 ms); by the
  main menu nothing is left to compile.
- Runs made earlier on a busy machine (two more Reach instances, each repainting nonstop) are
  not in the table: there the windowed FIFO and MAILBOX runs got only 12-20 presents a second
  in gameplay, because the compositor itself was starved of GPU time.

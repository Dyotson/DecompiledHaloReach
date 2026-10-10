# Frame rate above 30 fps: investigation

Status: investigation and proposal, nothing changed in the game yet. Addresses are in
`default.xex` (base version); `docs/perf.md` explains the measurements.

## How Reach paces itself

**Frame limiter.** The main loop (`sub_822377E0`) calls `Main_FrameLoopStep`
(`sub_820E1D18`) until the game quits. Before each frame it waits on the vblank event until
a 64-bit vblank counter at `0x8319F800` (incremented by the game's vblank interrupt
callback) has moved on by 2 since the last frame (`li r31, 2` at `0x820E1E18`; the
`addi r9, r11, 2` at `0x820E1E04` on another path), or by 3 after a flag at
`0x8367C184` is set. That flag is part of the network clock sync: `sub_8247BA48`, which
lines a client's game time up with the host's, sets it when the client runs ahead. At the
console's 60 Hz that is 30 fps. (The intro video and loading screens run at 60 fps; how they
get past this wait was not traced. A byte at `0x831510D9` skips it.)

**Game time.** `sub_82440410` is the per-frame game update. It measures the real time since
the last frame, and `sub_820EE150` (game time update) turns it into whole ticks:

```
ticks    = floor(tick_rate * dt * game_speed + leftover)    // tick_rate: short at game_time+4 (30)
leftover = the fraction left over, kept at game_time+0x14   // seconds per tick: float at +8 (1/30)
```

with a cap on ticks per frame. In a networked game the tick count comes from the
simulation's network clock instead (`sub_8220F130`), adjusted by `sub_8247BA48`. The ticks
then run the simulation (`sub_821F4DF0`, profiled as `main_thread_raw_game_ticks_ms`), and
player control gets `ticks * seconds_per_tick`
(`sub_82442790` → `sub_8247C978`, the function the mouse-look hooks live in). So the
simulation runs at a fixed 30 Hz of real time whatever the frame rate. After the ticks, some
systems update once per frame with the real or game delta time: `sub_82208CC8` (per-player
frame callbacks, effects) and six `*(dt)` calls at the end of `sub_82440410`.

**No render interpolation on the Xbox 360.** The 360 build has no interpolation system (its
only "interpolat" string is the scenario interpolators, a scripting feature). MCC's PC build
of Reach (`haloreach.dll`) added one: a `halo_interpolation` thread-local block, buffers of
110 MB and 2 × 27 MB allocated next to it, and locks named "CS:Interpolator Pending New
Objects" / "Pending Deleted Objects". MCC also kept Reach at 30 ticks a second: its 2024
change to 60 Hz ticks covered Halo 2, 3, 4 and Halo 2 Anniversary, not Reach, and needed
follow-up fixes (hitscan range and projectile speed halved per tick).

**Community patches.** xenia-canary/game-patches has no frame rate patch for Reach (base or
TU1), nor for Halo 3, ODST or Halo 4; only aspect ratio, lens flare and similar patches.

## Experiments

All with `tools/bench.sh` and `RECOMP_PERF`; the guest vblank was raised to 120 Hz with
`--video_mode_refresh_rate=120`, so the unchanged 2-vblank limiter targets 60 fps. These ran
next to other games (contended): fine for behaviour, not for frame rates.

- **The limiter follows the vblank.** The counter at `0x8319F800` advanced 242 in 2 s (read
  with `tools/guestmem.py`), and the game rendered 43-46 fps instead of 30 on the menus and
  in a SWAT game. (The intro video ran at 111 fps: it presents once per vblank.)
- **The simulation stays at real-time speed.** The SWAT clock went from 11:58 to 11:15 in
  42 s of real time at 43 fps.
- **The camera moves on every frame.** Turning in place (right stick held) at ~45 fps, 13
  consecutive frames dumped from guest memory were each rotated 68-120 px further than the
  one before; with the view updated only on 30 Hz ticks, about one frame in three would
  repeat the previous view. A hash of every presented frame found no frame identical to the
  one before it in 2000+ frames of play. Effects and other per-frame systems move too.
  Whether other objects (players, vehicles, projectiles) move between ticks was not
  measured; the per-tick simulation suggests they do not.

**Clean measurements** (reserved idle machine, 2026-10-10, commit `cb092d4`, windowed): with
the 2-vblank limiter at a 120 Hz vblank the main menu ran 45.6 fps and a SWAT game 45.1 fps,
both with a flat frame time (menu p50 21.9 ms, p99 22.3; game p50 22.2, p99 23.6); the title
screen 45.9 fps at 21.8 ms, the same with `--vulkan_readback_resolve` on or off.

**Where the 22 ms go (functional runs, title screen).** Not the limiter: with `--vsync=false`
(a vblank every millisecond, so the limiter never waits) the title screen still runs 45.7 fps
at 21.8 ms. Not CPU work: over 10 s the guest RENDER thread used 54% of a core, MAIN_THREAD
10%, nothing else more than 10%. gdb stack samples (12): MAIN_THREAD sat in an infinite wait
on a sync object in `sub_820E40F0` (called from `Main_FrameLoopStep` via `sub_820E1B60`) in 9;
RENDER either waited on an event in `sub_820CCFD8` (6) or spun in `sub_820CA090`'s loop
that polls for the main thread's next frame packet for up to 66 ms (5); the GPU command
processor thread was idle in 10 of 12. So the frame is bounded by a fixed hand-off or
synchronization latency, not by work. Not yet checked: who signals the object `sub_820E40F0`
waits on (table `0x8394F304`, index at `0x82BE4D94`), our critical sections
(`REACH_SDK_CRITICAL_SECTIONS=1`), and sleep/timer granularity in the waits the SDK runs
(the GPU command processor sleeps while polling `WAIT_REG_MEM`).

## Options

**(a) A higher tick rate (60 Hz).** The rate is data (`game_time+4`, with 1/30 at +8), but
the game is built around 30 ticks a second: animations advance per tick, scripts and AI
count ticks, physics steps per tick, and the network protocol exchanges ticks, so every peer
(Reach Live, System Link) would have to run the same rate. The constant 1/30 also appears
about 90 times in the read-only data. MCC moved four other Halo games to 60 Hz ticks and
had to fix hitscan range and projectile speed afterwards; it did not do it for Reach. Not
recommended.

**(b) 30 Hz ticks, interpolated rendering.** What MCC does: keep the previous and current
tick's state of every rendered object (node matrices, camera) and draw at
`previous + (current - previous) × fraction`, the fraction being the leftover tick already
kept at `game_time+0x14`. Smooth motion for everything, one tick of extra latency, no change
to gameplay or the network. It is the largest change: find where render data is taken from
the game state, snapshot it per tick, and handle objects created, deleted or teleported
between ticks (MCC's "Pending New/Deleted Objects" locks and large buffers show the size of
it).

**(c) Unlock the limiter, keep everything else.** Wait 1 vblank instead of 2 in
`Main_FrameLoopStep` (a mid-function hook on `0x820E1E18`/`0x820E1E04` that keeps the
network clock sync's extra vblank, or a higher guest vblank rate). Since the camera and
effects already update per frame, turning and looking around get 60 fps or more; objects
that only move on ticks still step at 30 Hz. No gameplay
or network change, as ticks are untouched. Cost: about twice the CPU and GPU work per second,
so the port has to hold 60 fps first: today something caps it at ~46 fps (above).

**Proposal:** find and remove the ~22 ms synchronization floor first (next steps: trace who
signals the `sub_820E40F0` wait object, time the guest's waits with a hook on
`sub_820CCF70`, try `REACH_SDK_CRITICAL_SECTIONS=1`). Then (c), behind a setting
(`frame_rate = 30 | 60 | unlocked`, default 30 until it is validated), as a hook on the
limiter rather than the vblank rate (the vblank also paces the intro video, which presented
111 frames a second at a 120 Hz vblank). Validate the campaign, Firefight and a Reach Live
match at 60. Then measure what still looks like 30 Hz (objects, the first-person weapon) and
decide whether (b) is worth it, starting with the objects closest to the camera.

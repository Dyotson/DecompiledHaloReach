# Keyboard and mouse

Reach on PC plays with keyboard and mouse out of the box, next to any pad. The device is
`src/input/kbm.cpp` (key bindings, mouse capture) plus three hooks in the game's player
control that apply mouse motion directly to the camera (`src/hooks/mouse_look.cpp`,
`reach-recomp/hints/mouse_look.toml`).

## Default bindings

They follow Reach's **Default** button layout (Settings > Controller in the game; the pad
control each key presses is in brackets, so another in-game layout moves the actions with it).

| Action | Keys | Pad |
| --- | --- | --- |
| Move | W A S D | left stick |
| Look | mouse | (direct, see below) |
| Fire | left mouse button | RT |
| Zoom | right or middle mouse button | right stick click |
| Throw grenade | G, mouse button 5 | LT |
| Melee | Q, mouse button 4 | RB |
| Jump | Space | A |
| Crouch | Ctrl, C | left stick click |
| Armor ability | Shift | LB |
| Action / reload (hold to pick up) | E, R | X |
| Swap weapons | 1, 2, mouse wheel | Y |
| Switch grenades | X | B |
| Night vision | N, Up | D-pad up |
| Scoreboard | Tab | Back |
| Game menu | Escape | Start |
| Menus | arrows (D-pad), W/S (stick), Enter (A), Backspace (B) | |

Each pad control has a `kbm_bind_*` cvar holding a comma-separated list of keys (key names
as in the SDK settings overlay, plus `LMB` `RMB` `MMB` `Mouse4` `Mouse5` `WheelUp`
`WheelDown`), e.g. `--kbm_bind_rb=F,Mouse4`. They are hot-reloadable from the settings overlay
(F4), category "Input/Keyboard and Mouse". A wheel notch holds its control for 80 ms (Reach
polls pads once per 1/30 s tick).

| Cvar | Default | Meaning |
| --- | --- | --- |
| `kbm` | true | keyboard and mouse device on guest user 0 |
| `kbm_sensitivity` | 2.5 | mouse look: 0.022° per count × this (2.5 ≈ 0.055°/count, a full turn in ~6500 counts) |
| `kbm_vertical_ratio` | 1.0 | vertical speed relative to horizontal |
| `kbm_invert_mouse` | false | invert vertical mouse look, on top of the game's Look Inversion setting |
| `kbm_bind_<control>` | see above | `a b x y lb rb lt rt ls rs back start dpad_up dpad_down dpad_left dpad_right move_forward move_back move_left move_right look_up look_down look_left look_right` |

The device is synthetic: the SDK routes it to guest user 0 and merges it with a pad there
(buttons OR, larger stick deflection wins), so pad and keyboard work at the same time. The
SDK's own keyboard driver (`mnk_mode`) stays off; turning it on as well would press its own
binds on top.

The mouse is captured (hidden, relative pointer mode) only while the window has focus, no SDK
overlay wants input and the camera is under player control. In menus, pauses and loads the
cursor is free; motion from those moments is dropped instead of snapping the camera.

## How mouse look works

Mouse-to-right-stick emulation feels wrong in Halo: the stick drives a turn *rate* with
acceleration, a dead zone and a maximum speed. The mouse gives an *angle*. So the stick path is
left alone and the mouse is added after it:

1. `sub_821F0048` (input update) polls the four pads into 0x3C-byte records at
   0x82BE4EC6 (raw sticks at +0x34: lx, ly, rx, ry).
2. `sub_820E8A68` (input abstraction) turns them into per-controller floats at
   0x838300C8 + 0x1D8 × controller: +0x198 forward, +0x19C strafe, +0x1A0 yaw (−rx),
   +0x1A4 pitch (ry, negated with Look Inversion), +0x1A8 flight pitch (also negated with
   flight inversion). It applies the stick layout from the controller settings at
   0x8382FE28 + 0xA8 × controller (+0x98 stick layout, +0x9A look inverted, +0x9B flight
   inverted; +0x00/+0x04 are the yaw/pitch rates in degrees from Look Sensitivity).
3. `sub_8247C978` (player control, per local player; r3 player, r4 controller, f1 tick, r7
   unit-control output) multiplies yaw/pitch by those rates and the stick acceleration,
   divides by the zoom magnification (`sub_820DA900`, at 0x8247D8B4), applies the unit's
   look-rate scale and aim-assist magnetism, and stores rate × tick at output+0x14 (yaw, left
   positive) and output+0x18 (pitch, up positive) at 0x8247DD14/0x8247DD1C. The caller
   (`sub_824806A0`, from the game tick `sub_82442790`) hands the output to the unit.

Hooks (`hints/mouse_look.toml`), all in `sub_8247C978`:

| Address | Hook | Does |
| --- | --- | --- |
| 0x8247D6D8 | `ReachMouseLookBegin(r15, r3)` | start of the "player has look control" branch; r3 is `sub_82484108` ("flying"), which picks the flight pitch input |
| 0x8247D8B8 | `ReachMouseLookZoom(r15, f0)` | zoomed: f0 = 1 / magnification |
| 0x8247DD20 | `ReachMouseLookApply(r15, r27)` | adds the mouse angle × zoom factor to output+0x14/+0x18 for controller 0 |

So mouse motion skips stick acceleration, the dead zone, the turn-rate cap and magnetism, but
keeps zoom scaling, Look Inversion (and flight inversion when flying) and the game's own pitch
limits. Cinematics, death and pauses never reach the hooks.

## Testing without a desktop

The input FIFO of `tools/live_session.sh` (`REACH_AUTOPRESS_FIFO`) drives the same paths:
`KEY:name[:hold]` holds a key or mouse button by binding name (`KEY:Space`, `KEY:LMB:1`,
`KEY:WheelUp`), `MOUSE:dx,dy` moves the mouse by counts (`MOUSE:1636,0` turns 90° right at
the default sensitivity). Never synthesize input on the real desktop.

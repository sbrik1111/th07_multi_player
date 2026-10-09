# th07_multi_player

An experimental modification that lets **two or three people play Touhou 7 ~
Perfect Cherry Blossom together**. Built on the decompilation at
[some100/th07](https://github.com/some100/th07), with
[RUEEE/th06_multi_net](https://github.com/RUEEE/th06_multi_net) as a reference
for the multiplayer design.

The original game data (`th07.dat` and `thbgm.dat`) is not included. Supply it
from your own legitimate copy of the game.

## Current status

Three-player netplay has been confirmed working in testing.

This release also includes independent character/shot
selection for all three players, shared-border handling, synchronized enemy
drops, and predictive rollback across stage transitions.

**v0.2.0**

* Reworked the core multiplayer systems.
* Added multiplayer replay recording and playback.
* Added an optional `lowlatency` setting for input sampling and frame timing.
* Added F5/F6 controls to adjust input delay during rollback netplay.
* Delayed hit sounds until the hit is confirmed and suppressed duplicate sounds
  when rollback replays a frame.

**v0.1.7**

* Fixed the screen breaking up while paused.
* Window size and BGM can now be changed while waiting for a match to start.

**v0.1.6**

* Greatly reduced menu latency in multiplayer.
* Fixed sessions freezing after pausing.
* Failed rollback recovery now logs an error instead of freezing the game.

**v0.1.5**

* Added power transfer: overlap a partner and tap shot eight times to send them 20 power.

**v0.1.4**

* Added the `Advanced settings` section for optional bot, display, and
  diagnostic controls.
* Added **Pin FPU control word** for machines that may use different floating-point settings.

**v0.1.3**

* Fixed a rollback freeze caused by bomb-effect history overflow.
* Added optional `netplay_trace.txt` logging.
* Improved guest bot movement, item collection, boss positioning, and bomb usage.

**v0.1.2**

* Fixed a serious item-drop synchronization issue caused by item randomization.

This is still an experimental release, so other desyncs and bugs may remain.

## Known issues

- **A session can still desync.** v0.1.2 fixed the known item-drop
  synchronization issue, but this does not guarantee that every possible cause
  has been found. If a session drifts apart, everyone should leave and rematch
- **The title screen and the ending run very slowly in three player
  sessions.** Both are outside the synchronized gameplay loop, and the extra
  peer makes them noticeably worse

## What it does

* **Two or three player netplay** over UDP. The host is P1, the guests are P2 and P3; guest-to-guest input is relayed through the host
* **The host must have the UDP port open/forwarded** so guests can connect
* **Local two player** on one keyboard
* Each player picks their **own character and shot type**
* Lives, bombs and power are per player; cherry and score are shared
* Life transfer between players, and revival of a player who is out of lives
* **Predictive rollback** so movement is not held back by the round trip
  (selected by default; the host chooses the input delay)


## Playing

1. Copy `th07.dat` and `thbgm.dat` next to `th07_multi.exe`
2. Everyone double-clicks `th07_multi.exe`, which opens the connection
   launcher
3. Choose Host or Guest under `Connect as`. Guests enter the host's IP address
4. Press the button below it (`Start hosting` or `Connect to host`)
5. When `cur state` lists the other players and `Start Game` lights up, the host
   presses it to start the game on every PC
6. In game, choose a character and shot for each player in turn

`Start Game (local)` in the same launcher starts a two player game on one PC.

Multiplayer replays are saved automatically as `.mpr` files in `replay/`.
To watch one, choose `Start Game (local)` in the launcher, then select
`Replay` from the game menu.

The host has to allow its UDP port through Windows Firewall. Playing over the
internet also needs a port forward on the host's router.

## Controls

In netplay each PC uses its own keyboard or pad with the original key layout,
guests included.

| Action | Key |
| --- | --- |
| Move | Arrow keys or numpad |
| Shot | `Z` |
| Bomb | `X` |
| Focus | `Shift` |
| Skip dialogue | `Ctrl` |
| Menu | `Esc` |
| Input delay during rollback netplay | `F5` decreases by one frame; `F6` increases by one frame |

Pads use each PC's own `th07.cfg`, so configure them before matching. While
the window is not focused the keyboard is ignored and only the pad is read.

Local two player shares one keyboard, so the second player uses a different
set. The `[KeyBind]` section of `mod_config.ini` can change it.

| Action | Key |
| --- | --- |
| Move | `I` / `J` / `K` / `L` |
| Shot | `F` |
| Bomb | `G` |
| Focus | `D` |

There is no keyboard mapping for a third local player.

## Launcher settings

The host chooses rollback and input delay; guests receive these session
settings when they connect. Display and audio choices are local to each PC.
The `BOT` checkbox in the launcher is intended for testing.

`Advanced settings` contains the optional `lowlatency` setting. It adjusts
input sampling and frame timing to reduce latency. It is off by default and
can be chosen separately on each PC.

## What changes in multiplayer

- **Boss health** scales with the player count: 1x for one player, 0.75x for
  two, 2/3 for three
- **Enemy drops** produce one life or bomb item per active player, and anyone
  may collect any of them
- **Power items** are assigned to the players in rotation as they drop, and a
  drop becomes cherry only when the player it fell to is already at full power.
  A player who is not at full power keeps receiving power
- **Point item extends** are granted to everyone when the threshold is reached
- **Life transfer**: overlap two ships within 20 pixels, then release shot and
  hold focus for 90 frames. A life item homes to the other player; a player
  who is out of lives is revived directly
- **Power transfer**: overlap two ships within 20 pixels and tap shot eight
  times. Twenty power crosses as power items that home to the other player. A
  counter appears over the ship from the fourth tap. It only offers itself when
  the giver has twenty to spare and the partner is short of full power
- **Unlocks** for Extra, Phantasm, every character and every practice stage are
  forced in memory so that differing `score.dat` progress cannot change the
  synchronized menu structure. Nothing is written back to the save
- The **difficulty cursor** always starts at Normal
- **Rank** loses less to a death or a bomb than in single player, divided by the
  player count, so that three ships losing lives do not flatten the difficulty
  curve three times as fast

## Predictive rollback

Predictive rollback continues using recent remote input until the actual input
arrives. If the prediction differs, the game rewinds to the affected frame and
replays the simulation. If an input remains missing beyond the prediction
window, play waits for it.


## Building

A 32-bit Windows build using MSVC 19.10 (Visual Studio 2017), the Windows 10
SDK, and the DirectX 8 SDK. Put the compiler under `prefix/msvc1410`, or set
`TH07_PREFIX` to a checkout containing that prefix.

```
python scripts/build.py
```

The result is `build/th07_multi.exe`. The build script fetches the DirectX 8
SDK files if they are missing.

## Limitations

- Experimental. There is no NAT traversal and no encryption
- A NAT rebind after matching cannot be recovered from
- Intended for playing with people you trust, not for a public server
- Everyone must run the **same `th07_multi.exe` and the same game data**
- Latency or packet loss beyond the prediction window stalls the game while it
  waits for the missing input

## Data and rights

Do not redistribute `th07.dat` or `thbgm.dat`. They are required to run, but
each player supplies them from their own copy of the game.

Rights over the decompiled portion follow
[some100/th07](https://github.com/some100/th07).

## Credits

- [some100/th07](https://github.com/some100/th07) for the decompilation this
  is built on
- [RUEEE/th06_multi_net](https://github.com/RUEEE/th06_multi_net) for the
  multiplayer design it follows

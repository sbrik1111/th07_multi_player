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

The current development line also includes independent character/shot
selection for all three players, shared-border handling, synchronized enemy
drops, and predictive rollback across stage transitions.

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
  has been found. If a session drifts apart, everyone should leave and rematch.
  `netplay_trace.txt`, written next to the executable, records what the peers
  stopped agreeing about. It can be switched off under `Advanced settings`, at
  the cost of leaving nothing to read if a session does go wrong
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
* **Predictive rollback** so movement is not held back by the round trip (on by default, zero added delay)


## Playing

1. Copy `th07.dat` and `thbgm.dat` next to `th07_multi_net.exe`
2. Everyone double-clicks `th07_multi_net.exe`, which opens the connection
   launcher
3. Choose Host or Guest under `Connect as`. Guests enter the host's IP address
4. Press the button below it (`Start hosting` or `Connect to host`)
5. When `cur state` lists the other players and `Start Game` lights up, anyone
   can press it; one message starts every PC
6. In game, choose a character and shot for P1, then P2, then P3

`Start Game (local)` in the same launcher starts a two player game on one PC.

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

`Advanced settings` holds eight switches. Display and diagnostic choices are
stored locally, so Host and Guests may choose different values.

| Setting | Effect |
| --- | --- |
| Guest evasive bot (test) | Hands the guest's ship to a bot that dodges, collects power and lives, keeps its distance from a boss, and bombs its way out of a hit |
| Net diagnostics | Draws `NET H RTT 25ms D0` at the top of the playfield |
| Show player names at stage start | Each player's name over their ship for the first four seconds of a stage |
| Show contribution stats (K/D) | Shows each player's defeated-enemy count and applied damage in the HUD |
| Write netplay_trace.txt | Records what the peers stopped agreeing about, for diagnosing a desync. A few megabytes an hour. Turn it on before a session you expect to report |
| Pin FPU control word | Holds the x87 control word to one value every frame, so a graphics driver cannot change how floats round mid-session |
| Verify EXE/game data compatibility | Rejects peers with incompatible executable, game data, or settings identities |
| Chain character-specific Stage 4 cards | Runs the Stage 4 character-specific boss cards for each distinct active character |

The Guest bot, display switches, trace setting, and compatibility display are
per-PC preferences; players do not have to agree on them. Contribution totals
remain synchronized for rollback even when their local HUD drawing is disabled.
The FPU pin and Stage 4 chain are simulation-affecting options, so the Host's
answer is applied to everyone.

A lost or recovering connection is always reported regardless of the
diagnostics setting.

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
- Replay saving is disabled during multiplayer

## Predictive rollback

Predictive rollback continues using the last confirmed remote input until the actual input arrives. If the prediction differs, the game rewinds to the affected frame and replays the simulation using a 24-frame history.

A 24-frame history is required for three-player sessions because input exchanged between guests must pass through two network links.


## Building

A 32-bit Windows build with MSVC 2002.

```
uv run scripts/build.py --no-matching
```

The result is `build/th07_multi_net.exe`. Placing the original `th07.exe` in
`resources/` lets the build take its icon; without it, add `--no-icon`.

`--no-matching` is required. The multiplayer executable is deliberately not
byte-identical to the original, so a matching build stops after a while on the
integrity check.

Dependencies: uv, ninja, and wine on Linux.

## Limitations

- Experimental. There is no NAT traversal and no encryption
- A NAT rebind after matching cannot be recovered from
- Intended for playing with people you trust, not for a public server
- Everyone must run the **same `th07_multi_net.exe` and the same game data**
- Latency or packet loss beyond the 24 frame history stalls the game while it
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

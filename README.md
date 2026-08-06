# th07_multi_player

An experimental modification that lets **two or three people play Touhou 7 ~
Perfect Cherry Blossom together**. Built on the decompilation at
[some100/th07](https://github.com/some100/th07), with
[RUEEE/th06_multi_net](https://github.com/RUEEE/th06_multi_net) as a reference
for the multiplayer design.

The original game data (`th07.dat` and `thbgm.dat`) is not included. Supply it
from your own legitimate copy of the game.

## What it does

- **Two or three player netplay** over UDP. The host is P1, the guests are P2
  and P3; guest-to-guest input is relayed through the host
- **Local two player** on one keyboard
- Each player picks their **own character and shot type**
- Lives, bombs and power are per player; cherry and score are shared
- Life transfer between players, and revival of a player who is out of lives
- **Predictive rollback** so movement is not held back by the round trip
  (on by default, zero added delay)

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

`Advanced settings` holds three switches. All three are per-PC preferences;
players do not have to agree on them.

| Setting | Default | Effect |
| --- | --- | --- |
| Guest evasive bot (test) | off | Hands the guest's ship to a bullet-dodging bot |
| Net diagnostics | off | Draws `NET H RTT 25ms D0` at the top of the playfield |
| Show player names at stage start | on | Each player's name over their ship for the first four seconds of a stage |

A lost or recovering connection is always reported regardless of the
diagnostics setting.

## What changes in multiplayer

- **Boss health** scales with the player count: 1x for one player, 0.75x for
  two, 2/3 for three
- **Enemy drops** produce one life or bomb item per active player, and anyone
  may collect any of them
- **Power items** convert every other one to cherry once any player is at full
  power; the rest stay as power
- **Point item extends** are granted to everyone when the threshold is reached
- **Life transfer**: overlap two ships within 20 pixels, then release shot and
  hold focus for 90 frames. A life item homes to the other player; a player
  who is out of lives is revived directly
- **Unlocks** for Extra, Phantasm, every character and every practice stage are
  forced in memory so that differing `score.dat` progress cannot change the
  synchronized menu structure. Nothing is written back to the save
- The **difficulty cursor** always starts at Normal
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

## Known issues

- **The screen breaks up while paused.** The pause menu draws over a frame the
  network code may not have finished, and the result can be torn or partly
  stale until play resumes
- **The title screen and the ending run very slowly in three player
  sessions.** Both are outside the synchronized gameplay loop, and the extra
  peer makes them noticeably worse
- **A session can still desync.** Most reports turned out to be a faulty
  detector and were fixed, but a real divergence remains: peers stop agreeing
  and their games drift apart with no recovery. It is not reliably
  reproducible. If it happens, everyone should leave and rematch

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

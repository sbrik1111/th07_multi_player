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

**v0.1.7** fixes the screen breaking up while the game is paused, and lets the
window size and the music be changed after a match has been made.

The game draws the stage across the whole window and then paints a border over
everything outside the playfield. While the pause menu was up, that border was
only scheduled by a counter the drawing code takes back down again, and the two
did not balance: on about one paused frame in sixty the border was not painted
and the stage appeared on top of the score panel. Firing a bomb cleared it, so
it looked like flicker that came and went. The border is now always painted
while a menu is up, with no counter in the way.

The launcher used to lock its whole form the moment matching started. The
display size and the BGM switch are decided per PC - no packet carries them and
no peer is told what you picked - so they stay usable while you wait, and are
read again on the way into the game. Everything the peers have to agree on is
still settled before matching.

**v0.1.6** makes the menus usable in a three-player session, and fixes a freeze
on pausing. The title, difficulty and loadout screens used to wait a full
network round trip for every single frame, because the rollback prediction that
covers gameplay is deliberately switched off outside it; walking from the title
to the start of stage 1 spent about fifty seconds waiting. Those screens now
carry a few frames of input delay instead, which every peer steps on the same
simulated frame, and the same walk waits about two seconds.

The freeze was separate and older. A rollback correction that arrived while the
pause menu was open was held until gameplay resumed - but frames keep being
exchanged while the menu is up, so by the time anyone pressed escape again the
frame that needed repairing had aged out of the rewind history. The repair then
failed on every following attempt, and the peer stopped advancing with nothing
on screen to say why. Any pause longer than four tenths of a second did it. The
correction is now applied when it arrives, and a repair that genuinely cannot be
made gives up and says so in `log.txt` instead of stopping the session.

**v0.1.5** adds a power transfer. Overlap a partner and tap shot eight times to
send them twenty power; it crosses the screen as power items that home to them,
the way a transferred life does.

**v0.1.4** turns every `Advanced settings` switch off by default. The
player-name labels and `netplay_trace.txt` were both on, so a fresh install drew
names over the ships and wrote a growing file next to the executable without
anyone having asked for either. All five start off now.

It also adds a fifth switch, **Pin FPU control word**. Direct3D sets the x87
control word when it creates the device and again on every reset, which decides
how the whole simulation rounds; pinning it stops a graphics driver from
changing that mid-session. It is off by default, because on every machine
measured the word was already the value it would be pinned to. Unlike the other
four it is host-authoritative: peers that round differently are the exact
failure it exists to prevent.

**v0.1.3** fixes a freeze. The rollback keeps a short history of saved frames,
and a frame whose bomb effects did not fit in its buffer was discarded instead
of saved. A rewind that later needed that frame found nothing, decided the
state could not be repaired, and waited - on every PC, at the same frame,
recoverable only by everyone pressing F8. The buffer held 128 effects; a
three-player stage reaches ninety-odd routinely, so the margin was thirty-one.
It now holds 1024, and an overflow says so in `log.txt` instead of surfacing
twenty minutes later as something else.

v0.1.3 also adds a switch for `netplay_trace.txt` under `Advanced settings`,
and gives the guest bot a better idea of what it is doing: it looks further
ahead and weighs a threat by how soon it arrives, picks up power and lives
instead of leaving them, keeps its distance from a boss, and bombs its way out
of a hit rather than guessing at one.

**v0.1.2** fixed a serious item-drop synchronization issue related to item
randomization, which could cause different items to appear between players.

This is still an experimental release, so other desyncs may remain.

## Known issues

- **Player-name labels may display incorrectly at the start of a stage.**
- **The screen breaks up while paused.** The pause menu draws over a frame the
  network code may not have finished, and the result can be torn or partly
  stale until play resumes
- **The title screen and the ending run very slowly in three player
  sessions.** Both are outside the synchronized gameplay loop, and the extra
  peer makes them noticeably worse
- **A session can still desync.** v0.1.2 fixed the known item-drop
  synchronization issue, but this does not guarantee that every possible cause
  has been found. If a session drifts apart, everyone should leave and rematch.
  `netplay_trace.txt`, written next to the executable, records what the peers
  stopped agreeing about. It can be switched off under `Advanced settings`, at
  the cost of leaving nothing to read if a session does go wrong

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

`Advanced settings` holds five switches. All five start off.

| Setting | Effect |
| --- | --- |
| Guest evasive bot (test) | Hands the guest's ship to a bot that dodges, collects power and lives, keeps its distance from a boss, and bombs its way out of a hit |
| Net diagnostics | Draws `NET H RTT 25ms D0` at the top of the playfield |
| Show player names at stage start | Each player's name over their ship for the first four seconds of a stage |
| Write netplay_trace.txt | Records what the peers stopped agreeing about, for diagnosing a desync. A few megabytes an hour. Turn it on before a session you expect to report |
| Pin FPU control word | Holds the x87 control word to one value every frame, so a graphics driver cannot change how floats round mid-session |

The first four are per-PC preferences; players do not have to agree on them.
The FPU pin is not: it decides how the simulation rounds, so the host's answer
is applied to everyone. Guests keep their own box but the host's value wins.

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

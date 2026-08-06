#pragma once

#include "inttypes.hpp"
#include "Multiplayer.hpp"

namespace Netplay
{
enum Mode
{
    MODE_SINGLE,
    MODE_LOCAL,
    MODE_HOST,
    MODE_GUEST
};

// These are the deterministic in-game controls shared by the TH06 reference
// implementation. They are carried in the same delayed input history as the
// controller state, so both peers apply an event on the same simulation frame.
enum InGameControl
{
    INGAME_CONTROL_NONE = 0,
    INGAME_CONTROL_QUICK_QUIT,
    INGAME_CONTROL_QUICK_RESTART,
    INGAME_CONTROL_INF_LIFE,
    INGAME_CONTROL_INF_BOMB,
    INGAME_CONTROL_INF_POWER,
    INGAME_CONTROL_ADD_DELAY,
    INGAME_CONTROL_DEC_DELAY,
    INGAME_CONTROL_INSANE_MODE,
    // Host-authoritative three-player lifecycle boundaries. These values are
    // copied into P1's input-history control lane, so rollback replay applies
    // every transition again at the exact same simulation frame.
    INGAME_CONTROL_P2_ABSENT,
    INGAME_CONTROL_P2_RESUME,
    INGAME_CONTROL_P2_LEFT,
    INGAME_CONTROL_P3_ABSENT,
    INGAME_CONTROL_P3_RESUME,
    INGAME_CONTROL_P3_LEFT
};

// Supported command lines:
//   --local [--three-player | --players 3]
//   --host [--port 35000] [--delay 1]
//   --join 192.0.2.10 [--port 35000]
//   --connect-ui (native Host/Guest/Local connection launcher)
//   (empty command line opens --connect-ui; use --single for original single player)
//   --invincible (test-only; may be combined with any mode)
//   --test (local test preset; may be combined with --host/--join)
//   --test-full-run (normal Stage 1..6 run with test automation)
//   --no-demo --quick-start --auto-shoot --auto-skip --windowed
//   --fullscreen / --window-size 640x480|960x720|1280x960
//   --auto-bomb (periodic one-frame bomb pulses for test runs)
//   --test-random-input (test-only random movement/slow/fast/shot/bomb input)
//   --test-evasive-input (test-only bullet/laser avoidance bot with shot/bomb)
//   --test-damage-events (scripted P1/P2 hits, death, spirit, and life transfer)
//   --no-invincible (override the invincible test-preset default)
//   --test-resource-drops (verify enemy life/bomb drops are doubled)
//   --test-p2-features (verify P2 shot/focus/bomb/Power/top collection)
//   --test-p3-features (verify P3 shot/focus/bomb/Power/top collection)
//   --test-hash-trace (log deterministic per-frame 3P state hashes)
//   --no-controller (ignore joystick input for deterministic test runs)
//   --rollback (zero-delay prediction with state snapshot/replay recovery)
//   --low-latency / --no-low-latency (update-before-draw frame loop)
//   --blt-prepare 0..16 --low-latency-spin
//   --test-rollback-input (test-only movement/shoot/focus/bomb input stream)
//   --difficulty normal --character reimu --shot a --stage 1
//   --p2-character marisa --p2-shot b (optional independent P2 loadout)
//   --p3-character sakuya --p3-shot a (optional independent P3 loadout)
//   --practice --seed 1 --test-seconds 60
//   --connect-timeout 5 (Host/Guest wait timeout in seconds)
//   --test-reconnect (test-only; reconnect after 120 network frames)
//   --test-controls (test-only; deterministic in-game controls)
//   --test-rng-mismatch (test-only; host injects one RNG mismatch)
//   --test-state-mismatch (test-only; host injects one logical-state mismatch)
//   --test-result-reconnect (test-only; both peers exercise Result/title reconnect)
//   --test-replay-block (test-only; both peers exercise multiplayer Replay guard)
//   --test-ui-sync (test-only; both peers exercise shared menu input)
//   --test-input-sync (test-only; both peers exercise separate P1/P2 input lanes)
//   --test-proximity (test-only; verify the TH06 close-player display fade)
//   --test-life-transfer (test-only; verify normal life transfer in local co-op)
//   --help (show the launcher command summary and exit)
bool Initialize(const char *commandLine);
void Shutdown();

Mode GetMode();
bool IsMultiplayer();
bool IsNetworked();
i32 GetInitialDifficultyCursor();
bool IsHost();
bool IsConnected();
int GetPlayerCount();
int GetLocalPlayerSlot();
bool IsPlayerTemporarilyAbsent(u8 playerId);
bool IsPlayerPermanentlyDeparted(u8 playerId);
bool IsWaitingForRemoteInput();
bool IsConnectionFailed();
bool IsRollbackReplay();
bool IsResyncing();
// OpenInputLagPatch-style source integration. Multiplayer enables this by
// default; explicit --low-latency also makes it available to --single.
bool LowLatencyEnabled();
bool LowLatencySpinWait();
bool LowLatencyDwmFlush();
int GetLowLatencyPrepareTimeMs();
bool AllowsMultipleInstances();
bool IsInvincible();
bool IsDemoDisabled();
bool ConsumeQuickStart();
int GetQuickStartDifficulty();
int GetQuickStartCharacter();
int GetQuickStartShot();
int GetQuickStartStage();
bool IsQuickStartPractice();
// Normal multiplayer follows the shared setup screen: P1 confirms first,
// then each unresolved partner confirms in slot order (P2, then P3). A fixed
// command/UI loadout and Quick Start bypass that slot's interactive selector.
bool ShouldSelectAdditionalPlayerLoadout();
int GetNextPendingLoadoutPlayerId();
void BeginPlayerLoadoutSelection(u8 playerId);
void CompletePlayerLoadoutSelection(u8 playerId, int character, int shot);
void CancelPlayerLoadoutSelection();
void ResetInteractiveLoadout();
// Returns the loadout that the player chain should use. Player 0 follows the
// original TH07 GameManager selection; player 1 falls back to that selection
// unless an independent P2 loadout was supplied by the host/command line.
int GetPlayerCharacter(u8 playerId);
int GetPlayerShot(u8 playerId);
// P2 needs a visual distinction only when both players selected the same
// character. Different characters keep their original palette on both PCs.
bool ShouldTintPlayer2();
bool ShouldTintPlayer(u8 playerId);
bool IsAutoShootEnabled();
bool IsAutoSkipEnabled();
bool IsProximityTestEnabled();
void ReportProximityTestResult(bool passed);
bool IsLifeTransferTestEnabled();
bool IsLifeTransferTestVerified();
void ReportLifeTransferTestResult(i32 recipient, i32 livesBefore,
                                  i32 livesAfter);
bool IsDamageEventTestEnabled();
// These predicates are pure functions of the synchronized simulation frame,
// so rollback replay injects the same hit/transfer sequence as the first pass.
bool ShouldInitializeDamageEventTest();
bool ShouldInjectDamageEvent(u8 playerId);
bool ShouldForceDamageTestTransfer(u8 giverId);
void ReportDamageEventHit(u8 playerId, i32 lives, i32 playerState);
void ReportDamageEventRespawn(u8 playerId, i32 livesBefore, i32 livesAfter);
void ReportDamageEventSpirit(u8 playerId);
void ReportDamageEventRevive(u8 giverId, u8 receiverId,
                             i32 giverLivesBefore, i32 giverLivesAfter);
bool ForceWindowed();
bool ForceFullscreen();
// The launcher stores BGM/SE preferences per PC. They are applied after the
// original th07.cfg has been loaded and are intentionally not network data.
void ApplyAudioPreferences();
bool IsBgmEnabled();
int GetWindowClientWidth();
int GetWindowClientHeight();
bool WasStartupCancelled();
bool NoSave();
// Multiplayer menu structure must not depend on each PC's score.dat. This is
// an in-memory policy only; it never writes unlock flags to the save file.
bool ShouldForceContentUnlocks();
void StartTestTimer();
bool HasTestTimedOut();
int GetDelay();
const char *GetStatusText();
u32 GetRoundTripMs();
// Names are entered in the connection launcher and exchanged during the
// HELLO/WELCOME handshake. playerId 0 is P1 and playerId 1 is P2.
const char *GetPlayerName(u8 playerId);
// Per-PC display preferences from the launcher's advanced section. They only
// affect what is drawn, so peers need not agree on them.
bool ShouldShowStagePlayerNames();
bool ShouldShowNetDiagnostics();
bool ShouldWriteFrameTrace();
// Draw P2's life/bomb icon rows in the original HUD sprite batch. Gui calls
// this next to P1's rows so both sets use the same render state.
void DrawPlayer2ResourceIcons();
u16 GetInitialRngSeed(u16 fallbackSeed);
InGameControl ConsumeSynchronizedControl();
void AdjustDelay(int amount);
void ToggleInsaneMode();
bool IsInsaneMode();

// A network frame samples its local input only once. While waiting for the
// matching remote input, this returns false so the pending sample is retained.
// GameWindow uses IsWaitingForRemoteInput() to keep an incomplete frame out
// of the backbuffer, preventing stale/partial HUD composites while a packet
// is in flight.
bool ShouldCaptureLocalInput();

// Produces the same P1/P2 input pair on both peers. Network play samples one
// local controller on each machine; local co-op uses both local inputs. A
// false result means the logical frame is still pending and simulation must
// not advance.
bool SynchronizeInputs(
    const u16 localPlayers[TH07_MULTI_MAX_PLAYERS], u16 rngSeed,
    u16 synchronizedPlayers[TH07_MULTI_MAX_PLAYERS]);
} // namespace Netplay

#include "Netplay.hpp"

#include <winsock2.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "GameErrorContext.hpp"
#include "GameManager.hpp"
#include "Gui.hpp"
#include "Chain.hpp"
#include "AsciiManager.hpp"
#include "AnmManager.hpp"
#include "EnemyManager.hpp"
#include "EffectManager.hpp"
#include "Player.hpp"
#include "Stage.hpp"
#include "BulletManager.hpp"
#include "Controller.hpp"
#include "GameWindow.hpp"
#include "ItemManager.hpp"
#include "LowLatency.hpp"
#include "MainMenu.hpp"
#include "Rng.hpp"
#include "ScreenEffect.hpp"
#include "SoundPlayer.hpp"

#pragma comment(lib, "ws2_32.lib")

namespace
{
const u32 NETPLAY_MAGIC = 0x374e4854; // "THN7"
// 27: verification records carry per-section hashes instead of one
// player/world composite, so a divergence names its subsystem.
const u16 NETPLAY_VERSION = 27;
const u16 NETPLAY_FLAG_INVINCIBLE = 1;
const u16 NETPLAY_FLAG_NO_DEMO = 2;
const u16 NETPLAY_FLAG_QUICK_START = 4;
const u16 NETPLAY_FLAG_AUTO_SHOOT = 8;
const u16 NETPLAY_FLAG_AUTO_SKIP = 16;
const u16 NETPLAY_FLAG_NO_SAVE = 32;
const u16 NETPLAY_FLAG_AUTO_BOMB = 64;
const u16 NETPLAY_FLAG_NO_CONTROLLER = 128;
const u16 NETPLAY_FLAG_P2_LOADOUT = 256;
const u16 NETPLAY_FLAG_ROLLBACK = 512;
const u16 NETPLAY_FLAG_TEST_DAMAGE_EVENTS = 1024;
// The native launcher has a second start barrier after HELLO/WELCOME. A
// command-line Guest must wait for that barrier when the Host was launched
// through the GUI, but old CLI Host/Guest pairs must continue to start
// immediately after WELCOME.
const u16 NETPLAY_FLAG_CONNECTION_UI = 2048;
const int DEFAULT_PORT = 35000;
// One frame is enough for the normal Tailscale/LAN path and removes one
// full 60 Hz frame of input latency from the old default. Users can still
// raise this with --delay or the connection UI when the route is unstable.
const int DEFAULT_DELAY = 1;
const int DEFAULT_CONNECT_TIMEOUT_SECONDS = 120;
// Keep enough redundant history to recover when one PC finishes a stage load
// later than the other. The prediction window can put the fast peer fourteen
// frames ahead; 32 records still fit comfortably below a normal UDP MTU and
// include the exact frame the slower peer needs to resume lockstep.
const int REDUNDANT_INPUT_COUNT = 32;
// A guest's own Host-authoritative echo is also used to correct an absence
// gap. Match the complete rollback history so all promoted predictions fit
// in one relay packet; the resulting packet remains well below 1200 bytes.
const int RELAY_INPUT_COUNT = 24;
// Must cover DETAILED_STATE_CONFIRM_LAG. Two records only span the newest two
// frames, but under load the simulation advances more than two frames between
// sends, so the verification stream had gaps and the frame being compared
// (always CONFIRM_LAG behind) almost never had a remote sample. Measured on a
// 120 ms three-player run: 2084 of 2087 comparison attempts were skipped for
// exactly that reason. Sizing this to the lag makes the coverage contiguous.
const int VERIFICATION_RECORD_COUNT = 18;
const int INPUT_RING_SIZE = 512;
// Detailed hashes are diagnostic-only and are compared after every possible
// predicted input in the rollback tail has been confirmed and replayed. Keep
// this below REDUNDANT_INPUT_COUNT so a corrected record is still resent.
const int DETAILED_STATE_CONFIRM_LAG = 16;
// A peer stamps its hashes and seed into the packet it is sending now, so
// the number the other side holds for frame F is whatever the sender
// believed when it last sent F - a prediction, if a rollback corrected F
// afterwards. Comparing that against a locally replayed frame reports a
// divergence between two peers whose state agrees. Measured: three peers
// whose player position, velocity, timers, lives, bombs, power and cherry
// were byte-identical on all 2955 shared frames still produced a
// "persistent state divergence ... body 1 shot 1" report.
//
// Comparing this far back instead means the newest packet describing the
// frame was sent after the sender confirmed it, and has had time to
// arrive. It has to clear the rollback reach (24 frames) as well as the
// confirmation lag.
const int DETAILED_STATE_COMPARE_LAG = 40;
// Boss HP sampling period, in synchronized frames. TH07's error buffer is a
// fixed 8 KiB, so a full boss fight has to stay well under a hundred lines.
const int BOSS_TRACE_INTERVAL = 180;
const int BOSS_TRACE_SLOT_COUNT = 8;
// Synchronized-frame period for the normal-play logical state comparison.
const int STATE_VERIFY_INTERVAL = 30;
// Frames between automated menu presses. The screens animate between steps
// and treat a held button as a repeat, so the lane has to return to released.
const int TEST_MENU_INPUT_INTERVAL = 10;
// Section-hash trace period, in synchronized frames.
// How far behind the current frame a pending comparison may fall before its
// remote sample is treated as lost. Must stay well under INPUT_RING_SIZE so
// the ring slot is still the frame we think it is.
const int DETAILED_STATE_ABANDON_LAG = 120;
// TH06 stops waiting for a missing delayed input after five seconds.
const DWORD REMOTE_INPUT_TIMEOUT_MS = 5000;
// The launcher hand-off only aligns when each process starts loading TH07.
// Direct3D, game-data, and rollback-snapshot initialization can take much
// longer on a PC starting two Guests at once. Keep the exact frame-0 barrier
// alive long enough for every runtime to finish loading; normal gameplay still
// uses the five-second disconnect timeout above.
const DWORD STARTUP_INPUT_TIMEOUT_MS = 30000;
// Start the absence announcement before the 22-frame prediction history is
// exhausted. Its twelve-frame future boundary lands at roughly the specified
// 30 missing frames while the remaining peers keep simulating continuously.
const DWORD TEMPORARY_ABSENCE_DETECT_MS = 300;
const u32 TEMPORARY_ABSENCE_LEAD_FRAMES = 12;
const u32 PLAYER_RESUME_LEAD_FRAMES = 12;
const u32 PLAYER_LEFT_LEAD_FRAMES = 30;
const DWORD LIFECYCLE_ANNOUNCE_INTERVAL_MS = 100;
// Fixed-delay mode gets a very short grace poll. Predictive rollback never
// enters this wait: it must sample and simulate immediately or it quietly
// turns into another full frame of input latency.
const DWORD REMOTE_INPUT_POLL_BUDGET_MS = 2;
// Keep sparse keyframes instead of copying TH07's 16 MiB fixed manager pools
// every simulation frame. Twelve two-frame keyframes cover 24 frames
// (400 ms), which is needed when guest-to-guest input traverses two links.
const int ROLLBACK_SNAPSHOT_COUNT = 12;
const int ROLLBACK_SNAPSHOT_INTERVAL = 2;
const int ROLLBACK_HISTORY_FRAMES =
    ROLLBACK_SNAPSHOT_COUNT * ROLLBACK_SNAPSHOT_INTERVAL;
const int ROLLBACK_MAX_PREDICTION_FRAMES =
    ROLLBACK_HISTORY_FRAMES - ROLLBACK_SNAPSHOT_INTERVAL;
// Three simultaneous bombs can exceed the old two-player ceiling of 64.
const int ROLLBACK_BOMB_EFFECT_SNAPSHOT_COUNT = 128;
// Direction/focus/shot are held from the latest confirmed sample. Bomb is an
// edge and is therefore predicted released; a real remote bomb rewinds from
// the keyframe just before it. SoundPlayer's existing per-frame queue
// de-duplication prevents repeated one-shot requests in the replay burst.
const u16 ROLLBACK_PREDICTABLE_INPUTS =
    TH_BUTTON_DIRECTION | TH_BUTTON_FOCUS | TH_BUTTON_SHOOT |
    TH_BUTTON_SKIP;
// These buttons change the shared game UI rather than either player's
// gameplay lane.  Treat an edge from either peer as P1 input on both peers so
// a guest can open/close the pause menu just like the host.
const u16 SHARED_UI_INPUTS =
    TH_BUTTON_MENU | TH_BUTTON_Q | TH_BUTTON_RESET;
// InputRecord::control otherwise contains only the small InGameControl enum.
// Carry each peer's pre-step gameplay readiness in its unused high bit so
// predictive rollback starts on one common, exact-input frame after loading.
const u16 INPUT_RECORD_ROLLBACK_GAMEPLAY = 0x8000;
const u16 INPUT_RECORD_CONTROL_MASK = 0x7fff;
const DWORD PING_INTERVAL_MS = 1000;
const u32 INVALID_FRAME = 0xffffffff;
const i32 TH07_SUPERVISOR_GAME_STATE = 2;
const int PLAYER_NAME_MAX_CHARS = 10;
const int PLAYER_NAME_LENGTH = 24;

enum DisplayMode
{
    DISPLAY_MODE_FROM_GAME_CONFIG = -1,
    DISPLAY_MODE_FULLSCREEN_640 = 0,
    DISPLAY_MODE_WINDOW_640 = 1,
    DISPLAY_MODE_WINDOW_960 = 2,
    DISPLAY_MODE_WINDOW_1280 = 3
};

enum PacketType
{
    PACKET_HELLO = 1,
    PACKET_WELCOME = 2,
    PACKET_INPUT = 3,
    PACKET_PING = 4,
    PACKET_PONG = 5,
    PACKET_CONTROL = 6
};

enum ControlType
{
    CONTROL_RESYNC_REQUEST = 1,
    CONTROL_RESYNC_ACK = 2,
    CONTROL_QUICK_START_READY = 3,
    CONTROL_QUICK_START = 4,
    CONTROL_START_GAME = 5,
    CONTROL_START_GAME_ACK = 6,
    CONTROL_START_GAME_COMMIT = 7,
    CONTROL_START_GAME_COMMIT_ACK = 8,
    CONTROL_PEER_EXIT = 9,
    CONTROL_PLAYER_LIFECYCLE = 10
};

enum HostPeerLifecycleStage
{
    HOST_PEER_PRESENT = 0,
    HOST_PEER_ABSENCE_PENDING,
    HOST_PEER_ABSENT,
    HOST_PEER_RESUME_PENDING,
    HOST_PEER_LEFT_PENDING,
    HOST_PEER_LEFT
};

#pragma pack(push, 1)
struct InputRecord
{
    u32 frame;
    u16 input;
    u16 rngSeed;
    u16 control;
};

struct VerificationRecord
{
    u32 frame;
    u32 stateHash;
    // Section hashes rather than one player/world composite. A composite only
    // says that something differs; locating the divergence then needs another
    // build and another reproduction. These name the subsystem directly.
    u32 bodyHash;
    u32 shotHash;
    u32 enemyHash;
    u32 bulletHash;
    u32 itemHash;
    u32 spellHash;
};

struct RelayInputRecord
{
    u32 frame;
    u16 input;
    u16 control;
};

struct NetPacket
{
    u32 magic;
    u16 version;
    u16 type;
    u32 session;
    u32 newestFrame;
    u16 delay;
    u16 initialRngSeed;
    u16 flags;
    u8 quickDifficulty;
    u8 quickCharacter;
    u8 quickShot;
    u8 quickStage;
    u8 quickPractice;
    u8 quickCharacter2;
    u8 quickShot2;
    u8 quickCharacter3;
    u8 quickShot3;
    u8 sourceSlot;
    u8 playerCount;
    u8 assignedSlot;
    u8 connectedPlayerMask;
    u8 relaySlotCount;
    u8 relaySlots[TH07_MULTI_MAX_GUESTS];
    char playerName[PLAYER_NAME_LENGTH];
    char playerNames[TH07_MULTI_MAX_PLAYERS][PLAYER_NAME_LENGTH];
    u32 sendTick;
    u32 echoTick;
    u16 controlType;
    u16 controlValue;
    u32 controlFrame;
    InputRecord records[REDUNDANT_INPUT_COUNT];
    VerificationRecord verifications[VERIFICATION_RECORD_COUNT];
    RelayInputRecord relayRecords[TH07_MULTI_MAX_GUESTS]
                                 [RELAY_INPUT_COUNT];
};
#pragma pack(pop)

Netplay::Mode g_mode = Netplay::MODE_SINGLE;
int g_playerCount = 1;
int g_localPlayerSlot = 0;
u8 g_absentPlayerMask = 0;
u8 g_departedPlayerMask = 0;
u8 g_connectedPlayerMask = 1;
DWORD g_lastPeerInputAdvanceTick[TH07_MULTI_MAX_PLAYERS] = {0, 0, 0};
u32 g_lastPeerInputFrame[TH07_MULTI_MAX_PLAYERS] = {
    INVALID_FRAME, INVALID_FRAME, INVALID_FRAME};
u32 g_peerSyntheticThroughFrame[TH07_MULTI_MAX_PLAYERS] = {
    INVALID_FRAME, INVALID_FRAME, INVALID_FRAME};
u32 g_peerNeutralStartFrame[TH07_MULTI_MAX_PLAYERS] = {
    INVALID_FRAME, INVALID_FRAME, INVALID_FRAME};
u32 g_peerResumeFrame[TH07_MULTI_MAX_PLAYERS] = {
    INVALID_FRAME, INVALID_FRAME, INVALID_FRAME};
u32 g_peerLeftFrame[TH07_MULTI_MAX_PLAYERS] = {
    INVALID_FRAME, INVALID_FRAME, INVALID_FRAME};
DWORD g_lastLifecycleAnnounceTick[TH07_MULTI_MAX_PLAYERS] = {0, 0, 0};
u8 g_hostPeerLifecycleStage[TH07_MULTI_MAX_PLAYERS] = {
    HOST_PEER_PRESENT, HOST_PEER_PRESENT, HOST_PEER_PRESENT};
u8 g_peerExitNoticeMask = 0;
SOCKET g_socket = INVALID_SOCKET;
sockaddr_in g_peerAddresses[TH07_MULTI_MAX_PLAYERS];
bool g_peerPresent[TH07_MULTI_MAX_PLAYERS] = {false, false, false};
bool g_connected = false;
bool g_peerExitReceived = false;
bool g_winsockStarted = false;
bool g_invincible = false;
// Per-PC display preferences. Neither changes simulation state, so they
// are never exchanged: two peers may disagree about them all session.
bool g_showStagePlayerNames = true;
bool g_showNetDiagnostics = false;
bool g_demoDisabled = false;
bool g_quickStartEnabled = false;
bool g_quickStartPending = false;
bool g_quickStartPractice = false;
bool g_quickStartReadySent = false;
bool g_quickStartRemoteReady = false;
u8 g_quickStartReadyMask = 0;
u32 g_quickStartFrame = INVALID_FRAME;
DWORD g_lastQuickStartSendTick = 0;
bool g_autoShoot = false;
bool g_autoSkip = false;
bool g_autoBomb = false;
bool g_testRollbackInputEnabled = false;
bool g_testRollbackBorderActivated = false;
bool g_testRollbackP1BorderVerified = false;
bool g_testRollbackP2BorderVerified = false;
u32 g_testRollbackBorderStartFrame = INVALID_FRAME;
u32 g_testDeferredPollFrame = INVALID_FRAME;
u32 g_testDeferredPollFrameCount = 0;
bool g_testRandomInputEnabled = false;
bool g_testEvasiveInputEnabled = false;
// Drives the title and loadout screens. Every other test driver deliberately
// refuses to write menu bits so a background window cannot steer a match, so
// this is the only one allowed to, and only when explicitly requested. Without
// it the interactive P2/P3 selection sequence has no automated coverage at all
// - which is how a crash in P3's bomb cut-in reached the real line.
bool g_testMenuInputEnabled = false;
u32 g_testMenuInputFrame = 0;
i32 g_testMenuLastState = -1;
i32 g_testMenuConfirmCount = 0;
bool g_testResourceDropsEnabled = false;
bool g_testResourceDropsVerified = false;
bool g_testP2FeaturesEnabled = false;
bool g_testP2FeaturesSetup = false;
bool g_testP3FeaturesEnabled = false;
bool g_testP3FeaturesSetup = false;
bool g_testHashTraceEnabled = false;
FILE *g_testHashTraceFile = NULL;
// Diagnostics that must survive a crash or a force-kill go here as well as
// into the error context. log.txt is written once, during a normal shutdown,
// out of an 8 KiB buffer that recycles.
FILE *g_netplayTraceFile = NULL;
bool g_netplayTraceFailed = false;
u32 g_inputTraceCount = 0;
u32 g_sharedLaneTraceCount = 0;
bool g_ignoreControllerInput = false;
bool g_localIgnoreControllerInput = false;
bool g_forceWindowed = false;
int g_displayMode = DISPLAY_MODE_FROM_GAME_CONFIG;
bool g_connectionUiLaunchMode = false;
bool g_cliGuestStartBarrierEligible = false;
bool g_audioPreferenceActive = false;
bool g_bgmEnabled = true;
bool g_seEnabled = true;
bool g_startupCancelled = false;
bool g_noSave = false;
bool g_localNoSave = false;
bool g_seedConfigured = false;
u16 g_configuredSeed = 1;
int g_quickDifficulty = 1;
int g_quickCharacter = 0;
int g_quickShot = 0;
int g_quickStage = 1;
int g_p2Character = -1;
int g_p2Shot = -1;
int g_p3Character = -1;
int g_p3Shot = -1;
bool g_p2LoadoutConfigured = false;
bool g_p2LoadoutSelected = false;
bool g_p3LoadoutConfigured = false;
bool g_p3LoadoutSelected = false;
int g_testSeconds = 0;
int g_autoBombPulseCount = 0;
u32 g_synchronizedBombPulseCount = 0;
bool g_testRandomInputLogged = false;
int g_testRandomInputBombPulseCount = 0;
bool g_testEvasiveInputLogged = false;
int g_testEvasiveInputBombPulseCount = 0;
int g_lastLoggedTestStage = 0;
int g_connectTimeoutSeconds = DEFAULT_CONNECT_TIMEOUT_SECONDS;
DWORD g_testStartedTick = 0;
u32 g_session = 0;
u32 g_frame = 0;
int g_delay = DEFAULT_DELAY;
bool g_rollbackEnabled = false;
bool g_rollbackEverEnabled = false;
bool g_rollbackReplaying = false;
bool g_rollbackPredictionActive = false;
bool g_lowLatencyEnabled = true;
bool g_lowLatencyExplicit = false;
bool g_lowLatencySpinWait = false;
bool g_lowLatencyDwmFlush = false;
int g_lowLatencyPrepareTimeMs = 1;
u16 g_initialRngSeed = 0;
u16 g_localInputs[INPUT_RING_SIZE];
u16 g_localRng[INPUT_RING_SIZE];
u16 g_localControls[INPUT_RING_SIZE];
u16 g_scheduledLifecycleControls[INPUT_RING_SIZE];
u32 g_scheduledLifecycleFrames[INPUT_RING_SIZE];
u32 g_localStateHash[INPUT_RING_SIZE];
u32 g_localPlayerHash[INPUT_RING_SIZE];
u32 g_localWorldHash[INPUT_RING_SIZE];
u32 g_localSpellHash[INPUT_RING_SIZE];
u32 g_localFrames[INPUT_RING_SIZE];
u8 g_localRollbackGameplay[INPUT_RING_SIZE];
u16 g_remoteInputsByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u16 g_remoteRngByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u16 g_remoteControlsByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u32 g_remoteStateHashByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u32 g_remotePlayerHashByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u32 g_remoteWorldHashByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u32 g_remoteBodyHashByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u32 g_remoteShotHashByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u32 g_remoteEnemyHashByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u32 g_remoteBulletHashByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u32 g_remoteItemHashByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u32 g_remoteSpellHashByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u32 g_remoteFramesByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u8 g_remoteRollbackGameplayByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u16 g_predictedRemoteInputsByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u16 g_predictedRemoteControlsByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u32 g_predictedRemoteFramesByPlayer[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u32 g_rollbackEarliestFrame = INVALID_FRAME;
u32 g_rollbackCount = 0;
u32 g_rollbackPredictedFrames = 0;
u32 g_rollbackReplayFrames = 0;
u32 g_rollbackMaxReplayFrames = 0;
u32 g_rollbackReplayTimeUs = 0;
u32 g_rollbackMaxReplayTimeUs = 0;
u32 g_rollbackRestoredBombEffects = 0;
u32 g_rollbackMaxBombEffects = 0;
u32 g_rollbackPredictionRefreshFrames = 0;
u32 g_rollbackGameplayFrames = 0;
DWORD g_rollbackGameplayStartedTick = 0;
i32 g_lastRollbackStateSignature = -1;
DWORD g_waitStartedTick = 0;
DWORD g_waitStatusTick = 0;
DWORD g_lastInputSendTick = 0;
u32 g_inputStallCount = 0;
u32 g_inputStallTotalMs = 0;
u32 g_inputStallMaxMs = 0;
u32 g_gameplayInputStallCount = 0;
u32 g_gameplayInputStallTotalMs = 0;
u32 g_gameplayInputStallMaxMs = 0;
bool g_inputTimingSummaryLogged = false;
bool g_rollbackTimingSummaryLogged = false;
bool g_controllerLaneLogged = false;
bool g_controllerConfigLogged = false;
bool g_rollbackMemoryLogged = false;
bool g_startupFrameBarrierLogged = false;
u32 g_lastDetailedStateComparedFrame = INVALID_FRAME;
u32 g_lastRollbackRngComparedFrame = INVALID_FRAME;
u32 g_lifecycleComparisonIgnoreUntilFrame = INVALID_FRAME;
bool g_detailedStateMismatch = false;
u32 g_firstDetailedStateMismatchFrame = INVALID_FRAME;
i32 g_lastLoggedSpellActive = -1;
i32 g_lastLoggedSpellState = -1;
i32 g_lastLoggedSpellIndex = -1;
// Boss life is synchronized state, but nothing compares it outside a
// --test-seconds run, so a divergence can survive an entire session
// unreported. These samples run in normal play too and are keyed on the
// synchronized simulation frame, so the three peer logs line up directly.
u32 g_lastBossTraceFrame = INVALID_FRAME;
// Every boss sample is folded in here, but only a periodic subset is logged.
// The periodic lines are recyclable, so on a long run they are gone by the
// end - which is exactly what happened to the one measurement that could
// have answered whether a divergence reached boss state. The accumulator
// rides in the non-discardable end-of-run summary instead, so it survives
// regardless of how long the session ran.
// Stage the rollback snapshots were taken in. Stage::objects holds pointers
// into the loaded STD data, and the snapshot stores those pointers as raw
// bytes. A stage change frees and reloads that data, so restoring a snapshot
// from the previous stage walks freed memory - observed as a crash in
// Stage::UpdateObjects at the objects[i]->flags read.
i32 g_rollbackSnapshotStage = -1;
u32 g_rollbackStageInvalidations = 0;
u32 g_bossTraceAccumulator = 0;
u32 g_bossTraceSamples = 0;
// The final accumulator alone is not comparable across peers: whichever peer
// shuts down first stops sampling first, so one side can hold a sample the
// other never took. Same state, different totals - indistinguishable from a
// real divergence. Keep the last few (frame, accumulator) pairs so the
// comparison can be made on frames both peers actually reached.
const int BOSS_CHECKPOINT_COUNT = 6;
u32 g_bossCheckpointFrames[BOSS_CHECKPOINT_COUNT];
u32 g_bossCheckpointAccumulators[BOSS_CHECKPOINT_COUNT];
// A sample is held here until the frame it was taken on is confirmed. Folding
// live state instead compares predictions: each peer predicts the remote inputs
// differently, so the boss takes a slightly different amount of damage until
// the correction arrives. That heals on the next rollback, but the accumulator
// had already eaten the predicted value, so identical peers reported different
// totals. Replays rewrite this entry, so the folded value is the corrected one.
u32 g_bossPendingSampleFrame = INVALID_FRAME;
i32 g_bossPendingBossCount = 0;
i32 g_bossPendingLifeSum = 0;
i32 g_bossPendingLife0 = -1;
i32 g_bossPendingMaxLife0 = -1;
// The spell section splits four ways, sampled on the same confirmed frames as
// the boss state. Reading them once at the moment a divergence is first
// reported does not work: each peer reports on its own frame, so the values
// are not comparable - the same mistake the boss accumulator started with.
// Three samples span 540 frames, which turned out to be far less than the gap
// between a divergence starting and being noticed: in the first real-line
// capture all four sub-hashes already differed at the oldest sample, so the
// series could not show where they parted.
const int SPELL_CHECKPOINT_COUNT = 12;
u32 g_spellCheckpointCount = 0;
u32 g_spellPendingSubHash[4] = {0, 0, 0, 0};
u32 g_spellCheckpointFrames[SPELL_CHECKPOINT_COUNT];
u32 g_spellCheckpointSubHash[SPELL_CHECKPOINT_COUNT][4];
u32 CalculateSpellInfoSubHash();
u32 CalculateSpellStageSubHash();
u32 CalculateSpellBossSubHash();
u32 CalculateSpellEclSubHash();
// Section hashes captured alongside the transmitted ones, so the trace can
// report a frame that every peer has already confirmed instead of live state
// that is still partly predicted.
u32 g_traceBodyHash[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u32 g_traceShotHash[TH07_MULTI_MAX_PLAYERS][INPUT_RING_SIZE];
u32 g_localBodyHash[INPUT_RING_SIZE];
u32 g_localShotHash[INPUT_RING_SIZE];
u32 g_traceEnemyHash[INPUT_RING_SIZE];
u32 g_traceBulletHash[INPUT_RING_SIZE];
u32 g_traceItemHash[INPUT_RING_SIZE];
u8 g_bossDeathLogged = 0;
// One-shot guards so the rollback failure paths are reported without filling
// TH07's fixed 8 KiB error buffer.
bool g_rollbackDeferralLogged = false;
bool g_rollbackWindowExhaustedLogged = false;
bool g_rollbackSnapshotMissingLogged = false;
bool g_simulationDivergenceLogged = false;
// Why the detailed state comparison did or did not run. Two rounds of guessing
// failed to explain zero comparisons in a high-latency session, so count every
// exit instead of inferring it.
u32 g_dsSkipGate = 0;
u32 g_dsSkipHorizon = 0;
u32 g_dsSkipLocalSlot = 0;
u32 g_dsSkipIgnore = 0;
u32 g_dsSkipZeroHash = 0;
u32 g_dsSkipRemote = 0;
u32 g_dsCompared = 0;
// A divergence that repairs itself was never a divergence. Predicted frames
// are hashed before the correcting replay arrives, so an isolated frame or two
// differs routinely; only a difference that survives consecutive confirmed
// comparisons means the simulations have actually parted.
const int STATE_MISMATCH_STREAK = 4;
u32 g_dsStreak[TH07_MULTI_MAX_PLAYERS];
u32 g_dsStreakFirstFrame[TH07_MULTI_MAX_PLAYERS];
u32 g_dsTransientCount = 0;
// Relayed guest-to-guest input travels two hops, and every rejection on that
// path is currently silent. Count each one: a dropped relayed correction
// leaves the receiving peer simulating a prediction it never revisits.
u32 g_relayDroppedWindow = 0;
u32 g_relayNoPrediction = 0;
u32 g_relayCorrections = 0;
u32 g_relayAccepted = 0;
u32 g_relayRedundant = 0;
u32 g_relayMaxLag = 0;
u16 g_lastLoggedControllerInput = 0;
u32 g_controllerInputLogCount = 0;
DWORD g_lastPingTick = 0;
DWORD g_lastRoundTripMs = 0;
DWORD g_lastRoundTripMsByPlayer[TH07_MULTI_MAX_PLAYERS] = {0, 0, 0};
u32 g_pingSequence = 0;
bool g_waitingForRemoteInput = false;
bool g_connectionFailed = false;
bool g_rngMismatch = false;
bool g_protocolMismatch = false;
bool g_reconnectKeyDown = false;
bool g_reconnectTestPending = false;
bool g_controlTestEnabled = false;
u32 g_controlTestFrame = 0;
bool g_testRngMismatchEnabled = false;
bool g_testRngMismatchInjected = false;
bool g_testStateMismatchEnabled = false;
bool g_testStateMismatchInjected = false;
// Positive control for the boss measurement. Without one, "the peers agree"
// is unfalsifiable: a metric that has quietly stopped tracking boss state
// reports agreement too, which is how the predicted-state sampling survived
// as long as it did.
bool g_testBossDesyncEnabled = false;
bool g_testBossDesyncLogged = false;
const int TEST_BOSS_DESYNC_INTERVAL = 600;
// Disables the stage-boundary snapshot invalidation so its effect can be
// measured against the same binary and the same instrument.
bool g_testKeepStaleStageSnapshots = false;
bool g_testResultReconnectEnabled = false;
u32 g_testResultPolicyFrames = 0;
bool g_testReplayBlockEnabled = false;
bool g_testReplayBlockInjected = false;
bool g_testUiSyncEnabled = false;
bool g_testUiSyncInjected = false;
bool g_testUiSyncVerified = false;
bool g_testUiSyncFailureReported = false;
bool g_testUiSyncUiFrame = false;
bool g_testInputSyncEnabled = false;
bool g_testInputSyncInjected = false;
bool g_testInputSyncVerified = false;
bool g_testInputSyncFailureReported = false;
u32 g_testInputSyncLocalFrame = 0;
bool g_testProximityEnabled = false;
bool g_testProximityVerified = false;
bool g_testProximityFailureReported = false;
bool g_testLifeTransferEnabled = false;
bool g_testLifeTransferVerified = false;
bool g_testLifeTransferFailureReported = false;
bool g_testDamageEventsEnabled = false;
u32 g_testDamageHitCount[2] = {0, 0};
bool g_testDamageSpiritSeen = false;
bool g_testDamageReviveSeen = false;
bool g_testDamageSummaryLogged = false;
bool g_testDamageFailureReported = false;
bool g_localP2KeyBindingsLoaded = false;
int g_localP2KeyUp = 'I';
int g_localP2KeyDown = 'K';
int g_localP2KeyLeft = 'J';
int g_localP2KeyRight = 'L';
int g_localP2KeyShoot = 'F';
int g_localP2KeyBomb = 'G';
int g_localP2KeyFocus = 'D';
bool g_inputArmed = false;
WNDPROC g_previousNetworkWindowProc = NULL;
HWND g_networkHookWindow = NULL;
u16 g_previousControlKeys = 0;
Netplay::InGameControl g_synchronizedControl =
    Netplay::INGAME_CONTROL_NONE;
bool g_insaneMode = false;
u8 g_finalResourceBonusAppliedMask = 0;
u32 g_resyncFrame = INVALID_FRAME;
u32 g_resyncIgnoreUntilFrame = INVALID_FRAME;
DWORD g_lastResyncSendTick = 0;
u8 g_resyncAwaitingAckMask = 0;
bool g_resultDetached = false;
bool g_resultReconnectAttempted = false;
ChainElem g_networkDrawGuardChain;
bool g_networkDrawGuardRegistered = false;
ChainElem g_player2LifeDrawChain;
bool g_player2LifeDrawChainRegistered = false;
ChainElem g_loadoutSelectionCalcChain;
ChainElem g_loadoutSelectionDrawChain;
bool g_loadoutSelectionChainsRegistered = false;
int g_selectingLoadoutPlayerId = -1;
int g_interactiveP1Character = 0;
int g_interactiveP1Shot = 0;
bool g_interactiveGameSeen = false;
bool g_interactiveTitleResetDone = false;
char g_localPlayerName[PLAYER_NAME_LENGTH] = "Player";
char g_remotePlayerName[PLAYER_NAME_LENGTH] = "Player2";
char g_player3Name[PLAYER_NAME_LENGTH] = "Player3";
char g_sessionPlayerNames[TH07_MULTI_MAX_PLAYERS][PLAYER_NAME_LENGTH] =
    {"Player", "Player2", "Player3"};
char g_status[160] = "single player";

int GetPrimaryRemotePlayerId()
{
    return g_mode == Netplay::MODE_GUEST ? 0 : 1;
}

// Keep the established two-player code readable while the storage itself is
// now indexed by absolute player slot. Three-player paths use the explicit
// *ByPlayer arrays and iterate every remote slot.
#define g_peer g_peerAddresses[GetPrimaryRemotePlayerId()]
#define g_hasPeer g_peerPresent[GetPrimaryRemotePlayerId()]
#define g_remoteInputs \
    g_remoteInputsByPlayer[GetPrimaryRemotePlayerId()]
#define g_remoteRng \
    g_remoteRngByPlayer[GetPrimaryRemotePlayerId()]
#define g_remoteControls \
    g_remoteControlsByPlayer[GetPrimaryRemotePlayerId()]
#define g_remoteStateHash \
    g_remoteStateHashByPlayer[GetPrimaryRemotePlayerId()]
#define g_remotePlayerHash \
    g_remotePlayerHashByPlayer[GetPrimaryRemotePlayerId()]
#define g_remoteWorldHash \
    g_remoteWorldHashByPlayer[GetPrimaryRemotePlayerId()]
#define g_remoteSpellHash \
    g_remoteSpellHashByPlayer[GetPrimaryRemotePlayerId()]
#define g_remoteFrames \
    g_remoteFramesByPlayer[GetPrimaryRemotePlayerId()]
#define g_remoteRollbackGameplay \
    g_remoteRollbackGameplayByPlayer[GetPrimaryRemotePlayerId()]
#define g_predictedRemoteInputs \
    g_predictedRemoteInputsByPlayer[GetPrimaryRemotePlayerId()]
#define g_predictedRemoteControls \
    g_predictedRemoteControlsByPlayer[GetPrimaryRemotePlayerId()]
#define g_predictedRemoteFrames \
    g_predictedRemoteFramesByPlayer[GetPrimaryRemotePlayerId()]

bool SameAddress(const sockaddr_in &a, const sockaddr_in &b);

bool IsExpectedRemotePlayerId(int playerId)
{
    if (playerId < 0 || playerId >= g_playerCount ||
        playerId == g_localPlayerSlot ||
        (g_departedPlayerMask & (1 << playerId)) != 0)
    {
        return false;
    }
    // Before player-chain construction the active flags are not populated.
    // Once gameplay is running, a deterministic Stage 2 boundary removes the
    // slot from input requirements on the same frame on every peer.
    if (g_GameManager.globals &&
        g_Supervisor.curState == TH07_SUPERVISOR_GAME_STATE &&
        !IsPlayerSlotActive((u8)playerId))
    {
        return false;
    }
    return true;
}

int FindPeerPlayerId(const sockaddr_in &address)
{
    int playerId;
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        if (g_peerPresent[playerId] &&
            SameAddress(address, g_peerAddresses[playerId]))
        {
            return playerId;
        }
    }
    return -1;
}

int CountConnectedGuestPeers()
{
    int count = 0;
    int playerId;
    for (playerId = 1; playerId < g_playerCount; playerId++)
    {
        if (g_peerPresent[playerId])
        {
            count++;
        }
    }
    return count;
}

bool AreAllExpectedPeersConnected()
{
    if (g_mode == Netplay::MODE_HOST)
    {
        return CountConnectedGuestPeers() == g_playerCount - 1;
    }
    if (g_mode == Netplay::MODE_GUEST)
    {
        return g_peerPresent[0] &&
            g_connectedPlayerMask == (u8)((1 << g_playerCount) - 1);
    }
    return true;
}

u8 GetExpectedGuestMask()
{
    return (u8)(((1 << g_playerCount) - 1) & ~1);
}

// These objects are fixed-size game state pools. The byte arrays avoid
// running their constructors for every snapshot while retaining the exact
// in-process pointer values stored by the original game.
struct RollbackSnapshot
{
    u32 simulationFrame;
    u32 chainSignature;
    u8 hasGlobals;
    u8 hasDefaultConfig;
    u8 hasGuiImpl;
    u8 reserved;
    void *globalsAddress;
    void *defaultConfigAddress;
    void *guiImplAddress;
    int delay;
    u8 inputArmed;
    u16 previousControlKeys;
    u16 synchronizedControl;
    u8 insaneMode;
    u8 padding[3];
    u8 gameManager[sizeof(GameManager)];
    u8 defaultConfig[sizeof(GameConfiguration)];
    u8 globals[sizeof(ZunGlobals)];
    u8 rng[sizeof(Rng)];
    u8 multiplayerResources[sizeof(g_MultiplayerPlayerResources)];
    u8 players[TH07_MULTI_MAX_PLAYERS][sizeof(Player)];
    u8 enemyManager[sizeof(EnemyManager)];
    u8 bulletManager[sizeof(BulletManager)];
    u8 itemManager[sizeof(ItemManager)];
    u8 effectManager[sizeof(EffectManager)];
    u8 stage[sizeof(Stage)];
    u8 gui[sizeof(Gui)];
    u8 guiImpl[sizeof(GuiImpl)];
    u8 asciiManager[sizeof(AsciiManager)];
    u8 supervisor[sizeof(Supervisor)];
    u8 globalEclVars[sizeof(EclGlobalVars)];
    u8 activePlayerMask;
    u8 absentPlayerMask;
    u8 departedPlayerMask;
    u8 playerMaskPadding;
    u16 curFrameRawInputs[TH07_MULTI_MAX_PLAYERS];
    u16 curFrameGameInputs[TH07_MULTI_MAX_PLAYERS];
    u16 lastFrameRawInputs[TH07_MULTI_MAX_PLAYERS];
    u16 lastFrameGameInputs[TH07_MULTI_MAX_PLAYERS];
    u16 isEighthFrameOfHeldInput;
    u16 numOfFramesInputsWereHeld;
    u32 bombEffectCount;
    u8 bombEffects[ROLLBACK_BOMB_EFFECT_SNAPSHOT_COUNT][sizeof(BombEffects)];
    u8 hasAnmOffset;
    u8 anmOffsetPadding[3];
    Float2 anmOffset;
};

static RollbackSnapshot g_rollbackSnapshots[ROLLBACK_SNAPSHOT_COUNT];

int GetRollbackSnapshotIndex(u32 simulationFrame)
{
    return (int)((simulationFrame / ROLLBACK_SNAPSHOT_INTERVAL) %
                 ROLLBACK_SNAPSHOT_COUNT);
}

bool IsRollbackSnapshotFrame(u32 simulationFrame)
{
    return simulationFrame % ROLLBACK_SNAPSHOT_INTERVAL == 0;
}

void SetStatus(const char *text);
void GetConnectionUiConfigPath(char *path, int pathSize);
void ResetPlayerLifecycleTrace();

// Writes one line to both the error context and netplay_trace.txt, flushing
// so the line is on disk before the next frame. Only low-rate diagnostics
// use this; a per-frame line would put a disk write in the frame loop.
void NetplayTraceWriteLine(const char *line)
{
    if (!g_netplayTraceFile && !g_netplayTraceFailed)
    {
        g_netplayTraceFile = fopen("netplay_trace.txt", "wb");
        if (!g_netplayTraceFile)
        {
            g_netplayTraceFailed = true;
        }
    }
    if (g_netplayTraceFile)
    {
        fputs(line, g_netplayTraceFile);
        fflush(g_netplayTraceFile);
    }
}

// Same destination, but it stays out of the error context. Use this for
// anything that can fire more than a handful of times per run.
void NetplayTraceFileBuffered(const char *line)
{
    if (!g_netplayTraceFile && !g_netplayTraceFailed)
    {
        g_netplayTraceFile = fopen("netplay_trace.txt", "wb");
        if (!g_netplayTraceFile)
        {
            g_netplayTraceFailed = true;
        }
    }
    if (g_netplayTraceFile)
    {
        fputs(line, g_netplayTraceFile);
    }
}

void NetplayTraceFile(const char *fmt, ...)
{
    char line[512];
    va_list args;

    va_start(args, fmt);
    vsprintf(line, fmt, args);
    va_end(args);
    NetplayTraceWriteLine(line);
}

void NetplayTrace(const char *fmt, ...)
{
    char line[512];
    va_list args;

    va_start(args, fmt);
    vsprintf(line, fmt, args);
    va_end(args);
    g_GameErrorContext.Log("%s", line);
    NetplayTraceWriteLine(line);
}
void ResetInputRings();
bool SameAddress(const sockaddr_in &a, const sockaddr_in &b);
bool ResolveAddress(const char *host, int port, sockaddr_in *out);
void CloseNetworkSocket();
bool CreateSocket(int port);
void InitializePacket(NetPacket *packet, PacketType type);
bool SendPacket(const NetPacket &packet);
bool SendPacketToPlayer(const NetPacket &packet, int playerId);
void SendPacketToAllPeers(const NetPacket &packet);
int AssignHostGuestSlot(const sockaddr_in &address,
                        const char *playerName);
void SendWelcomeToAllGuests(int repeatCount);
bool AcceptWelcomePacket(const NetPacket &packet, const sockaddr_in &from,
                         bool armQuickStart);
void SendPing();
void SendControl(ControlType type, u32 frame, u16 value);
void NotifyPeerExit();
void HandlePeerExit();
bool ReceivePacket(NetPacket *packet, sockaddr_in *from);
bool ValidatePacketVersion(const NetPacket &packet);
void StoreRemoteInputs(const NetPacket &packet, int remotePlayerId);
void StoreRelayedInputs(const NetPacket &packet, int relayIndex,
                        int remotePlayerId);
void UpdateHostPeerLifecycles();
void PrepareHostSyntheticInputs(u32 throughFrame);
void SendPendingLifecycleAnnouncements();
u32 ScheduleLifecycleControl(Netplay::InGameControl control, u32 frame,
                             bool announce);
Netplay::InGameControl GetScheduledLifecycleControl(u32 frame);
void ApplyHostOptions(const NetPacket &packet, bool armQuickStart);
bool IsSharedUiFrame();
bool TryGetRemoteInput(u32 frame, bool allowPrediction);
u16 PredictRemoteInput(u32 frame);
u16 PredictRemoteInputForPlayer(u32 frame, int remotePlayerId);
Netplay::InGameControl NormalizeControl(u16 value);
Netplay::InGameControl SelectSynchronizedControl(u16 local, u16 remote);
void ApplyPlayerLifecycleTransition(Netplay::InGameControl control);
void ApplySynchronizedControl(Netplay::InGameControl control);

u32 NetworkDrawGuard(void *)
{
    // A timeout clears g_waitingForRemoteInput so F8 can drive the reconnect
    // path. The simulation still has no complete next frame, however. Drawing
    // the remaining chains in that state exposes a partially updated
    // backbuffer (most visible during dense spellcards). Keep the last fully
    // rendered frame until an input arrives or reconnect resets the failure.
    bool gameplayState = g_GameManager.notInMenu &&
        g_Supervisor.curState == TH07_SUPERVISOR_GAME_STATE;
    bool freezeIncompleteFrame = gameplayState &&
        (g_waitingForRemoteInput || g_connectionFailed);
    return freezeIncompleteFrame ? CHAIN_CALLBACK_RESULT_BREAK
                                 : CHAIN_CALLBACK_RESULT_CONTINUE;
}

void DrawPlayer2ResourceIconsInternal()
{
    AnmVm *lifeVm;
    AnmVm *bombVm;
    AnmVm lifeIconVm;
    AnmVm bombIconVm;
    i32 lives;
    i32 bombs;
    i32 i;
    f32 x;

    if (!Netplay::IsMultiplayer() || !g_Player2Active ||
        g_Supervisor.curState != 2 || !g_Gui.impl)
    {
        return;
    }

    // Draw these rows beside P1's rows in Gui::DrawGameScene. Keeping both
    // players in the same ANM batch avoids the late-chain render-state seam
    // that can appear on P2's star icons after the stage starts.
    g_Gui.showLives = 2;
    g_Gui.showBombs = 2;
    g_Gui.showPower = 2;

    // Gui::DrawGameScene clears all six compact rows before drawing the
    // labels. This function runs after that label pass, so clearing here
    // would erase the right edge of P2's Player/Bomb/Power labels.

    lifeVm = &g_Gui.impl->vms0[9];
    lives = GetPlayerLives(1);
    if (lives < 0)
    {
        lives = 0;
    }
    if (lives > 8)
    {
        lives = 8;
    }
    lifeIconVm = *lifeVm;
    lifeIconVm.scale.x = 0.65f;
    lifeIconVm.scale.y = 0.65f;
    for (i = 0, x = 532.0f; i < lives; i++, x += 11.0f)
    {
        lifeIconVm.pos = D3DXVECTOR3(x, 136.0f, 0.46f);
        g_AnmManager->DrawNoRotation(&lifeIconVm);
    }

    bombVm = &g_Gui.impl->vms0[10];
    bombs = GetPlayerBombs(1);
    if (bombs < 0)
    {
        bombs = 0;
    }
    if (bombs > 8)
    {
        bombs = 8;
    }
    bombIconVm = *bombVm;
    bombIconVm.scale.x = 0.65f;
    bombIconVm.scale.y = 0.65f;
    for (i = 0, x = 532.0f; i < bombs; i++, x += 11.0f)
    {
        bombIconVm.pos = D3DXVECTOR3(x, 148.0f, 0.46f);
        g_AnmManager->DrawNoRotation(&bombIconVm);
    }
}

u32 DrawPlayer2LifeIcons(void *)
{
    static bool p2HudLogged = false;
    i32 power;
    VertexDiffuseXyzrhw powerBarVerts[4];
    Float2 savedPowerTextScale;
    D3DXVECTOR3 powerTextPosition;
    D3DCOLOR savedPowerTextColor;
    i32 savedPowerTextGui;

    if (!Netplay::IsMultiplayer() || !g_Player2Active ||
        g_Supervisor.curState != 2 || !g_Gui.impl)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }
    // Multiplayer draws one compact Power row below each player's resources.
    power = GetPlayerPower(1);
    if (power < 0)
    {
        power = 0;
    }
    if (power > 128)
    {
        power = 128;
    }
    if (power > 0)
    {
        g_AnmManager->Flush();
        powerBarVerts[0].pos = D3DXVECTOR3(532.0f, 162.0f, 0.1f);
        powerBarVerts[1].pos =
            D3DXVECTOR3(532.0f + (f32)power * 0.25f,
                        162.0f, 0.1f);
        powerBarVerts[2].pos = D3DXVECTOR3(532.0f, 172.0f, 0.1f);
        powerBarVerts[3].pos =
            D3DXVECTOR3(532.0f + (f32)power * 0.25f,
                        172.0f, 0.1f);
        powerBarVerts[0].diffuse.color =
            powerBarVerts[2].diffuse.color = 0xe0e0e0ff;
        powerBarVerts[1].diffuse.color =
            powerBarVerts[3].diffuse.color = 0x80e0e0ff;
        powerBarVerts[0].w = powerBarVerts[1].w = powerBarVerts[2].w =
            powerBarVerts[3].w = 1.0f;
        if (!g_Supervisor.cfg.disableTextureBlend)
        {
            g_Supervisor.d3dDevice->SetTextureStageState(
                0, D3DTSS_ALPHAOP, 2);
            g_Supervisor.d3dDevice->SetTextureStageState(
                0, D3DTSS_COLOROP, 2);
        }
        g_Supervisor.d3dDevice->SetTextureStageState(
            0, D3DTSS_ALPHAARG1, 0);
        g_Supervisor.d3dDevice->SetTextureStageState(
            0, D3DTSS_COLORARG1, 0);
        if (!g_Supervisor.cfg.disableZBuffer)
        {
            g_Supervisor.SetRenderState(D3DRS_ZWRITEENABLE, 0);
        }
        g_Supervisor.d3dDevice->SetVertexShader(
            D3DFVF_DIFFUSE | D3DFVF_XYZRHW);
        g_Supervisor.d3dDevice->DrawPrimitiveUP(
            D3DPT_TRIANGLESTRIP, 2, &powerBarVerts,
            sizeof(VertexDiffuseXyzrhw));
        g_AnmManager->SetVertexShader(255);
        g_AnmManager->SetColorOp(255);
        g_AnmManager->SetBlendMode(255);
        g_AnmManager->SetZWriteDisable(255);
        if (!g_Supervisor.cfg.disableTextureBlend)
        {
            g_Supervisor.d3dDevice->SetTextureStageState(
                0, D3DTSS_ALPHAOP, 4);
            g_Supervisor.d3dDevice->SetTextureStageState(
                0, D3DTSS_COLOROP, 4);
        }
        g_Supervisor.d3dDevice->SetTextureStageState(
            0, D3DTSS_ALPHAARG1, 2);
        g_Supervisor.d3dDevice->SetTextureStageState(
            0, D3DTSS_COLORARG1, 2);
    }
    powerTextPosition = D3DXVECTOR3(532.0f, 162.0f, 0.0f);
    savedPowerTextScale = g_AsciiManager.scale;
    savedPowerTextColor = g_AsciiManager.color;
    savedPowerTextGui = g_AsciiManager.isGui;
    g_AsciiManager.scale.x = 0.65f;
    g_AsciiManager.scale.y = 0.65f;
    g_AsciiManager.color = 0xffffffff;
    g_AsciiManager.isGui = 0;
    if (power < 128)
    {
        AsciiManager::AddFormatText(&g_AsciiManager, &powerTextPosition,
                                    "%d", power);
    }
    else
    {
        g_AsciiManager.AddString(&powerTextPosition, "MAX");
    }
    g_AsciiManager.scale = savedPowerTextScale;
    g_AsciiManager.color = savedPowerTextColor;
    g_AsciiManager.isGui = savedPowerTextGui;
    if (!p2HudLogged)
    {
        p2HudLogged = true;
        g_GameErrorContext.Log(
            "info : P2 HUD lives/bombs/power verified\r\n");
    }
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

MainMenu *FindActiveMainMenu()
{
    ChainElem *element = g_Chain.calcChain.next;
    while (element)
    {
        if (element->callback == (ChainCallback)MainMenu::OnUpdate)
        {
            return (MainMenu *)element->arg;
        }
        element = element->next;
    }
    return NULL;
}

bool IsCharacterSelectionState(i32 state)
{
    return state == STATE_NORMAL_SELECT_CHARACTER ||
        state == STATE_EXTRA_SELECT_CHARACTER;
}

bool IsShotSelectionState(i32 state)
{
    return state == STATE_NORMAL_SELECT_SHOTTYPE ||
        state == STATE_EXTRA_SELECT_SHOTTYPE;
}

void RestoreInteractiveP1Loadout()
{
    g_GameManager.character = (u8)g_interactiveP1Character;
    g_GameManager.shotType = (u8)g_interactiveP1Shot;
}

u32 UpdateInteractiveLoadoutSelection(void *)
{
    MainMenu *menu = FindActiveMainMenu();

    if (g_Supervisor.curState == 2)
    {
        g_interactiveGameSeen = true;
        g_interactiveTitleResetDone = false;
    }
    else if (g_Supervisor.curState == 1 && g_interactiveGameSeen &&
             !g_interactiveTitleResetDone)
    {
        Netplay::ResetInteractiveLoadout();
        g_interactiveTitleResetDone = true;
    }

    if (!menu)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    if (g_selectingLoadoutPlayerId >= 1)
    {
        if (IsCharacterSelectionState(menu->gameState) &&
            menu->menuSubState == 1 &&
            WAS_PRESSED_RAW(TH_BUTTON_RETURNMENU))
        {
            RestoreInteractiveP1Loadout();
            Netplay::CancelPlayerLoadoutSelection();
            g_selectingLoadoutPlayerId = -1;
            menu->cursor = g_interactiveP1Character;
            return CHAIN_CALLBACK_RESULT_CONTINUE;
        }
        if (IsShotSelectionState(menu->gameState) &&
            menu->menuSubState == 1 &&
            WAS_PRESSED_RAW(TH_BUTTON_SELECTMENU))
        {
            int completedPlayerId = g_selectingLoadoutPlayerId;
            int nextPlayerId;

            g_GameManager.shotType = (u8)menu->cursor;
            Netplay::CompletePlayerLoadoutSelection(
                (u8)completedPlayerId, g_GameManager.character,
                g_GameManager.shotType);
            nextPlayerId = Netplay::GetNextPendingLoadoutPlayerId();
            if (nextPlayerId >= 1)
            {
                Netplay::BeginPlayerLoadoutSelection((u8)nextPlayerId);
                g_GameManager.character =
                    (u8)Netplay::GetPlayerCharacter((u8)nextPlayerId);
                g_GameManager.shotType =
                    (u8)Netplay::GetPlayerShot((u8)nextPlayerId);
                g_selectingLoadoutPlayerId = nextPlayerId;
                menu->SetGameState(
                    menu->gameState == STATE_EXTRA_SELECT_SHOTTYPE
                        ? STATE_EXTRA_SELECT_CHARACTER
                        : STATE_NORMAL_SELECT_CHARACTER);
                menu->cursor = g_GameManager.character;
                g_SoundPlayer.PlaySoundByIdx(SOUND_SELECT, 0);
                g_SoundPlayer.ProcessQueues();
                return CHAIN_CALLBACK_RESULT_CONTINUE;
            }
            RestoreInteractiveP1Loadout();
            g_selectingLoadoutPlayerId = -1;
            // Let the original shot-selection handler consume the same
            // confirm edge and start the game with the restored P1 loadout.
            menu->cursor = g_GameManager.shotType;
        }
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    if (Netplay::ShouldSelectAdditionalPlayerLoadout() &&
        IsShotSelectionState(menu->gameState) &&
        menu->menuSubState == 1 &&
        WAS_PRESSED_RAW(TH_BUTTON_SELECTMENU))
    {
        int nextPlayerId;

        g_interactiveP1Character = g_GameManager.character;
        g_interactiveP1Shot = menu->cursor;
        g_GameManager.shotType = (u8)g_interactiveP1Shot;
        nextPlayerId = Netplay::GetNextPendingLoadoutPlayerId();
        Netplay::BeginPlayerLoadoutSelection((u8)nextPlayerId);
        g_GameManager.character =
            (u8)Netplay::GetPlayerCharacter((u8)nextPlayerId);
        g_GameManager.shotType =
            (u8)Netplay::GetPlayerShot((u8)nextPlayerId);
        g_selectingLoadoutPlayerId = nextPlayerId;
        menu->SetGameState(menu->gameState == STATE_EXTRA_SELECT_SHOTTYPE
                               ? STATE_EXTRA_SELECT_CHARACTER
                               : STATE_NORMAL_SELECT_CHARACTER);
        menu->cursor = g_GameManager.character;
        g_SoundPlayer.PlaySoundByIdx(SOUND_SELECT, 0);
        g_SoundPlayer.ProcessQueues();
    }
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

u32 DrawInteractiveLoadoutPrompt(void *)
{
    MainMenu *menu = FindActiveMainMenu();
    static const char *characterNames[3] = {
        "Reimu", "Marisa", "Sakuya",
    };
    const char *currentPrompt = NULL;
    int currentPlayerId = g_selectingLoadoutPlayerId >= 1
        ? g_selectingLoadoutPlayerId : 0;
    int playerId;
    int character;
    int shot;
    int playerCount;
    bool loadoutSelected;
    D3DXVECTOR3 position;
    Float2 savedScale;
    D3DCOLOR savedColor;
    i32 savedGui;
    i32 savedSelected;

    if (!menu)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }
    if (IsCharacterSelectionState(menu->gameState))
    {
        currentPrompt = "SELECT CHARACTER";
    }
    else if (IsShotSelectionState(menu->gameState))
    {
        currentPrompt = "SELECT SHOT TYPE";
    }
    if (!currentPrompt || (g_selectingLoadoutPlayerId < 1 &&
                           !Netplay::ShouldSelectAdditionalPlayerLoadout()))
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    playerCount = Netplay::GetPlayerCount();
    if (playerCount < 2 || playerCount > TH07_MULTI_MAX_PLAYERS)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    savedScale = g_AsciiManager.scale;
    savedColor = g_AsciiManager.color;
    savedGui = g_AsciiManager.isGui;
    savedSelected = g_AsciiManager.isSelected;
    g_AsciiManager.scale.x = 0.70f;
    g_AsciiManager.scale.y = 0.70f;
    g_AsciiManager.isGui = 0;
    g_AsciiManager.isSelected = 0;

    // Use the empty center-left area of the character/shot screens. Completed
    // rows retain their chosen loadout, the active row shows what is being
    // selected, and later rows remain empty so the P1 -> P2 -> P3 progression
    // is visible without covering the built-in character description or the
    // difficulty label at the bottom.
    for (playerId = 0; playerId < playerCount; playerId++)
    {
        position = D3DXVECTOR3(
            40.0f, 220.0f + 18.0f * playerId, 0.0f);
        if (playerId == currentPlayerId)
        {
            g_AsciiManager.color = 0xffffff80;
            AsciiManager::AddFormatText(
                &g_AsciiManager, &position, "%s: %s",
                Netplay::GetPlayerName((u8)playerId), currentPrompt);
            continue;
        }

        loadoutSelected = playerId == 0
            ? g_selectingLoadoutPlayerId >= 1
            : (playerId == 1
                   ? g_p2LoadoutConfigured || g_p2LoadoutSelected
                   : g_p3LoadoutConfigured || g_p3LoadoutSelected);
        if (!loadoutSelected)
        {
            g_AsciiManager.color = 0xffa0a0a0;
            AsciiManager::AddFormatText(
                &g_AsciiManager, &position, "%s:",
                Netplay::GetPlayerName((u8)playerId));
            continue;
        }

        if (playerId == 0)
        {
            character = g_interactiveP1Character;
            shot = g_interactiveP1Shot;
        }
        else
        {
            character = Netplay::GetPlayerCharacter((u8)playerId);
            shot = Netplay::GetPlayerShot((u8)playerId);
        }
        if (character < 0 || character > 2 || shot < 0 || shot > 1)
        {
            g_AsciiManager.color = 0xffa0a0a0;
            AsciiManager::AddFormatText(
                &g_AsciiManager, &position, "%s:",
                Netplay::GetPlayerName((u8)playerId));
            continue;
        }

        g_AsciiManager.color = 0xff80c0ff;
        AsciiManager::AddFormatText(
            &g_AsciiManager, &position, "%s: %s %c",
            Netplay::GetPlayerName((u8)playerId), characterNames[character],
            (char)('A' + shot));
    }
    g_AsciiManager.scale = savedScale;
    g_AsciiManager.color = savedColor;
    g_AsciiManager.isGui = savedGui;
    g_AsciiManager.isSelected = savedSelected;
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

int KeyNameToVirtualKey(const char *name, int fallback)
{
    int length;

    if (!name || !name[0])
    {
        return fallback;
    }
    if (_stricmp(name, "up") == 0)
    {
        return VK_UP;
    }
    if (_stricmp(name, "down") == 0)
    {
        return VK_DOWN;
    }
    if (_stricmp(name, "left") == 0)
    {
        return VK_LEFT;
    }
    if (_stricmp(name, "right") == 0)
    {
        return VK_RIGHT;
    }
    if (_stricmp(name, "enter") == 0 ||
        _stricmp(name, "numpad_enter") == 0)
    {
        return VK_RETURN;
    }
    if (_stricmp(name, "lshift") == 0)
    {
        return VK_LSHIFT;
    }
    if (_stricmp(name, "rshift") == 0)
    {
        return VK_RSHIFT;
    }
    if (_stricmp(name, "lcontrol") == 0)
    {
        return VK_LCONTROL;
    }
    if (_stricmp(name, "rcontrol") == 0)
    {
        return VK_RCONTROL;
    }
    if (_stricmp(name, "space") == 0)
    {
        return VK_SPACE;
    }
    if (_stricmp(name, "minus") == 0)
    {
        return VK_OEM_MINUS;
    }
    if (_stricmp(name, "equals") == 0)
    {
        return VK_OEM_PLUS;
    }
    if (_stricmp(name, "backspace") == 0)
    {
        return VK_BACK;
    }
    if (_stricmp(name, "tab") == 0)
    {
        return VK_TAB;
    }
    if (_stricmp(name, "lbracket") == 0)
    {
        return VK_OEM_4;
    }
    if (_stricmp(name, "rbracket") == 0)
    {
        return VK_OEM_6;
    }
    if (_stricmp(name, "semicolon") == 0)
    {
        return VK_OEM_1;
    }
    if (_stricmp(name, "apostrophe") == 0)
    {
        return VK_OEM_7;
    }
    if (_stricmp(name, "grave") == 0)
    {
        return VK_OEM_3;
    }
    if (_stricmp(name, "backslash") == 0)
    {
        return VK_OEM_5;
    }
    if (_stricmp(name, "comma") == 0)
    {
        return VK_OEM_COMMA;
    }
    if (_stricmp(name, "period") == 0)
    {
        return VK_OEM_PERIOD;
    }
    if (_stricmp(name, "slash") == 0)
    {
        return VK_OEM_2;
    }
    if (_stricmp(name, "multiply") == 0)
    {
        return VK_MULTIPLY;
    }
    if (_stricmp(name, "subtract") == 0)
    {
        return VK_SUBTRACT;
    }
    if (_stricmp(name, "add") == 0)
    {
        return VK_ADD;
    }
    if (_stricmp(name, "divide") == 0)
    {
        return VK_DIVIDE;
    }
    if (_stricmp(name, "lmenu") == 0)
    {
        return VK_LMENU;
    }
    if (_stricmp(name, "rmenu") == 0)
    {
        return VK_RMENU;
    }
    if (_stricmp(name, "prior") == 0)
    {
        return VK_PRIOR;
    }
    if (_stricmp(name, "end") == 0)
    {
        return VK_END;
    }
    if (_stricmp(name, "insert") == 0)
    {
        return VK_INSERT;
    }
    if (_stricmp(name, "delete") == 0)
    {
        return VK_DELETE;
    }
    if (_stricmp(name, "home") == 0)
    {
        return VK_HOME;
    }
    if (_stricmp(name, "numpad_0") == 0)
    {
        return VK_NUMPAD0;
    }
    if (_stricmp(name, "numpad_1") == 0)
    {
        return VK_NUMPAD1;
    }
    if (_stricmp(name, "numpad_2") == 0)
    {
        return VK_NUMPAD2;
    }
    if (_stricmp(name, "numpad_3") == 0)
    {
        return VK_NUMPAD3;
    }
    if (_stricmp(name, "numpad_4") == 0)
    {
        return VK_NUMPAD4;
    }
    if (_stricmp(name, "numpad_5") == 0)
    {
        return VK_NUMPAD5;
    }
    if (_stricmp(name, "numpad_6") == 0)
    {
        return VK_NUMPAD6;
    }
    if (_stricmp(name, "numpad_7") == 0)
    {
        return VK_NUMPAD7;
    }
    if (_stricmp(name, "numpad_8") == 0)
    {
        return VK_NUMPAD8;
    }
    if (_stricmp(name, "numpad_9") == 0)
    {
        return VK_NUMPAD9;
    }
    length = strlen(name);
    if (length == 5 && _strnicmp(name, "key_", 4) == 0 &&
        ((name[4] >= 'A' && name[4] <= 'Z') ||
         (name[4] >= 'a' && name[4] <= 'z')))
    {
        return name[4] >= 'a' && name[4] <= 'z' ? name[4] - 'a' + 'A'
                                                : name[4];
    }
    if (length == 5 && _strnicmp(name, "key_", 4) == 0 &&
        name[4] >= '0' && name[4] <= '9')
    {
        return name[4];
    }
    if (length == 1 &&
        ((name[0] >= 'A' && name[0] <= 'Z') ||
         (name[0] >= '0' && name[0] <= '9')))
    {
        return name[0];
    }
    return fallback;
}

void LoadLocalP2KeyBindings()
{
    char path[MAX_PATH];
    char value[32];

    if (g_localP2KeyBindingsLoaded)
    {
        return;
    }
    g_localP2KeyBindingsLoaded = true;
    GetConnectionUiConfigPath(path, sizeof(path));
    GetPrivateProfileStringA("KeyBind", "Up_2P", "key_I", value,
                             sizeof(value), path);
    g_localP2KeyUp = KeyNameToVirtualKey(value, 'I');
    GetPrivateProfileStringA("KeyBind", "Down_2P", "key_K", value,
                             sizeof(value), path);
    g_localP2KeyDown = KeyNameToVirtualKey(value, 'K');
    GetPrivateProfileStringA("KeyBind", "Left_2P", "key_J", value,
                             sizeof(value), path);
    g_localP2KeyLeft = KeyNameToVirtualKey(value, 'J');
    GetPrivateProfileStringA("KeyBind", "Right_2P", "key_L", value,
                             sizeof(value), path);
    g_localP2KeyRight = KeyNameToVirtualKey(value, 'L');
    GetPrivateProfileStringA("KeyBind", "Shoot_2P", "key_F", value,
                             sizeof(value), path);
    g_localP2KeyShoot = KeyNameToVirtualKey(value, 'F');
    GetPrivateProfileStringA("KeyBind", "Bomb_2P", "key_G", value,
                             sizeof(value), path);
    g_localP2KeyBomb = KeyNameToVirtualKey(value, 'G');
    GetPrivateProfileStringA("KeyBind", "Focus_2P", "key_D", value,
                             sizeof(value), path);
    g_localP2KeyFocus = KeyNameToVirtualKey(value, 'D');
    g_GameErrorContext.Log(
        "info : local P2 keybinds %d %d %d %d %d %d %d\r\n",
        g_localP2KeyUp, g_localP2KeyDown, g_localP2KeyLeft,
        g_localP2KeyRight, g_localP2KeyShoot, g_localP2KeyBomb,
        g_localP2KeyFocus);
}

bool IsGameWindowFocused();
bool IsVirtualKeyDown(int virtualKey);

u16 CaptureConfiguredLocalP2Input(u16 originalInput)
{
    u16 buttons = 0;
    const u16 p2GameplayMask = TH_BUTTON_DIRECTION | TH_BUTTON_SHOOT |
        TH_BUTTON_BOMB | TH_BUTTON_FOCUS;

    if (!IsGameWindowFocused())
    {
        return 0;
    }
    LoadLocalP2KeyBindings();
    if (IsVirtualKeyDown(g_localP2KeyUp))
    {
        buttons |= TH_BUTTON_UP;
    }
    if (IsVirtualKeyDown(g_localP2KeyDown))
    {
        buttons |= TH_BUTTON_DOWN;
    }
    if (IsVirtualKeyDown(g_localP2KeyLeft))
    {
        buttons |= TH_BUTTON_LEFT;
    }
    if (IsVirtualKeyDown(g_localP2KeyRight))
    {
        buttons |= TH_BUTTON_RIGHT;
    }
    if (IsVirtualKeyDown(g_localP2KeyShoot))
    {
        buttons |= TH_BUTTON_SHOOT;
    }
    if (IsVirtualKeyDown(g_localP2KeyBomb))
    {
        buttons |= TH_BUTTON_BOMB;
    }
    if (IsVirtualKeyDown(g_localP2KeyFocus))
    {
        buttons |= TH_BUTTON_FOCUS;
    }
    return (g_ignoreControllerInput ? 0
                                    : (originalInput & (u16)~p2GameplayMask)) |
           buttons;
}

bool IsGameWindowFocused()
{
    if (!g_GameWindow.window)
    {
        return true;
    }
    return GetForegroundWindow() == g_GameWindow.window;
}

bool IsVirtualKeyDown(int virtualKey)
{
    return IsGameWindowFocused() && virtualKey != 0 &&
        (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

bool IsAnyInputKeyDown()
{
    static const int p1Keys[] = {
        VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
        VK_NUMPAD8, VK_NUMPAD2, VK_NUMPAD4, VK_NUMPAD6,
        VK_NUMPAD7, VK_NUMPAD9, VK_NUMPAD1, VK_NUMPAD3,
        VK_HOME, 'D', 'Z', 'X', VK_SHIFT, VK_LSHIFT, VK_RSHIFT,
        VK_ESCAPE, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_RETURN,
        'Q', 'S', 'R', VK_F2, VK_F3, VK_F4, 'M', 'N', VK_F6, VK_F8
    };
    int i;

    for (i = 0; i < (int)(sizeof(p1Keys) / sizeof(p1Keys[0])); i++)
    {
        if (IsVirtualKeyDown(p1Keys[i]))
        {
            return true;
        }
    }
    if (g_mode == Netplay::MODE_LOCAL)
    {
        LoadLocalP2KeyBindings();
        if (IsVirtualKeyDown(g_localP2KeyUp) ||
            IsVirtualKeyDown(g_localP2KeyDown) ||
            IsVirtualKeyDown(g_localP2KeyLeft) ||
            IsVirtualKeyDown(g_localP2KeyRight) ||
            IsVirtualKeyDown(g_localP2KeyShoot) ||
            IsVirtualKeyDown(g_localP2KeyBomb) ||
            IsVirtualKeyDown(g_localP2KeyFocus))
        {
            return true;
        }
    }
    return false;
}

u16 CaptureSafeP1Input(u16 originalInput)
{
    u16 buttons = 0;
    u16 controllerButtons;

    // Controller::GetInput() reads a DirectInput keyboard state into a
    // stack buffer but does not validate GetDeviceState's HRESULT. Rebuild
    // the keyboard part from GetAsyncKeyState while DirectInput is active;
    // keep the controller part through the original helper.
    // GetAsyncKeyState is system-wide. Do not let typing in another window
    // become gameplay, menu, quick-restart, or delay-control input while a
    // network peer keeps calculating in the background.
    if (!IsGameWindowFocused())
    {
        // Suppress system-wide keyboard state while another window owns the
        // focus, but keep a locally configured gamepad alive. This is
        // especially important for an SSH-launched Guest window, which may
        // not become the foreground window until the player clicks it.
        controllerButtons = g_ignoreControllerInput
            ? 0
            : Controller::GetControllerInput(0);
        if (controllerButtons != g_lastLoggedControllerInput &&
            controllerButtons != 0 && g_controllerInputLogCount < 32)
        {
            g_GameErrorContext.Log(
                "info : local controller input 0x%04x routed to %s lane while unfocused\r\n",
                controllerButtons,
                g_mode == Netplay::MODE_GUEST ? "P2" : "P1");
            g_controllerInputLogCount++;
        }
        g_lastLoggedControllerInput = controllerButtons;
        return controllerButtons;
    }
    if (!g_Supervisor.keyboard)
    {
        return g_ignoreControllerInput ? 0 : originalInput;
    }
    if (IsVirtualKeyDown(VK_UP) || IsVirtualKeyDown(VK_NUMPAD8))
    {
        buttons |= TH_BUTTON_UP;
    }
    if (IsVirtualKeyDown(VK_DOWN) || IsVirtualKeyDown(VK_NUMPAD2))
    {
        buttons |= TH_BUTTON_DOWN;
    }
    if (IsVirtualKeyDown(VK_LEFT) || IsVirtualKeyDown(VK_NUMPAD4))
    {
        buttons |= TH_BUTTON_LEFT;
    }
    if (IsVirtualKeyDown(VK_RIGHT) || IsVirtualKeyDown(VK_NUMPAD6))
    {
        buttons |= TH_BUTTON_RIGHT;
    }
    if (IsVirtualKeyDown(VK_NUMPAD7))
    {
        buttons |= TH_BUTTON_UP_LEFT;
    }
    if (IsVirtualKeyDown(VK_NUMPAD9))
    {
        buttons |= TH_BUTTON_UP_RIGHT;
    }
    if (IsVirtualKeyDown(VK_NUMPAD1))
    {
        buttons |= TH_BUTTON_DOWN_LEFT;
    }
    if (IsVirtualKeyDown(VK_NUMPAD3))
    {
        buttons |= TH_BUTTON_DOWN_RIGHT;
    }
    if (IsVirtualKeyDown(VK_HOME))
    {
        buttons |= TH_BUTTON_HOME;
    }
    if (IsVirtualKeyDown('D'))
    {
        buttons |= TH_BUTTON_D;
    }
    if (IsVirtualKeyDown('Z'))
    {
        buttons |= TH_BUTTON_SHOOT;
    }
    if (IsVirtualKeyDown('X'))
    {
        buttons |= TH_BUTTON_BOMB;
    }
    if (IsVirtualKeyDown(VK_SHIFT))
    {
        buttons |= TH_BUTTON_FOCUS;
    }
    if (IsVirtualKeyDown(VK_ESCAPE))
    {
        buttons |= TH_BUTTON_MENU;
    }
    if (IsVirtualKeyDown(VK_CONTROL))
    {
        buttons |= TH_BUTTON_SKIP;
    }
    if (IsVirtualKeyDown('Q'))
    {
        buttons |= TH_BUTTON_Q;
    }
    if (IsVirtualKeyDown('S'))
    {
        buttons |= TH_BUTTON_S;
    }
    if (IsVirtualKeyDown('R'))
    {
        buttons |= TH_BUTTON_RESET;
    }
    if (IsVirtualKeyDown(VK_RETURN))
    {
        buttons |= TH_BUTTON_ENTER;
    }
    controllerButtons = g_ignoreControllerInput
        ? 0
        : Controller::GetControllerInput(0);
    if (controllerButtons != g_lastLoggedControllerInput &&
        controllerButtons != 0 && g_controllerInputLogCount < 32)
    {
        g_GameErrorContext.Log(
            "info : local controller input 0x%04x routed to %s lane\r\n",
            controllerButtons,
            g_mode == Netplay::MODE_GUEST ? "P2" : "P1");
        g_controllerInputLogCount++;
    }
    g_lastLoggedControllerInput = controllerButtons;
    return buttons | controllerButtons;
}

void ArmInputAfterRelease(u16 *player1, u16 *player2)
{
    if (g_inputArmed)
    {
        return;
    }
    if (!IsAnyInputKeyDown())
    {
        g_inputArmed = true;
        return;
    }
    // A launcher key or a transient DirectInput failure must not become a
    // held direction in the first menu frame. Require a full release before
    // accepting gameplay/menu input again; the next press then works normally.
    *player1 = 0;
    *player2 = 0;
}

enum ControlKeyMask
{
    CONTROL_KEY_F2 = 1,
    CONTROL_KEY_F3 = 2,
    CONTROL_KEY_F4 = 4,
    CONTROL_KEY_Q = 8,
    CONTROL_KEY_R = 16,
    CONTROL_KEY_M = 32,
    CONTROL_KEY_N = 64,
    CONTROL_KEY_F6 = 128
};

Netplay::InGameControl CaptureLocalControl()
{
    u16 keys = 0;
    u16 pressed;
    u32 testFrame;

    if (!g_inputArmed)
    {
        return Netplay::INGAME_CONTROL_NONE;
    }
    testFrame = g_controlTestFrame++;

    if (g_controlTestEnabled)
    {
        switch (testFrame)
        {
        case 120:
            return Netplay::INGAME_CONTROL_INF_LIFE;
        case 150:
            return Netplay::INGAME_CONTROL_INF_BOMB;
        case 180:
            return Netplay::INGAME_CONTROL_INF_POWER;
        case 210:
            return Netplay::INGAME_CONTROL_ADD_DELAY;
        case 240:
            return Netplay::INGAME_CONTROL_DEC_DELAY;
        case 270:
            return Netplay::INGAME_CONTROL_INSANE_MODE;
        default:
            break;
        }
    }

    if (IsVirtualKeyDown(VK_F2))
    {
        keys |= CONTROL_KEY_F2;
    }
    if (IsVirtualKeyDown(VK_F3))
    {
        keys |= CONTROL_KEY_F3;
    }
    if (IsVirtualKeyDown(VK_F4))
    {
        keys |= CONTROL_KEY_F4;
    }
    if (IsVirtualKeyDown('Q'))
    {
        keys |= CONTROL_KEY_Q;
    }
    if (IsVirtualKeyDown('R'))
    {
        keys |= CONTROL_KEY_R;
    }
    if (IsVirtualKeyDown('M'))
    {
        keys |= CONTROL_KEY_M;
    }
    if (IsVirtualKeyDown('N'))
    {
        keys |= CONTROL_KEY_N;
    }
    if (IsVirtualKeyDown(VK_F6))
    {
        keys |= CONTROL_KEY_F6;
    }

    pressed = keys & (u16)~g_previousControlKeys;
    g_previousControlKeys = keys;
    if (pressed & CONTROL_KEY_F2)
    {
        return Netplay::INGAME_CONTROL_INF_LIFE;
    }
    if (pressed & CONTROL_KEY_F3)
    {
        return Netplay::INGAME_CONTROL_INF_BOMB;
    }
    if (pressed & CONTROL_KEY_F4)
    {
        return Netplay::INGAME_CONTROL_INF_POWER;
    }
    if (pressed & CONTROL_KEY_Q)
    {
        return Netplay::INGAME_CONTROL_QUICK_QUIT;
    }
    if (pressed & CONTROL_KEY_R)
    {
        return Netplay::INGAME_CONTROL_QUICK_RESTART;
    }
    if (pressed & CONTROL_KEY_M)
    {
        return Netplay::INGAME_CONTROL_ADD_DELAY;
    }
    if (pressed & CONTROL_KEY_N)
    {
        return Netplay::INGAME_CONTROL_DEC_DELAY;
    }
    if (pressed & CONTROL_KEY_F6)
    {
        return Netplay::INGAME_CONTROL_INSANE_MODE;
    }
    return Netplay::INGAME_CONTROL_NONE;
}

enum
{
    UI_ID_HOST_IP = 100,
    UI_ID_PORT,
    UI_ID_DELAY,
    // Was two buttons, one per role; the role now comes from the radio
    // pair. The id is unchanged so existing launcher probes still find
    // the control that starts matching.
    UI_ID_CONNECT,
    UI_ID_UNUSED_AS_GUEST,
    UI_ID_START_GAME,
    UI_ID_START_LOCAL,
    UI_ID_CANCEL,
    // Keep the original button IDs stable for existing launcher probes.
    // IDs 108 and 109 belonged to the removed P2 loadout fields.
    UI_ID_ROLLBACK = 110,
    UI_ID_STATUS = 111,
    UI_ID_DISPLAY_FULLSCREEN_640 = 112,
    UI_ID_DISPLAY_WINDOW_640 = 113,
    UI_ID_DISPLAY_WINDOW_960 = 114,
    UI_ID_DISPLAY_WINDOW_1280 = 115,
    UI_ID_PLAYER_NAME = 116,
    UI_ID_BGM = 117,
    UI_ID_SE = 118,
    UI_ID_GUEST_EVASIVE_BOT = 119,
    UI_ID_PLAYER_COUNT_2 = 120,
    UI_ID_PLAYER_COUNT_3 = 121,
    UI_ID_ROLE_HOST = 122,
    UI_ID_ROLE_GUEST = 123,
    UI_ID_DELAY_LABEL = 124,
    UI_ID_PLAYER_COUNT_LABEL = 125,
    UI_ID_HOST_SETTINGS = 126,
    UI_ID_ADVANCED = 127,
    UI_ID_SHOW_STAGE_NAMES = 128,
    UI_ID_SHOW_NET_STATS = 129,
    UI_ID_STATUS_LABEL = 130,
};

// Everything below the role selector is pushed down by its height. Keeping it
// as one constant means the rest of the layout stays written in its original
// coordinates.
const int UI_ROLE_ROW_HEIGHT = 34;

// Height of the two advanced rows. Hiding the checkboxes without closing
// the space they occupied left a blank band in the middle of the window,
// so everything below moves up by this much while they are hidden.
const int UI_ADVANCED_BLOCK_HEIGHT = 52;

const UINT CONNECTION_UI_TIMER_ID = 1;
const UINT CONNECTION_UI_TIMER_INTERVAL_MS = 15;
const DWORD CONNECTION_UI_GUEST_TIMEOUT_MS = 1000;
const DWORD CONNECTION_UI_HELLO_INTERVAL_MS = 250;
// The initiator waits this long after the final commit and adds half the
// measured RTT. The receiver starts the same countdown when that commit
// arrives, which makes both launcher windows hand off to D3D at nearly the
// same wall-clock time without assuming synchronized PC clocks.
const DWORD CONNECTION_UI_START_DELAY_MS = 500;
const DWORD CONNECTION_UI_START_RESEND_MS = 100;

struct ConnectionUiSelection
{
    Netplay::Mode mode;
    char host[128];
    char playerName[PLAYER_NAME_LENGTH];
    int port;
    int delay;
    int playerCount;
    int p2Character;
    int p2Shot;
    int displayMode;
    bool bgmEnabled;
    bool seEnabled;
    bool rollback;
    bool evasiveBot;
    bool showStageNames;
    bool showNetStats;
    bool cancelled;
};

struct ConnectionUiState
{
    HWND window;
    HWND hostEdit;
    HWND playerNameEdit;
    HWND portEdit;
    HWND delayEdit;
    HWND bgmCheck;
    HWND seCheck;
    HWND rollbackCheck;
    HWND evasiveBotCheck;
    HWND stageNamesCheck;
    HWND netStatsCheck;
    HWND connectButton;
    HWND startButton;
    HWND status;
    ConnectionUiSelection selection;
    bool finished;
    bool rollbackAllowed;
    bool attemptingConnection;
    bool startRequested;
    bool startCommitted;
    bool startCommitConfirmed;
    u8 startAckMask;
    u8 startCommitAckMask;
    DWORD guestWaitStartedTick;
    DWORD lastHelloSendTick;
    DWORD lastPingTick;
    DWORD lastStartSendTick;
    DWORD launchAtTick;
    // Set once the host's WELCOME has told the guest what it decided.
    bool hostSettingsKnown;
    bool advancedVisible;
};

// Plain ASCII on purpose. The launcher talks to Windows through the ANSI entry
// points, so anything outside the system code page would render differently
// depending on the machine's locale.
#define UI_ADVANCED_LABEL "Advanced settings"

ConnectionUiState g_connectionUi;

// Both are defined next to the control layout further down, but the launcher
// poll loop above it needs them when the host's WELCOME arrives.
void SetConnectionUiRole(HWND window, bool guest);
void ShowConnectionUiHostSettings();
void SetConnectionUiAdvancedVisible(HWND window, bool visible);

void GetModuleSiblingPath(const char *filename, char *path, int pathSize)
{
    DWORD length = GetModuleFileNameA(NULL, path, pathSize);
    int i;
    if (length == 0 || length >= (DWORD)pathSize)
    {
        strncpy(path, filename, pathSize - 1);
        path[pathSize - 1] = '\0';
        return;
    }
    for (i = (int)length - 1; i >= 0; i--)
    {
        if (path[i] == '\\' || path[i] == '/')
        {
            path[i + 1] = '\0';
            strncat(path, filename, pathSize - strlen(path) - 1);
            return;
        }
    }
    strncpy(path, filename, pathSize - 1);
    path[pathSize - 1] = '\0';
}

void GetConnectionUiConfigPath(char *path, int pathSize)
{
    GetModuleSiblingPath("mod_config.ini", path, pathSize);
}

bool IsPlayerNameCharacter(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9');
}

bool SanitizePlayerName(const char *input, int inputSize, char *output,
                        int outputSize)
{
    int i;
    int count = 0;
    if (outputSize <= 1)
    {
        return false;
    }
    for (i = 0; i < inputSize && input[i] != '\0' &&
             count < outputSize - 1;
         i++)
    {
        char c = input[i];
        if (!IsPlayerNameCharacter(c))
        {
            continue;
        }
        output[count++] = c;
    }
    output[count] = '\0';
    return count != 0;
}

void SetPlayerName(char *destination, const char *source,
                   const char *fallback)
{
    if (!SanitizePlayerName(source, PLAYER_NAME_LENGTH,
                            destination, PLAYER_NAME_MAX_CHARS + 1))
    {
        strncpy(destination, fallback, PLAYER_NAME_LENGTH - 1);
        destination[PLAYER_NAME_LENGTH - 1] = '\0';
    }
}

void SetLocalPlayerName(const char *name)
{
    SetPlayerName(g_localPlayerName, name, "Player");
    if (g_localPlayerSlot >= 0 &&
        g_localPlayerSlot < TH07_MULTI_MAX_PLAYERS)
    {
        SetPlayerName(g_sessionPlayerNames[g_localPlayerSlot],
                      g_localPlayerName,
                      g_localPlayerSlot == 0 ? "Player" :
                      (g_localPlayerSlot == 1 ? "Player2" : "Player3"));
    }
}

void SetRemotePlayerName(const char *name)
{
    int remotePlayerId = GetPrimaryRemotePlayerId();
    SetPlayerName(g_remotePlayerName, name, "Player2");
    if (remotePlayerId >= 0 && remotePlayerId < TH07_MULTI_MAX_PLAYERS)
    {
        SetPlayerName(g_sessionPlayerNames[remotePlayerId], name,
                      remotePlayerId == 0 ? "Player" :
                      (remotePlayerId == 1 ? "Player2" : "Player3"));
    }
}

void SetSessionPlayerName(u8 playerId, const char *name)
{
    const char *fallback;
    if (playerId >= TH07_MULTI_MAX_PLAYERS)
    {
        return;
    }
    fallback = playerId == 0 ? "Player" :
        (playerId == 1 ? "Player2" : "Player3");
    SetPlayerName(g_sessionPlayerNames[playerId], name, fallback);
    if (playerId == (u8)g_localPlayerSlot)
    {
        SetPlayerName(g_localPlayerName, name, fallback);
    }
    else if (playerId == (u8)GetPrimaryRemotePlayerId())
    {
        SetPlayerName(g_remotePlayerName, name, fallback);
    }
    if (playerId == 2)
    {
        SetPlayerName(g_player3Name, name, "Player3");
    }
}

void ApplyPacketPlayerNames(const NetPacket &packet)
{
    int playerId;
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        SetSessionPlayerName((u8)playerId, packet.playerNames[playerId]);
    }
}

void LoadLocalPlayerNameConfig()
{
    char path[MAX_PATH];
    char value[128];
    GetConnectionUiConfigPath(path, sizeof(path));
    GetPrivateProfileStringA("Connection", "PlayerName", "Player", value,
                             sizeof(value), path);
    SetPlayerName(g_localPlayerName, value, "Player");
}

void LoadLowLatencyConfig()
{
    char path[MAX_PATH];
    int value;

    GetConnectionUiConfigPath(path, sizeof(path));
    g_lowLatencyEnabled =
        GetPrivateProfileIntA("LowLatency", "Enabled", 1, path) != 0;
    value = GetPrivateProfileIntA("LowLatency", "BltPrepareTime", 1, path);
    if (value < 0)
    {
        value = 0;
    }
    if (value > 16)
    {
        value = 16;
    }
    g_lowLatencyPrepareTimeMs = value;
    g_lowLatencySpinWait =
        GetPrivateProfileIntA("LowLatency", "Sleep", 1, path) == 0;
    g_lowLatencyDwmFlush =
        GetPrivateProfileIntA("LowLatency", "DwmFlush", 0, path) != 0;
}

void LoadConnectionUiConfig(ConnectionUiSelection *selection)
{
    char path[MAX_PATH];
    char value[128];
    GetConnectionUiConfigPath(path, sizeof(path));
    GetPrivateProfileStringA("Connection", "PlayerName", "Player", value,
                             sizeof(value), path);
    if (!SanitizePlayerName(value, sizeof(value), selection->playerName,
                            PLAYER_NAME_MAX_CHARS + 1))
    {
        strncpy(selection->playerName, "Player",
                sizeof(selection->playerName) - 1);
        selection->playerName[sizeof(selection->playerName) - 1] = '\0';
    }
    GetPrivateProfileStringA("Connection", "HostIP", "127.0.0.1", value,
                             sizeof(value), path);
    strncpy(selection->host, value, sizeof(selection->host) - 1);
    selection->host[sizeof(selection->host) - 1] = '\0';
    selection->port = GetPrivateProfileIntA("Connection", "Port", 35000, path);
    selection->delay = GetPrivateProfileIntA("Connection", "Delay", 1, path);
    selection->playerCount =
        GetPrivateProfileIntA("Connection", "PlayerCount", 2, path);
    selection->rollback =
        GetPrivateProfileIntA("Connection", "Rollback", 1, path) != 0;
    selection->displayMode =
        GetPrivateProfileIntA("Display", "Mode", DISPLAY_MODE_WINDOW_640,
                              path);
    selection->bgmEnabled =
        GetPrivateProfileIntA("Audio", "BGM", 1, path) != 0;
    selection->seEnabled =
        GetPrivateProfileIntA("Audio", "SE", 1, path) != 0;
    selection->evasiveBot =
        GetPrivateProfileIntA("Connection", "GuestEvasiveBot", 0, path) != 0;
    selection->showStageNames =
        GetPrivateProfileIntA("Display", "StagePlayerNames", 1, path) != 0;
    selection->showNetStats =
        GetPrivateProfileIntA("Display", "NetDiagnostics", 0, path) != 0;
    // P2 is selected in the in-game TH06-style setup screen. Keep these
    // fields at -1 so a stale pre-UI configuration cannot bypass that step.
    selection->p2Character = -1;
    selection->p2Shot = -1;
    if (selection->port < 1 || selection->port > 65535)
    {
        selection->port = 35000;
    }
    if (selection->delay < 0 || selection->delay > 10)
    {
        selection->delay = 1;
    }
    if (selection->playerCount != 2 && selection->playerCount != 3)
    {
        selection->playerCount = 2;
    }
    if (selection->displayMode < DISPLAY_MODE_FULLSCREEN_640 ||
        selection->displayMode > DISPLAY_MODE_WINDOW_1280)
    {
        selection->displayMode = DISPLAY_MODE_WINDOW_640;
    }
}

void LoadDisplayPreferences()
{
    char path[MAX_PATH];
    GetConnectionUiConfigPath(path, sizeof(path));
    g_showStagePlayerNames =
        GetPrivateProfileIntA("Display", "StagePlayerNames", 1, path) != 0;
    g_showNetDiagnostics =
        GetPrivateProfileIntA("Display", "NetDiagnostics", 0, path) != 0;
}

void SaveConnectionUiConfig(const ConnectionUiSelection *selection)
{
    char path[MAX_PATH];
    char value[32];
    if (g_localNoSave)
    {
        return;
    }
    GetConnectionUiConfigPath(path, sizeof(path));
    WritePrivateProfileStringA("Connection", "PlayerName",
                               selection->playerName, path);
    WritePrivateProfileStringA("Connection", "HostIP", selection->host, path);
    sprintf(value, "%d", selection->port);
    WritePrivateProfileStringA("Connection", "Port", value, path);
    sprintf(value, "%d", selection->delay);
    WritePrivateProfileStringA("Connection", "Delay", value, path);
    sprintf(value, "%d", selection->playerCount);
    WritePrivateProfileStringA("Connection", "PlayerCount", value, path);
    WritePrivateProfileStringA("Connection", "Rollback",
                               selection->rollback ? "1" : "0", path);
    sprintf(value, "%d", selection->displayMode);
    WritePrivateProfileStringA("Display", "Mode", value, path);
    WritePrivateProfileStringA("Audio", "BGM",
                               selection->bgmEnabled ? "1" : "0", path);
    WritePrivateProfileStringA("Audio", "SE",
                               selection->seEnabled ? "1" : "0", path);
    WritePrivateProfileStringA("Connection", "GuestEvasiveBot",
                               selection->evasiveBot ? "1" : "0", path);
    WritePrivateProfileStringA("Display", "StagePlayerNames",
                               selection->showStageNames ? "1" : "0",
                               path);
    WritePrivateProfileStringA("Display", "NetDiagnostics",
                               selection->showNetStats ? "1" : "0", path);
}

void SetConnectionUiStatus(const char *text)
{
    if (g_connectionUi.status)
    {
        SetWindowTextA(g_connectionUi.status, text);
    }
}

bool ReadConnectionUiFields()
{
    char playerName[128];
    char portText[32];
    char delayText[32];
    char *end;
    long port;
    long delay;

    memset(portText, 0, sizeof(portText));
    memset(delayText, 0, sizeof(delayText));
    if (!g_connectionUi.window || !IsWindow(g_connectionUi.window))
    {
        SetConnectionUiStatus("launcher window handle is unavailable");
        return false;
    }
    // Resolve by control ID at click time so a repeated connection attempt or
    // an automated launcher probe cannot leave a stale edit-control handle.
    g_connectionUi.hostEdit =
        GetDlgItem(g_connectionUi.window, UI_ID_HOST_IP);
    g_connectionUi.playerNameEdit =
        GetDlgItem(g_connectionUi.window, UI_ID_PLAYER_NAME);
    g_connectionUi.portEdit =
        GetDlgItem(g_connectionUi.window, UI_ID_PORT);
    g_connectionUi.delayEdit =
        GetDlgItem(g_connectionUi.window, UI_ID_DELAY);
    g_connectionUi.bgmCheck =
        GetDlgItem(g_connectionUi.window, UI_ID_BGM);
    g_connectionUi.seCheck =
        GetDlgItem(g_connectionUi.window, UI_ID_SE);
    g_connectionUi.rollbackCheck =
        GetDlgItem(g_connectionUi.window, UI_ID_ROLLBACK);
    g_connectionUi.evasiveBotCheck =
        GetDlgItem(g_connectionUi.window, UI_ID_GUEST_EVASIVE_BOT);
    g_connectionUi.stageNamesCheck =
        GetDlgItem(g_connectionUi.window, UI_ID_SHOW_STAGE_NAMES);
    g_connectionUi.netStatsCheck =
        GetDlgItem(g_connectionUi.window, UI_ID_SHOW_NET_STATS);
    if (!g_connectionUi.hostEdit || !g_connectionUi.playerNameEdit ||
        !g_connectionUi.portEdit ||
        !g_connectionUi.delayEdit || !g_connectionUi.bgmCheck ||
        !g_connectionUi.seCheck || !g_connectionUi.rollbackCheck ||
        !g_connectionUi.evasiveBotCheck)
    {
        SetConnectionUiStatus("launcher controls are unavailable");
        return false;
    }
    if (GetWindowTextA(g_connectionUi.playerNameEdit, playerName,
                       sizeof(playerName)) <= 0 ||
        GetWindowTextA(g_connectionUi.hostEdit,
                       g_connectionUi.selection.host,
                       sizeof(g_connectionUi.selection.host)) <= 0 ||
        GetWindowTextA(g_connectionUi.portEdit, portText,
                       sizeof(portText)) <= 0 ||
        GetWindowTextA(g_connectionUi.delayEdit, delayText,
                       sizeof(delayText)) <= 0)
    {
        SetConnectionUiStatus("enter a player name and connection fields");
        return false;
    }
    if (!SanitizePlayerName(playerName, sizeof(playerName),
                            g_connectionUi.selection.playerName,
                            PLAYER_NAME_MAX_CHARS + 1))
    {
        SetConnectionUiStatus("Player name must use ASCII letters or digits");
        return false;
    }
    port = strtol(portText, &end, 10);
    if (end == portText || *end != '\0' || port < 1 || port > 65535)
    {
        SetConnectionUiStatus("Port must be between 1 and 65535");
        return false;
    }
    delay = strtol(delayText, &end, 10);
    if (end == delayText || *end != '\0' || delay < 0 || delay > 10)
    {
        SetConnectionUiStatus("Delay must be between 0 and 10");
        return false;
    }
    if (g_connectionUi.selection.host[0] == '\0')
    {
        SetConnectionUiStatus("Enter a host name or IP address");
        return false;
    }
    g_connectionUi.selection.port = (int)port;
    g_connectionUi.selection.delay = (int)delay;
    g_connectionUi.selection.rollback =
        SendMessageA(g_connectionUi.rollbackCheck, BM_GETCHECK, 0, 0) ==
        BST_CHECKED;
    if (IsDlgButtonChecked(g_connectionUi.window,
                           UI_ID_DISPLAY_FULLSCREEN_640) == BST_CHECKED)
    {
        g_connectionUi.selection.displayMode = DISPLAY_MODE_FULLSCREEN_640;
    }
    else if (IsDlgButtonChecked(g_connectionUi.window,
                                UI_ID_DISPLAY_WINDOW_960) == BST_CHECKED)
    {
        g_connectionUi.selection.displayMode = DISPLAY_MODE_WINDOW_960;
    }
    else if (IsDlgButtonChecked(g_connectionUi.window,
                                UI_ID_DISPLAY_WINDOW_1280) == BST_CHECKED)
    {
        g_connectionUi.selection.displayMode = DISPLAY_MODE_WINDOW_1280;
    }
    else
    {
        g_connectionUi.selection.displayMode = DISPLAY_MODE_WINDOW_640;
    }
    g_connectionUi.selection.bgmEnabled =
        SendMessageA(g_connectionUi.bgmCheck, BM_GETCHECK, 0, 0) ==
        BST_CHECKED;
    g_connectionUi.selection.seEnabled =
        SendMessageA(g_connectionUi.seCheck, BM_GETCHECK, 0, 0) ==
        BST_CHECKED;
    g_connectionUi.selection.evasiveBot =
        SendMessageA(g_connectionUi.evasiveBotCheck, BM_GETCHECK, 0, 0) ==
        BST_CHECKED;
    if (g_connectionUi.stageNamesCheck)
    {
        g_connectionUi.selection.showStageNames =
            SendMessageA(g_connectionUi.stageNamesCheck, BM_GETCHECK, 0,
                         0) == BST_CHECKED;
    }
    if (g_connectionUi.netStatsCheck)
    {
        g_connectionUi.selection.showNetStats =
            SendMessageA(g_connectionUi.netStatsCheck, BM_GETCHECK, 0,
                         0) == BST_CHECKED;
    }
    g_connectionUi.selection.playerCount =
        IsDlgButtonChecked(g_connectionUi.window,
                           UI_ID_PLAYER_COUNT_3) == BST_CHECKED
            ? 3 : 2;
    g_connectionUi.selection.p2Character = -1;
    g_connectionUi.selection.p2Shot = -1;
    return true;
}

void SetConnectionUiNetworkFieldsEnabled(bool enabled)
{
    EnableWindow(g_connectionUi.hostEdit, enabled ? TRUE : FALSE);
    EnableWindow(g_connectionUi.playerNameEdit, enabled ? TRUE : FALSE);
    EnableWindow(g_connectionUi.portEdit, enabled ? TRUE : FALSE);
    EnableWindow(g_connectionUi.delayEdit, enabled ? TRUE : FALSE);
    EnableWindow(g_connectionUi.bgmCheck, enabled ? TRUE : FALSE);
    EnableWindow(g_connectionUi.seCheck, enabled ? TRUE : FALSE);
    EnableWindow(g_connectionUi.rollbackCheck,
                 enabled && g_connectionUi.selection.playerCount != 3
                     ? TRUE : FALSE);
    EnableWindow(g_connectionUi.evasiveBotCheck, enabled ? TRUE : FALSE);
    EnableWindow(g_connectionUi.stageNamesCheck, enabled ? TRUE : FALSE);
    EnableWindow(g_connectionUi.netStatsCheck, enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(g_connectionUi.window, UI_ID_PLAYER_COUNT_2),
                 enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(g_connectionUi.window, UI_ID_PLAYER_COUNT_3),
                 enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(g_connectionUi.window,
                            UI_ID_DISPLAY_FULLSCREEN_640),
                 enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(g_connectionUi.window,
                            UI_ID_DISPLAY_WINDOW_640),
                 enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(g_connectionUi.window,
                            UI_ID_DISPLAY_WINDOW_960),
                 enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(g_connectionUi.window,
                            UI_ID_DISPLAY_WINDOW_1280),
                 enabled ? TRUE : FALSE);
}

void UpdateConnectionUiPeerStatus()
{
    char text[256];
    int firstPlayerId;
    int lastPlayerId;
    int playerId;

    text[0] = '\0';
    firstPlayerId = g_mode == Netplay::MODE_HOST ? 1 : 0;
    lastPlayerId = g_mode == Netplay::MODE_HOST ? g_playerCount : 1;
    for (playerId = firstPlayerId; playerId < lastPlayerId; playerId++)
    {
        char line[128];
        if (!g_peerPresent[playerId])
        {
            sprintf(line, "P%d: waiting...", playerId + 1);
        }
        else
        {
            const char *address = inet_ntoa(
                g_peerAddresses[playerId].sin_addr);
            unsigned port = (unsigned)ntohs(
                g_peerAddresses[playerId].sin_port);
            DWORD rtt = g_lastRoundTripMsByPlayer[playerId];
            const char *name = Netplay::GetPlayerName((u8)playerId);
            if (rtt != 0)
            {
                sprintf(line, "P%d  %-10s  %s:%u  %lu ms",
                        playerId + 1, name ? name : "Player",
                        address ? address : "peer", port,
                        (unsigned long)(rtt / 2));
            }
            else
            {
                sprintf(line, "P%d  %-10s  %s:%u  connected",
                        playerId + 1, name ? name : "Player",
                        address ? address : "peer", port);
            }
        }
        if (text[0] != '\0')
        {
            strncat(text, "\r\n", sizeof(text) - strlen(text) - 1);
        }
        strncat(text, line, sizeof(text) - strlen(text) - 1);
    }
    if (text[0] == '\0')
    {
        strcpy(text, "no connection");
    }
    SetConnectionUiStatus(text);
}

void ResetConnectionUiAttempt(const char *status)
{
    CloseNetworkSocket();
    g_mode = Netplay::MODE_SINGLE;
    g_session = 0;
    g_initialRngSeed = 0;
    g_connectionFailed = false;
    g_protocolMismatch = false;
    g_connectionUi.selection.mode = Netplay::MODE_SINGLE;
    g_connectionUi.attemptingConnection = false;
    g_connectionUi.startRequested = false;
    g_connectionUi.startCommitted = false;
    g_connectionUi.startCommitConfirmed = false;
    g_connectionUi.startAckMask = 0;
    g_connectionUi.startCommitAckMask = 0;
    g_connectionUi.guestWaitStartedTick = 0;
    g_connectionUi.lastHelloSendTick = 0;
    g_connectionUi.lastPingTick = 0;
    g_connectionUi.lastStartSendTick = 0;
    g_connectionUi.launchAtTick = 0;
    SetConnectionUiNetworkFieldsEnabled(true);
    EnableWindow(g_connectionUi.connectButton, TRUE);
    EnableWindow(g_connectionUi.startButton, FALSE);
    if (g_connectionUi.window && IsWindow(g_connectionUi.window))
    {
        SetWindowTextA(
            g_connectionUi.connectButton,
            IsDlgButtonChecked(g_connectionUi.window, UI_ID_ROLE_GUEST) ==
                    BST_CHECKED
                ? "Connect to host" : "Start hosting");
    }
    if (g_connectionUi.window && IsWindow(g_connectionUi.window))
    {
        HWND cancelButton =
            GetDlgItem(g_connectionUi.window, UI_ID_CANCEL);
        HWND hostSettings =
            GetDlgItem(g_connectionUi.window, UI_ID_HOST_SETTINGS);
        if (cancelButton)
        {
            SetWindowTextA(cancelButton, "Cancel");
        }
        // This line reports what a host decided during the attempt that
        // just ended. The next one may be against a different host, so
        // leaving it on screen would state something no longer agreed.
        g_connectionUi.hostSettingsKnown = false;
        if (hostSettings)
        {
            ShowWindow(hostSettings, SW_HIDE);
        }
    }
    SetConnectionUiStatus(status ? status : "no connection");
}

void InitializeHostSession()
{
    g_session = GetTickCount() ^ GetCurrentProcessId() ^ 0x37485450;
    if (g_seedConfigured)
    {
        g_initialRngSeed = g_configuredSeed;
    }
    else
    {
        g_initialRngSeed = (u16)(g_session ^ (g_session >> 16));
        if (g_initialRngSeed == 0)
        {
            g_initialRngSeed = 1;
        }
    }
}

void EnterConnectionUiConnectedState()
{
    g_connected = AreAllExpectedPeersConnected();
    g_connectionFailed = false;
    g_connectionUi.attemptingConnection = !g_connected;
    EnableWindow(g_connectionUi.startButton,
                 g_connected && g_mode == Netplay::MODE_HOST
                     ? TRUE : FALSE);
    if (g_mode == Netplay::MODE_HOST)
    {
        SetWindowTextA(g_connectionUi.connectButton,
                       g_connected ? "connected" : "waiting guests");
    }
    else if (g_mode == Netplay::MODE_GUEST)
    {
        SetWindowTextA(g_connectionUi.connectButton,
                       g_connected ? "connected" : "waiting players");
    }
    UpdateConnectionUiPeerStatus();
    SendPing();
    g_connectionUi.lastPingTick = GetTickCount();
}

bool BeginConnectionUiNetwork(Netplay::Mode mode)
{
    NetPacket hello;
    char status[160];
    char requestedHost[128];
    int requestedPort;
    int requestedDelay;
    int requestedPlayerCount;
    bool requestedRollback;
    DWORD now;

    CloseNetworkSocket();
    ResetInputRings();
    if (!ReadConnectionUiFields())
    {
        return false;
    }
    SetLocalPlayerName(g_connectionUi.selection.playerName);
    strncpy(requestedHost, g_connectionUi.selection.host,
            sizeof(requestedHost) - 1);
    requestedHost[sizeof(requestedHost) - 1] = '\0';
    requestedPort = g_connectionUi.selection.port;
    requestedDelay = g_connectionUi.selection.delay;
    requestedPlayerCount = g_connectionUi.selection.playerCount;
    requestedRollback = g_connectionUi.selection.rollback;
    if (requestedPlayerCount == 3)
    {
        requestedRollback = true;
    }
    SaveConnectionUiConfig(&g_connectionUi.selection);
    memset(g_peerAddresses, 0, sizeof(g_peerAddresses));
    memset(g_peerPresent, 0, sizeof(g_peerPresent));
    g_connected = false;
    g_connectionFailed = false;
    g_protocolMismatch = false;
    g_connectionUi.selection.mode = mode;
    strncpy(g_connectionUi.selection.host, requestedHost,
            sizeof(g_connectionUi.selection.host) - 1);
    g_connectionUi.selection.host[
        sizeof(g_connectionUi.selection.host) - 1] = '\0';
    g_connectionUi.selection.port = requestedPort;
    g_connectionUi.selection.delay = requestedDelay;
    g_connectionUi.selection.playerCount = requestedPlayerCount;
    g_connectionUi.selection.rollback = requestedRollback;
    g_mode = mode;
    g_playerCount = requestedPlayerCount;
    g_localPlayerSlot = mode == Netplay::MODE_HOST ? 0 : 1;
    g_connectedPlayerMask = (u8)(1 << g_localPlayerSlot);
    SetSessionPlayerName((u8)g_localPlayerSlot,
                         g_connectionUi.selection.playerName);
    g_rollbackEnabled = requestedRollback &&
        g_connectionUi.rollbackAllowed;
    if (g_playerCount == 3)
    {
        g_rollbackEnabled = true;
    }
    g_rollbackEverEnabled = g_rollbackEverEnabled || g_rollbackEnabled;
    g_delay = g_rollbackEnabled ? 0 : requestedDelay;
    SetConnectionUiNetworkFieldsEnabled(false);
    EnableWindow(g_connectionUi.startButton, FALSE);
    {
        // While a search is running the button stops the search. It only
        // closes the launcher when there is no search to stop.
        HWND cancelButton =
            GetDlgItem(g_connectionUi.window, UI_ID_CANCEL);
        if (cancelButton)
        {
            SetWindowTextA(cancelButton, "Stop Search");
        }
    }
    now = GetTickCount();
    g_connectionUi.attemptingConnection = true;
    g_connectionUi.startRequested = false;
    g_connectionUi.startCommitted = false;
    g_connectionUi.startCommitConfirmed = false;
    g_connectionUi.startAckMask = 0;
    g_connectionUi.startCommitAckMask = 0;
    g_connectionUi.lastHelloSendTick = 0;
    g_connectionUi.lastPingTick = 0;
    g_connectionUi.lastStartSendTick = 0;
    g_connectionUi.launchAtTick = 0;

    if (mode == Netplay::MODE_HOST)
    {
        InitializeHostSession();
        if (!CreateSocket(requestedPort))
        {
            char error[160];
            strncpy(error, g_status, sizeof(error) - 1);
            error[sizeof(error) - 1] = '\0';
            ResetConnectionUiAttempt(error);
            return false;
        }
        EnableWindow(g_connectionUi.connectButton, FALSE);
        SetWindowTextA(g_connectionUi.connectButton, "waiting guests");
        sprintf(status, "waiting guests 0/%d on UDP %d...",
                g_playerCount - 1, requestedPort);
        SetConnectionUiStatus(status);
        SetStatus(status);
        return true;
    }

    g_session = 0;
    g_initialRngSeed = 0;
    if (!CreateSocket(0))
    {
        char error[160];
        strncpy(error, g_status, sizeof(error) - 1);
        error[sizeof(error) - 1] = '\0';
        ResetConnectionUiAttempt(error);
        return false;
    }
    if (!ResolveAddress(requestedHost, requestedPort, &g_peerAddresses[0]))
    {
        ResetConnectionUiAttempt("host name could not be resolved");
        SetStatus("host name could not be resolved");
        return false;
    }
    g_peerPresent[0] = true;
    EnableWindow(g_connectionUi.connectButton, FALSE);
    SetWindowTextA(g_connectionUi.connectButton, "waiting msg...");
    SetConnectionUiStatus("trying connection...");
    sprintf(status, "connecting to %s:%d", requestedHost, requestedPort);
    SetStatus(status);
    g_connectionUi.guestWaitStartedTick = now;
    InitializePacket(&hello, PACKET_HELLO);
    SendPacket(hello);
    g_connectionUi.lastHelloSendTick = now;
    return true;
}

void SendConnectionUiControlBurst(ControlType type, u32 frame = 0)
{
    int repeat;
    for (repeat = 0; repeat < 8; repeat++)
    {
        SendControl(type, frame, 0);
    }
}

void CommitConnectionUiNetworkStart()
{
    DWORD maximumRoundTrip = 0;
    DWORD halfRoundTrip;
    int playerId;
    for (playerId = 1; playerId < g_playerCount; playerId++)
    {
        if (g_lastRoundTripMsByPlayer[playerId] > maximumRoundTrip)
        {
            maximumRoundTrip = g_lastRoundTripMsByPlayer[playerId];
        }
    }
    halfRoundTrip = maximumRoundTrip != 0
        ? maximumRoundTrip / 2
        : CONNECTION_UI_TIMER_INTERVAL_MS;
    if (halfRoundTrip > 250)
    {
        halfRoundTrip = 250;
    }
    if (g_connectionUi.startCommitted)
    {
        return;
    }
    g_connectionUi.startCommitted = true;
    g_connectionUi.startCommitConfirmed = false;
    g_connectionUi.launchAtTick = GetTickCount() +
        CONNECTION_UI_START_DELAY_MS + halfRoundTrip;
    SetConnectionUiStatus("both ready; synchronized launch...");
    SendConnectionUiControlBurst(
        CONTROL_START_GAME_COMMIT, CONNECTION_UI_START_DELAY_MS);
    g_connectionUi.lastStartSendTick = GetTickCount();
}

void AcceptConnectionUiNetworkStart(u32 delayMs)
{
    if (delayMs < 100 || delayMs > 2000)
    {
        delayMs = CONNECTION_UI_START_DELAY_MS;
    }
    if (!g_connectionUi.startCommitted)
    {
        g_connectionUi.startCommitted = true;
        g_connectionUi.startCommitConfirmed = true;
        g_connectionUi.launchAtTick = GetTickCount() + delayMs;
        EnableWindow(g_connectionUi.startButton, FALSE);
        SetConnectionUiStatus("both ready; synchronized launch...");
    }
}

void FinishConnectionUiNetworkStart(bool requestStart)
{

    if (!g_connected)
    {
        SetConnectionUiStatus("no connection");
        return;
    }
    if (g_mode == Netplay::MODE_GUEST)
    {
        g_connectionUi.selection.rollback = g_rollbackEnabled;
        g_connectionUi.selection.delay = g_delay;
    }
    SaveConnectionUiConfig(&g_connectionUi.selection);
    if (requestStart)
    {
        if (g_mode != Netplay::MODE_HOST ||
            !AreAllExpectedPeersConnected())
        {
            SetConnectionUiStatus("Host must wait for every guest");
            return;
        }
        // Keep both launchers open through request, acknowledgement, and final
        // commit. The old two-message path let Guest begin D3D initialization
        // before Host had even processed the acknowledgement.
        g_connectionUi.startRequested = true;
        g_connectionUi.startCommitted = false;
        g_connectionUi.startCommitConfirmed = false;
        g_connectionUi.startAckMask = 0;
        g_connectionUi.startCommitAckMask = 0;
        g_connectionUi.launchAtTick = 0;
        EnableWindow(g_connectionUi.startButton, FALSE);
        SetConnectionUiStatus("waiting for peer ready...");
        SendConnectionUiControlBurst(CONTROL_START_GAME);
        g_connectionUi.lastStartSendTick = GetTickCount();
        return;
    }
    g_connectionUi.finished = true;
    if (g_connectionUi.window)
    {
        DestroyWindow(g_connectionUi.window);
    }
}

void PollConnectionUiNetwork()
{
    NetPacket packet;
    sockaddr_in from;
    DWORD now = GetTickCount();

    if (g_mode == Netplay::MODE_GUEST && !g_connected &&
        g_connectionUi.attemptingConnection &&
        now - g_connectionUi.lastHelloSendTick >=
            CONNECTION_UI_HELLO_INTERVAL_MS)
    {
        NetPacket hello;
        InitializePacket(&hello, PACKET_HELLO);
        SendPacket(hello);
        g_connectionUi.lastHelloSendTick = now;
    }

    while (g_socket != INVALID_SOCKET && ReceivePacket(&packet, &from))
    {
        int fromPlayerId = FindPeerPlayerId(from);
        if (fromPlayerId < 0 &&
            !(g_mode == Netplay::MODE_HOST &&
              packet.type == PACKET_HELLO))
        {
            continue;
        }
        if (g_mode == Netplay::MODE_GUEST && fromPlayerId != 0)
        {
            continue;
        }
        if (!ValidatePacketVersion(packet))
        {
            ResetConnectionUiAttempt(
                "version mismatch; use the same build");
            SetStatus("protocol version mismatch; use the same build on both peers");
            return;
        }

        if (g_mode == Netplay::MODE_HOST && packet.type == PACKET_HELLO)
        {
            int assignedPlayerId = AssignHostGuestSlot(
                from, packet.playerName);
            if (assignedPlayerId < 1)
            {
                SetConnectionUiStatus("session already has every guest");
                continue;
            }
            SendWelcomeToAllGuests(4);
            g_connected = AreAllExpectedPeersConnected();
            EnterConnectionUiConnectedState();
            SetStatus(g_connected
                          ? "host matched every guest in launcher"
                          : "host waiting for remaining guests");
            continue;
        }
        if (g_mode == Netplay::MODE_GUEST &&
            packet.type == PACKET_WELCOME &&
            (!g_connected || packet.session == g_session))
        {
            if (!g_connected || packet.session == g_session)
            {
                char delay[32];
                if (!AcceptWelcomePacket(packet, from, true))
                {
                    continue;
                }
                g_connectionUi.selection.rollback = g_rollbackEnabled;
                g_connectionUi.selection.delay = g_delay;
                g_connectionUi.selection.playerCount = g_playerCount;
                sprintf(delay, "%d", g_delay);
                SetWindowTextA(g_connectionUi.delayEdit, delay);
                SendMessageA(g_connectionUi.rollbackCheck, BM_SETCHECK,
                             g_rollbackEnabled ? BST_CHECKED : BST_UNCHECKED,
                             0);
                EnableWindow(g_connectionUi.rollbackCheck,
                             g_playerCount == 3 ? FALSE : TRUE);
                CheckRadioButton(
                    g_connectionUi.window,
                    UI_ID_PLAYER_COUNT_2, UI_ID_PLAYER_COUNT_3,
                    g_playerCount == 3 ? UI_ID_PLAYER_COUNT_3
                                       : UI_ID_PLAYER_COUNT_2);
                ShowConnectionUiHostSettings();
                g_connectionUi.guestWaitStartedTick = now;
                EnterConnectionUiConnectedState();
                SetStatus(g_connected
                              ? "guest matched all players in launcher"
                              : "guest waiting for remaining players");
            }
            continue;
        }
        if (!g_connected || packet.session != g_session)
        {
            continue;
        }
        if (packet.type == PACKET_CONTROL &&
            packet.controlType == CONTROL_PEER_EXIT)
        {
            if (g_mode == Netplay::MODE_HOST && fromPlayerId >= 1)
            {
                g_peerPresent[fromPlayerId] = false;
                g_connectedPlayerMask &= (u8)~(1 << fromPlayerId);
                g_connected = false;
                g_connectionUi.startRequested = false;
                g_connectionUi.startCommitted = false;
                EnableWindow(g_connectionUi.startButton, FALSE);
                SendWelcomeToAllGuests(2);
                UpdateConnectionUiPeerStatus();
                SetStatus("guest left launcher; waiting replacement");
                continue;
            }
            g_peerExitReceived = true;
            g_connectionUi.selection.cancelled = true;
            g_connectionUi.finished = true;
            SetConnectionUiStatus("peer closed the game");
            SetStatus("peer closed the game; exiting");
            g_GameErrorContext.Log(
                "info : peer closed the game while launcher was open; exiting\r\n");
            CloseNetworkSocket();
            if (g_connectionUi.window)
            {
                DestroyWindow(g_connectionUi.window);
            }
            return;
        }
        if (packet.type == PACKET_PING)
        {
            NetPacket reply;
            InitializePacket(&reply, PACKET_PONG);
            reply.sendTick = GetTickCount();
            reply.echoTick = packet.sendTick;
            SendPacketToPlayer(reply, fromPlayerId);
        }
        else if (packet.type == PACKET_PONG)
        {
            g_lastRoundTripMs = GetTickCount() - packet.echoTick;
            if (fromPlayerId >= 0 &&
                fromPlayerId < TH07_MULTI_MAX_PLAYERS)
            {
                g_lastRoundTripMsByPlayer[fromPlayerId] =
                    g_lastRoundTripMs;
            }
            UpdateConnectionUiPeerStatus();
        }
        else if (packet.type == PACKET_CONTROL &&
                  packet.controlType == CONTROL_START_GAME)
        {
            SendConnectionUiControlBurst(CONTROL_START_GAME_ACK);
            if (!g_connectionUi.startRequested &&
                !g_connectionUi.startCommitted)
            {
                EnableWindow(g_connectionUi.startButton, FALSE);
                SetConnectionUiStatus("peer ready; waiting final launch...");
            }
        }
        else if (packet.type == PACKET_CONTROL &&
                  packet.controlType == CONTROL_START_GAME_ACK &&
                  g_mode == Netplay::MODE_HOST &&
                  g_connectionUi.startRequested &&
                  !g_connectionUi.startCommitted)
        {
            g_connectionUi.startAckMask |= (u8)(1 << fromPlayerId);
            if ((g_connectionUi.startAckMask & GetExpectedGuestMask()) ==
                GetExpectedGuestMask())
            {
                CommitConnectionUiNetworkStart();
            }
        }
        else if (packet.type == PACKET_CONTROL &&
                 packet.controlType == CONTROL_START_GAME_COMMIT)
        {
            SendConnectionUiControlBurst(CONTROL_START_GAME_COMMIT_ACK);
            AcceptConnectionUiNetworkStart(packet.controlFrame);
        }
        else if (packet.type == PACKET_CONTROL &&
                 packet.controlType == CONTROL_START_GAME_COMMIT_ACK &&
                 g_mode == Netplay::MODE_HOST &&
                 g_connectionUi.startRequested &&
                 g_connectionUi.startCommitted)
        {
            g_connectionUi.startCommitAckMask |=
                (u8)(1 << fromPlayerId);
            g_connectionUi.startCommitConfirmed =
                (g_connectionUi.startCommitAckMask &
                 GetExpectedGuestMask()) == GetExpectedGuestMask();
        }
    }

    now = GetTickCount();
    if (g_mode == Netplay::MODE_GUEST && !g_connected &&
        g_session == 0 &&
        g_connectionUi.attemptingConnection &&
        now - g_connectionUi.guestWaitStartedTick >
            CONNECTION_UI_GUEST_TIMEOUT_MS)
    {
        ResetConnectionUiAttempt("no connection");
        SetStatus("guest connection attempt timed out");
        return;
    }
    if (g_connected &&
        now - g_connectionUi.lastPingTick >= PING_INTERVAL_MS)
    {
        SendPing();
        g_connectionUi.lastPingTick = now;
    }
    if (g_connected && g_connectionUi.startRequested &&
        now - g_connectionUi.lastStartSendTick >=
            CONNECTION_UI_START_RESEND_MS)
    {
        if (g_connectionUi.startCommitted)
        {
            SendConnectionUiControlBurst(
                CONTROL_START_GAME_COMMIT, CONNECTION_UI_START_DELAY_MS);
        }
        else
        {
            SendConnectionUiControlBurst(CONTROL_START_GAME);
        }
        g_connectionUi.lastStartSendTick = now;
    }
    if (g_connectionUi.startCommitted &&
        g_connectionUi.launchAtTick != 0 &&
        now >= g_connectionUi.launchAtTick &&
        (!g_connectionUi.startRequested ||
         g_connectionUi.startCommitConfirmed))
    {
        FinishConnectionUiNetworkStart(false);
    }
}

HWND CreateConnectionUiControlAt(const char *className, const char *text,
                                 DWORD style, int x, int y, int width,
                                 int height, HWND parent, int id)
{
    return CreateWindowA(className, text, WS_CHILD | WS_VISIBLE | style, x, y,
                         width, height, parent, (HMENU)(INT_PTR)id,
                         GetModuleHandleA(NULL), NULL);
}

// The role selector sits above the rest of the form, so every other control
// moves down by its height. Applying the shift here keeps the layout below
// written in the coordinates it was designed with.
HWND CreateConnectionUiControl(const char *className, const char *text,
                               DWORD style, int x, int y, int width,
                               int height, HWND parent, int id)
{
    return CreateConnectionUiControlAt(className, text, style, x,
                                       y + UI_ROLE_ROW_HEIGHT, width, height,
                                       parent, id);
}

// The guest receives delay, rollback and player count from the host's WELCOME
// packet, so those controls are not its to set. Hiding them is clearer than
// showing values that will be overwritten on connect.
void SetConnectionUiRole(HWND window, bool guest)
{
    static const int hostOnlyIds[] = {
        UI_ID_DELAY_LABEL, UI_ID_DELAY, UI_ID_ROLLBACK,
        UI_ID_PLAYER_COUNT_LABEL, UI_ID_PLAYER_COUNT_2, UI_ID_PLAYER_COUNT_3
    };
    int show = guest ? SW_HIDE : SW_SHOW;
    int i;

    HWND hostSettings;

    for (i = 0; i < (int)(sizeof(hostOnlyIds) / sizeof(hostOnlyIds[0])); i++)
    {
        HWND control = GetDlgItem(window, hostOnlyIds[i]);
        if (control)
        {
            ShowWindow(control, show);
        }
    }
    // Reuses the space the hidden controls occupied - the two are never
    // wanted at the same time.
    hostSettings = GetDlgItem(window, UI_ID_HOST_SETTINGS);
    if (hostSettings)
    {
        ShowWindow(hostSettings,
                   guest && g_connectionUi.hostSettingsKnown ? SW_SHOW
                                                             : SW_HIDE);
    }
    CheckRadioButton(window, UI_ID_ROLE_HOST, UI_ID_ROLE_GUEST,
                     guest ? UI_ID_ROLE_GUEST : UI_ID_ROLE_HOST);
    if (g_connectionUi.connectButton)
    {
        SetWindowTextA(g_connectionUi.connectButton,
                       guest ? "Connect to host" : "Start hosting");
    }
}

// The evasive bot is a test aid, not something a normal session touches, so it
// stays behind the advanced toggle. Its value still lives in the config file
// either way - hiding a checkbox does not clear it, and ReadConnectionUiFields
// still reads it.
struct ConnectionUiMovedControl
{
    int id;
    int expandedY;
};

void SetConnectionUiAdvancedVisible(HWND window, bool visible)
{
    static const int advancedIds[] = {
        UI_ID_GUEST_EVASIVE_BOT, UI_ID_SHOW_NET_STATS,
        UI_ID_SHOW_STAGE_NAMES
    };
    // The y each control sits at while the advanced rows are shown.
    static const ConnectionUiMovedControl movedControls[] = {
        { UI_ID_CONNECT, 416 },
        { UI_ID_STATUS_LABEL, 456 }, { UI_ID_STATUS, 476 },
        { UI_ID_START_GAME, 556 }, { UI_ID_START_LOCAL, 556 },
        { UI_ID_CANCEL, 596 }
    };
    int shift = visible ? 0 : UI_ADVANCED_BLOCK_HEIGHT;
    int index;
    RECT windowRect;

    g_connectionUi.advancedVisible = visible;
    for (index = 0;
         index < (int)(sizeof(advancedIds) / sizeof(advancedIds[0]));
         index++)
    {
        HWND control = GetDlgItem(window, advancedIds[index]);
        if (control)
        {
            ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
        }
    }
    for (index = 0;
         index < (int)(sizeof(movedControls) / sizeof(movedControls[0]));
         index++)
    {
        HWND control = GetDlgItem(window, movedControls[index].id);
        RECT controlRect;
        POINT topLeft;
        if (!control || !GetWindowRect(control, &controlRect))
        {
            continue;
        }
        topLeft.x = controlRect.left;
        topLeft.y = controlRect.top;
        ScreenToClient(window, &topLeft);
        SetWindowPos(control, NULL, topLeft.x,
                     movedControls[index].expandedY + UI_ROLE_ROW_HEIGHT -
                         shift,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    if (GetWindowRect(window, &windowRect))
    {
        SetWindowPos(window, NULL, 0, 0,
                     windowRect.right - windowRect.left,
                     662 + UI_ROLE_ROW_HEIGHT - shift,
                     SWP_NOMOVE | SWP_NOZORDER);
    }
    InvalidateRect(window, NULL, TRUE);
    UpdateWindow(window);
}

// The guest cannot set delay, rollback or player count, but it still needs to
// know what it just agreed to. Reporting them after the WELCOME arrives is the
// point at which they are actually known.
void ShowConnectionUiHostSettings()
{
    char text[160];
    HWND control;

    if (!g_connectionUi.window || !IsWindow(g_connectionUi.window))
    {
        return;
    }
    control = GetDlgItem(g_connectionUi.window, UI_ID_HOST_SETTINGS);
    if (!control)
    {
        return;
    }
    sprintf(text, "From host: %s, delay %d, %d players",
            g_rollbackEnabled ? "rollback on" : "rollback off", g_delay,
            g_playerCount);
    SetWindowTextA(control, text);
    g_connectionUi.hostSettingsKnown = true;
    ShowWindow(control, SW_SHOW);
}

LRESULT CALLBACK ConnectionUiWndProc(HWND window, UINT message, WPARAM wParam,
                                     LPARAM lParam)
{
    (void)lParam;
    if (message == WM_CREATE)
    {
        char playerName[PLAYER_NAME_LENGTH];
        char port[32];
        char delay[32];
        int displayControlId;
        strncpy(playerName, g_connectionUi.selection.playerName,
                sizeof(playerName) - 1);
        playerName[sizeof(playerName) - 1] = '\0';
        sprintf(port, "%d", g_connectionUi.selection.port);
        sprintf(delay, "%d", g_connectionUi.selection.delay);
        // Above the shift, so it uses real window coordinates.
        CreateConnectionUiControlAt("STATIC", "Connect as:", SS_LEFT, 20, 14,
                                    100, 22, window, 0);
        CreateConnectionUiControlAt(
            "BUTTON", "Host", BS_AUTORADIOBUTTON | WS_GROUP, 122, 12, 90, 22,
            window, UI_ID_ROLE_HOST);
        CreateConnectionUiControlAt(
            "BUTTON", "Guest", BS_AUTORADIOBUTTON, 218, 12, 90, 22, window,
            UI_ID_ROLE_GUEST);

        CreateConnectionUiControl("STATIC", "Display mode:", SS_LEFT, 20,
                                  16, 120, 22, window, 0);
        CreateConnectionUiControl(
            "BUTTON", "Fullscreen (640 x 480)",
            BS_AUTORADIOBUTTON | WS_GROUP, 36, 42, 230, 22, window,
            UI_ID_DISPLAY_FULLSCREEN_640);
        CreateConnectionUiControl(
            "BUTTON", "Window (640 x 480)", BS_AUTORADIOBUTTON, 36, 66,
            230, 22, window, UI_ID_DISPLAY_WINDOW_640);
        CreateConnectionUiControl(
            "BUTTON", "Window (960 x 720)", BS_AUTORADIOBUTTON, 36, 90,
            230, 22, window, UI_ID_DISPLAY_WINDOW_960);
        CreateConnectionUiControl(
            "BUTTON", "Window (1280 x 960)", BS_AUTORADIOBUTTON, 36, 114,
            230, 22, window, UI_ID_DISPLAY_WINDOW_1280);
        displayControlId = UI_ID_DISPLAY_FULLSCREEN_640 +
            g_connectionUi.selection.displayMode;
        CheckRadioButton(window, UI_ID_DISPLAY_FULLSCREEN_640,
                         UI_ID_DISPLAY_WINDOW_1280, displayControlId);

        CreateConnectionUiControl("STATIC", "Audio:", SS_LEFT, 280, 16,
                                  110, 22, window, 0);
        g_connectionUi.bgmCheck = CreateConnectionUiControl(
            "BUTTON", "BGM", BS_AUTOCHECKBOX, 280, 42, 110, 22, window,
            UI_ID_BGM);
        g_connectionUi.seCheck = CreateConnectionUiControl(
            "BUTTON", "SE", BS_AUTOCHECKBOX, 280, 66, 110, 22, window,
            UI_ID_SE);
        SendMessageA(g_connectionUi.bgmCheck, BM_SETCHECK,
                     g_connectionUi.selection.bgmEnabled ? BST_CHECKED
                                                          : BST_UNCHECKED,
                     0);
        SendMessageA(g_connectionUi.seCheck, BM_SETCHECK,
                     g_connectionUi.selection.seEnabled ? BST_CHECKED
                                                         : BST_UNCHECKED,
                     0);
        CreateConnectionUiControl("STATIC", "Host players:", SS_LEFT,
                                  280, 96, 110, 20, window,
                                  UI_ID_PLAYER_COUNT_LABEL);
        CreateConnectionUiControl(
            "BUTTON", "2", BS_AUTORADIOBUTTON | WS_GROUP,
            280, 118, 48, 22, window, UI_ID_PLAYER_COUNT_2);
        CreateConnectionUiControl(
            "BUTTON", "3", BS_AUTORADIOBUTTON,
            334, 118, 48, 22, window, UI_ID_PLAYER_COUNT_3);
        CheckRadioButton(
            window, UI_ID_PLAYER_COUNT_2, UI_ID_PLAYER_COUNT_3,
            g_connectionUi.selection.playerCount == 3
                ? UI_ID_PLAYER_COUNT_3 : UI_ID_PLAYER_COUNT_2);

        CreateConnectionUiControl("STATIC", "Player name:", SS_LEFT, 20,
                                  154, 90, 22, window, 0);
        g_connectionUi.playerNameEdit = CreateConnectionUiControl(
            "EDIT", playerName, WS_BORDER | ES_AUTOHSCROLL, 110, 152, 270, 24,
            window, UI_ID_PLAYER_NAME);
        SendMessageA(g_connectionUi.playerNameEdit, EM_LIMITTEXT,
                     PLAYER_NAME_MAX_CHARS, 0);
        CreateConnectionUiControl("STATIC", "Host IP:", SS_LEFT, 20, 194, 80,
                                  22, window, 0);
        g_connectionUi.hostEdit = CreateConnectionUiControl(
            "EDIT", g_connectionUi.selection.host,
            WS_BORDER | ES_AUTOHSCROLL, 110, 192, 270, 24, window,
            UI_ID_HOST_IP);
        CreateConnectionUiControl("STATIC", "Port:", SS_LEFT, 20, 234, 80, 22,
                                  window, 0);
        g_connectionUi.portEdit = CreateConnectionUiControl(
            "EDIT", port, WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, 110, 232,
            100, 24, window, UI_ID_PORT);
        CreateConnectionUiControl("STATIC", "Fallback:", SS_LEFT, 20,
                                  274, 80, 22, window, UI_ID_DELAY_LABEL);
        g_connectionUi.delayEdit = CreateConnectionUiControl(
            "EDIT", delay, WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, 110, 272,
            100, 24, window, UI_ID_DELAY);
        CreateConnectionUiControl("STATIC", "", SS_LEFT, 20, 274, 360, 22,
                                  window, UI_ID_HOST_SETTINGS);
        g_connectionUi.rollbackCheck = CreateConnectionUiControl(
            "BUTTON", "Predictive rollback (lowest input lag)",
            BS_AUTOCHECKBOX, 20, 306, 330, 24, window, UI_ID_ROLLBACK);
        SendMessageA(g_connectionUi.rollbackCheck, BM_SETCHECK,
                     g_connectionUi.selection.rollback ? BST_CHECKED
                                                       : BST_UNCHECKED,
                     0);
        if (g_connectionUi.selection.playerCount == 3)
        {
            SendMessageA(g_connectionUi.rollbackCheck, BM_SETCHECK,
                         BST_CHECKED, 0);
            EnableWindow(g_connectionUi.rollbackCheck, FALSE);
        }
        CreateConnectionUiControl(
            "BUTTON", UI_ADVANCED_LABEL, BS_PUSHBUTTON, 20, 334, 170, 24,
            window, UI_ID_ADVANCED);
        g_connectionUi.evasiveBotCheck = CreateConnectionUiControl(
            "BUTTON", "Guest evasive bot (test)",
            BS_AUTOCHECKBOX, 20, 362, 215, 22, window,
            UI_ID_GUEST_EVASIVE_BOT);
        g_connectionUi.netStatsCheck = CreateConnectionUiControl(
            "BUTTON", "Net diagnostics",
            BS_AUTOCHECKBOX, 245, 362, 145, 22, window,
            UI_ID_SHOW_NET_STATS);
        g_connectionUi.stageNamesCheck = CreateConnectionUiControl(
            "BUTTON", "Show player names at stage start",
            BS_AUTOCHECKBOX, 20, 386, 370, 22, window,
            UI_ID_SHOW_STAGE_NAMES);
        SendMessageA(g_connectionUi.netStatsCheck, BM_SETCHECK,
                     g_connectionUi.selection.showNetStats ? BST_CHECKED
                                                           : BST_UNCHECKED,
                     0);
        SendMessageA(g_connectionUi.stageNamesCheck, BM_SETCHECK,
                     g_connectionUi.selection.showStageNames
                         ? BST_CHECKED : BST_UNCHECKED,
                     0);
        SendMessageA(g_connectionUi.evasiveBotCheck, BM_SETCHECK,
                     g_connectionUi.selection.evasiveBot ? BST_CHECKED
                                                         : BST_UNCHECKED,
                     0);
        g_connectionUi.connectButton = CreateConnectionUiControl(
            "BUTTON", "Start hosting", BS_PUSHBUTTON, 20, 416, 370, 32,
            window, UI_ID_CONNECT);
        CreateConnectionUiControl("STATIC", "cur state:", SS_LEFT, 20, 456,
                                  80, 20, window, UI_ID_STATUS_LABEL);
        g_connectionUi.status = CreateConnectionUiControl(
            "STATIC", "no connection", SS_LEFT | WS_BORDER, 20, 476, 370,
            72, window, UI_ID_STATUS);
        g_connectionUi.startButton = CreateConnectionUiControl(
            "BUTTON", "Start Game",
            BS_DEFPUSHBUTTON | WS_DISABLED, 20, 556, 160, 32, window,
            UI_ID_START_GAME);
        CreateConnectionUiControl("BUTTON", "Start Game (local)",
                                  BS_PUSHBUTTON, 220, 556, 160, 32, window,
                                  UI_ID_START_LOCAL);
        CreateConnectionUiControl("BUTTON", "Cancel", BS_PUSHBUTTON, 280,
                                  596, 100, 28, window, UI_ID_CANCEL);
        SetConnectionUiRole(
            window, g_connectionUi.selection.mode == Netplay::MODE_GUEST);
        SetConnectionUiAdvancedVisible(window, false);
        SetTimer(window, CONNECTION_UI_TIMER_ID,
                 CONNECTION_UI_TIMER_INTERVAL_MS, NULL);
        return 0;
    }
    if (message == WM_COMMAND && HIWORD(wParam) == EN_CHANGE &&
        LOWORD(wParam) == UI_ID_PLAYER_NAME)
    {
        char raw[128];
        char filtered[PLAYER_NAME_MAX_CHARS + 1];
        int length;
        raw[0] = '\0';
        GetWindowTextA(g_connectionUi.playerNameEdit, raw, sizeof(raw));
        SanitizePlayerName(raw, sizeof(raw), filtered, sizeof(filtered));
        if (strcmp(raw, filtered) != 0)
        {
            SetWindowTextA(g_connectionUi.playerNameEdit, filtered);
            length = (int)strlen(filtered);
            SendMessageA(g_connectionUi.playerNameEdit, EM_SETSEL, length,
                         length);
        }
        return 0;
    }
    if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED)
    {
        if (LOWORD(wParam) == UI_ID_PLAYER_COUNT_3)
        {
            SendMessageA(g_connectionUi.rollbackCheck, BM_SETCHECK,
                         BST_CHECKED, 0);
            EnableWindow(g_connectionUi.rollbackCheck, FALSE);
            return 0;
        }
        if (LOWORD(wParam) == UI_ID_PLAYER_COUNT_2)
        {
            EnableWindow(g_connectionUi.rollbackCheck, TRUE);
            return 0;
        }
        switch (LOWORD(wParam))
        {
        case UI_ID_ADVANCED:
            SetConnectionUiAdvancedVisible(
                window, !g_connectionUi.advancedVisible);
            return 0;
        case UI_ID_ROLE_HOST:
            SetConnectionUiRole(window, false);
            return 0;
        case UI_ID_ROLE_GUEST:
            SetConnectionUiRole(window, true);
            return 0;
        case UI_ID_CONNECT:
        {
            bool guest =
                IsDlgButtonChecked(window, UI_ID_ROLE_GUEST) ==
                BST_CHECKED;
            SetConnectionUiRole(window, guest);
            BeginConnectionUiNetwork(guest ? Netplay::MODE_GUEST
                                           : Netplay::MODE_HOST);
            return 0;
        }
        case UI_ID_START_LOCAL:
            if (ReadConnectionUiFields())
            {
                SetLocalPlayerName(g_connectionUi.selection.playerName);
                SaveConnectionUiConfig(&g_connectionUi.selection);
            }
            CloseNetworkSocket();
            g_connectionUi.selection.mode = Netplay::MODE_LOCAL;
            g_mode = Netplay::MODE_LOCAL;
            g_connectionUi.finished = true;
            DestroyWindow(window);
            return 0;
        case UI_ID_START_GAME:
            FinishConnectionUiNetworkStart(true);
            return 0;
        case UI_ID_CANCEL:
            if (g_connectionUi.attemptingConnection)
            {
                // Backing out of a search is not the same as quitting.
                // Drop the session and return to the settings the search
                // was started from, so the port, display mode, delay and
                // the rest can be changed and the search retried without
                // relaunching the game.
                NotifyPeerExit();
                ResetConnectionUiAttempt("search cancelled");
                return 0;
            }
            g_connectionUi.selection.cancelled = true;
            g_connectionUi.finished = true;
            NotifyPeerExit();
            CloseNetworkSocket();
            DestroyWindow(window);
            return 0;
        default:
            break;
        }
    }
    if (message == WM_TIMER && wParam == CONNECTION_UI_TIMER_ID)
    {
        PollConnectionUiNetwork();
        return 0;
    }
    if (message == WM_CLOSE)
    {
        g_connectionUi.selection.cancelled = true;
        g_connectionUi.finished = true;
        NotifyPeerExit();
        CloseNetworkSocket();
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY)
    {
        KillTimer(window, CONNECTION_UI_TIMER_ID);
        if (!g_connectionUi.finished)
        {
            g_connectionUi.selection.cancelled = true;
            g_connectionUi.finished = true;
            CloseNetworkSocket();
        }
        g_connectionUi.window = NULL;
        return 0;
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

bool RunConnectionUi(ConnectionUiSelection *selection, bool rollbackAllowed)
{
    WNDCLASSA windowClass;
    MSG message;
    HINSTANCE instance = GetModuleHandleA(NULL);
    const char *className = "th07_multi_net_connection_ui";

    g_connectionUiLaunchMode = true;
    memset(&g_connectionUi, 0, sizeof(g_connectionUi));
    g_connectionUi.selection.mode = Netplay::MODE_SINGLE;
    LoadConnectionUiConfig(&g_connectionUi.selection);
    SetLocalPlayerName(g_connectionUi.selection.playerName);
    if (g_displayMode != DISPLAY_MODE_FROM_GAME_CONFIG)
    {
        g_connectionUi.selection.displayMode = g_displayMode;
    }
    g_connectionUi.rollbackAllowed = rollbackAllowed;
    if (!rollbackAllowed)
    {
        g_connectionUi.selection.rollback = false;
    }
    g_connectionUi.selection.cancelled = false;
    memset(&windowClass, 0, sizeof(windowClass));
    windowClass.lpfnWndProc = ConnectionUiWndProc;
    windowClass.hInstance = instance;
    windowClass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&windowClass);
    g_connectionUi.window = CreateWindowExA(
        WS_EX_APPWINDOW, className, "th07_multi_net - Connection",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
         CW_USEDEFAULT, CW_USEDEFAULT, 430,
         662 + UI_ROLE_ROW_HEIGHT - UI_ADVANCED_BLOCK_HEIGHT, NULL,
         NULL, instance, NULL);
    if (!g_connectionUi.window)
    {
        SetStatus("connection UI could not be created");
        return false;
    }
    ShowWindow(g_connectionUi.window, SW_SHOW);
    UpdateWindow(g_connectionUi.window);
    // A remote BM_CLICK can set finished while GetMessage is blocked. Pump
    // with PeekMessage so the loop observes that flag without waiting for a
    // second message after the dialog has already been destroyed.
    while (!g_connectionUi.finished)
    {
        if (!PeekMessageA(&message, NULL, 0, 0, PM_REMOVE))
        {
            Sleep(1);
            continue;
        }
        if (message.message == WM_QUIT)
        {
            g_connectionUi.selection.cancelled = true;
            g_connectionUi.finished = true;
            NotifyPeerExit();
            break;
        }
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    if (g_connectionUi.window)
    {
        DestroyWindow(g_connectionUi.window);
        g_connectionUi.window = NULL;
    }
    if (g_connectionUi.selection.cancelled ||
        (g_connectionUi.selection.mode != Netplay::MODE_HOST &&
         g_connectionUi.selection.mode != Netplay::MODE_GUEST &&
         g_connectionUi.selection.mode != Netplay::MODE_LOCAL))
    {
        CloseNetworkSocket();
    }
    *selection = g_connectionUi.selection;
    return !g_connectionUi.selection.cancelled;
}

void SetStatus(const char *text)
{
    strncpy(g_status, text, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
    OutputDebugStringA("th07_multi_net: ");
    OutputDebugStringA(g_status);
    OutputDebugStringA("\r\n");
}

void ResetInputRings()
{
    int i;
    int playerId;
    for (i = 0; i < INPUT_RING_SIZE; i++)
    {
        g_localFrames[i] = INVALID_FRAME;
        g_localInputs[i] = 0;
        g_localRng[i] = 0;
        g_localControls[i] = Netplay::INGAME_CONTROL_NONE;
        g_scheduledLifecycleFrames[i] = INVALID_FRAME;
        g_scheduledLifecycleControls[i] =
            Netplay::INGAME_CONTROL_NONE;
        g_localStateHash[i] = 0;
        g_localPlayerHash[i] = 0;
        g_localWorldHash[i] = 0;
        g_localSpellHash[i] = 0;
        g_localRollbackGameplay[i] = 0;
        for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
        {
            g_remoteFramesByPlayer[playerId][i] = INVALID_FRAME;
            g_remoteInputsByPlayer[playerId][i] = 0;
            g_remoteRngByPlayer[playerId][i] = 0;
            g_remoteControlsByPlayer[playerId][i] =
                Netplay::INGAME_CONTROL_NONE;
            g_remoteStateHashByPlayer[playerId][i] = 0;
            g_remotePlayerHashByPlayer[playerId][i] = 0;
            g_remoteWorldHashByPlayer[playerId][i] = 0;
            g_remoteSpellHashByPlayer[playerId][i] = 0;
            g_remoteRollbackGameplayByPlayer[playerId][i] = 0;
            g_predictedRemoteInputsByPlayer[playerId][i] = 0;
            g_predictedRemoteControlsByPlayer[playerId][i] =
                Netplay::INGAME_CONTROL_NONE;
            g_predictedRemoteFramesByPlayer[playerId][i] = INVALID_FRAME;
        }
    }
    for (i = 0; i < ROLLBACK_SNAPSHOT_COUNT; i++)
    {
        g_rollbackSnapshots[i].simulationFrame = INVALID_FRAME;
    }
    g_frame = 0;
    {
        DWORD now = GetTickCount();
        for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
        {
            g_lastPeerInputAdvanceTick[playerId] = now;
            g_lastPeerInputFrame[playerId] = INVALID_FRAME;
            g_peerSyntheticThroughFrame[playerId] = INVALID_FRAME;
            g_peerNeutralStartFrame[playerId] = INVALID_FRAME;
            g_peerResumeFrame[playerId] = INVALID_FRAME;
            g_peerLeftFrame[playerId] = INVALID_FRAME;
            g_lastLifecycleAnnounceTick[playerId] = 0;
            g_hostPeerLifecycleStage[playerId] = HOST_PEER_PRESENT;
        }
    }
    g_peerExitNoticeMask = 0;
    g_absentPlayerMask = 0;
    g_departedPlayerMask = 0;
    g_waitStartedTick = 0;
    g_waitStatusTick = 0;
    g_lastInputSendTick = 0;
    g_lastPingTick = 0;
    g_lastRoundTripMs = 0;
    memset(g_lastRoundTripMsByPlayer, 0,
           sizeof(g_lastRoundTripMsByPlayer));
    g_pingSequence = 0;
    g_waitingForRemoteInput = false;
    g_inputStallCount = 0;
    g_inputStallTotalMs = 0;
    g_inputStallMaxMs = 0;
    g_gameplayInputStallCount = 0;
    g_gameplayInputStallTotalMs = 0;
    g_gameplayInputStallMaxMs = 0;
    g_inputTimingSummaryLogged = false;
    g_rollbackTimingSummaryLogged = false;
    g_controllerLaneLogged = false;
    g_controllerConfigLogged = false;
    g_startupFrameBarrierLogged = false;
    g_lastDetailedStateComparedFrame = INVALID_FRAME;
    g_lastRollbackRngComparedFrame = INVALID_FRAME;
    g_lifecycleComparisonIgnoreUntilFrame = INVALID_FRAME;
    g_detailedStateMismatch = false;
    g_firstDetailedStateMismatchFrame = INVALID_FRAME;
    g_lastLoggedSpellActive = -1;
    g_lastLoggedSpellState = -1;
    g_lastLoggedSpellIndex = -1;
    g_lastBossTraceFrame = INVALID_FRAME;
    g_bossTraceAccumulator = 0;
    g_bossTraceSamples = 0;
    for (int checkpoint = 0; checkpoint < BOSS_CHECKPOINT_COUNT; checkpoint++)
    {
        g_bossCheckpointFrames[checkpoint] = INVALID_FRAME;
        g_bossCheckpointAccumulators[checkpoint] = 0;
    }
    g_bossPendingSampleFrame = INVALID_FRAME;
    ResetPlayerLifecycleTrace();
    g_bossPendingBossCount = 0;
    g_bossPendingLifeSum = 0;
    g_bossPendingLife0 = -1;
    g_bossPendingMaxLife0 = -1;
    g_spellCheckpointCount = 0;
    g_rollbackSnapshotStage = -1;
    g_rollbackStageInvalidations = 0;
    g_bossDeathLogged = 0;
    g_rollbackDeferralLogged = false;
    g_rollbackWindowExhaustedLogged = false;
    g_rollbackSnapshotMissingLogged = false;
    g_simulationDivergenceLogged = false;
    g_lastLoggedControllerInput = 0;
    g_controllerInputLogCount = 0;
    g_connectionFailed = false;
    g_peerExitReceived = false;
    g_rngMismatch = false;
    g_protocolMismatch = false;
    g_reconnectKeyDown = false;
    g_controlTestFrame = 0;
    g_testRngMismatchInjected = false;
    g_testStateMismatchInjected = false;
    g_testResultPolicyFrames = 0;
    g_testReplayBlockInjected = false;
    g_testUiSyncInjected = false;
    g_testUiSyncVerified = false;
    g_testUiSyncFailureReported = false;
    g_testUiSyncUiFrame = false;
    g_testInputSyncInjected = false;
    g_testInputSyncVerified = false;
    g_testInputSyncFailureReported = false;
    g_testInputSyncLocalFrame = 0;
    g_testProximityVerified = false;
    g_testProximityFailureReported = false;
    g_testLifeTransferVerified = false;
    g_testLifeTransferFailureReported = false;
    g_testDamageHitCount[0] = 0;
    g_testDamageHitCount[1] = 0;
    g_testDamageSpiritSeen = false;
    g_testDamageReviveSeen = false;
    g_testDamageSummaryLogged = false;
    g_testDamageFailureReported = false;
    g_autoBombPulseCount = 0;
    g_synchronizedBombPulseCount = 0;
    g_testRandomInputLogged = false;
    g_testRandomInputBombPulseCount = 0;
    g_testEvasiveInputLogged = false;
    g_testEvasiveInputBombPulseCount = 0;
    g_testResourceDropsVerified = false;
    g_testP2FeaturesSetup = false;
    g_testP3FeaturesSetup = false;
    g_testRollbackBorderActivated = false;
    g_testRollbackP1BorderVerified = false;
    g_testRollbackP2BorderVerified = false;
    g_testRollbackBorderStartFrame = INVALID_FRAME;
    g_testDeferredPollFrame = INVALID_FRAME;
    g_testDeferredPollFrameCount = 0;
    g_lastLoggedTestStage = 0;
    g_localP2KeyBindingsLoaded = false;
    g_inputArmed = false;
    g_previousControlKeys = 0;
    g_synchronizedControl = Netplay::INGAME_CONTROL_NONE;
    g_resyncFrame = INVALID_FRAME;
    g_resyncIgnoreUntilFrame = INVALID_FRAME;
    g_lastResyncSendTick = 0;
    g_resyncAwaitingAckMask = 0;
    g_quickStartReadySent = false;
    g_quickStartRemoteReady = false;
    g_quickStartReadyMask = 0;
    g_quickStartFrame = INVALID_FRAME;
    g_lastQuickStartSendTick = 0;
    g_rollbackReplaying = false;
    g_rollbackEverEnabled = g_rollbackEnabled;
    g_rollbackPredictionActive = false;
    g_rollbackEarliestFrame = INVALID_FRAME;
    g_rollbackCount = 0;
    g_rollbackPredictedFrames = 0;
    g_rollbackReplayFrames = 0;
    g_rollbackMaxReplayFrames = 0;
    g_rollbackReplayTimeUs = 0;
    g_rollbackMaxReplayTimeUs = 0;
    g_rollbackRestoredBombEffects = 0;
    g_rollbackMaxBombEffects = 0;
    g_rollbackPredictionRefreshFrames = 0;
    g_rollbackGameplayFrames = 0;
    g_rollbackGameplayStartedTick = 0;
    g_lastRollbackStateSignature = -1;
}

bool SameAddress(const sockaddr_in &a, const sockaddr_in &b)
{
    return a.sin_family == b.sin_family &&
           a.sin_port == b.sin_port &&
           a.sin_addr.s_addr == b.sin_addr.s_addr;
}

bool ResolveAddress(const char *host, int port, sockaddr_in *out)
{
    unsigned long address;
    hostent *resolved;

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons((u_short)port);
    address = inet_addr(host);
    if (address != INADDR_NONE)
    {
        out->sin_addr.s_addr = address;
        return true;
    }

    resolved = gethostbyname(host);
    if (!resolved || resolved->h_addrtype != AF_INET || !resolved->h_addr_list[0])
    {
        return false;
    }
    memcpy(&out->sin_addr, resolved->h_addr_list[0], sizeof(out->sin_addr));
    return true;
}

void CloseNetworkSocket()
{
    if (g_socket != INVALID_SOCKET)
    {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
    if (g_winsockStarted)
    {
        WSACleanup();
        g_winsockStarted = false;
    }
    memset(g_peerAddresses, 0, sizeof(g_peerAddresses));
    memset(g_peerPresent, 0, sizeof(g_peerPresent));
    g_connectedPlayerMask = (u8)(1 << g_localPlayerSlot);
    g_connected = false;
}

bool CreateSocket(int port)
{
    WSADATA data;
    sockaddr_in local;
    u_long nonBlocking = 1;
    BOOL reuse = TRUE;

    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        SetStatus("WSAStartup failed");
        return false;
    }
    g_winsockStarted = true;
    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET)
    {
        SetStatus("socket creation failed");
        CloseNetworkSocket();
        return false;
    }
    setsockopt(g_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse,
               sizeof(reuse));
    if (ioctlsocket(g_socket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        SetStatus("non-blocking socket setup failed");
        CloseNetworkSocket();
        return false;
    }

    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons((u_short)port);
    if (bind(g_socket, (sockaddr *)&local, sizeof(local)) == SOCKET_ERROR)
    {
        SetStatus("UDP port is already in use");
        CloseNetworkSocket();
        return false;
    }
    return true;
}

void InitializePacket(NetPacket *packet, PacketType type)
{
    int i;
    int playerId;
    int relayIndex;
    memset(packet, 0, sizeof(*packet));
    packet->magic = NETPLAY_MAGIC;
    packet->version = NETPLAY_VERSION;
    packet->type = (u16)type;
    packet->session = g_session;
    packet->delay = (u16)g_delay;
    packet->initialRngSeed = g_initialRngSeed;
    packet->flags = 0;
    if (g_invincible)
    {
        packet->flags |= NETPLAY_FLAG_INVINCIBLE;
    }
    if (g_demoDisabled)
    {
        packet->flags |= NETPLAY_FLAG_NO_DEMO;
    }
    if (g_quickStartEnabled)
    {
        packet->flags |= NETPLAY_FLAG_QUICK_START;
    }
    if (g_autoShoot)
    {
        packet->flags |= NETPLAY_FLAG_AUTO_SHOOT;
    }
    if (g_autoSkip)
    {
        packet->flags |= NETPLAY_FLAG_AUTO_SKIP;
    }
    if (g_autoBomb)
    {
        packet->flags |= NETPLAY_FLAG_AUTO_BOMB;
    }
    if (g_ignoreControllerInput)
    {
        packet->flags |= NETPLAY_FLAG_NO_CONTROLLER;
    }
    if (g_noSave)
    {
        packet->flags |= NETPLAY_FLAG_NO_SAVE;
    }
    if (g_connectionUiLaunchMode)
    {
        packet->flags |= NETPLAY_FLAG_CONNECTION_UI;
    }
    if (g_p2LoadoutConfigured && g_p2Character >= 0 &&
        g_p2Character <= 2 && g_p2Shot >= 0 && g_p2Shot <= 1)
    {
        packet->flags |= NETPLAY_FLAG_P2_LOADOUT;
    }
    if (g_rollbackEnabled)
    {
        packet->flags |= NETPLAY_FLAG_ROLLBACK;
    }
    if (g_testDamageEventsEnabled)
    {
        packet->flags |= NETPLAY_FLAG_TEST_DAMAGE_EVENTS;
    }
    packet->quickDifficulty = (u8)g_quickDifficulty;
    packet->quickCharacter = (u8)g_quickCharacter;
    packet->quickShot = (u8)g_quickShot;
    packet->quickStage = (u8)g_quickStage;
    packet->quickPractice = g_quickStartPractice ? 1 : 0;
    packet->quickCharacter2 =
        g_p2Character >= 0 && g_p2Character <= 2 ? (u8)g_p2Character : 0xff;
    packet->quickShot2 =
        g_p2Shot >= 0 && g_p2Shot <= 1 ? (u8)g_p2Shot : 0xff;
    packet->quickCharacter3 =
        g_p3Character >= 0 && g_p3Character <= 2 ? (u8)g_p3Character : 0xff;
    packet->quickShot3 =
        g_p3Shot >= 0 && g_p3Shot <= 1 ? (u8)g_p3Shot : 0xff;
    packet->sourceSlot = (u8)g_localPlayerSlot;
    packet->playerCount = (u8)g_playerCount;
    packet->assignedSlot = 0xff;
    packet->connectedPlayerMask = 0;
    if (g_mode == Netplay::MODE_HOST)
    {
        packet->connectedPlayerMask = 1;
        for (playerId = 1; playerId < g_playerCount; playerId++)
        {
            if (g_peerPresent[playerId])
            {
                packet->connectedPlayerMask |= (u8)(1 << playerId);
            }
        }
    }
    packet->relaySlotCount = 0;
    for (relayIndex = 0; relayIndex < TH07_MULTI_MAX_GUESTS;
         relayIndex++)
    {
        packet->relaySlots[relayIndex] = 0xff;
    }
    strncpy(packet->playerName, g_localPlayerName, PLAYER_NAME_LENGTH - 1);
    packet->playerName[PLAYER_NAME_LENGTH - 1] = '\0';
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        strncpy(packet->playerNames[playerId],
                Netplay::GetPlayerName((u8)playerId),
                PLAYER_NAME_LENGTH - 1);
        packet->playerNames[playerId][PLAYER_NAME_LENGTH - 1] = '\0';
    }
    packet->newestFrame = INVALID_FRAME;
    for (i = 0; i < REDUNDANT_INPUT_COUNT; i++)
    {
        packet->records[i].frame = INVALID_FRAME;
    }
    for (i = 0; i < VERIFICATION_RECORD_COUNT; i++)
    {
        packet->verifications[i].frame = INVALID_FRAME;
    }
    for (relayIndex = 0; relayIndex < TH07_MULTI_MAX_GUESTS;
         relayIndex++)
    {
        for (i = 0; i < RELAY_INPUT_COUNT; i++)
        {
            packet->relayRecords[relayIndex][i].frame = INVALID_FRAME;
        }
    }
}

bool SendPacket(const NetPacket &packet)
{
    return SendPacketToPlayer(packet, GetPrimaryRemotePlayerId());
}

bool SendPacketToPlayer(const NetPacket &packet, int playerId)
{
    int sent;
    if (g_socket == INVALID_SOCKET ||
        playerId < 0 || playerId >= TH07_MULTI_MAX_PLAYERS ||
        !g_peerPresent[playerId])
    {
        return false;
    }
    sent = sendto(g_socket, (const char *)&packet, sizeof(packet), 0,
                  (const sockaddr *)&g_peerAddresses[playerId],
                  sizeof(g_peerAddresses[playerId]));
    return sent == sizeof(packet);
}

void SendPacketToAllPeers(const NetPacket &packet)
{
    int playerId;
    for (playerId = 0; playerId < g_playerCount; playerId++)
    {
        if (playerId != g_localPlayerSlot && g_peerPresent[playerId])
        {
            SendPacketToPlayer(packet, playerId);
        }
    }
}

int AssignHostGuestSlot(const sockaddr_in &address, const char *playerName)
{
    int playerId = FindPeerPlayerId(address);
    if (playerId >= 1)
    {
        SetSessionPlayerName((u8)playerId, playerName);
        return playerId;
    }
    for (playerId = 1; playerId < g_playerCount; playerId++)
    {
        if (!g_peerPresent[playerId])
        {
            g_peerAddresses[playerId] = address;
            g_peerPresent[playerId] = true;
            g_connectedPlayerMask |= (u8)(1 << playerId);
            g_lastPeerInputAdvanceTick[playerId] = GetTickCount();
            g_lastPeerInputFrame[playerId] = INVALID_FRAME;
            g_hostPeerLifecycleStage[playerId] = HOST_PEER_PRESENT;
            SetSessionPlayerName((u8)playerId, playerName);
            g_GameErrorContext.Log(
                "info : Host assigned guest %s:%u to P%d (%d/%d guests)\r\n",
                inet_ntoa(address.sin_addr),
                (unsigned)ntohs(address.sin_port), playerId + 1,
                CountConnectedGuestPeers(), g_playerCount - 1);
            return playerId;
        }
    }
    return -1;
}

void SendWelcomeToPlayer(int playerId, int repeatCount)
{
    NetPacket welcome;
    int repeat;
    if (playerId < 1 || playerId >= g_playerCount ||
        !g_peerPresent[playerId])
    {
        return;
    }
    InitializePacket(&welcome, PACKET_WELCOME);
    welcome.sourceSlot = 0;
    welcome.assignedSlot = (u8)playerId;
    welcome.playerCount = (u8)g_playerCount;
    for (repeat = 0; repeat < repeatCount; repeat++)
    {
        SendPacketToPlayer(welcome, playerId);
    }
}

void SendWelcomeToAllGuests(int repeatCount)
{
    int playerId;
    for (playerId = 1; playerId < g_playerCount; playerId++)
    {
        SendWelcomeToPlayer(playerId, repeatCount);
    }
}

void SendPing()
{
    NetPacket packet;
    InitializePacket(&packet, PACKET_PING);
    packet.sendTick = GetTickCount();
    packet.echoTick = g_pingSequence++;
    SendPacketToAllPeers(packet);
}

void SendControl(ControlType type, u32 frame, u16 value)
{
    NetPacket packet;
    InitializePacket(&packet, PACKET_CONTROL);
    packet.controlType = (u16)type;
    packet.controlValue = value;
    packet.controlFrame = frame;
    SendPacketToAllPeers(packet);
}

bool IsLifecycleControl(Netplay::InGameControl control)
{
    return control >= Netplay::INGAME_CONTROL_P2_ABSENT &&
        control <= Netplay::INGAME_CONTROL_P3_LEFT;
}

int GetLifecycleControlPlayerId(Netplay::InGameControl control)
{
    if (control >= Netplay::INGAME_CONTROL_P2_ABSENT &&
        control <= Netplay::INGAME_CONTROL_P2_LEFT)
    {
        return 1;
    }
    if (control >= Netplay::INGAME_CONTROL_P3_ABSENT &&
        control <= Netplay::INGAME_CONTROL_P3_LEFT)
    {
        return 2;
    }
    return -1;
}

Netplay::InGameControl GetAbsentControlForPlayer(int playerId)
{
    return playerId == 1 ? Netplay::INGAME_CONTROL_P2_ABSENT
                         : Netplay::INGAME_CONTROL_P3_ABSENT;
}

Netplay::InGameControl GetResumeControlForPlayer(int playerId)
{
    return playerId == 1 ? Netplay::INGAME_CONTROL_P2_RESUME
                         : Netplay::INGAME_CONTROL_P3_RESUME;
}

Netplay::InGameControl GetLeftControlForPlayer(int playerId)
{
    return playerId == 1 ? Netplay::INGAME_CONTROL_P2_LEFT
                         : Netplay::INGAME_CONTROL_P3_LEFT;
}

bool IsAbsentLifecycleControl(Netplay::InGameControl control)
{
    return control == Netplay::INGAME_CONTROL_P2_ABSENT ||
        control == Netplay::INGAME_CONTROL_P3_ABSENT;
}

bool IsResumeLifecycleControl(Netplay::InGameControl control)
{
    return control == Netplay::INGAME_CONTROL_P2_RESUME ||
        control == Netplay::INGAME_CONTROL_P3_RESUME;
}

bool IsLeftLifecycleControl(Netplay::InGameControl control)
{
    return control == Netplay::INGAME_CONTROL_P2_LEFT ||
        control == Netplay::INGAME_CONTROL_P3_LEFT;
}

bool IsInputRoutingSlotActive(int playerId)
{
    // Player objects only exist while a stage runs: CutChain clears every
    // slot but P1 on the way back to a menu. Routing the input lanes by
    // that flag silenced a guest's own menu presses, because a guest owns
    // lane P2 or P3 and those are inactive outside gameplay.
    if (playerId < 0 || playerId >= g_playerCount)
    {
        return false;
    }
    if (playerId == 0)
    {
        return true;
    }
    return !Netplay::IsPlayerPermanentlyDeparted((u8)playerId);
}

bool ShouldForceNeutralPlayerInput(u32 frame, int playerId)
{
    Netplay::InGameControl lifecycle =
        GetScheduledLifecycleControl(frame);
    int lifecyclePlayerId = GetLifecycleControlPlayerId(lifecycle);
    bool forceNeutral;

    if (playerId < 1 || playerId >= g_playerCount)
    {
        return false;
    }
    forceNeutral = (g_absentPlayerMask & (1 << playerId)) != 0;
    if (lifecyclePlayerId == playerId)
    {
        if (IsAbsentLifecycleControl(lifecycle) ||
            IsLeftLifecycleControl(lifecycle))
        {
            forceNeutral = true;
        }
        else if (IsResumeLifecycleControl(lifecycle))
        {
            forceNeutral = false;
        }
    }
    return forceNeutral;
}

void ApplyLifecycleInputPolicy(
    u32 frame, u16 inputs[TH07_MULTI_MAX_PLAYERS],
    u16 controls[TH07_MULTI_MAX_PLAYERS])
{
    Netplay::InGameControl lifecycle =
        GetScheduledLifecycleControl(frame);
    int playerId;

    if (lifecycle != Netplay::INGAME_CONTROL_NONE)
    {
        controls[0] = (u16)lifecycle;
    }
    for (playerId = 1; playerId < g_playerCount; playerId++)
    {
        if (ShouldForceNeutralPlayerInput(frame, playerId))
        {
            inputs[playerId] = 0;
        }
    }
}

Netplay::InGameControl GetScheduledLifecycleControl(u32 frame)
{
    int slot = (int)(frame % INPUT_RING_SIZE);
    if (g_scheduledLifecycleFrames[slot] == frame)
    {
        return (Netplay::InGameControl)g_scheduledLifecycleControls[slot];
    }
    return Netplay::INGAME_CONTROL_NONE;
}

u32 ScheduleLifecycleControl(Netplay::InGameControl control, u32 frame,
                             bool announce)
{
    int slot;
    int playerId = GetLifecycleControlPlayerId(control);
    Netplay::InGameControl existing;
    bool newlyScheduled;

    if (!IsLifecycleControl(control) || playerId < 1 ||
        playerId >= g_playerCount || frame == INVALID_FRAME)
    {
        return INVALID_FRAME;
    }
    if (frame <= g_frame && announce)
    {
        frame = g_frame + 1;
    }
    // Only one compact control value fits in an input record. Host-owned
    // events are moved by at most a couple of frames if both guests change
    // lifecycle state at once; receivers must preserve the advertised frame.
    for (;;)
    {
        slot = (int)(frame % INPUT_RING_SIZE);
        existing = g_scheduledLifecycleFrames[slot] == frame
            ? (Netplay::InGameControl)g_scheduledLifecycleControls[slot]
            : Netplay::INGAME_CONTROL_NONE;
        if (existing == Netplay::INGAME_CONTROL_NONE || existing == control)
        {
            break;
        }
        if (!announce)
        {
            g_GameErrorContext.Log(
                "error : conflicting lifecycle controls %d/%d at frame %lu\r\n",
                (int)existing, (int)control, (unsigned long)frame);
            return INVALID_FRAME;
        }
        frame++;
    }

    newlyScheduled = existing != control;
    g_scheduledLifecycleFrames[slot] = frame;
    g_scheduledLifecycleControls[slot] = (u16)control;
    if (newlyScheduled)
    {
        u32 ignoreUntil = frame >
                INVALID_FRAME - (u32)DETAILED_STATE_CONFIRM_LAG
            ? INVALID_FRAME - 1
            : frame + (u32)DETAILED_STATE_CONFIRM_LAG;
        if (g_lifecycleComparisonIgnoreUntilFrame == INVALID_FRAME ||
            ignoreUntil > g_lifecycleComparisonIgnoreUntilFrame)
        {
            // The Host may promote up to the whole prediction tail when a
            // peer disappears. Healthy peers need time to receive that
            // authoritative history and replay it before their old RNG/hash
            // metadata is meaningful. Suppress diagnostics only; simulation
            // and input rollback continue normally.
            g_lifecycleComparisonIgnoreUntilFrame = ignoreUntil;
        }
    }
    if (g_mode == Netplay::MODE_HOST &&
        g_localFrames[slot] == frame)
    {
        g_localControls[slot] = (u16)control;
    }
    if (g_mode == Netplay::MODE_GUEST && g_localPlayerSlot != 0)
    {
        if (g_remoteFramesByPlayer[0][slot] == frame)
        {
            g_remoteControlsByPlayer[0][slot] = (u16)control;
        }
        if (g_predictedRemoteFramesByPlayer[0][slot] == frame)
        {
            g_predictedRemoteControlsByPlayer[0][slot] = (u16)control;
            g_remoteControlsByPlayer[0][slot] = (u16)control;
        }
        if (newlyScheduled && frame < g_frame &&
            (g_rollbackEarliestFrame == INVALID_FRAME ||
             frame < g_rollbackEarliestFrame))
        {
            g_rollbackEarliestFrame = frame;
        }
    }
    if (announce && g_mode == Netplay::MODE_HOST)
    {
        SendControl(CONTROL_PLAYER_LIFECYCLE, frame, (u16)control);
        g_lastLifecycleAnnounceTick[playerId] = GetTickCount();
    }
    return frame;
}

void PromoteHostPredictedInputs(int playerId, u32 throughFrame)
{
    int slot;
    for (slot = 0; slot < INPUT_RING_SIZE; slot++)
    {
        u32 predictedFrame =
            g_predictedRemoteFramesByPlayer[playerId][slot];
        if (predictedFrame == INVALID_FRAME || predictedFrame > throughFrame)
        {
            continue;
        }
        // Keep the held-input prediction that every healthy peer already
        // simulated, but make it authoritative so the rollback window can
        // continue advancing until the announced neutral boundary.
        g_remoteFramesByPlayer[playerId][slot] = predictedFrame;
        g_remoteControlsByPlayer[playerId][slot] =
            Netplay::INGAME_CONTROL_NONE;
        g_remoteStateHashByPlayer[playerId][slot] = 0;
        g_remotePlayerHashByPlayer[playerId][slot] = 0;
        g_remoteWorldHashByPlayer[playerId][slot] = 0;
        g_remoteSpellHashByPlayer[playerId][slot] = 0;
        g_remoteRollbackGameplayByPlayer[playerId][slot] = 1;
        g_predictedRemoteFramesByPlayer[playerId][slot] = INVALID_FRAME;
        if (g_peerSyntheticThroughFrame[playerId] == INVALID_FRAME ||
            predictedFrame > g_peerSyntheticThroughFrame[playerId])
        {
            g_peerSyntheticThroughFrame[playerId] = predictedFrame;
        }
    }
}

bool ShouldHostSynthesizePlayerFrame(int playerId, u32 frame)
{
    u8 stage;
    if (g_mode != Netplay::MODE_HOST || g_playerCount < 3 ||
        playerId < 1 || playerId >= g_playerCount ||
        !IsPlayerSlotActive((u8)playerId))
    {
        return false;
    }
    stage = g_hostPeerLifecycleStage[playerId];
    if (stage == HOST_PEER_PRESENT || stage == HOST_PEER_LEFT)
    {
        return false;
    }
    if (g_peerResumeFrame[playerId] != INVALID_FRAME &&
        frame >= g_peerResumeFrame[playerId])
    {
        return false;
    }
    return true;
}

void SynthesizeHostPlayerInput(int playerId, u32 frame)
{
    int slot = (int)(frame % INPUT_RING_SIZE);
    bool neutral;
    u16 synthesizedInput;

    if (!ShouldHostSynthesizePlayerFrame(playerId, frame) ||
        g_remoteFramesByPlayer[playerId][slot] == frame)
    {
        return;
    }
    neutral = g_peerNeutralStartFrame[playerId] != INVALID_FRAME &&
        frame >= g_peerNeutralStartFrame[playerId] &&
        (g_peerResumeFrame[playerId] == INVALID_FRAME ||
         frame < g_peerResumeFrame[playerId]);
    synthesizedInput = neutral ? 0
        : PredictRemoteInputForPlayer(frame, playerId);
    g_remoteFramesByPlayer[playerId][slot] = frame;
    g_remoteInputsByPlayer[playerId][slot] = synthesizedInput;
    g_remoteRngByPlayer[playerId][slot] = 0;
    g_remoteControlsByPlayer[playerId][slot] =
        Netplay::INGAME_CONTROL_NONE;
    g_remoteStateHashByPlayer[playerId][slot] = 0;
    g_remotePlayerHashByPlayer[playerId][slot] = 0;
    g_remoteWorldHashByPlayer[playerId][slot] = 0;
    g_remoteSpellHashByPlayer[playerId][slot] = 0;
    g_remoteRollbackGameplayByPlayer[playerId][slot] = 1;
    g_predictedRemoteFramesByPlayer[playerId][slot] = INVALID_FRAME;
    if (g_peerSyntheticThroughFrame[playerId] == INVALID_FRAME ||
        frame > g_peerSyntheticThroughFrame[playerId])
    {
        g_peerSyntheticThroughFrame[playerId] = frame;
    }
}

void PrepareHostSyntheticInputs(u32 throughFrame)
{
    int playerId;
    for (playerId = 1; playerId < g_playerCount; playerId++)
    {
        u32 startFrame;
        u32 frame;
        if (!ShouldHostSynthesizePlayerFrame(playerId, throughFrame))
        {
            continue;
        }
        if (g_peerSyntheticThroughFrame[playerId] != INVALID_FRAME)
        {
            startFrame = g_peerSyntheticThroughFrame[playerId] + 1;
        }
        else if (g_lastPeerInputFrame[playerId] != INVALID_FRAME)
        {
            startFrame = g_lastPeerInputFrame[playerId] + 1;
        }
        else
        {
            startFrame = throughFrame;
        }
        if (throughFrame >= INPUT_RING_SIZE &&
            startFrame < throughFrame - INPUT_RING_SIZE + 1)
        {
            startFrame = throughFrame - INPUT_RING_SIZE + 1;
        }
        for (frame = startFrame; frame <= throughFrame; frame++)
        {
            SynthesizeHostPlayerInput(playerId, frame);
            if (frame == INVALID_FRAME)
            {
                break;
            }
        }
    }
}

void BeginHostPeerAbsence(int playerId)
{
    u32 boundary;
    if (playerId < 1 || playerId >= g_playerCount ||
        g_hostPeerLifecycleStage[playerId] != HOST_PEER_PRESENT)
    {
        return;
    }
    boundary = ScheduleLifecycleControl(
        GetAbsentControlForPlayer(playerId),
        g_frame + TEMPORARY_ABSENCE_LEAD_FRAMES, true);
    if (boundary == INVALID_FRAME)
    {
        return;
    }
    g_hostPeerLifecycleStage[playerId] = HOST_PEER_ABSENCE_PENDING;
    g_peerNeutralStartFrame[playerId] = boundary;
    g_peerResumeFrame[playerId] = INVALID_FRAME;
    PromoteHostPredictedInputs(playerId, g_frame);
    PrepareHostSyntheticInputs(g_frame);
    g_GameErrorContext.Log(
        "info : P%d Stage 1 absence scheduled frame %lu last_input %lu\r\n",
        playerId + 1, (unsigned long)boundary,
        (unsigned long)g_lastPeerInputFrame[playerId]);
}

void BeginHostPeerResume(int playerId)
{
    u32 boundary;
    u32 minimumBoundary;
    u8 stage = g_hostPeerLifecycleStage[playerId];
    if (stage != HOST_PEER_ABSENCE_PENDING &&
        stage != HOST_PEER_ABSENT)
    {
        return;
    }
    minimumBoundary = g_frame + PLAYER_RESUME_LEAD_FRAMES;
    if (g_peerNeutralStartFrame[playerId] != INVALID_FRAME &&
        minimumBoundary <= g_peerNeutralStartFrame[playerId])
    {
        minimumBoundary = g_peerNeutralStartFrame[playerId] + 1;
    }
    boundary = ScheduleLifecycleControl(
        GetResumeControlForPlayer(playerId), minimumBoundary, true);
    if (boundary == INVALID_FRAME)
    {
        return;
    }
    g_hostPeerLifecycleStage[playerId] = HOST_PEER_RESUME_PENDING;
    g_peerResumeFrame[playerId] = boundary;
    g_GameErrorContext.Log(
        "info : P%d resume scheduled frame %lu newest_input %lu\r\n",
        playerId + 1, (unsigned long)boundary,
        (unsigned long)g_lastPeerInputFrame[playerId]);
}

void BeginHostPeerLeft(int playerId)
{
    u32 boundary;
    u8 stage = g_hostPeerLifecycleStage[playerId];
    if (stage == HOST_PEER_LEFT_PENDING || stage == HOST_PEER_LEFT)
    {
        return;
    }
    if (stage == HOST_PEER_PRESENT)
    {
        BeginHostPeerAbsence(playerId);
    }
    boundary = ScheduleLifecycleControl(
        GetLeftControlForPlayer(playerId),
        g_frame + PLAYER_LEFT_LEAD_FRAMES, true);
    if (boundary == INVALID_FRAME)
    {
        return;
    }
    g_hostPeerLifecycleStage[playerId] = HOST_PEER_LEFT_PENDING;
    g_peerLeftFrame[playerId] = boundary;
    g_peerResumeFrame[playerId] = INVALID_FRAME;
    g_GameErrorContext.Log(
        "info : P%d Stage 2 departure scheduled frame %lu\r\n",
        playerId + 1, (unsigned long)boundary);
}

void NoteHostPeerInputAdvanced(int playerId, u32 newestFrame)
{
    if (g_mode != Netplay::MODE_HOST || playerId < 1 ||
        playerId >= g_playerCount || newestFrame == INVALID_FRAME ||
        (g_lastPeerInputFrame[playerId] != INVALID_FRAME &&
         newestFrame <= g_lastPeerInputFrame[playerId]))
    {
        return;
    }
    g_lastPeerInputFrame[playerId] = newestFrame;
    g_lastPeerInputAdvanceTick[playerId] = GetTickCount();
    if ((g_peerExitNoticeMask & (1 << playerId)) == 0)
    {
        BeginHostPeerResume(playerId);
    }
}

void SendPendingLifecycleAnnouncements()
{
    int playerId;
    DWORD now;
    if (g_mode != Netplay::MODE_HOST || g_playerCount < 3)
    {
        return;
    }
    now = GetTickCount();
    for (playerId = 1; playerId < g_playerCount; playerId++)
    {
        Netplay::InGameControl control = Netplay::INGAME_CONTROL_NONE;
        u32 frame = INVALID_FRAME;
        u8 stage = g_hostPeerLifecycleStage[playerId];
        if (stage == HOST_PEER_ABSENCE_PENDING)
        {
            control = GetAbsentControlForPlayer(playerId);
            frame = g_peerNeutralStartFrame[playerId];
        }
        else if (stage == HOST_PEER_RESUME_PENDING)
        {
            control = GetResumeControlForPlayer(playerId);
            frame = g_peerResumeFrame[playerId];
        }
        else if (stage == HOST_PEER_LEFT_PENDING)
        {
            control = GetLeftControlForPlayer(playerId);
            frame = g_peerLeftFrame[playerId];
        }
        if (control != Netplay::INGAME_CONTROL_NONE &&
            frame != INVALID_FRAME &&
            now - g_lastLifecycleAnnounceTick[playerId] >=
                LIFECYCLE_ANNOUNCE_INTERVAL_MS)
        {
            SendControl(CONTROL_PLAYER_LIFECYCLE, frame, (u16)control);
            g_lastLifecycleAnnounceTick[playerId] = now;
        }
    }
}

void UpdateHostPeerLifecycles()
{
    int playerId;
    DWORD now;
    if (g_mode != Netplay::MODE_HOST || g_playerCount < 3 ||
        !g_connected || !g_rollbackPredictionActive ||
        !g_GameManager.notInMenu ||
        g_Supervisor.curState != TH07_SUPERVISOR_GAME_STATE)
    {
        return;
    }
    now = GetTickCount();
    for (playerId = 1; playerId < g_playerCount; playerId++)
    {
        u8 stage;
        DWORD elapsed;
        if (!IsPlayerSlotActive((u8)playerId))
        {
            continue;
        }
        stage = g_hostPeerLifecycleStage[playerId];
        if (stage == HOST_PEER_ABSENCE_PENDING &&
            g_peerNeutralStartFrame[playerId] != INVALID_FRAME &&
            g_frame >= g_peerNeutralStartFrame[playerId])
        {
            g_hostPeerLifecycleStage[playerId] = HOST_PEER_ABSENT;
            stage = HOST_PEER_ABSENT;
        }
        else if (stage == HOST_PEER_RESUME_PENDING &&
                 g_peerResumeFrame[playerId] != INVALID_FRAME &&
                 g_frame >= g_peerResumeFrame[playerId])
        {
            g_hostPeerLifecycleStage[playerId] = HOST_PEER_PRESENT;
            g_peerNeutralStartFrame[playerId] = INVALID_FRAME;
            g_peerResumeFrame[playerId] = INVALID_FRAME;
            stage = HOST_PEER_PRESENT;
        }
        // LEFT_PENDING remains authoritative through the boundary frame.
        // ApplyPlayerLifecycleTransition changes it to LEFT only after that
        // frame's final synthesized zero input has been relayed and consumed.
        if (stage == HOST_PEER_LEFT ||
            g_lastPeerInputAdvanceTick[playerId] == 0)
        {
            continue;
        }
        elapsed = now - g_lastPeerInputAdvanceTick[playerId];
        if (stage == HOST_PEER_PRESENT &&
            elapsed >= TEMPORARY_ABSENCE_DETECT_MS)
        {
            BeginHostPeerAbsence(playerId);
            stage = g_hostPeerLifecycleStage[playerId];
        }
        if (stage != HOST_PEER_PRESENT &&
            stage != HOST_PEER_LEFT_PENDING &&
            elapsed >= REMOTE_INPUT_TIMEOUT_MS)
        {
            BeginHostPeerLeft(playerId);
        }
    }
    PrepareHostSyntheticInputs(g_frame);
    SendPendingLifecycleAnnouncements();
}

void NotifyPeerExit()
{
    int repeat;

    if ((g_mode != Netplay::MODE_HOST && g_mode != Netplay::MODE_GUEST) ||
        g_peerExitReceived || g_socket == INVALID_SOCKET || !g_hasPeer)
    {
        return;
    }
    // UDP has no delivery guarantee. A short burst makes a normal window
    // close reliably visible to the other peer without keeping a shutdown
    // thread or a second socket alive during cleanup.
    for (repeat = 0; repeat < 8; repeat++)
    {
        SendControl(CONTROL_PEER_EXIT, g_frame, 0);
        Sleep(10);
    }
}

// Defined at the end of this namespace, next to the counters it reports.
void LogEndOfRunSummary();

void HandlePeerExit()
{
    if (g_peerExitReceived)
    {
        return;
    }
    g_peerExitReceived = true;
    g_connected = false;
    g_connectionFailed = false;
    g_waitingForRemoteInput = false;
    SetStatus("peer closed the game; exiting");
    g_GameErrorContext.Log(
        "info : peer closed the game; shutting down this peer\r\n");
    // Whichever peer reaches its own test deadline first exits and takes the
    // others down with it, so a guest usually never sees its own timeout. The
    // summary was only written on the timeout path, which is why the guests'
    // measurements were missing from most runs rather than some.
    LogEndOfRunSummary();
    g_GameWindow.isAppClosing = 1;
}

u32 CalculateResyncFrame()
{
    u32 leadFrames = (u32)(g_delay * 2 + 2);
    if (g_frame > INVALID_FRAME - leadFrames)
    {
        return g_frame + 1;
    }
    return g_frame + leadFrames;
}

void MarkRngMismatch(u32 frame, u16 localSeed, u16 remoteSeed,
                     bool notifyPeer, u32 requestedResyncFrame)
{
    char status[160];
    u32 resyncFrame = requestedResyncFrame;

    // The host owns the resync boundary. A guest may first detect the
    // mismatch locally, but it accepts the host's later boundary packet.
    if (g_rngMismatch &&
        !(g_mode == Netplay::MODE_GUEST &&
          requestedResyncFrame != INVALID_FRAME))
    {
        return;
    }

    if (resyncFrame == INVALID_FRAME)
    {
        resyncFrame = CalculateResyncFrame();
    }
    if (resyncFrame <= g_frame)
    {
        resyncFrame = g_frame + 1;
    }
    g_resyncFrame = resyncFrame;
    g_rngMismatch = true;
    g_connectionFailed = false;
    g_connected = true;
    g_waitingForRemoteInput = false;
    g_lastResyncSendTick = GetTickCount();
    if (notifyPeer)
    {
        SendControl(CONTROL_RESYNC_REQUEST, g_resyncFrame, localSeed);
    }
    // Player.cpp draws this status in a narrow in-game HUD slot. Keep the
    // detailed seed/input evidence in log.txt, but never let the diagnostic
    // string overflow into the score and boss UI.
    sprintf(status, "RNG sync at %lu", (unsigned long)g_resyncFrame);
    SetStatus(status);
    {
        int slot = (int)(frame % INPUT_RING_SIZE);
        u16 localInput = g_localFrames[slot] == frame
            ? g_localInputs[slot] : 0xffff;
        u16 remoteInput = g_remoteFrames[slot] == frame
            ? g_remoteInputs[slot] : 0xffff;
        g_GameErrorContext.Log(
            "info : RNG mismatch frame %lu local_seed %u remote_seed %u local_input %u remote_input %u local_frame %lu remote_frame %lu\r\n",
            (unsigned long)frame, (unsigned)localSeed,
            (unsigned)remoteSeed, (unsigned)localInput,
            (unsigned)remoteInput, (unsigned long)g_localFrames[slot],
            (unsigned long)g_remoteFrames[slot]);
        // Identical inputs with different seeds is not a dropped packet: the
        // two simulations took different paths. The resync below realigns the
        // seed only, so the world state that already diverged - boss life in
        // particular - stays wrong on one side forever. Report that plainly
        // instead of letting the successful-looking resync hide it.
        if (localInput != 0xffff && localInput == remoteInput &&
            localSeed != remoteSeed && !g_simulationDivergenceLogged)
        {
            g_simulationDivergenceLogged = true;
            g_GameErrorContext.Log(
                "error : simulation diverged at frame %lu with identical inputs; the RNG resync realigns the seed but does not repair world state (boss/enemy life stays out of sync)\r\n",
                (unsigned long)frame);
        }
    }
}

bool ReceivePacket(NetPacket *packet, sockaddr_in *from)
{
    int fromSize = sizeof(*from);
    int received;
    memset(from, 0, sizeof(*from));
    received = recvfrom(g_socket, (char *)packet, sizeof(*packet), 0,
                        (sockaddr *)from, &fromSize);
    if (received == SOCKET_ERROR)
    {
        return false;
    }
    if (received != sizeof(*packet) || packet->magic != NETPLAY_MAGIC)
    {
        return false;
    }
    return true;
}

bool ValidatePacketVersion(const NetPacket &packet)
{
    if (packet.version != NETPLAY_VERSION)
    {
        g_protocolMismatch = true;
        g_connectionFailed = true;
        g_connected = false;
        return false;
    }
    return true;
}

void StoreRemoteInputs(const NetPacket &packet, int remotePlayerId)
{
    int i;
    u32 newestFrame = packet.newestFrame;
    const u16 rollbackCorrectionMask = 0xffff;
    bool correctionDetected = false;
    u16 *remoteInputs;
    u16 *remoteRng;
    u16 *remoteControls;
    u32 *remoteStateHash;
    u32 *remotePlayerHash;
    u32 *remoteWorldHash;
    u32 *remoteSpellHash;
    u32 *remoteFrames;
    u8 *remoteRollbackGameplay;
    u16 *predictedRemoteInputs;
    u16 *predictedRemoteControls;
    u32 *predictedRemoteFrames;

    if (remotePlayerId < 0 ||
        remotePlayerId >= TH07_MULTI_MAX_PLAYERS ||
        remotePlayerId == g_localPlayerSlot)
    {
        return;
    }
    remoteInputs = g_remoteInputsByPlayer[remotePlayerId];
    remoteRng = g_remoteRngByPlayer[remotePlayerId];
    remoteControls = g_remoteControlsByPlayer[remotePlayerId];
    remoteStateHash = g_remoteStateHashByPlayer[remotePlayerId];
    remotePlayerHash = g_remotePlayerHashByPlayer[remotePlayerId];
    remoteWorldHash = g_remoteWorldHashByPlayer[remotePlayerId];
    remoteSpellHash = g_remoteSpellHashByPlayer[remotePlayerId];
    remoteFrames = g_remoteFramesByPlayer[remotePlayerId];
    remoteRollbackGameplay =
        g_remoteRollbackGameplayByPlayer[remotePlayerId];
    predictedRemoteInputs =
        g_predictedRemoteInputsByPlayer[remotePlayerId];
    predictedRemoteControls =
        g_predictedRemoteControlsByPlayer[remotePlayerId];
    predictedRemoteFrames =
        g_predictedRemoteFramesByPlayer[remotePlayerId];

    // A delayed UDP packet can arrive after its ring slot has been reused.
    // Do not let an old record overwrite a newer frame in that slot. The
    // newest frame is also checked against the current simulation window so a
    // malformed packet cannot move the receive buffer arbitrarily far ahead.
    if (newestFrame == INVALID_FRAME ||
        (newestFrame >= g_frame &&
         newestFrame - g_frame >= REDUNDANT_INPUT_COUNT) ||
        (g_frame > newestFrame && g_frame - newestFrame >= INPUT_RING_SIZE))
    {
        return;
    }
    NoteHostPeerInputAdvanced(remotePlayerId, newestFrame);
    for (i = 0; i < REDUNDANT_INPUT_COUNT; i++)
    {
        const InputRecord &record = packet.records[i];
        u16 remoteControl = record.control & INPUT_RECORD_CONTROL_MASK;
        if (record.frame != INVALID_FRAME && record.frame <= newestFrame &&
            newestFrame - record.frame < REDUNDANT_INPUT_COUNT &&
            (record.frame >= g_frame ||
             g_frame - record.frame < INPUT_RING_SIZE))
        {
            int slot = (int)(record.frame % INPUT_RING_SIZE);
            if (g_mode == Netplay::MODE_HOST && remotePlayerId > 0 &&
                ((g_peerSyntheticThroughFrame[remotePlayerId] !=
                      INVALID_FRAME &&
                  record.frame <=
                      g_peerSyntheticThroughFrame[remotePlayerId]) ||
                 (g_peerResumeFrame[remotePlayerId] != INVALID_FRAME &&
                  record.frame < g_peerResumeFrame[remotePlayerId])))
            {
                continue;
            }
            if (remotePlayerId == 0 &&
                IsLifecycleControl(
                    (Netplay::InGameControl)remoteControl))
            {
                ScheduleLifecycleControl(
                    (Netplay::InGameControl)remoteControl,
                    record.frame, false);
            }
            if (predictedRemoteFrames[slot] == record.frame)
            {
                if (g_rollbackEnabled &&
                    ((predictedRemoteInputs[slot] ^ record.input) &
                     TH_BUTTON_BOMB) != 0 &&
                    g_synchronizedBombPulseCount < 24)
                {
                    g_GameErrorContext.Log(
                        "info : rollback bomb edge correction frame %lu remote %d\r\n",
                        (unsigned long)record.frame,
                        (record.input & TH_BUTTON_BOMB) != 0 ? 1 : 0);
                    g_synchronizedBombPulseCount++;
                }
                if (((predictedRemoteInputs[slot] ^ record.input) &
                     rollbackCorrectionMask) != 0 ||
                    predictedRemoteControls[slot] != remoteControl)
                {
                    correctionDetected = true;
                    if (g_rollbackEarliestFrame == INVALID_FRAME ||
                        record.frame < g_rollbackEarliestFrame)
                    {
                        g_rollbackEarliestFrame = record.frame;
                    }
                }
                predictedRemoteFrames[slot] = INVALID_FRAME;
            }
            remoteFrames[slot] = record.frame;
            remoteInputs[slot] = record.input;
            remoteRng[slot] = record.rngSeed;
            remoteControls[slot] = remoteControl;
            remoteStateHash[slot] = 0;
            remotePlayerHash[slot] = 0;
            remoteWorldHash[slot] = 0;
            remoteSpellHash[slot] = 0;
            remoteRollbackGameplay[slot] =
                (record.control & INPUT_RECORD_ROLLBACK_GAMEPLAY) != 0 ? 1 : 0;
        }
    }
    for (i = 0; i < VERIFICATION_RECORD_COUNT; i++)
    {
        const VerificationRecord &verification = packet.verifications[i];
        if (verification.frame != INVALID_FRAME &&
            verification.frame <= newestFrame &&
            newestFrame - verification.frame <
                REDUNDANT_INPUT_COUNT + DETAILED_STATE_CONFIRM_LAG &&
            (verification.frame >= g_frame ||
             g_frame - verification.frame < INPUT_RING_SIZE))
        {
            int slot = (int)(verification.frame % INPUT_RING_SIZE);
            if (remoteFrames[slot] == verification.frame)
            {
                remoteStateHash[slot] = verification.stateHash;
                remoteSpellHash[slot] = verification.spellHash;
                g_remoteBodyHashByPlayer[remotePlayerId][slot] =
                    verification.bodyHash;
                g_remoteShotHashByPlayer[remotePlayerId][slot] =
                    verification.shotHash;
                g_remoteEnemyHashByPlayer[remotePlayerId][slot] =
                    verification.enemyHash;
                g_remoteBulletHashByPlayer[remotePlayerId][slot] =
                    verification.bulletHash;
                g_remoteItemHashByPlayer[remotePlayerId][slot] =
                    verification.itemHash;
            }
        }
    }
    if (g_rollbackEnabled && correctionDetected &&
        g_rollbackEarliestFrame != INVALID_FRAME)
    {
        // The pending rewind will re-simulate every outstanding predicted
        // frame after the first mismatch. Feed that corrected replay the most
        // recent confirmed held input now. Without this, each frame in an
        // 80-200 ms network tail retains the old direction and causes another
        // near-identical rewind when its packet arrives one frame later.
        for (i = 0; i < INPUT_RING_SIZE; i++)
        {
            u32 predictedFrame = predictedRemoteFrames[i];
            if (predictedFrame != INVALID_FRAME &&
                predictedFrame > g_rollbackEarliestFrame)
            {
                u16 refreshed = PredictRemoteInputForPlayer(
                    predictedFrame, remotePlayerId);
                if (refreshed != predictedRemoteInputs[i])
                {
                    predictedRemoteInputs[i] = refreshed;
                    remoteInputs[i] = refreshed;
                    g_rollbackPredictionRefreshFrames++;
                }
            }
        }
    }
}

void ApplyHostOptions(const NetPacket &packet, bool armQuickStart)
{
    g_invincible = (packet.flags & NETPLAY_FLAG_INVINCIBLE) != 0;
    g_demoDisabled = (packet.flags & NETPLAY_FLAG_NO_DEMO) != 0;
    g_quickStartEnabled =
        (packet.flags & NETPLAY_FLAG_QUICK_START) != 0;
    g_autoShoot = (packet.flags & NETPLAY_FLAG_AUTO_SHOOT) != 0;
    g_autoSkip = (packet.flags & NETPLAY_FLAG_AUTO_SKIP) != 0;
    g_autoBomb = (packet.flags & NETPLAY_FLAG_AUTO_BOMB) != 0;
    // Controller suppression is strictly local. Each PC loads its own
    // th07.cfg and routes that configured pad to its own lane (Host=P1,
    // Guest=P2/P3), so a Host test flag must never disable the Guest's pad.
    g_ignoreControllerInput = g_localIgnoreControllerInput;
    NetplayTrace(
        "info : controller suppression local %d effective %d mode %d\r\n",
        g_localIgnoreControllerInput ? 1 : 0,
        g_ignoreControllerInput ? 1 : 0, (int)g_mode);
    g_rollbackEnabled = (packet.flags & NETPLAY_FLAG_ROLLBACK) != 0;
    g_rollbackEverEnabled = g_rollbackEverEnabled || g_rollbackEnabled;
    // Scripted damage changes simulation state, so it is Host-authoritative
    // and must be active on both peers. Unlike random controller input, this
    // flag is therefore inherited from the Host handshake.
    g_testDamageEventsEnabled =
        (packet.flags & NETPLAY_FLAG_TEST_DAMAGE_EVENTS) != 0;
    // Random input is a local test driver. Do not inherit it from the Host:
    // this allows a human Host to play normally while only the Guest ship is
    // automated (and also supports the inverse arrangement).
    // Saving is a local safety decision. A host may force it off for both
    // peers, but it must never re-enable saving that the guest disabled.
    g_noSave = g_localNoSave ||
        (packet.flags & NETPLAY_FLAG_NO_SAVE) != 0;
    if (packet.quickDifficulty <= 3)
    {
        g_quickDifficulty = packet.quickDifficulty;
    }
    if (packet.quickCharacter <= 2)
    {
        g_quickCharacter = packet.quickCharacter;
    }
    if (packet.quickShot <= 1)
    {
        g_quickShot = packet.quickShot;
    }
    if (packet.quickStage >= 1 && packet.quickStage <= 6)
    {
        g_quickStage = packet.quickStage;
    }
    g_quickStartPractice = packet.quickPractice != 0;
    if ((packet.flags & NETPLAY_FLAG_P2_LOADOUT) != 0 &&
        packet.quickCharacter2 <= 2 && packet.quickShot2 <= 1)
    {
        g_p2Character = packet.quickCharacter2;
        g_p2Shot = packet.quickShot2;
        g_p2LoadoutConfigured = true;
        g_p2LoadoutSelected = false;
    }
    if (packet.quickCharacter3 <= 2 && packet.quickShot3 <= 1)
    {
        g_p3Character = packet.quickCharacter3;
        g_p3Shot = packet.quickShot3;
        g_p3LoadoutConfigured = true;
        g_p3LoadoutSelected = false;
    }
    else if (!g_quickStartEnabled)
    {
        g_p3Character = -1;
        g_p3Shot = -1;
        g_p3LoadoutConfigured = false;
        g_p3LoadoutSelected = false;
    }
    else if (!g_quickStartEnabled)
    {
        // The host owns the normal-match setup. If it did not provide a
        // fixed P2 loadout, both peers enter the shared P1->P2 selector.
        g_p2Character = -1;
        g_p2Shot = -1;
        g_p2LoadoutConfigured = false;
        g_p2LoadoutSelected = false;
    }
    if (armQuickStart)
    {
        g_quickStartPending = g_quickStartEnabled;
    }
}

bool AcceptWelcomePacket(const NetPacket &packet, const sockaddr_in &from,
                         bool armQuickStart)
{
    bool welcomeChanged;
    if (packet.assignedSlot < 1 ||
        packet.assignedSlot >= TH07_MULTI_MAX_PLAYERS ||
        packet.playerCount < 2 ||
        packet.playerCount > TH07_MULTI_MAX_PLAYERS ||
        packet.assignedSlot >= packet.playerCount)
    {
        SetStatus("Host returned an invalid player slot");
        return false;
    }
    welcomeChanged = g_session != packet.session ||
        g_playerCount != packet.playerCount ||
        g_localPlayerSlot != packet.assignedSlot ||
        g_connectedPlayerMask != packet.connectedPlayerMask;
    g_session = packet.session;
    g_playerCount = packet.playerCount;
    g_localPlayerSlot = packet.assignedSlot;
    memset(g_peerAddresses, 0, sizeof(g_peerAddresses));
    memset(g_peerPresent, 0, sizeof(g_peerPresent));
    g_peerAddresses[0] = from;
    g_peerPresent[0] = true;
    g_delay = packet.delay;
    g_initialRngSeed = packet.initialRngSeed;
    ApplyPacketPlayerNames(packet);
    ApplyHostOptions(packet, armQuickStart);
    if (g_playerCount == 3)
    {
        g_rollbackEnabled = true;
        g_rollbackEverEnabled = true;
        g_delay = 0;
    }
    g_connectedPlayerMask = packet.connectedPlayerMask;
    g_connected = g_connectedPlayerMask ==
        (u8)((1 << g_playerCount) - 1);
    g_connectionFailed = false;
    if (welcomeChanged)
    {
        g_GameErrorContext.Log(
            "info : Guest accepted P%d slot in %d-player session peers 0x%02x rollback %d delay %d\r\n",
            g_localPlayerSlot + 1, g_playerCount,
            (unsigned)g_connectedPlayerMask,
            g_rollbackEnabled ? 1 : 0, g_delay);
    }
    return true;
}

void PollPackets()
{
    NetPacket packet;
    sockaddr_in from;

    // Loopback normally delivers every packet before the next 60 Hz update,
    // leaving the dynamic-effect restore path untested. In the dedicated
    // rollback input test only, hold three Guest receive frames for one update
    // so the following frame must correct a direction change. One selected
    // correction occurs while a bomb effect is active.
    if (g_testRollbackInputEnabled && g_rollbackEnabled &&
        g_mode == Netplay::MODE_GUEST && g_GameManager.notInMenu &&
        !g_rollbackReplaying && g_testDeferredPollFrameCount < 3 &&
        g_frame % 180 == 90)
    {
        if (g_testDeferredPollFrame != g_frame)
        {
            g_testDeferredPollFrame = g_frame;
            g_testDeferredPollFrameCount++;
            g_GameErrorContext.Log(
                "info : rollback test deferred packet poll at frame %lu (%lu/3)\r\n",
                (unsigned long)g_frame,
                (unsigned long)g_testDeferredPollFrameCount);
        }
        return;
    }
    while (ReceivePacket(&packet, &from))
    {
        int fromPlayerId = FindPeerPlayerId(from);
        int relayIndex;

        // An unknown address is only allowed to introduce itself to a Host.
        // Established traffic is always bound to the exact IP+UDP-port pair.
        if (fromPlayerId < 0 &&
            !(g_mode == Netplay::MODE_HOST &&
              packet.type == PACKET_HELLO))
        {
            continue;
        }
        if (g_mode == Netplay::MODE_GUEST && fromPlayerId != 0)
        {
            continue;
        }
        if (!ValidatePacketVersion(packet))
        {
            return;
        }
        if (packet.type == PACKET_HELLO &&
            g_mode == Netplay::MODE_HOST)
        {
            int assignedPlayerId = AssignHostGuestSlot(
                from, packet.playerName);
            if (assignedPlayerId >= 1)
            {
                // Refresh every WELCOME when a new name/slot arrives, so P2
                // learns P3's name before the Host enables Start Game.
                SendWelcomeToAllGuests(2);
                g_connected = AreAllExpectedPeersConnected();
            }
            continue;
        }
        if (packet.type == PACKET_WELCOME &&
            g_mode == Netplay::MODE_GUEST &&
            (!g_connected || packet.session == g_session))
        {
            if (!g_connected)
            {
                AcceptWelcomePacket(packet, from, true);
            }
            else
            {
                ApplyPacketPlayerNames(packet);
            }
            continue;
        }
        if (packet.session != g_session)
        {
            continue;
        }
        if (packet.type == PACKET_INPUT && packet.session == g_session)
        {
            if (packet.sourceSlot != (u8)fromPlayerId ||
                packet.playerCount != (u8)g_playerCount)
            {
                continue;
            }
            StoreRemoteInputs(packet, fromPlayerId);
            if (g_mode == Netplay::MODE_GUEST && fromPlayerId == 0)
            {
                for (relayIndex = 0;
                     relayIndex < packet.relaySlotCount &&
                     relayIndex < TH07_MULTI_MAX_GUESTS;
                     relayIndex++)
                {
                    StoreRelayedInputs(
                        packet, relayIndex,
                        (int)packet.relaySlots[relayIndex]);
                }
            }
        }
        else if (packet.type == PACKET_PING &&
                 packet.session == g_session)
        {
            NetPacket reply;
            InitializePacket(&reply, PACKET_PONG);
            reply.sendTick = GetTickCount();
            reply.echoTick = packet.sendTick;
            SendPacketToPlayer(reply, fromPlayerId);
        }
        else if (packet.type == PACKET_PONG &&
                 packet.session == g_session)
        {
            char status[160];
            g_lastRoundTripMs = GetTickCount() - packet.echoTick;
            if (fromPlayerId >= 0 &&
                fromPlayerId < TH07_MULTI_MAX_PLAYERS)
            {
                g_lastRoundTripMsByPlayer[fromPlayerId] =
                    g_lastRoundTripMs;
            }
            if (g_rngMismatch && g_resyncFrame != INVALID_FRAME)
            {
                sprintf(status,
                        "RNG resync at frame %lu (rtt %lu ms)",
                        (unsigned long)g_resyncFrame,
                        (unsigned long)g_lastRoundTripMs);
            }
            else
            {
                sprintf(status, "connected (rtt %lu ms)",
                        (unsigned long)g_lastRoundTripMs);
            }
            SetStatus(status);
        }
        else if (packet.type == PACKET_CONTROL &&
                 packet.session == g_session)
        {
            if (packet.controlType == CONTROL_PEER_EXIT)
            {
                if (g_mode == Netplay::MODE_GUEST || g_playerCount == 2)
                {
                    HandlePeerExit();
                    return;
                }
                if (fromPlayerId > 0 &&
                    (g_peerExitNoticeMask & (1 << fromPlayerId)) == 0)
                {
                    g_peerExitNoticeMask |= (u8)(1 << fromPlayerId);
                    g_lastPeerInputAdvanceTick[fromPlayerId] =
                        GetTickCount() - TEMPORARY_ABSENCE_DETECT_MS;
                    BeginHostPeerAbsence(fromPlayerId);
                    g_GameErrorContext.Log(
                        "info : P%d sent peer-exit; Stage 1 boundary announced\r\n",
                        fromPlayerId + 1);
                }
                continue;
            }
            if (packet.controlType == CONTROL_START_GAME)
            {
                // The peer may still be in its launcher if the first ACK was
                // lost while this process was loading the game window.
                SendConnectionUiControlBurst(CONTROL_START_GAME_ACK);
            }
            else if (packet.controlType == CONTROL_START_GAME_COMMIT)
            {
                // The receiver may have crossed the synchronized launcher
                // deadline before its final ACK reached the initiator.
                SendConnectionUiControlBurst(CONTROL_START_GAME_COMMIT_ACK);
            }
            else if (packet.controlType == CONTROL_QUICK_START_READY)
            {
                g_quickStartRemoteReady = true;
                if (fromPlayerId >= 0 &&
                    fromPlayerId < TH07_MULTI_MAX_PLAYERS)
                {
                    g_quickStartReadyMask |= (u8)(1 << fromPlayerId);
                }
            }
            else if (packet.controlType == CONTROL_QUICK_START)
            {
                // A quick-start boundary is deliberately sent well ahead of
                // the current frame.  This keeps both peers in the title
                // scene until the same network frame even when one machine
                // finishes loading Direct3D or the stage a little later.
                u32 maxFrame = g_frame + 120;
                if (packet.controlFrame != INVALID_FRAME &&
                    packet.controlFrame <= maxFrame)
                {
                    g_quickStartFrame = packet.controlFrame;
                    g_quickStartRemoteReady = true;
                }
            }
            else if (packet.controlType == CONTROL_RESYNC_REQUEST)
            {
                u32 leadFrames = (u32)(g_delay * 2 + 2);
                u32 maxFrame = g_frame + leadFrames;

                // A Guest acknowledges a request that arrived after its
                // boundary, so a lost request/ack pair can recover without
                // starting a second, incompatible resync boundary.
                if (g_mode == Netplay::MODE_GUEST &&
                    packet.controlFrame <= g_frame)
                {
                    SendControl(CONTROL_RESYNC_ACK,
                                packet.controlFrame, 1);
                }
                else if (packet.controlFrame > g_frame &&
                         packet.controlFrame <= maxFrame)
                {
                    u16 localSeed = g_initialRngSeed;
                    int slot = (int)(packet.controlFrame % INPUT_RING_SIZE);
                    if (g_localFrames[slot] == packet.controlFrame)
                    {
                        localSeed = g_localRng[slot];
                    }
                    // The boundary is a future frame, so the ring slot almost
                    // never holds it and localSeed stays at the placeholder.
                    // Say so, otherwise the "RNG mismatch" line logged below
                    // reads like a local detection with a bogus seed.
                    g_GameErrorContext.Log(
                        "info : accepting peer resync boundary frame %lu peer_seed %u (local sample not yet simulated)\r\n",
                        (unsigned long)packet.controlFrame,
                        (unsigned)packet.controlValue);
                    MarkRngMismatch(
                        packet.controlFrame, localSeed,
                        packet.controlValue, g_mode == Netplay::MODE_HOST,
                        packet.controlFrame);
                }
            }
            else if (packet.controlType == CONTROL_RESYNC_ACK &&
                     g_mode == Netplay::MODE_HOST &&
                     g_resyncAwaitingAckMask != 0 &&
                     packet.controlFrame == g_resyncFrame)
            {
                if (fromPlayerId > 0 &&
                    fromPlayerId < TH07_MULTI_MAX_PLAYERS)
                {
                    g_resyncAwaitingAckMask &=
                        (u8)~(1 << fromPlayerId);
                    if (g_testSeconds > 0)
                    {
                        g_GameErrorContext.Log(
                            "info : RNG resync ACK P%d frame %lu remaining_mask 0x%02x\r\n",
                            fromPlayerId + 1,
                            (unsigned long)packet.controlFrame,
                            (unsigned)g_resyncAwaitingAckMask);
                    }
                }
                if (g_resyncAwaitingAckMask == 0)
                {
                    g_resyncFrame = INVALID_FRAME;
                    SetStatus("RNG resync acknowledged by all peers");
                }
            }
            else if (packet.controlType == CONTROL_PLAYER_LIFECYCLE &&
                     g_mode == Netplay::MODE_GUEST &&
                     fromPlayerId == 0 &&
                     IsLifecycleControl(
                         (Netplay::InGameControl)packet.controlValue))
            {
                ScheduleLifecycleControl(
                    (Netplay::InGameControl)packet.controlValue,
                    packet.controlFrame, false);
            }
        }
    }
}

void PumpWaitingWindow(HWND window)
{
    MSG message;
    while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    UpdateWindow(window);
}

bool WaitForPeer(const char *caption)
{
    HWND statusWindow;
    DWORD started = GetTickCount();
    DWORD lastSend = 0;
    DWORD cliGuestLaunchAtTick = 0;
    bool waitingForGuiLaunch = false;
    NetPacket hello;

    statusWindow = CreateWindowExA(
        WS_EX_TOOLWINDOW, "STATIC", caption,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | SS_CENTER,
        CW_USEDEFAULT, CW_USEDEFAULT, 420, 100, NULL, NULL,
        GetModuleHandleA(NULL), NULL);
    if (statusWindow)
    {
        ShowWindow(statusWindow, SW_SHOW);
        UpdateWindow(statusWindow);
    }

    InitializePacket(&hello, PACKET_HELLO);
    while (GetTickCount() - started <
           (DWORD)g_connectTimeoutSeconds * 1000)
    {
        NetPacket packet;
        sockaddr_in from;
        DWORD now = GetTickCount();

        if (statusWindow)
        {
            PumpWaitingWindow(statusWindow);
            if (!IsWindow(statusWindow))
            {
                SetStatus("connection cancelled");
                return false;
            }
        }

        if (g_mode == Netplay::MODE_GUEST && now - lastSend >= 250)
        {
            SendPacket(hello);
            lastSend = now;
        }

        while (ReceivePacket(&packet, &from))
        {
            int fromPlayerId = FindPeerPlayerId(from);
            // A guest already knows the host endpoint. Ignore packets from
            // other local processes before interpreting their version.
            if (g_mode == Netplay::MODE_GUEST &&
                (fromPlayerId != 0 ||
                 !SameAddress(from, g_peerAddresses[0])))
            {
                continue;
            }
            if (!ValidatePacketVersion(packet))
            {
                break;
            }
            if (g_mode == Netplay::MODE_HOST && packet.type == PACKET_HELLO)
            {
                int assignedPlayerId = AssignHostGuestSlot(
                    from, packet.playerName);
                if (assignedPlayerId < 1)
                {
                    continue;
                }
                SendWelcomeToAllGuests(4);
                g_connected = AreAllExpectedPeersConnected();
                if (!g_connected)
                {
                    char waiting[128];
                    sprintf(waiting,
                            "th07_multi_net - Waiting guests (%d/%d)",
                            CountConnectedGuestPeers(), g_playerCount - 1);
                    if (statusWindow)
                    {
                        SetWindowTextA(statusWindow, waiting);
                    }
                    SetStatus(waiting);
                    continue;
                }
                if (statusWindow)
                {
                    DestroyWindow(statusWindow);
                }
                SetStatus(g_playerCount == 3
                              ? "host connected to P2 and P3"
                              : "host connected");
                return true;
            }
            if (g_mode == Netplay::MODE_GUEST &&
                SameAddress(from, g_peerAddresses[0]) &&
                packet.type == PACKET_WELCOME)
            {
                if (!g_connected &&
                    !AcceptWelcomePacket(packet, from, true))
                {
                    continue;
                }
                ApplyPacketPlayerNames(packet);
                if (!g_connected)
                {
                    char waiting[128];
                    sprintf(waiting,
                            "assigned P%d; waiting for all %d players",
                            g_localPlayerSlot + 1, g_playerCount);
                    SetStatus(waiting);
                    if (statusWindow)
                    {
                        SetWindowTextA(statusWindow, waiting);
                    }
                    continue;
                }
                if (g_cliGuestStartBarrierEligible &&
                    (packet.flags & NETPLAY_FLAG_CONNECTION_UI) != 0)
                {
                    // The GUI Host is still in its launcher. Do not create
                    // the game window or advance the title scene until the
                    // same START/COMMIT sequence used by GUI-to-GUI matches
                    // has completed.
                    waitingForGuiLaunch = true;
                    g_GameErrorContext.Log(
                        "info : CLI Guest waiting for GUI Host start barrier\r\n");
                    SetStatus("guest connected; waiting for Host Start Game");
                    continue;
                }
                g_cliGuestStartBarrierEligible = false;
                if (statusWindow)
                {
                    DestroyWindow(statusWindow);
                }
                SetStatus("guest connected");
                return true;
            }
            if (g_mode == Netplay::MODE_GUEST && waitingForGuiLaunch &&
                packet.session == g_session &&
                packet.type == PACKET_CONTROL)
            {
                if (packet.controlType == CONTROL_PEER_EXIT)
                {
                    g_peerExitReceived = true;
                    SetStatus("Host closed the connection");
                    return false;
                }
                if (packet.controlType == CONTROL_START_GAME)
                {
                    SendConnectionUiControlBurst(CONTROL_START_GAME_ACK);
                    SetStatus("Host ready; waiting synchronized launch");
                }
                else if (packet.controlType == CONTROL_START_GAME_COMMIT)
                {
                    DWORD delayMs = packet.controlFrame;
                    if (delayMs < 100 || delayMs > 2000)
                    {
                        delayMs = CONNECTION_UI_START_DELAY_MS;
                    }
                    SendConnectionUiControlBurst(
                        CONTROL_START_GAME_COMMIT_ACK);
                    if (cliGuestLaunchAtTick == 0)
                    {
                        cliGuestLaunchAtTick = GetTickCount() + delayMs;
                        g_GameErrorContext.Log(
                            "info : CLI Guest received GUI start commit; launching in %lu ms\r\n",
                            (unsigned long)delayMs);
                        SetStatus("synchronized launch committed");
                    }
                }
            }
        }
        if (waitingForGuiLaunch && cliGuestLaunchAtTick != 0 &&
            GetTickCount() >= cliGuestLaunchAtTick)
        {
            g_cliGuestStartBarrierEligible = false;
            if (statusWindow)
            {
                DestroyWindow(statusWindow);
            }
            g_GameErrorContext.Log(
                "info : CLI Guest passed GUI Host start barrier\r\n");
            SetStatus("guest connected; synchronized launch");
            return true;
        }
        if (g_protocolMismatch)
        {
            break;
        }
        Sleep(1);
    }

    if (statusWindow)
    {
        DestroyWindow(statusWindow);
    }
    if (g_protocolMismatch)
    {
        SetStatus("protocol version mismatch; use the same build on both peers");
    }
    else
    {
        SetStatus("connection timed out");
    }
    return false;
}

bool ConsumeReconnectRequest()
{
    bool pressed = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (!pressed)
    {
        g_reconnectKeyDown = false;
        return false;
    }
    if (g_reconnectKeyDown)
    {
        return false;
    }
    g_reconnectKeyDown = true;
    return true;
}

bool ReconnectToPeer()
{
    SetStatus("reconnecting; press F8 on both peers");
    g_connected = false;
    g_connectionFailed = false;
    g_rngMismatch = false;
    g_cliGuestStartBarrierEligible = false;
    ResetInputRings();
    if (g_mode == Netplay::MODE_HOST)
    {
        g_session = GetTickCount() ^ GetCurrentProcessId() ^ 0x5245434f;
    }
    else
    {
        g_session = 0;
    }
    if (!WaitForPeer("th07_multi_net - Reconnecting (press F8 on both peers)"))
    {
        g_connectionFailed = true;
        return false;
    }
    ResetInputRings();
    g_GameErrorContext.Log(
        "info : rollback snapshot bytes %lu count %d total %lu history %dF bomb_effect_limit %d\r\n",
        (unsigned long)sizeof(RollbackSnapshot),
        ROLLBACK_SNAPSHOT_COUNT,
        (unsigned long)(sizeof(RollbackSnapshot) *
                        ROLLBACK_SNAPSHOT_COUNT),
        ROLLBACK_HISTORY_FRAMES,
        ROLLBACK_BOMB_EFFECT_SNAPSHOT_COUNT);
    g_connected = true;
    SetStatus("reconnected");
    return true;
}

const i32 TH07_SUPERVISOR_TITLE_STATE = 1;
const i32 TH07_SUPERVISOR_RESULT_STATE = 5;

bool IsSharedUiFrame()
{
    // TH06 combines both controllers while a common menu is active. Keep
    // gameplay input in the P1/P2 lanes, but let either peer operate the
    // pause, retry, title, result, and other shared menus.
    if (g_testInputSyncEnabled && g_testInputSyncInjected &&
        !g_testInputSyncVerified)
    {
        // The lane test deliberately uses gameplay semantics even if the
        // short test run is still transitioning out of the title scene.
        return false;
    }
    return g_resultDetached ||
           g_testUiSyncUiFrame ||
           g_Supervisor.curState != TH07_SUPERVISOR_GAME_STATE ||
           g_GameManager.isInPauseMenu || g_GameManager.isInRetryMenu;
}

bool HasSharedUiInput(u16 localInput, u16 remoteInput)
{
    return ((localInput | remoteInput) & SHARED_UI_INPUTS) != 0;
}

void VerifyTestInputLanes(u16 player1, u16 player2, u32 frame)
{
    u16 directionMask = TH_BUTTON_UP | TH_BUTTON_DOWN;
    bool p1HasUp = (player1 & TH_BUTTON_UP) != 0;
    bool p1HasDown = (player1 & TH_BUTTON_DOWN) != 0;
    bool p2HasUp = (player2 & TH_BUTTON_UP) != 0;
    bool p2HasDown = (player2 & TH_BUTTON_DOWN) != 0;

    if (!g_testInputSyncEnabled || !g_testInputSyncInjected ||
        g_testInputSyncVerified || g_testInputSyncFailureReported ||
        frame < 120)
    {
        return;
    }
    if (p1HasUp && !p1HasDown && p2HasDown && !p2HasUp)
    {
        g_testInputSyncVerified = true;
        g_GameErrorContext.Log(
            "info : test P1/P2 input lanes verified at frame %lu\r\n",
            (unsigned long)frame);
        SetStatus("test P1/P2 input lanes passed");
        return;
    }
    if (frame >= 124)
    {
        g_testInputSyncFailureReported = true;
        g_GameErrorContext.Log(
            "error : P1/P2 input lane synchronization failed (p1 %u, p2 %u, mask %u)\r\n",
            (unsigned int)(player1 & directionMask),
            (unsigned int)(player2 & directionMask),
            (unsigned int)directionMask);
        SetStatus("test P1/P2 input lanes failed");
    }
}

LRESULT CALLBACK NetworkWindowProc(HWND window, UINT message, WPARAM wParam,
                                   LPARAM lParam)
{
    LRESULT result;
    if (g_previousNetworkWindowProc)
    {
        result = CallWindowProcA(g_previousNetworkWindowProc, window, message,
                                 wParam, lParam);
    }
    else
    {
        result = DefWindowProcA(window, message, wParam, lParam);
    }
    if (message == WM_ACTIVATEAPP && Netplay::IsNetworked())
    {
        // GameWindow::WindowProc clears this after the second peer steals
        // focus. Restore it after the original handler so the calculation
        // loop keeps advancing in the background peer as well.
        g_GameWindow.isAppActive = 1;
    }
    return result;
}

void InstallNetworkWindowHook()
{
    WNDPROC currentProc;
    LONG_PTR previousProc;
    if (!Netplay::IsNetworked() || !g_GameWindow.window ||
        g_networkHookWindow == g_GameWindow.window)
    {
        return;
    }
    currentProc = (WNDPROC)GetWindowLongPtrA(g_GameWindow.window,
                                             GWLP_WNDPROC);
    if (!currentProc || currentProc == NetworkWindowProc)
    {
        return;
    }
    previousProc = SetWindowLongPtrA(g_GameWindow.window, GWLP_WNDPROC,
                                     (LONG_PTR)NetworkWindowProc);
    if (previousProc != 0)
    {
        g_previousNetworkWindowProc = (WNDPROC)previousProc;
        g_networkHookWindow = g_GameWindow.window;
    }
}

bool TryReconnectAfterResult()
{
    g_resultReconnectAttempted = true;
    if (!ReconnectToPeer())
    {
        SetStatus("result reconnect failed; press F8 on both peers");
        return false;
    }
    g_resultDetached = false;
    g_resultReconnectAttempted = false;
    g_testResultReconnectEnabled = false;
    // ReconnectToPeer resets the input rings, including the edge latch. Keep
    // the F8 edge consumed for this frame so the normal reconnect path below
    // does not immediately start a second handshake while the key is held.
    g_reconnectKeyDown = true;
    return true;
}

i32 GetConnectionPolicySceneState()
{
    if (g_testResultReconnectEnabled && !g_resultDetached &&
        g_frame >= 120)
    {
        return TH07_SUPERVISOR_RESULT_STATE;
    }
    if (g_testResultReconnectEnabled && g_resultDetached)
    {
        g_testResultPolicyFrames++;
        if (g_testResultPolicyFrames >= 30)
        {
            return TH07_SUPERVISOR_TITLE_STATE;
        }
    }
    return g_Supervisor.curState;
}

// TH06 deliberately does not keep the session active while the score/result
// screen is running. This keeps the two peers from trying to advance gameplay
// state while each player is reading the result, and lets the next title
// transition establish a fresh input window. F8 remains available if the
// automatic title reconnect cannot find the other peer.
bool ApplyResultConnectionPolicy()
{
    bool reconnectPressed;
    i32 sceneState;

    if (!Netplay::IsNetworked())
    {
        return false;
    }

    sceneState = GetConnectionPolicySceneState();

    reconnectPressed = false;
    if (g_resultDetached ||
        sceneState == TH07_SUPERVISOR_RESULT_STATE)
    {
        reconnectPressed = ConsumeReconnectRequest();
    }
    if (sceneState == TH07_SUPERVISOR_RESULT_STATE)
    {
        if (!g_resultDetached)
        {
            g_resultDetached = true;
            g_resultReconnectAttempted = false;
            g_connected = false;
            g_connectionFailed = false;
            g_waitingForRemoteInput = false;
            g_rngMismatch = false;
            g_resyncFrame = INVALID_FRAME;
            g_resyncAwaitingAckMask = 0;
            SetStatus("result screen; network paused");
        }
        if (reconnectPressed)
        {
            TryReconnectAfterResult();
        }
        return g_resultDetached;
    }

    if (g_resultDetached &&
        ((sceneState == TH07_SUPERVISOR_TITLE_STATE &&
          !g_resultReconnectAttempted) ||
         reconnectPressed))
    {
        TryReconnectAfterResult();
    }
    return g_resultDetached;
}

// The TH06 title menu disables Replay in multiplayer. TH07's original menu
// is CP932 and its draw code is kept byte-for-byte, so enforce the same rule
// at the scene boundary as a safety net: cancel a selected user replay and
// rebuild the title menu instead of entering a P1-only replay session.
void BlockMultiplayerReplay()
{
    if (!Netplay::IsMultiplayer() || !g_GameManager.replay ||
        g_GameManager.demo || g_Supervisor.curState != TH07_SUPERVISOR_GAME_STATE)
    {
        return;
    }

    g_GameManager.SetReplay(0);
    g_GameManager.replayStage = 0;
    g_Supervisor.wantedState = 0;
    g_Supervisor.curState = TH07_SUPERVISOR_TITLE_STATE;
    SetStatus("Replay is disabled during multiplayer");
}

void InjectTestReplaySelection()
{
    if (!g_testReplayBlockEnabled || g_testReplayBlockInjected ||
        !Netplay::IsMultiplayer() ||
        g_Supervisor.curState != TH07_SUPERVISOR_GAME_STATE ||
        g_frame < 120)
    {
        return;
    }
    g_GameManager.demo = 0;
    g_GameManager.SetReplay(1);
    g_testReplayBlockInjected = true;
}

u32 HashStateValue(u32 hash, u32 value)
{
    hash ^= value;
    return hash * 16777619u;
}

u32 StateFloatBits(f32 value)
{
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

u32 HashStateBytes(u32 hash, const void *data, u32 size)
{
    const u8 *bytes = (const u8 *)data;
    u32 i;
    for (i = 0; i < size; i++)
    {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

u32 HashTimerState(u32 hash, const ZunTimer &timer)
{
    hash = HashStateValue(hash, (u32)timer.previous);
    hash = HashStateValue(hash, StateFloatBits(timer.subFrame));
    return HashStateValue(hash, (u32)timer.current);
}

u32 HashFloat2State(u32 hash, const Float2 &value)
{
    hash = HashStateValue(hash, StateFloatBits(value.x));
    return HashStateValue(hash, StateFloatBits(value.y));
}

u32 HashVector3State(u32 hash, const D3DXVECTOR3 &value)
{
    hash = HashStateValue(hash, StateFloatBits(value.x));
    hash = HashStateValue(hash, StateFloatBits(value.y));
    return HashStateValue(hash, StateFloatBits(value.z));
}

u32 NormalizeModulePointer(const void *pointer)
{
    ULONG_PTR address;
    ULONG_PTR module;
    if (!pointer)
    {
        return 0;
    }
    address = (ULONG_PTR)pointer;
    module = (ULONG_PTR)GetModuleHandleA(NULL);
    return (u32)(address - module);
}

u32 NormalizeEclPointer(const void *pointer)
{
    if (!pointer)
    {
        return 0;
    }
    if (!g_EclManager.eclFile)
    {
        return NormalizeModulePointer(pointer);
    }
    return (u32)((ULONG_PTR)pointer - (ULONG_PTR)g_EclManager.eclFile);
}

u32 HashEnemyEclContextState(u32 hash, const EnemyEclContext &context)
{
    i32 i;
    hash = HashStateValue(hash, NormalizeEclPointer(context.curInstr));
    hash = HashTimerState(hash, context.time);
    hash = HashStateValue(hash, NormalizeModulePointer((const void *)context.func));
    hash = HashStateValue(hash, NormalizeEclPointer(context.eclExInstr));
    hash = HashStateBytes(hash, &context.eclContextArgs,
                          sizeof(context.eclContextArgs));
    hash = HashTimerState(hash, context.timer2);
    for (i = 0; i < 8; i++)
    {
        const EclInterp &interp = context.interps[i];
        hash = HashStateValue(hash,
                              NormalizeModulePointer((const void *)interp.fn));
        if (interp.fn)
        {
            hash = HashTimerState(hash, interp.timer);
            hash = HashStateBytes(hash, interp.args, sizeof(interp.args));
        }
    }
    hash = HashStateValue(hash, (u32)context.compareRegister);
    hash = HashStateValue(hash, (u32)context.isPeriodicSub);
    return HashStateValue(hash, (u32)(u16)context.subId);
}

u32 HashPlayerLogicalState(u32 hash, const Player &player, u32 playerId)
{
    i32 i;
    hash = HashStateValue(hash, playerId);
    hash = HashVector3State(hash, player.positionCenter);
    hash = HashVector3State(hash, player.prevFramePos);
    hash = HashFloat2State(hash, player.velocity);
    hash = HashStateValue(hash, (u32)player.isBombing);
    hash = HashStateValue(hash, (u32)player.respawnTimer);
    hash = HashStateValue(hash, (u32)player.borderInvulnerabilityTime);
    hash = HashStateValue(hash, (u32)player.bulletGracePeriod);
    hash = HashStateValue(hash, (u32)player.itemType);
    hash = HashStateValue(hash, (u32)(u8)player.playerState);
    hash = HashStateValue(hash, (u32)player.initParam);
    hash = HashStateValue(hash, (u32)(u8)player.optionState);
    hash = HashStateValue(hash, (u32)(u8)player.isFocus);
    hash = HashStateValue(hash, (u32)player.hasBorder);
    hash = HashTimerState(hash, player.focusMovementTimer);
    hash = HashStateValue(hash, (u32)player.playerDirection);
    hash = HashStateValue(hash,
                          StateFloatBits(player.previousHorizontalSpeed));
    hash = HashStateValue(hash,
                          StateFloatBits(player.previousVerticalSpeed));
    hash = HashStateValue(hash, (u32)player.targetingEnemy);
    hash = HashTimerState(hash, player.fireBulletTimer);
    hash = HashTimerState(hash, player.invulnerabilityTimer);
    hash = HashTimerState(hash, player.borderTimer);
    hash = HashStateValue(hash, (u32)player.lifeGiveTimer);
    hash = HashStateValue(hash, StateFloatBits(player.optionAngle));
    hash = HashStateValue(hash, (u32)player.bombInfo.isInUse);
    hash = HashStateValue(hash, (u32)player.bombInfo.isFocus);
    hash = HashStateValue(hash, (u32)player.bombInfo.bombDuration);
    hash = HashStateValue(hash, (u32)player.bombInfo.cherryDrain);
    hash = HashTimerState(hash, player.bombInfo.bombTimer);

    for (i = 0; i < 96; i++)
    {
        const PlayerBullet &bullet = player.bullets[i];
        if (bullet.bulletState == 0)
        {
            continue;
        }
        hash = HashStateValue(hash, (u32)i);
        hash = HashStateValue(hash, (u32)(u16)bullet.bulletState);
        hash = HashStateValue(hash, (u32)(u16)bullet.bulletState2);
        hash = HashVector3State(hash, bullet.pos);
        hash = HashVector3State(hash, bullet.hitboxSize);
        hash = HashFloat2State(hash, bullet.velocity);
        hash = HashFloat2State(hash, bullet.offset);
        hash = HashStateValue(hash, StateFloatBits(bullet.speed));
        hash = HashStateValue(hash, StateFloatBits(bullet.angle));
        hash = HashTimerState(hash, bullet.timer);
        hash = HashStateValue(hash, (u32)(u16)bullet.damage);
        hash = HashStateValue(hash, (u32)(u16)bullet.timerIdx);
        hash = HashStateValue(hash, (u32)(u16)bullet.optionId);
        hash = HashStateValue(hash, (u32)(u16)bullet.trailLength);
        hash = HashStateValue(
            hash, NormalizeModulePointer((const void *)bullet.updateCallback));
        hash = HashStateValue(
            hash, NormalizeModulePointer((const void *)bullet.hitCallback));
    }
    hash = HashStateValue(hash, 0xffffffffu);

    for (i = 0; i < 112; i++)
    {
        const BombProjectile &box = player.bombDamageBoxes[i];
        if (box.lifetime == 0)
        {
            continue;
        }
        hash = HashStateValue(hash, (u32)i);
        hash = HashVector3State(hash, box.pos);
        hash = HashVector3State(hash, box.size);
        hash = HashStateValue(hash, (u32)box.lifetime);
        hash = HashStateValue(hash, (u32)box.itemType);
    }
    hash = HashStateValue(hash, 0xfffffffeu);
    for (i = 0; i < 96; i++)
    {
        const BombClearBox &box = player.bombClearBoxes[i];
        if (box.lifetime == 0)
        {
            continue;
        }
        hash = HashStateValue(hash, (u32)i);
        hash = HashStateBytes(hash, &box.pos, sizeof(box.pos));
        hash = HashStateBytes(hash, &box.size, sizeof(box.size));
        hash = HashStateValue(hash, (u32)box.lifetime);
        hash = HashStateValue(hash, (u32)box.itemType);
    }
    for (i = 0; i < 128; i++)
    {
        const PlayerBombSubInfo &sub = player.bombInfo.subInfo[i];
        if (sub.state == 0)
        {
            continue;
        }
        hash = HashStateValue(hash, (u32)i);
        hash = HashStateValue(hash, (u32)sub.state);
        hash = HashStateValue(hash, (u32)sub.counter);
        hash = HashStateValue(hash, StateFloatBits(sub.accel));
        hash = HashStateValue(hash, StateFloatBits(sub.speed));
        hash = HashStateValue(hash, StateFloatBits(sub.angle));
        hash = HashVector3State(hash, sub.bombRegionPositions);
        hash = HashVector3State(hash, sub.bombRegionVelocities);
        hash = HashVector3State(hash, sub.bombRegionAcceleration);
        hash = HashTimerState(hash, sub.timer);
    }
    return hash;
}

u32 CalculatePlayerStateHash()
{
    u32 hash = 2166136261u;
    i32 playerId;
    if (!g_GameManager.notInMenu ||
        g_Supervisor.curState != TH07_SUPERVISOR_GAME_STATE)
    {
        return 0;
    }
    hash = HashStateValue(hash, (u32)GetActivePlayerMask());
    hash = HashStateValue(hash, (u32)g_absentPlayerMask);
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        if (!IsPlayerSlotActive((u8)playerId))
        {
            continue;
        }
        hash = HashPlayerLogicalState(
            hash, g_Players[playerId], (u32)playerId + 1);
        hash = HashStateValue(
            hash, (u32)GetPlayerLives((u8)playerId));
        hash = HashStateValue(
            hash, (u32)GetPlayerBombs((u8)playerId));
        hash = HashStateValue(
            hash, (u32)GetPlayerPower((u8)playerId));
    }
    return hash != 0 ? hash : 1;
}

u32 HashEnemyLogicalState(u32 hash, const Enemy &enemy, u32 index)
{
    i32 i;
    i32 depth;
    hash = HashStateValue(hash, index);
    hash = HashEnemyEclContextState(hash, enemy.currentContext);
    depth = enemy.stackDepth;
    if (depth < 0)
    {
        depth = 0;
    }
    if (depth > 15)
    {
        depth = 15;
    }
    hash = HashStateValue(hash, (u32)depth);
    for (i = 0; i <= depth; i++)
    {
        hash = HashEnemyEclContextState(hash, enemy.savedContextStack[i]);
    }
    hash = HashStateValue(hash, (u32)enemy.deathCallbackSub);
    hash = HashStateValue(hash, (u32)enemy.runInterrupt);
    hash = HashVector3State(hash, enemy.position);
    hash = HashVector3State(hash, enemy.axisSpeed);
    hash = HashVector3State(hash, enemy.prevPos);
    hash = HashVector3State(hash, enemy.deltaPos);
    hash = HashVector3State(hash, enemy.hitboxSize);
    hash = HashVector3State(hash, enemy.grazeSize);
    hash = HashStateValue(hash, StateFloatBits(enemy.angle));
    hash = HashStateValue(hash, StateFloatBits(enemy.angularVelocity));
    hash = HashStateValue(hash, StateFloatBits(enemy.moveAngle));
    hash = HashStateValue(hash, StateFloatBits(enemy.moveAngularVelocity));
    hash = HashStateValue(hash, StateFloatBits(enemy.moveSpeed));
    hash = HashStateValue(hash, StateFloatBits(enemy.moveAcceleration));
    hash = HashStateValue(hash, StateFloatBits(enemy.moveRadius));
    hash = HashStateValue(hash, StateFloatBits(enemy.moveRadialVelocity));
    hash = HashVector3State(hash, enemy.shootOffset);
    hash = HashVector3State(hash, enemy.moveInterp);
    hash = HashVector3State(hash, enemy.moveInterpStartPos);
    hash = HashTimerState(hash, enemy.moveInterpTimer);
    hash = HashStateValue(hash, (u32)enemy.moveInterpStartTime);
    hash = HashStateValue(hash, (u32)enemy.life);
    hash = HashStateValue(hash, (u32)enemy.maxLife);
    hash = HashStateValue(hash, (u32)enemy.score);
    hash = HashTimerState(hash, enemy.timer);
    hash = HashStateValue(hash, (u32)enemy.shootInterval);
    hash = HashTimerState(hash, enemy.shootIntervalTimer);
    hash = HashStateValue(hash, (u32)enemy.laserIdx);
    hash = HashStateValue(hash, (u32)enemy.itemDrop);
    hash = HashStateValue(hash, (u32)enemy.bossId);
    hash = HashStateValue(hash, (u32)(u8)enemy.flags1);
    hash = HashStateValue(hash, (u32)(u8)enemy.flags2);
    hash = HashStateValue(hash, (u32)(u8)enemy.flags3);
    hash = HashStateValue(hash, (u32)(u8)enemy.flags4);
    hash = HashStateValue(hash, (u32)enemy.spellcardDelayTimer);
    hash = HashStateValue(hash, (u32)enemy.lastDamage);
    hash = HashStateValue(hash, (u32)enemy.effectsNum);
    hash = HashStateValue(hash, enemy.specialEffect ? 1 : 0);
    hash = HashStateBytes(hash, enemy.lifeCallbackThreshold,
                          sizeof(enemy.lifeCallbackThreshold));
    hash = HashStateBytes(hash, enemy.lifeCallbackSub,
                          sizeof(enemy.lifeCallbackSub));
    hash = HashStateValue(hash, (u32)enemy.timerCallbackThreshold);
    hash = HashStateValue(hash, (u32)enemy.timerCallbackSub);
    hash = HashStateValue(hash, (u32)enemy.periodicCallbackSub);
    hash = HashTimerState(hash, enemy.periodicTimer);
    hash = HashTimerState(hash, enemy.periodicCounter);
    hash = HashTimerState(hash, enemy.invincibilityTimer);

    hash = HashStateValue(hash, (u32)(u16)enemy.bulletProps.sprite);
    hash = HashStateValue(hash, (u32)(u16)enemy.bulletProps.spriteOffset);
    hash = HashVector3State(hash, enemy.bulletProps.position);
    hash = HashStateValue(hash, StateFloatBits(enemy.bulletProps.angle1));
    hash = HashStateValue(hash, StateFloatBits(enemy.bulletProps.angle2));
    hash = HashStateValue(hash, StateFloatBits(enemy.bulletProps.speed1));
    hash = HashStateValue(hash, StateFloatBits(enemy.bulletProps.speed2));
    hash = HashStateBytes(hash, enemy.bulletProps.commands,
                          sizeof(enemy.bulletProps.commands));
    hash = HashStateValue(hash, (u32)(u16)enemy.bulletProps.count1);
    hash = HashStateValue(hash, (u32)(u16)enemy.bulletProps.count2);
    hash = HashStateValue(hash, (u32)enemy.bulletProps.aimMode);
    hash = HashStateValue(hash, enemy.bulletProps.flags);

    hash = HashStateValue(hash, (u32)(u16)enemy.laserProps.sprite);
    hash = HashStateValue(hash, (u32)(u16)enemy.laserProps.spriteOffset);
    hash = HashVector3State(hash, enemy.laserProps.position);
    hash = HashStateValue(hash, StateFloatBits(enemy.laserProps.angle1));
    hash = HashStateValue(hash, StateFloatBits(enemy.laserProps.angle2));
    hash = HashStateValue(hash, StateFloatBits(enemy.laserProps.speed1));
    hash = HashStateValue(hash, StateFloatBits(enemy.laserProps.speed2));
    hash = HashStateBytes(hash, enemy.laserProps.commands,
                          sizeof(enemy.laserProps.commands));
    hash = HashStateValue(hash, (u32)enemy.laserProps.type);
    hash = HashStateValue(hash, enemy.laserProps.flags);
    return hash;
}

u32 CalculateWorldStateHash()
{
    u32 hash = 2166136261u;
    i32 i;
    if (!g_GameManager.notInMenu ||
        g_Supervisor.curState != TH07_SUPERVISOR_GAME_STATE)
    {
        return 0;
    }

    hash = HashStateBytes(hash, &g_GlobalEclVars, sizeof(g_GlobalEclVars));
    hash = HashStateValue(hash, (u32)g_EnemyManager.randomItemSpawnIdx);
    hash = HashStateValue(hash, (u32)g_EnemyManager.randomItemTableIdx);
    hash = HashStateValue(hash, (u32)g_EnemyManager.enemyCountReal);
    hash = HashTimerState(hash, g_EnemyManager.timer);
    hash = HashTimerState(hash, g_EnemyManager.timelineTime);
    for (i = 0; i < 16; i++)
    {
        hash = HashTimerState(hash,
                              g_EnemyManager.timelines[i].timelineTime);
        hash = HashStateValue(
            hash,
            NormalizeEclPointer(g_EnemyManager.timelines[i].timelineInstr));
    }
    for (i = 0; i < 481; i++)
    {
        const Enemy &enemy = g_EnemyManager.enemies[i];
        if (!enemy.active)
        {
            continue;
        }
        hash = HashEnemyLogicalState(hash, enemy, (u32)i);
    }
    hash = HashStateValue(hash, 0xffffff00u);

    hash = HashStateValue(hash, (u32)g_BulletManager.bulletCount);
    hash = HashStateValue(hash, (u32)g_BulletManager.screenClearTime);
    hash = HashTimerState(hash, g_BulletManager.time);
    hash = HashStateValue(hash, (u32)g_BulletManager.updateCount);
    hash = HashStateValue(hash, (u32)g_BulletManager.itemType);
    for (i = 0; i < 1025; i++)
    {
        const Bullet &bullet = g_BulletManager.bullets[i];
        if (bullet.state == BULLET_INACTIVE || bullet.state == BULLET_END_ARRAY)
        {
            continue;
        }
        hash = HashStateValue(hash, (u32)i);
        hash = HashStateValue(hash, (u32)bullet.state);
        hash = HashStateValue(hash, (u32)bullet.state2);
        hash = HashVector3State(hash, bullet.pos);
        hash = HashVector3State(hash, bullet.velocity);
        hash = HashStateValue(hash, StateFloatBits(bullet.speed));
        hash = HashStateValue(hash, StateFloatBits(bullet.acceleration));
        hash = HashStateValue(hash, StateFloatBits(bullet.angularVelocity));
        hash = HashStateValue(hash, StateFloatBits(bullet.angle));
        hash = HashTimerState(hash, bullet.timer1);
        hash = HashTimerState(hash, bullet.timer2);
        hash = HashStateValue(hash, (u32)bullet.spawnDelay);
        hash = HashStateValue(hash, (u32)bullet.exFlags);
        hash = HashStateValue(hash, (u32)bullet.moreFlags);
        hash = HashStateValue(hash, (u32)(u16)bullet.spriteOffset);
        hash = HashStateValue(hash, (u32)bullet.outOfBoundsTime);
        hash = HashStateValue(hash, (u32)bullet.spawned);
        hash = HashStateValue(hash, (u32)bullet.grazed);
        hash = HashStateValue(hash, (u32)bullet.curCmdIdx);
        hash = HashStateBytes(hash, bullet.commands, sizeof(bullet.commands));
        hash = HashStateBytes(hash, bullet.commandStates,
                              sizeof(bullet.commandStates));
    }
    hash = HashStateValue(hash, 0xffffff01u);
    for (i = 0; i < 64; i++)
    {
        const Laser &laser = g_BulletManager.lasers[i];
        if (!laser.inUse)
        {
            continue;
        }
        hash = HashStateValue(hash, (u32)i);
        hash = HashVector3State(hash, laser.pos);
        hash = HashStateValue(hash, StateFloatBits(laser.angle));
        hash = HashStateValue(hash, StateFloatBits(laser.startOffset));
        hash = HashStateValue(hash, StateFloatBits(laser.endOffset));
        hash = HashStateValue(hash, StateFloatBits(laser.startLength));
        hash = HashStateValue(hash, StateFloatBits(laser.width));
        hash = HashStateValue(hash, StateFloatBits(laser.targetWidth));
        hash = HashStateValue(hash, StateFloatBits(laser.speed));
        hash = HashStateValue(hash, (u32)laser.startTime);
        hash = HashStateValue(hash, (u32)laser.hitboxStartTime);
        hash = HashStateValue(hash, (u32)laser.duration);
        hash = HashStateValue(hash, (u32)laser.endTime);
        hash = HashStateValue(hash, (u32)laser.hitboxEndTime);
        hash = HashTimerState(hash, laser.timer);
        hash = HashStateValue(hash, (u32)laser.flags);
        hash = HashStateValue(hash, (u32)laser.state);
    }

    hash = HashStateValue(hash, (u32)g_ItemManager.nextIndex);
    hash = HashStateValue(hash, (u32)g_ItemManager.activeItemCount);
    for (i = 0; i < 1101; i++)
    {
        const Item &item = g_ItemManager.items[i];
        if (!item.isInUse)
        {
            continue;
        }
        hash = HashStateValue(hash, (u32)i);
        hash = HashVector3State(hash, item.currentPosition);
        hash = HashVector3State(hash, item.startPosition);
        hash = HashVector3State(hash, item.targetPosition);
        hash = HashTimerState(hash, item.timer);
        hash = HashStateValue(hash, (u32)(u8)item.itemType);
        hash = HashStateValue(hash, (u32)(u8)item.state);
        hash = HashStateValue(hash, (u32)(u8)item.autoCollect);
    }

    hash = HashStateValue(hash, (u32)g_EffectManager.nextIndex);
    hash = HashStateValue(hash, (u32)g_EffectManager.activeEffects);
    hash = HashStateValue(hash, (u32)g_EffectManager.activeEffectsCount);
    for (i = 0; i < 409; i++)
    {
        const Effect &effect = g_EffectManager.effects[i];
        if (!effect.inUseFlag)
        {
            continue;
        }
        hash = HashStateValue(hash, (u32)i);
        hash = HashVector3State(hash, effect.pos1);
        hash = HashVector3State(hash, effect.custom);
        hash = HashVector3State(hash, effect.velocity);
        hash = HashVector3State(hash, effect.acceleration);
        hash = HashVector3State(hash, effect.basePosition);
        hash = HashVector3State(hash, effect.emitterPosition);
        hash = HashVector3State(hash, effect.direction);
        hash = HashStateValue(hash, StateFloatBits(effect.radius));
        hash = HashStateValue(hash, StateFloatBits(effect.angularVelocity));
        hash = HashTimerState(hash, effect.timer);
        hash = HashStateValue(
            hash, NormalizeModulePointer((const void *)effect.callback));
        hash = HashStateValue(hash, (u32)(u8)effect.effectId);
        hash = HashStateValue(hash, (u32)(u8)effect.isFadingOut);
    }

    hash = HashTimerState(hash, g_Stage.scriptTime);
    hash = HashStateValue(hash, (u32)g_Stage.instructionIndex);
    hash = HashStateValue(hash, (u32)g_Stage.stageFrameCounter);
    hash = HashStateValue(hash, g_Stage.stage);
    hash = HashVector3State(hash, g_Stage.position);
    hash = HashStateValue(hash, (u32)g_Stage.scriptWaitTime);
    hash = HashStateValue(hash, (u32)g_Stage.positionInterpEndTime);
    hash = HashVector3State(hash, g_Stage.positionInterpInitial);
    hash = HashStateValue(hash, (u32)g_Stage.positionInterpStartTime);
    return hash != 0 ? hash : 1;
}

i32 GetEnemyPoolIndex(const Enemy *enemy)
{
    if (!enemy || enemy < &g_EnemyManager.enemies[0] ||
        enemy >= &g_EnemyManager.enemies[481])
    {
        return -1;
    }
    return (i32)(enemy - &g_EnemyManager.enemies[0]);
}

// The spell section is the one that differs on every observed real-line
// divergence, but it folds fifteen scalars plus eight bosses into a single
// value, so "spell 1" says almost nothing. These four cover the same ground
// split by origin, which is the same narrowing that turned "something
// differs" into "the enemy section" earlier in this investigation.
u32 CalculateSpellInfoSubHash()
{
    const SpellcardInfo &spell = g_EnemyManager.spellcardInfo;
    u32 hash = 2166136261u;
    hash = HashStateValue(hash, spell.isCapturing);
    hash = HashStateValue(hash, spell.isActive);
    hash = HashStateValue(hash, (u32)spell.captureScore);
    hash = HashStateValue(hash, (u32)spell.grazeBonusScore);
    hash = HashStateValue(hash, (u32)spell.scoreDrainRate);
    hash = HashStateValue(hash, (u32)spell.spellcardIdx);
    hash = HashStateValue(hash, spell.usedBomb);
    return hash;
}

u32 CalculateSpellStageSubHash()
{
    u32 hash = 2166136261u;
    hash = HashTimerState(hash, g_EnemyManager.timer);
    hash = HashStateValue(hash, (u32)g_Stage.spellCardState);
    hash = HashStateValue(hash, (u32)g_Stage.ticksSinceSpellcardStarted);
    hash = HashStateValue(hash, (u32)g_Stage.clearBackground);
    hash = HashStateValue(hash, (u32)g_Stage.numSpellcardVms);
    hash = HashStateValue(hash, (u32)g_Stage.spellcardVmsIdx);
    hash = HashStateValue(hash, (u32)g_Gui.spellcardSecondsRemaining);
    hash = HashStateValue(hash, (u32)g_GameManager.currentStage);
    hash = HashStateValue(hash, (u32)g_GameManager.framesThisStage);
    hash = HashStateValue(
        hash, StateFloatBits(g_Supervisor.effectiveFramerateMultiplier));
    return hash;
}

u32 CalculateSpellBossSubHash()
{
    u32 hash = 2166136261u;
    i32 i;
    for (i = 0; i < 8; i++)
    {
        const Enemy *boss = g_EnemyManager.bosses[i];
        hash = HashStateValue(hash, (u32)(GetEnemyPoolIndex(boss) + 1));
        if (!boss)
        {
            continue;
        }
        hash = HashStateValue(hash, (u32)boss->life);
        hash = HashStateValue(hash, (u32)boss->maxLife);
        hash = HashTimerState(hash, boss->timer);
        hash = HashStateValue(hash, (u32)boss->timerCallbackThreshold);
        hash = HashStateValue(hash, (u32)boss->timerCallbackSub);
        hash = HashStateValue(hash, (u32)boss->runInterrupt);
        hash = HashStateValue(hash, (u32)boss->stackDepth);
        hash = HashStateValue(hash, (u32)boss->spellcardDelayTimer);
    }
    return hash;
}

// Separated because the ECL context is the remaining pointer-bearing pool the
// snapshot copies as raw bytes - the hazard that turned out to be real for
// Stage. If this is the quarter that differs, that is where to look.
u32 CalculateSpellEclSubHash()
{
    u32 hash = 2166136261u;
    i32 i;
    for (i = 0; i < 8; i++)
    {
        const Enemy *boss = g_EnemyManager.bosses[i];
        if (!boss)
        {
            hash = HashStateValue(hash, 0);
            continue;
        }
        hash = HashEnemyEclContextState(hash, boss->currentContext);
    }
    return hash;
}

u32 CalculateSpellStateHash()
{
    u32 hash = 2166136261u;
    i32 i;
    const SpellcardInfo &spell = g_EnemyManager.spellcardInfo;
    if (!g_GameManager.notInMenu ||
        g_Supervisor.curState != TH07_SUPERVISOR_GAME_STATE)
    {
        return 0;
    }
    hash = HashStateValue(hash, spell.isCapturing);
    hash = HashStateValue(hash, spell.isActive);
    hash = HashStateValue(hash, (u32)spell.captureScore);
    hash = HashStateValue(hash, (u32)spell.grazeBonusScore);
    hash = HashStateValue(hash, (u32)spell.scoreDrainRate);
    hash = HashStateValue(hash, (u32)spell.spellcardIdx);
    hash = HashStateValue(hash, spell.usedBomb);
    hash = HashTimerState(hash, g_EnemyManager.timer);
    hash = HashStateValue(hash, (u32)g_Stage.spellCardState);
    hash = HashStateValue(hash, (u32)g_Stage.ticksSinceSpellcardStarted);
    hash = HashStateValue(hash, (u32)g_Stage.clearBackground);
    hash = HashStateValue(hash, (u32)g_Stage.numSpellcardVms);
    hash = HashStateValue(hash, (u32)g_Stage.spellcardVmsIdx);
    hash = HashStateValue(hash, (u32)g_Gui.spellcardSecondsRemaining);
    // lastSpellcardSecondsRemaining is updated by Gui::DrawStageElements,
    // so it depends on render cadence and is not simulation state.
    hash = HashStateValue(hash, (u32)g_GameManager.currentStage);
    hash = HashStateValue(hash, (u32)g_GameManager.framesThisStage);
    hash = HashStateValue(hash,
                          StateFloatBits(g_Supervisor.effectiveFramerateMultiplier));
    for (i = 0; i < 8; i++)
    {
        const Enemy *boss = g_EnemyManager.bosses[i];
        hash = HashStateValue(hash, (u32)(GetEnemyPoolIndex(boss) + 1));
        if (!boss)
        {
            continue;
        }
        hash = HashStateValue(hash, (u32)boss->life);
        hash = HashStateValue(hash, (u32)boss->maxLife);
        hash = HashTimerState(hash, boss->timer);
        hash = HashStateValue(hash, (u32)boss->timerCallbackThreshold);
        hash = HashStateValue(hash, (u32)boss->timerCallbackSub);
        hash = HashStateValue(hash, (u32)boss->runInterrupt);
        hash = HashStateValue(hash, (u32)boss->stackDepth);
        hash = HashStateValue(hash, (u32)boss->spellcardDelayTimer);
        hash = HashEnemyEclContextState(hash, boss->currentContext);
    }
    return hash != 0 ? hash : 1;
}

// A --test-seconds run hashes every frame. Normal play samples instead: the
// full player/world/spell walk is far too expensive to run at 60 Hz next to
// the rollback replay budget, but a divergence never repairs itself, so a
// periodic sample still catches it within a fraction of a second. The
// interval is keyed on the synchronized frame, so every peer samples the same
// frames and the stored hashes stay comparable.
u32 HashTracePlayerBody(const Player &player, u32 playerId);
u32 HashTracePlayerShots(const Player &player);
u32 HashTraceEnemies();
u32 HashTraceBullets();
u32 HashTraceItems();

void RefreshTraceSectionHashesForSlot(int slot, u32 frame)
{
    int playerId;
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        if (IsPlayerSlotActive((u8)playerId))
        {
            g_traceBodyHash[playerId][slot] =
                HashTracePlayerBody(g_Players[playerId], (u32)playerId);
            g_traceShotHash[playerId][slot] =
                HashTracePlayerShots(g_Players[playerId]);
        }
        else
        {
            g_traceBodyHash[playerId][slot] = 0;
            g_traceShotHash[playerId][slot] = 0;
        }
    }
    g_localBodyHash[slot] = 0;
    g_localShotHash[slot] = 0;
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        g_localBodyHash[slot] = HashStateValue(g_localBodyHash[slot],
                                              g_traceBodyHash[playerId][slot]);
        g_localShotHash[slot] = HashStateValue(g_localShotHash[slot],
                                              g_traceShotHash[playerId][slot]);
    }
    if (g_localBodyHash[slot] == 0) { g_localBodyHash[slot] = 1; }
    if (g_localShotHash[slot] == 0) { g_localShotHash[slot] = 1; }
    g_traceEnemyHash[slot] = HashTraceEnemies();
    g_traceBulletHash[slot] = HashTraceBullets();
    g_traceItemHash[slot] = HashTraceItems();
}

bool ShouldSampleDetailedState(u32 frame)
{
    return g_testSeconds > 0 || frame % (u32)STATE_VERIFY_INTERVAL == 0;
}

void CaptureBossSample(u32 frame);

// Perturbs boss HP on the host only, so a run can be checked for the metric
// actually reporting a divergence rather than only for it staying quiet.
// Clearing bits rather than subtracting keeps it idempotent: this frame may be
// executed several times - once live and once per replay that reaches it - and
// a real divergence has to survive every one of them by the same amount.
void ApplyTestBossDesync(u32 frame)
{
    Enemy *boss;
    if (!g_testBossDesyncEnabled || g_mode != Netplay::MODE_HOST ||
        frame % (u32)TEST_BOSS_DESYNC_INTERVAL != 0)
    {
        return;
    }
    if (!g_GameManager.notInMenu ||
        g_Supervisor.curState != TH07_SUPERVISOR_GAME_STATE)
    {
        return;
    }
    boss = g_EnemyManager.bosses[0];
    if (!boss || boss->life <= 0x7f)
    {
        return;
    }
    boss->life &= ~0x7f;
    if (!g_testBossDesyncLogged)
    {
        g_testBossDesyncLogged = true;
        g_GameErrorContext.Log(
            "info : test boss desync applied frame %lu life %d\r\n",
            (unsigned long)frame, boss->life);
    }
}

// Which peer first saw a player die, break its border or lose a bomb.
//
// A divergence report says the body and item sections differ, which is a
// subsystem, not a cause. Player deaths scatter power items through
// SpawnItem's state-2 path, and that path draws two random numbers per
// item, so one peer seeing a death the others did not is enough to move
// the RNG streams apart while enemies and bullets still agree - exactly
// the signature that was measured. Diffing this trace across the three
// peers gives the first frame on which they stopped agreeing, and about
// whom.

// Long enough that a sample is still present when its frame is confirmed.
const int PLAYER_TRACE_RING = DETAILED_STATE_CONFIRM_LAG * 4;
const u32 PLAYER_TRACE_LINE_LIMIT = 400;
const int PLAYER_TRACE_BODY_INTERVAL = 4;
const u32 PLAYER_TRACE_POSITION_LIMIT = 30000;

struct PlayerTraceSample
{
    u32 frame;
    u8 state[TH07_MULTI_MAX_PLAYERS];
    u8 flags[TH07_MULTI_MAX_PLAYERS];
    i16 lives[TH07_MULTI_MAX_PLAYERS];
    i16 bombs[TH07_MULTI_MAX_PLAYERS];
    i16 power[TH07_MULTI_MAX_PLAYERS];
    i32 cherry[TH07_MULTI_MAX_PLAYERS];
    u32 positionBits[TH07_MULTI_MAX_PLAYERS][3];
    u32 velocityBits[TH07_MULTI_MAX_PLAYERS][2];
    i32 respawnTimer[TH07_MULTI_MAX_PLAYERS];
    i32 invulnerablePrevious[TH07_MULTI_MAX_PLAYERS];
    i32 invulnerableCurrent[TH07_MULTI_MAX_PLAYERS];
    u32 invulnerableSubFrame[TH07_MULTI_MAX_PLAYERS];
    u32 bodyHash[TH07_MULTI_MAX_PLAYERS];
};

enum PlayerTraceFlag
{
    PLAYER_TRACE_ACTIVE = 1,
    PLAYER_TRACE_FOCUS = 2,
    PLAYER_TRACE_BORDER = 4,
    PLAYER_TRACE_BOMBING = 8,
    PLAYER_TRACE_RESPAWNING = 16,
    PLAYER_TRACE_INVULNERABLE = 32
};

PlayerTraceSample g_playerTraceRing[PLAYER_TRACE_RING];
PlayerTraceSample g_playerTraceLast;
bool g_playerTraceHasLast = false;
u32 g_playerTraceLines = 0;
u32 g_playerTracePositionLines = 0;

u32 HashTracePlayerBody(const Player &player, u32 playerId);

void ResetPlayerLifecycleTrace()
{
    int index;
    for (index = 0; index < PLAYER_TRACE_RING; index++)
    {
        g_playerTraceRing[index].frame = INVALID_FRAME;
    }
    g_playerTraceHasLast = false;
    g_playerTraceLines = 0;
    g_playerTracePositionLines = 0;
}

// Called from the same place as the boss sample, so a replayed frame
// overwrites the prediction it made the first time through.
void CapturePlayerLifecycleSample(u32 frame)
{
    PlayerTraceSample *sample;
    int playerId;

    if (!g_GameManager.notInMenu ||
        g_Supervisor.curState != TH07_SUPERVISOR_GAME_STATE)
    {
        return;
    }
    sample = &g_playerTraceRing[frame % (u32)PLAYER_TRACE_RING];
    sample->frame = frame;
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        const Player *player = &g_Players[playerId];
        u8 flags = 0;
        if (!IsPlayerSlotActive((u8)playerId))
        {
            sample->state[playerId] = 0;
            sample->flags[playerId] = 0;
            sample->lives[playerId] = -1;
            sample->bombs[playerId] = -1;
            sample->power[playerId] = -1;
            sample->cherry[playerId] = -1;
            sample->positionBits[playerId][0] = 0;
            sample->positionBits[playerId][1] = 0;
            sample->positionBits[playerId][2] = 0;
            sample->velocityBits[playerId][0] = 0;
            sample->velocityBits[playerId][1] = 0;
            sample->respawnTimer[playerId] = 0;
            sample->invulnerablePrevious[playerId] = 0;
            sample->invulnerableCurrent[playerId] = 0;
            sample->invulnerableSubFrame[playerId] = 0;
            sample->bodyHash[playerId] = 0;
            continue;
        }
        flags |= PLAYER_TRACE_ACTIVE;
        if (player->isFocus) { flags |= PLAYER_TRACE_FOCUS; }
        if (player->hasBorder) { flags |= PLAYER_TRACE_BORDER; }
        if (player->bombInfo.isInUse) { flags |= PLAYER_TRACE_BOMBING; }
        if (player->respawnTimer > 0) { flags |= PLAYER_TRACE_RESPAWNING; }
        if (player->invulnerabilityTimer.current > 0)
        {
            flags |= PLAYER_TRACE_INVULNERABLE;
        }
        sample->state[playerId] = (u8)player->playerState;
        sample->flags[playerId] = flags;
        sample->lives[playerId] = (i16)GetPlayerLives((u8)playerId);
        sample->bombs[playerId] = (i16)GetPlayerBombs((u8)playerId);
        sample->power[playerId] = (i16)GetPlayerPower((u8)playerId);
        sample->cherry[playerId] = GetPlayerCherryPlus((u8)playerId);
        sample->positionBits[playerId][0] =
            StateFloatBits(player->positionCenter.x);
        sample->positionBits[playerId][1] =
            StateFloatBits(player->positionCenter.y);
        sample->velocityBits[playerId][0] =
            StateFloatBits(player->velocity.x);
        sample->velocityBits[playerId][1] =
            StateFloatBits(player->velocity.y);
        sample->positionBits[playerId][2] =
            StateFloatBits(player->positionCenter.z);
        sample->respawnTimer[playerId] = (i32)player->respawnTimer;
        sample->invulnerablePrevious[playerId] =
            player->invulnerabilityTimer.previous;
        sample->invulnerableCurrent[playerId] =
            player->invulnerabilityTimer.current;
        sample->invulnerableSubFrame[playerId] =
            StateFloatBits(player->invulnerabilityTimer.subFrame);
        sample->bodyHash[playerId] =
            HashTracePlayerBody(*player, (u32)playerId);
    }
}

bool PlayerTraceSampleDiffers(const PlayerTraceSample *a,
                              const PlayerTraceSample *b)
{
    int playerId;
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        if (a->state[playerId] != b->state[playerId] ||
            a->flags[playerId] != b->flags[playerId] ||
            a->lives[playerId] != b->lives[playerId] ||
            a->bombs[playerId] != b->bombs[playerId] ||
            a->power[playerId] != b->power[playerId] ||
            a->cherry[playerId] != b->cherry[playerId])
        {
            return true;
        }
    }
    return false;
}

// Emits the confirmed frame only. A predicted frame would report
// transitions that the next rollback takes back, and the whole point is
// that a line present on one peer and absent on another means something.
void LogPlayerLifecycleTrace(u32 currentFrame)
{
    PlayerTraceSample *sample;
    u32 frame;
    char line[512];
    char *cursor = line;
    int playerId;

    if (currentFrame < (u32)DETAILED_STATE_CONFIRM_LAG ||
        g_playerTraceLines >= PLAYER_TRACE_LINE_LIMIT)
    {
        return;
    }
    frame = currentFrame - (u32)DETAILED_STATE_CONFIRM_LAG;
    sample = &g_playerTraceRing[frame % (u32)PLAYER_TRACE_RING];
    if (sample->frame != frame)
    {
        return;
    }
    if (g_playerTraceHasLast &&
        !PlayerTraceSampleDiffers(sample, &g_playerTraceLast))
    {
        return;
    }
    g_playerTraceLast = *sample;
    g_playerTraceHasLast = true;
    g_playerTraceLines++;
    cursor += sprintf(cursor, "info : player state frame %lu",
                      (unsigned long)frame);
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        cursor += sprintf(cursor, " P%d s%d f%02x l%d b%d p%d c%ld",
                          playerId + 1, (int)sample->state[playerId],
                          (unsigned)sample->flags[playerId],
                          (int)sample->lives[playerId],
                          (int)sample->bombs[playerId],
                          (int)sample->power[playerId],
                          (long)sample->cherry[playerId]);
    }
    sprintf(cursor, "\r\n");
    NetplayTraceFile("%s", line);
}

// Every field the transmitted body hash is built from, one line per confirmed
// frame, so the three peers' traces can be diffed field by field. This is what
// showed that a reported body divergence was not one: position, velocity,
// timers and resources were identical on every shared frame.
//
// A line per frame is only affordable for an automated run, so it is limited
// to one. Normal play keeps the change-triggered state line instead.
void LogPlayerBodyTrace(u32 currentFrame)
{
    PlayerTraceSample *sample;
    u32 frame;
    char line[1024];
    char *cursor = line;
    int playerId;

    if (g_testSeconds <= 0 ||
        currentFrame < (u32)DETAILED_STATE_CONFIRM_LAG ||
        g_playerTracePositionLines >= PLAYER_TRACE_POSITION_LIMIT)
    {
        return;
    }
    frame = currentFrame - (u32)DETAILED_STATE_CONFIRM_LAG;
    if (frame % (u32)PLAYER_TRACE_BODY_INTERVAL != 0)
    {
        return;
    }
    sample = &g_playerTraceRing[frame % (u32)PLAYER_TRACE_RING];
    if (sample->frame != frame)
    {
        return;
    }
    g_playerTracePositionLines++;
    cursor += sprintf(cursor, "info : player body frame %lu",
                      (unsigned long)frame);
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        cursor += sprintf(
            cursor, " P%d h%08lx p%08lx,%08lx,%08lx v%08lx,%08lx r%ld i%ld/%08lx/%ld",
            playerId + 1,
            (unsigned long)sample->bodyHash[playerId],
            (unsigned long)sample->positionBits[playerId][0],
            (unsigned long)sample->positionBits[playerId][1],
            (unsigned long)sample->positionBits[playerId][2],
            (unsigned long)sample->velocityBits[playerId][0],
            (unsigned long)sample->velocityBits[playerId][1],
            (long)sample->respawnTimer[playerId],
            (long)sample->invulnerablePrevious[playerId],
            (unsigned long)sample->invulnerableSubFrame[playerId],
            (long)sample->invulnerableCurrent[playerId]);
    }
    sprintf(cursor, "\r\n");
    NetplayTraceFileBuffered(line);
}

void RefreshDetailedStateHashesForSlot(int slot, u32 frame)
{
    if (slot < 0 || slot >= INPUT_RING_SIZE)
    {
        return;
    }
    // Rides along here because this is the one capture point that both the
    // live path and the rollback replay loop already call. Hooking the live
    // path alone is what made the boss samples record predictions.
    ApplyTestBossDesync(frame);
    CaptureBossSample(frame);
    CapturePlayerLifecycleSample(frame);
    if (!ShouldSampleDetailedState(frame))
    {
        g_localPlayerHash[slot] = 0;
        g_localWorldHash[slot] = 0;
        g_localSpellHash[slot] = 0;
        return;
    }
    g_localPlayerHash[slot] = CalculatePlayerStateHash();
    g_localWorldHash[slot] = CalculateWorldStateHash();
    g_localSpellHash[slot] = CalculateSpellStateHash();
    RefreshTraceSectionHashesForSlot(slot, frame);
}

// Section hashes for locating a divergence. The composite player/world hashes
// say that something differs but not what, so these narrow it to a subsystem.
// They are independent of the transmitted hashes on purpose: they only have to
// be deterministic and identical across peers so the three logs can be diffed.
u32 HashTracePlayerBody(const Player &player, u32 playerId)
{
    u32 hash = HashStateValue(0, playerId);
    hash = HashVector3State(hash, player.positionCenter);
    hash = HashFloat2State(hash, player.velocity);
    hash = HashStateValue(hash, (u32)(u8)player.playerState);
    hash = HashStateValue(hash, (u32)(u8)player.isFocus);
    hash = HashStateValue(hash, (u32)player.hasBorder);
    hash = HashStateValue(hash, (u32)player.respawnTimer);
    hash = HashStateValue(hash, (u32)player.bombInfo.isInUse);
    hash = HashTimerState(hash, player.invulnerabilityTimer);
    return hash != 0 ? hash : 1;
}

u32 HashTracePlayerShots(const Player &player)
{
    u32 hash = 0;
    i32 i;
    for (i = 0; i < 96; i++)
    {
        const PlayerBullet &bullet = player.bullets[i];
        if (bullet.bulletState == 0)
        {
            continue;
        }
        hash = HashStateValue(hash, (u32)i);
        hash = HashStateValue(hash, (u32)(u16)bullet.bulletState);
        hash = HashVector3State(hash, bullet.pos);
        hash = HashStateValue(hash, (u32)(u16)bullet.damage);
    }
    return hash != 0 ? hash : 1;
}

u32 HashTraceEnemies()
{
    u32 hash = 0;
    i32 i;
    for (i = 0; i < 481; i++)
    {
        const Enemy &enemy = g_EnemyManager.enemies[i];
        if (!enemy.active)
        {
            continue;
        }
        hash = HashStateValue(hash, (u32)i);
        hash = HashStateValue(hash, (u32)enemy.life);
        hash = HashVector3State(hash, enemy.position);
    }
    return hash != 0 ? hash : 1;
}

u32 HashTraceBullets()
{
    u32 hash = 0;
    i32 i;
    for (i = 0; i < 1025; i++)
    {
        const Bullet &bullet = g_BulletManager.bullets[i];
        if (bullet.state == BULLET_INACTIVE ||
            bullet.state == BULLET_END_ARRAY)
        {
            continue;
        }
        hash = HashStateValue(hash, (u32)i);
        hash = HashStateValue(hash, (u32)bullet.state);
        hash = HashVector3State(hash, bullet.pos);
    }
    return hash != 0 ? hash : 1;
}

u32 HashTraceItems()
{
    u32 hash = 0;
    i32 i;
    for (i = 0; i < 1100; i++)
    {
        const Item &item = g_ItemManager.items[i];
        if (!item.isInUse)
        {
            continue;
        }
        hash = HashStateValue(hash, (u32)i);
        hash = HashStateValue(hash, (u32)item.itemType);
        hash = HashStateValue(hash, (u32)item.state);
        hash = HashVector3State(hash, item.currentPosition);
    }
    return hash != 0 ? hash : 1;
}

void LogSpellLifecycle(u32 frame)
{
    i32 active;
    i32 state;
    i32 index;
    if (g_testSeconds <= 0 || !g_GameManager.notInMenu ||
        g_Supervisor.curState != TH07_SUPERVISOR_GAME_STATE)
    {
        return;
    }
    active = g_EnemyManager.spellcardInfo.isActive ? 1 : 0;
    state = g_Stage.spellCardState;
    index = active ? g_EnemyManager.spellcardInfo.spellcardIdx : -1;
    if (active == g_lastLoggedSpellActive &&
        state == g_lastLoggedSpellState &&
        index == g_lastLoggedSpellIndex)
    {
        return;
    }
    g_lastLoggedSpellActive = active;
    g_lastLoggedSpellState = state;
    g_lastLoggedSpellIndex = index;
    g_GameErrorContext.Log(
        "info : spell lifecycle frame %lu active %d stage_state %d index %d boss0 %d boss0_life %d\r\n",
        (unsigned long)frame, active, state, index,
        GetEnemyPoolIndex(g_EnemyManager.bosses[0]),
        g_EnemyManager.bosses[0] ? g_EnemyManager.bosses[0]->life : -1);
}

// Samples boss HP so the three peer logs can be diffed directly. Unlike the
// other diagnostics this is not gated on --test-seconds: a boss that dies on
// one peer but survives on another is exactly the divergence that normal play
// currently cannot report.
//
// Rollback replays revisit sampled frames, and the replayed value is the
// correct one. CaptureBossSample therefore runs on both the live and the replay
// path and overwrites the pending entry; LogBossLifeTrace folds it only once
// the frame is old enough that no further replay can reach it.
void CaptureBossSample(u32 frame)
{
    i32 bossIndex;
    i32 bossCount = 0;
    i32 lifeSum = 0;
    i32 boss0Life = -1;
    i32 boss0MaxLife = -1;

    // Sample on fixed multiples of the synchronized frame, not "at least
    // INTERVAL since the last one". The elapsed-time form makes the whole
    // series depend on which frame a peer happened to take its first sample,
    // so two peers holding identical state produce different accumulators -
    // the same sample count with different values, which reads exactly like
    // a divergence and is not one.
    if (frame % (u32)BOSS_TRACE_INTERVAL != 0)
    {
        return;
    }
    if (g_lastBossTraceFrame != INVALID_FRAME && frame <= g_lastBossTraceFrame)
    {
        return;
    }
    if (!g_GameManager.notInMenu ||
        g_Supervisor.curState != TH07_SUPERVISOR_GAME_STATE)
    {
        return;
    }
    for (bossIndex = 0; bossIndex < BOSS_TRACE_SLOT_COUNT; bossIndex++)
    {
        Enemy *boss = g_EnemyManager.bosses[bossIndex];
        if (!boss)
        {
            continue;
        }
        bossCount++;
        lifeSum += boss->life;
        if (bossIndex == 0)
        {
            boss0Life = boss->life;
            boss0MaxLife = boss->maxLife;
        }
    }
    g_spellPendingSubHash[0] = CalculateSpellInfoSubHash();
    g_spellPendingSubHash[1] = CalculateSpellStageSubHash();
    g_spellPendingSubHash[2] = CalculateSpellBossSubHash();
    g_spellPendingSubHash[3] = CalculateSpellEclSubHash();
    g_bossPendingSampleFrame = frame;
    g_bossPendingBossCount = bossCount;
    g_bossPendingLifeSum = lifeSum;
    g_bossPendingLife0 = boss0Life;
    g_bossPendingMaxLife0 = boss0MaxLife;
}

void LogBossLifeTrace(u32 frame)
{
    i32 bossIndex;

    if (!g_GameManager.notInMenu ||
        g_Supervisor.curState != TH07_SUPERVISOR_GAME_STATE)
    {
        return;
    }
    for (bossIndex = 0; bossIndex < BOSS_TRACE_SLOT_COUNT; bossIndex++)
    {
        Enemy *boss = g_EnemyManager.bosses[bossIndex];
        if (!boss)
        {
            continue;
        }
        // The death moment is the reported symptom, so report it on the exact
        // frame it happens instead of waiting for the next periodic sample.
        // This one is deliberately live: it reports when a peer first believes
        // the boss died, prediction included.
        if (boss->life <= 0 &&
            (g_bossDeathLogged & (u8)(1 << bossIndex)) == 0)
        {
            g_bossDeathLogged |= (u8)(1 << bossIndex);
            g_GameErrorContext.Log(
                "info : boss death frame %lu slot %d pool %d life %d\r\n",
                (unsigned long)frame, bossIndex,
                GetEnemyPoolIndex(boss), boss->life);
        }
        else if (boss->life > 0)
        {
            g_bossDeathLogged &= (u8)~(1 << bossIndex);
        }
    }
    if (g_bossPendingSampleFrame == INVALID_FRAME ||
        frame < g_bossPendingSampleFrame + (u32)DETAILED_STATE_CONFIRM_LAG)
    {
        return;
    }
    g_lastBossTraceFrame = g_bossPendingSampleFrame;
    {
        // Recorded before the no-boss early return: a section can start
        // differing while no boss is on screen, and dropping those samples
        // would hide where the series first parts.
        u32 spellSlot = g_spellCheckpointCount % (u32)SPELL_CHECKPOINT_COUNT;
        int subHashIndex;
        g_spellCheckpointFrames[spellSlot] = g_bossPendingSampleFrame;
        for (subHashIndex = 0; subHashIndex < 4; subHashIndex++)
        {
            g_spellCheckpointSubHash[spellSlot][subHashIndex] =
                g_spellPendingSubHash[subHashIndex];
        }
        g_spellCheckpointCount++;
    }
    if (g_bossPendingBossCount == 0)
    {
        g_bossPendingSampleFrame = INVALID_FRAME;
        return;
    }
    // Fold first: the accumulator has to cover every sample, including the
    // ones whose printed line is later recycled out of the fixed buffer. On a
    // long run the printed lines are all gone by the end, which is how the
    // one measurement that could show a divergence reaching boss state went
    // missing. The accumulator rides in the end-of-run summary instead.
    g_bossTraceAccumulator =
        HashStateValue(g_bossTraceAccumulator, g_bossPendingSampleFrame);
    g_bossTraceAccumulator =
        HashStateValue(g_bossTraceAccumulator, (u32)g_bossPendingBossCount);
    g_bossTraceAccumulator =
        HashStateValue(g_bossTraceAccumulator, (u32)g_bossPendingLifeSum);
    g_bossCheckpointFrames[g_bossTraceSamples % BOSS_CHECKPOINT_COUNT] =
        g_bossPendingSampleFrame;
    g_bossCheckpointAccumulators[g_bossTraceSamples % BOSS_CHECKPOINT_COUNT] =
        g_bossTraceAccumulator;
    g_bossTraceSamples++;
    g_GameErrorContext.Log(
        "info : boss trace frame %lu count %d life0 %d max0 %d sum %d acc %08lx\r\n",
        (unsigned long)g_bossPendingSampleFrame, g_bossPendingBossCount,
        g_bossPendingLife0, g_bossPendingMaxLife0, g_bossPendingLifeSum,
        (unsigned long)g_bossTraceAccumulator);
    g_bossPendingSampleFrame = INVALID_FRAME;
}

void CompareConfirmedRollbackRng(u32 currentFrame)
{
    u32 frame;
    int slot;
    int remotePlayerId;

    if (!g_rollbackEnabled ||
        g_rollbackEarliestFrame != INVALID_FRAME ||
        currentFrame < (u32)DETAILED_STATE_COMPARE_LAG)
    {
        return;
    }
    frame = g_lastRollbackRngComparedFrame == INVALID_FRAME
        ? currentFrame - (u32)DETAILED_STATE_COMPARE_LAG
        : g_lastRollbackRngComparedFrame + 1;
    // Same catch-up as the detailed comparison: a one-frame-per-call walker
    // cannot keep pace with the simulation once it has fallen behind.
    if (currentFrame - frame > (u32)DETAILED_STATE_ABANDON_LAG)
    {
        frame = currentFrame - (u32)DETAILED_STATE_COMPARE_LAG;
    }
    if (frame + (u32)DETAILED_STATE_COMPARE_LAG > currentFrame)
    {
        return;
    }
    slot = (int)(frame % INPUT_RING_SIZE);
    if (g_localFrames[slot] != frame ||
        !g_localRollbackGameplay[slot])
    {
        g_lastRollbackRngComparedFrame = frame;
        return;
    }
    if (g_resyncIgnoreUntilFrame != INVALID_FRAME &&
        frame < g_resyncIgnoreUntilFrame)
    {
        g_lastRollbackRngComparedFrame = frame;
        return;
    }
    if (g_lifecycleComparisonIgnoreUntilFrame != INVALID_FRAME &&
        frame < g_lifecycleComparisonIgnoreUntilFrame)
    {
        g_lastRollbackRngComparedFrame = frame;
        return;
    }
    for (remotePlayerId = 0; remotePlayerId < g_playerCount;
         remotePlayerId++)
    {
        if (!IsExpectedRemotePlayerId(remotePlayerId) ||
            (g_mode == Netplay::MODE_GUEST && remotePlayerId != 0) ||
            (g_mode == Netplay::MODE_HOST && remotePlayerId > 0 &&
             g_hostPeerLifecycleStage[remotePlayerId] !=
                 HOST_PEER_PRESENT))
        {
            continue;
        }
        if (g_remoteFramesByPlayer[remotePlayerId][slot] != frame ||
            !g_remoteRollbackGameplayByPlayer[remotePlayerId][slot])
        {
            // Same reasoning as the detailed comparison: once the frame is far
            // enough behind, its sample is never arriving and holding the
            // walker here would silently disable RNG divergence detection for
            // the rest of the session.
            if (currentFrame - frame > (u32)DETAILED_STATE_ABANDON_LAG)
            {
                g_lastRollbackRngComparedFrame = frame;
            }
            return;
        }
        if (!g_rngMismatch &&
            g_localRng[slot] !=
                g_remoteRngByPlayer[remotePlayerId][slot])
        {
            MarkRngMismatch(
                frame, g_localRng[slot],
                g_remoteRngByPlayer[remotePlayerId][slot], true,
                INVALID_FRAME);
            break;
        }
    }
    g_lastRollbackRngComparedFrame = frame;
}

void CompareConfirmedDetailedState(u32 currentFrame)
{
    u32 frame;
    int slot;
    int remotePlayerId;
    // This runs in normal play as well. It only logs; it never schedules a
    // resync, so a false positive costs one line rather than a desync recovery.
    if (!g_rollbackEnabled ||
        g_rollbackEarliestFrame != INVALID_FRAME ||
        currentFrame < (u32)DETAILED_STATE_COMPARE_LAG)
    {
        g_dsSkipGate++;
        return;
    }
    frame = g_lastDetailedStateComparedFrame == INVALID_FRAME
        ? currentFrame - (u32)DETAILED_STATE_COMPARE_LAG
        : g_lastDetailedStateComparedFrame + 1;
    // The walker only ever advances one frame per call while the simulation
    // also advances one frame per call, so any stretch where this is skipped
    // is never recovered. Left alone it drifts arbitrarily far into the past
    // and stops describing the present at all. Snap back to the confirmation
    // horizon instead of crawling.
    if (currentFrame - frame > (u32)DETAILED_STATE_ABANDON_LAG)
    {
        frame = currentFrame - (u32)DETAILED_STATE_COMPARE_LAG;
    }
    if (frame + (u32)DETAILED_STATE_COMPARE_LAG > currentFrame)
    {
        g_dsSkipHorizon++;
        return;
    }
    slot = (int)(frame % INPUT_RING_SIZE);
    if (g_localFrames[slot] != frame)
    {
        g_dsSkipLocalSlot++;
        return;
    }
    if (!g_localRollbackGameplay[slot])
    {
        g_dsSkipLocalSlot++;
        g_lastDetailedStateComparedFrame = frame;
        return;
    }
    if ((g_resyncIgnoreUntilFrame != INVALID_FRAME &&
         frame < g_resyncIgnoreUntilFrame) ||
        (g_lifecycleComparisonIgnoreUntilFrame != INVALID_FRAME &&
         frame < g_lifecycleComparisonIgnoreUntilFrame))
    {
        g_dsSkipIgnore++;
        g_lastDetailedStateComparedFrame = frame;
        return;
    }
    if (g_localBodyHash[slot] == 0 || g_localShotHash[slot] == 0 ||
        g_traceEnemyHash[slot] == 0 || g_traceBulletHash[slot] == 0 ||
        g_traceItemHash[slot] == 0 || g_localSpellHash[slot] == 0)
    {
        g_dsSkipZeroHash++;
        // Normal play only samples every STATE_VERIFY_INTERVAL frames, so an
        // unsampled frame must advance the walker instead of blocking it.
        // Waiting here would pin the comparison to the first unsampled frame.
        g_lastDetailedStateComparedFrame = frame;
        return;
    }
    for (remotePlayerId = 0; remotePlayerId < g_playerCount;
         remotePlayerId++)
    {
        bool playerMismatch;
        bool worldMismatch;
        bool spellMismatch;
        bool bodyMismatch;
        bool shotMismatch;
        bool enemyMismatch;
        bool bulletMismatch;
        bool itemMismatch;
        if (!IsExpectedRemotePlayerId(remotePlayerId) ||
            (g_mode == Netplay::MODE_GUEST && remotePlayerId != 0) ||
            (g_mode == Netplay::MODE_HOST && remotePlayerId > 0 &&
             g_hostPeerLifecycleStage[remotePlayerId] !=
                 HOST_PEER_PRESENT))
        {
            continue;
        }
        if (g_remoteFramesByPlayer[remotePlayerId][slot] != frame ||
            !g_remoteRollbackGameplayByPlayer[remotePlayerId][slot] ||
            g_remoteBodyHashByPlayer[remotePlayerId][slot] == 0 ||
            g_remoteShotHashByPlayer[remotePlayerId][slot] == 0 ||
            g_remoteEnemyHashByPlayer[remotePlayerId][slot] == 0 ||
            g_remoteBulletHashByPlayer[remotePlayerId][slot] == 0 ||
            g_remoteItemHashByPlayer[remotePlayerId][slot] == 0 ||
            g_remoteSpellHashByPlayer[remotePlayerId][slot] == 0)
        {
            // Waiting is correct while the sample may still arrive, but a
            // frame this far behind never will: the ring slot has already been
            // reused. Holding the walker there stalled it permanently, which
            // is why a high-latency three-player session produced no state
            // comparison at all. Give up on the frame and move on.
            g_dsSkipRemote++;
            if (currentFrame - frame > (u32)DETAILED_STATE_ABANDON_LAG)
            {
                g_lastDetailedStateComparedFrame = frame;
            }
            return;
        }
        g_dsCompared++;
        bodyMismatch = g_localBodyHash[slot] !=
            g_remoteBodyHashByPlayer[remotePlayerId][slot];
        shotMismatch = g_localShotHash[slot] !=
            g_remoteShotHashByPlayer[remotePlayerId][slot];
        enemyMismatch = g_traceEnemyHash[slot] !=
            g_remoteEnemyHashByPlayer[remotePlayerId][slot];
        bulletMismatch = g_traceBulletHash[slot] !=
            g_remoteBulletHashByPlayer[remotePlayerId][slot];
        itemMismatch = g_traceItemHash[slot] !=
            g_remoteItemHashByPlayer[remotePlayerId][slot];
        spellMismatch = g_localSpellHash[slot] !=
            g_remoteSpellHashByPlayer[remotePlayerId][slot];
        playerMismatch = bodyMismatch || shotMismatch;
        worldMismatch = enemyMismatch || bulletMismatch || itemMismatch;
        if (!(playerMismatch || worldMismatch || spellMismatch))
        {
            if (g_dsStreak[remotePlayerId] != 0)
            {
                g_dsTransientCount++;
            }
            g_dsStreak[remotePlayerId] = 0;
            continue;
        }
        if (g_dsStreak[remotePlayerId] == 0)
        {
            g_dsStreakFirstFrame[remotePlayerId] = frame;
        }
        g_dsStreak[remotePlayerId]++;
        if (g_dsStreak[remotePlayerId] >= (u32)STATE_MISMATCH_STREAK &&
            !g_detailedStateMismatch)
        {
            g_detailedStateMismatch = true;
            g_firstDetailedStateMismatchFrame = frame;
            // Per-player lives/bombs/power are in the transmitted player hash
            // but in none of the trace sections, so a report of "body matches,
            // shots differ" cannot say whether the shots differ because the
            // power behind them already did. Recorded locally on each peer
            // rather than added as a section, which would change the packet
            // layout and the protocol version.
            {
                i32 reportId;
                char resources[192];
                char *cursor = resources;
                resources[0] = '\0';
                for (reportId = 0; reportId < TH07_MULTI_MAX_PLAYERS;
                     reportId++)
                {
                    cursor += sprintf(cursor, " P%d %d/%d/%d", reportId + 1,
                                      GetPlayerLives((u8)reportId),
                                      GetPlayerBombs((u8)reportId),
                                      GetPlayerPower((u8)reportId));
                }
                g_GameErrorContext.Log(
                    "info : divergence resources lives/bombs/power%s frame %lu\r\n",
                    resources, (unsigned long)frame);
            }
            g_GameErrorContext.Log(
                "info : divergence spell info %08lx stage %08lx boss %08lx ecl %08lx frame %lu\r\n",
                (unsigned long)CalculateSpellInfoSubHash(),
                (unsigned long)CalculateSpellStageSubHash(),
                (unsigned long)CalculateSpellBossSubHash(),
                (unsigned long)CalculateSpellEclSubHash(),
                (unsigned long)frame);
            g_GameErrorContext.Log(
                "error : persistent state divergence from frame %lu peer P%d body %d %08lx/%08lx shot %d %08lx/%08lx enemy %d %08lx/%08lx bullet %d %08lx/%08lx item %d %08lx/%08lx spell %d %08lx/%08lx global %08lx/%08lx rng %u/%u\r\n",
                (unsigned long)g_dsStreakFirstFrame[remotePlayerId],
                remotePlayerId + 1,
                bodyMismatch ? 1 : 0,
                (unsigned long)g_localBodyHash[slot],
                (unsigned long)g_remoteBodyHashByPlayer[remotePlayerId][slot],
                shotMismatch ? 1 : 0,
                (unsigned long)g_localShotHash[slot],
                (unsigned long)g_remoteShotHashByPlayer[remotePlayerId][slot],
                enemyMismatch ? 1 : 0,
                (unsigned long)g_traceEnemyHash[slot],
                (unsigned long)g_remoteEnemyHashByPlayer[remotePlayerId][slot],
                bulletMismatch ? 1 : 0,
                (unsigned long)g_traceBulletHash[slot],
                (unsigned long)g_remoteBulletHashByPlayer[remotePlayerId][slot],
                itemMismatch ? 1 : 0,
                (unsigned long)g_traceItemHash[slot],
                (unsigned long)g_remoteItemHashByPlayer[remotePlayerId][slot],
                spellMismatch ? 1 : 0,
                (unsigned long)g_localSpellHash[slot],
                (unsigned long)g_remoteSpellHashByPlayer
                    [remotePlayerId][slot],
                (unsigned long)g_localStateHash[slot],
                (unsigned long)g_remoteStateHashByPlayer
                    [remotePlayerId][slot],
                (unsigned)g_localRng[slot],
                (unsigned)g_remoteRngByPlayer[remotePlayerId][slot]);
            SetStatus("stable rollback state mismatch; see log.txt");
        }
    }
    if (!g_detailedStateMismatch && frame != 0 && frame % 1800 == 0)
    {
        g_GameErrorContext.Log(
            "info : stable rollback state verified through frame %lu against all authoritative peers\r\n",
            (unsigned long)frame);
    }
    g_lastDetailedStateComparedFrame = frame;
}

// This deliberately hashes only stable logical values. Pointer addresses,
// allocation order, and render-only objects are process-local and must not
// participate in a network comparison. The checksum is an early divergence
// detector; the full TH07 object graph is still not serialized for rollback.
u32 CalculateLogicalStateHash()
{
    u32 hash = 2166136261u;
    ZunGlobals *globals = g_GameManager.globals;
    i32 playerId;

    // The input ring also covers the title/menu transition so both peers can
    // reach the same quick-start boundary.  Menu objects and timers are
    // intentionally local, however, and may be at different draw frames
    // before gameplay begins.  Do not turn that expected startup difference
    // into an RNG resync.  A zero hash means "not gameplay" and is ignored by
    // the comparison below.
    if (!g_GameManager.notInMenu ||
        g_Supervisor.curState != TH07_SUPERVISOR_GAME_STATE)
    {
        return 0;
    }

    hash = HashStateValue(hash, g_GameManager.flags);
    hash = HashStateValue(hash, (u32)g_GameManager.difficulty);
    hash = HashStateValue(hash, (u32)GetActivePlayerMask());
    hash = HashStateValue(hash, (u32)g_absentPlayerMask);
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        if (!IsPlayerSlotActive((u8)playerId))
        {
            continue;
        }
        hash = HashStateValue(
            hash, (u32)Netplay::GetPlayerCharacter((u8)playerId));
        hash = HashStateValue(
            hash, (u32)Netplay::GetPlayerShot((u8)playerId));
        hash = HashStateValue(
            hash, (u32)GetPlayerLives((u8)playerId));
        hash = HashStateValue(
            hash, (u32)GetPlayerBombs((u8)playerId));
        hash = HashStateValue(
            hash, (u32)GetPlayerPower((u8)playerId));
    }
    hash = HashStateValue(hash, (u32)g_GameManager.isPaused);
    hash = HashStateValue(hash, (u32)g_GameManager.isInPauseMenu);
    hash = HashStateValue(hash, (u32)g_GameManager.isInRetryMenu);
    hash = HashStateValue(hash, (u32)g_GameManager.currentStage);
    hash = HashStateValue(hash, (u32)g_GameManager.framesThisStage);
    hash = HashStateValue(hash, (u32)g_GameManager.stageRngSeed);
    hash = HashStateValue(hash, (u32)g_GameManager.cherry);
    hash = HashStateValue(hash, (u32)g_GameManager.cherryPlus);
    hash = HashStateValue(hash, (u32)g_GameManager.cherryMax);
    hash = HashStateValue(hash, (u32)g_GameManager.rank.rank);
    hash = HashStateValue(hash, (u32)g_GameManager.subrank);
    hash = HashStateValue(
        hash, StateFloatBits(g_Supervisor.effectiveFramerateMultiplier));
    hash = HashStateValue(hash, (u32)g_Rng.seed);
    hash = HashStateValue(hash, (u32)g_Rng.seedBackup);
    hash = HashStateValue(hash, g_Rng.generationCount);
    if (globals)
    {
        hash = HashStateValue(hash, 1);
        hash = HashStateValue(hash, globals->score);
        hash = HashStateValue(hash, globals->guiScore);
        hash = HashStateValue(hash, globals->guiScoreDifference);
        hash = HashStateValue(hash, StateFloatBits(globals->livesRemaining));
        hash = HashStateValue(hash, StateFloatBits(globals->bombsRemaining));
        hash = HashStateValue(hash, StateFloatBits(globals->currentPower));
        hash = HashStateValue(hash, (u32)globals->grazeInStage);
        hash = HashStateValue(hash, (u32)globals->grazeInTotal);
        hash = HashStateValue(hash, (u32)globals->spellCardsCaptured);
        hash = HashStateValue(hash, (u32)globals->numRetries);
        hash = HashStateValue(hash, (u32)globals->pointItemsCollectedThisStage);
        hash = HashStateValue(hash, (u32)globals->pointItemsCollectedForExtend);
        hash = HashStateValue(hash, (u32)globals->extendsFromPointItems);
        hash = HashStateValue(hash, (u32)globals->nextNeededPointItemsForExtend);
        hash = HashStateValue(hash, (u32)globals->cherryStart);
    }
    else
    {
        hash = HashStateValue(hash, 0);
    }
    return hash != 0 ? hash : 1;
}

bool IsBombEffectCalcCallback(ChainCallback callback)
{
    return callback == (ChainCallback)BombEffects::OnUpdateFadeIn ||
        callback == (ChainCallback)BombEffects::OnUpdateFadeOut ||
        callback == (ChainCallback)BombEffects::OnUpdatePulse ||
        callback == (ChainCallback)BombEffects::OnUpdateScreenShake;
}

u32 CalculateCalcChainSignature()
{
    ChainElem *element = &g_Chain.calcChain;
    u32 hash = 2166136261u;
    i32 count = 0;
    i32 traversed = 0;

    // BombEffects nodes are rebuilt from their complete object snapshots, so
    // their heap addresses must not make the stable-chain signature fail.
    while (element && traversed < 256)
    {
        if (!IsBombEffectCalcCallback(element->callback))
        {
            hash = HashStateValue(hash,
                                  (u32)(ULONG_PTR)element->callback);
            hash = HashStateValue(hash, (u32)(ULONG_PTR)element->arg);
            hash = HashStateValue(hash, (u32)(i32)element->priority);
            count++;
        }
        element = element->next;
        traversed++;
    }
    return HashStateValue(hash, (u32)count);
}

bool IsRollbackGameplayState()
{
    return (g_mode == Netplay::MODE_HOST || g_mode == Netplay::MODE_GUEST) &&
        g_rollbackEnabled &&
        g_GameManager.notInMenu && !g_GameManager.replay &&
        !g_GameManager.isPaused && !g_GameManager.isInPauseMenu &&
        !g_GameManager.isInRetryMenu &&
        g_Supervisor.curState == TH07_SUPERVISOR_GAME_STATE;
}

bool IsRollbackGameplayFrame()
{
    return IsRollbackGameplayState() && !g_rollbackReplaying;
}

void RefreshRollbackPredictionState()
{
    i32 signature;
    bool gameplay;

    if (!g_rollbackEnabled ||
        (g_mode != Netplay::MODE_HOST && g_mode != Netplay::MODE_GUEST))
    {
        return;
    }
    gameplay = IsRollbackGameplayState();
    signature = (g_Supervisor.curState & 0xff) |
        (g_GameManager.notInMenu ? 0x100 : 0) |
        (g_GameManager.replay ? 0x200 : 0) |
        (g_GameManager.isPaused ? 0x400 : 0) |
        (g_GameManager.isInPauseMenu ? 0x800 : 0) |
        (g_GameManager.isInRetryMenu ? 0x1000 : 0) |
        (g_GameManager.isTimeStopped ? 0x2000 : 0) |
        (gameplay ? 0x4000 : 0) |
        (g_rollbackPredictionActive ? 0x8000 : 0);
    if (g_testSeconds > 0 && signature != g_lastRollbackStateSignature)
    {
        g_lastRollbackStateSignature = signature;
        g_GameErrorContext.Log(
            "info : rollback state frame %lu supervisor %d menu %d replay %d paused %d pause_menu %d retry_menu %d time_stop %d prediction %d\r\n",
            (unsigned long)g_frame, g_Supervisor.curState,
            g_GameManager.notInMenu ? 0 : 1,
            g_GameManager.replay ? 1 : 0,
            g_GameManager.isPaused ? 1 : 0,
            g_GameManager.isInPauseMenu ? 1 : 0,
            g_GameManager.isInRetryMenu ? 1 : 0,
            g_GameManager.isTimeStopped ? 1 : 0,
            g_rollbackPredictionActive ? 1 : 0);
    }
    if (g_rollbackPredictionActive && !gameplay)
    {
        g_rollbackPredictionActive = false;
        if (g_testSeconds > 0)
        {
            g_GameErrorContext.Log(
                "info : rollback prediction suspended at frame %lu\r\n",
                (unsigned long)g_frame);
        }
    }
}

bool IsRollbackPredictionFrame()
{
    // Fixed managers, both players, and heap-backed BombEffects are all part
    // of RollbackSnapshot. Prediction starts only after both peers reported
    // gameplay readiness on the same exact-input frame; this prevents a fast
    // loader from exhausting the rollback tail while its peer is still loading.
    return g_rollbackPredictionActive && IsRollbackGameplayFrame();
}

bool SaveRollbackBombEffects(RollbackSnapshot *snapshot)
{
    ChainElem *element = g_Chain.calcChain.next;

    snapshot->bombEffectCount = 0;
    while (element)
    {
        if (IsBombEffectCalcCallback(element->callback))
        {
            if (!element->arg ||
                snapshot->bombEffectCount >=
                    ROLLBACK_BOMB_EFFECT_SNAPSHOT_COUNT)
            {
                return false;
            }
            memcpy(snapshot->bombEffects[snapshot->bombEffectCount],
                   element->arg, sizeof(BombEffects));
            snapshot->bombEffectCount++;
        }
        element = element->next;
    }
    if (snapshot->bombEffectCount > g_rollbackMaxBombEffects)
    {
        g_rollbackMaxBombEffects = snapshot->bombEffectCount;
    }
    return true;
}

void RemoveRollbackBombEffects()
{
    ChainElem *element = g_Chain.calcChain.next;

    while (element)
    {
        ChainElem *next = element->next;
        if (IsBombEffectCalcCallback(element->callback))
        {
            g_Chain.Cut(element);
        }
        element = next;
    }
}

bool RestoreRollbackBombEffects(const RollbackSnapshot *snapshot)
{
    u32 i;

    if (snapshot->bombEffectCount > ROLLBACK_BOMB_EFFECT_SNAPSHOT_COUNT)
    {
        return false;
    }
    RemoveRollbackBombEffects();
    for (i = 0; i < snapshot->bombEffectCount; i++)
    {
        BombEffects saved;
        BombEffects *restored;
        ChainElem *calcChain;
        ChainElem *drawChain;

        memcpy(&saved, snapshot->bombEffects[i], sizeof(saved));
        restored = BombEffects::RegisterChain(
            saved.type, saved.duration, saved.args[0], saved.args[1],
            saved.args[2]);
        if (!restored)
        {
            return false;
        }
        calcChain = restored->calcChain;
        drawChain = restored->drawChain;
        memcpy(restored, &saved, sizeof(saved));
        restored->calcChain = calcChain;
        restored->drawChain = drawChain;
    }
    return true;
}

// Snapshots only describe the stage they were taken in. Drop the whole
// history when the stage changes rather than letting a later rewind restore
// pointers into STD data that has since been freed.
void InvalidateRollbackSnapshotsOnStageChange()
{
    i32 stage = (i32)g_GameManager.currentStage;
    int i;

    // Control arm for measuring what this invalidation is worth. Comparing the
    // fixed build against an older one would also compare two different
    // measurement instruments, and the instrument is what was wrong the last
    // time this effect was evaluated. One binary, one switch.
    if (g_testKeepStaleStageSnapshots)
    {
        return;
    }
    if (stage == g_rollbackSnapshotStage)
    {
        return;
    }
    g_rollbackSnapshotStage = stage;
    for (i = 0; i < ROLLBACK_SNAPSHOT_COUNT; i++)
    {
        g_rollbackSnapshots[i].simulationFrame = INVALID_FRAME;
    }
    // A correction pending across the boundary can no longer be replayed
    // from any surviving keyframe either.
    g_rollbackEarliestFrame = INVALID_FRAME;
    g_rollbackStageInvalidations++;
    g_GameErrorContext.Log(
        "info : rollback history invalidated for stage %d\r\n", stage);
}

void SaveRollbackSnapshot(u32 simulationFrame)
{
    RollbackSnapshot *snapshot =
        &g_rollbackSnapshots[GetRollbackSnapshotIndex(simulationFrame)];
    i32 playerId;

    InvalidateRollbackSnapshotsOnStageChange();

    // Every variable-length/optional field is reset explicitly below. A full
    // memset would write another 16 MiB before the manager copies and was a
    // major source of 40-fps drops in delay-0 rollback.
    snapshot->simulationFrame = INVALID_FRAME;
    snapshot->hasGlobals = 0;
    snapshot->hasDefaultConfig = 0;
    snapshot->hasGuiImpl = 0;
    snapshot->hasAnmOffset = 0;
    snapshot->bombEffectCount = 0;
    snapshot->chainSignature = CalculateCalcChainSignature();
    snapshot->globalsAddress = g_GameManager.globals;
    snapshot->defaultConfigAddress = g_GameManager.defaultCfg;
    snapshot->guiImplAddress = g_Gui.impl;
    snapshot->delay = g_delay;
    snapshot->inputArmed = g_inputArmed ? 1 : 0;
    snapshot->previousControlKeys = g_previousControlKeys;
    snapshot->synchronizedControl = (u16)g_synchronizedControl;
    snapshot->insaneMode = g_insaneMode ? 1 : 0;
    snapshot->activePlayerMask = GetActivePlayerMask();
    snapshot->absentPlayerMask = g_absentPlayerMask;
    snapshot->departedPlayerMask = g_departedPlayerMask;
    if (!SaveRollbackBombEffects(snapshot))
    {
        snapshot->simulationFrame = INVALID_FRAME;
        return;
    }
    if (g_AnmManager)
    {
        snapshot->hasAnmOffset = 1;
        snapshot->anmOffset = g_AnmManager->offset;
    }

    memcpy(snapshot->gameManager, &g_GameManager, sizeof(g_GameManager));
    if (g_GameManager.defaultCfg)
    {
        snapshot->hasDefaultConfig = 1;
        memcpy(snapshot->defaultConfig, g_GameManager.defaultCfg,
               sizeof(GameConfiguration));
    }
    if (g_GameManager.globals)
    {
        snapshot->hasGlobals = 1;
        memcpy(snapshot->globals, g_GameManager.globals, sizeof(ZunGlobals));
    }
    memcpy(snapshot->rng, &g_Rng, sizeof(g_Rng));
    memcpy(snapshot->multiplayerResources,
           g_MultiplayerPlayerResources,
           sizeof(g_MultiplayerPlayerResources));
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        memcpy(snapshot->players[playerId], &g_Players[playerId],
               sizeof(Player));
    }
    memcpy(snapshot->enemyManager, &g_EnemyManager, sizeof(g_EnemyManager));
    memcpy(snapshot->bulletManager, &g_BulletManager,
           sizeof(g_BulletManager));
    memcpy(snapshot->itemManager, &g_ItemManager, sizeof(g_ItemManager));
    memcpy(snapshot->effectManager, &g_EffectManager,
           sizeof(g_EffectManager));
    memcpy(snapshot->stage, &g_Stage, sizeof(g_Stage));
    memcpy(snapshot->gui, &g_Gui, sizeof(g_Gui));
    if (g_Gui.impl)
    {
        snapshot->hasGuiImpl = 1;
        memcpy(snapshot->guiImpl, g_Gui.impl, sizeof(GuiImpl));
    }
    memcpy(snapshot->asciiManager, &g_AsciiManager, sizeof(g_AsciiManager));
    memcpy(snapshot->supervisor, &g_Supervisor, sizeof(g_Supervisor));
    memcpy(snapshot->globalEclVars, &g_GlobalEclVars,
           sizeof(g_GlobalEclVars));
    memcpy(snapshot->curFrameRawInputs, g_CurFrameRawInputs,
           sizeof(g_CurFrameRawInputs));
    memcpy(snapshot->curFrameGameInputs, g_CurFrameGameInputs,
           sizeof(g_CurFrameGameInputs));
    memcpy(snapshot->lastFrameRawInputs, g_LastFrameRawInputs,
           sizeof(g_LastFrameRawInputs));
    memcpy(snapshot->lastFrameGameInputs, g_LastFrameGameInputs,
           sizeof(g_LastFrameGameInputs));
    snapshot->isEighthFrameOfHeldInput = g_IsEighthFrameOfHeldInput;
    snapshot->numOfFramesInputsWereHeld = g_NumOfFramesInputsWereHeld;
    snapshot->simulationFrame = simulationFrame;
}

bool RestoreRollbackSnapshot(const RollbackSnapshot *snapshot)
{
    i32 playerId;
    if (!snapshot || snapshot->simulationFrame == INVALID_FRAME ||
        snapshot->globalsAddress != g_GameManager.globals ||
        snapshot->defaultConfigAddress != g_GameManager.defaultCfg ||
        snapshot->guiImplAddress != g_Gui.impl ||
        snapshot->chainSignature != CalculateCalcChainSignature())
    {
        return false;
    }

    memcpy(&g_GameManager, snapshot->gameManager, sizeof(g_GameManager));
    if (snapshot->hasDefaultConfig && g_GameManager.defaultCfg)
    {
        memcpy(g_GameManager.defaultCfg, snapshot->defaultConfig,
               sizeof(GameConfiguration));
    }
    if (snapshot->hasGlobals && g_GameManager.globals)
    {
        memcpy(g_GameManager.globals, snapshot->globals, sizeof(ZunGlobals));
    }
    memcpy(&g_Rng, snapshot->rng, sizeof(g_Rng));
    memcpy(g_MultiplayerPlayerResources,
           snapshot->multiplayerResources,
           sizeof(g_MultiplayerPlayerResources));
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        memcpy(&g_Players[playerId], snapshot->players[playerId],
               sizeof(Player));
    }
    memcpy(&g_EnemyManager, snapshot->enemyManager, sizeof(g_EnemyManager));
    memcpy(&g_BulletManager, snapshot->bulletManager,
           sizeof(g_BulletManager));
    memcpy(&g_ItemManager, snapshot->itemManager, sizeof(g_ItemManager));
    memcpy(&g_EffectManager, snapshot->effectManager,
           sizeof(g_EffectManager));
    memcpy(&g_Stage, snapshot->stage, sizeof(g_Stage));
    memcpy(&g_Gui, snapshot->gui, sizeof(g_Gui));
    if (snapshot->hasGuiImpl && g_Gui.impl)
    {
        memcpy(g_Gui.impl, snapshot->guiImpl, sizeof(GuiImpl));
    }
    memcpy(&g_AsciiManager, snapshot->asciiManager, sizeof(g_AsciiManager));
    memcpy(&g_Supervisor, snapshot->supervisor, sizeof(g_Supervisor));
    memcpy(&g_GlobalEclVars, snapshot->globalEclVars,
           sizeof(g_GlobalEclVars));
    if (!RestoreRollbackBombEffects(snapshot))
    {
        return false;
    }
    if (snapshot->hasAnmOffset && g_AnmManager)
    {
        g_AnmManager->offset = snapshot->anmOffset;
    }
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        g_PlayerActive[playerId] =
            (snapshot->activePlayerMask & (1 << playerId)) != 0;
    }
    g_absentPlayerMask = snapshot->absentPlayerMask;
    g_departedPlayerMask = snapshot->departedPlayerMask;
    g_delay = snapshot->delay;
    g_inputArmed = snapshot->inputArmed != 0;
    g_previousControlKeys = snapshot->previousControlKeys;
    g_synchronizedControl =
        (Netplay::InGameControl)snapshot->synchronizedControl;
    g_insaneMode = snapshot->insaneMode != 0;
    memcpy(g_CurFrameRawInputs, snapshot->curFrameRawInputs,
           sizeof(g_CurFrameRawInputs));
    memcpy(g_CurFrameGameInputs, snapshot->curFrameGameInputs,
           sizeof(g_CurFrameGameInputs));
    memcpy(g_LastFrameRawInputs, snapshot->lastFrameRawInputs,
           sizeof(g_LastFrameRawInputs));
    memcpy(g_LastFrameGameInputs, snapshot->lastFrameGameInputs,
           sizeof(g_LastFrameGameInputs));
    g_IsEighthFrameOfHeldInput = snapshot->isEighthFrameOfHeldInput;
    g_NumOfFramesInputsWereHeld = snapshot->numOfFramesInputsWereHeld;
    return true;
}

u32 FindOldestPredictedFrame()
{
    u32 oldest = INVALID_FRAME;
    i32 playerId;
    i32 i;

    for (playerId = 0; playerId < g_playerCount; playerId++)
    {
        if (!IsExpectedRemotePlayerId(playerId))
        {
            continue;
        }
        for (i = 0; i < INPUT_RING_SIZE; i++)
        {
            u32 predictedFrame =
                g_predictedRemoteFramesByPlayer[playerId][i];
            if (predictedFrame != INVALID_FRAME &&
                (oldest == INVALID_FRAME || predictedFrame < oldest))
            {
                oldest = predictedFrame;
            }
        }
    }
    return oldest;
}

u16 PredictRemoteInputForPlayer(u32 frame, int remotePlayerId)
{
    i32 offset;

    if (remotePlayerId < 0 || remotePlayerId >= TH07_MULTI_MAX_PLAYERS)
    {
        return 0;
    }

    // Holding the last confirmed movement/shoot state is less disruptive than
    // freezing the local player whenever one UDP datagram is late. Edge-like
    // controls (bomb/menu/debug actions) are intentionally not predicted.
    for (offset = 1; offset < INPUT_RING_SIZE && frame >= (u32)offset;
         offset++)
    {
        u32 previousFrame = frame - (u32)offset;
        int previousSlot = (int)(previousFrame % INPUT_RING_SIZE);
        if (g_remoteFramesByPlayer[remotePlayerId][previousSlot] ==
                previousFrame &&
            g_predictedRemoteFramesByPlayer[remotePlayerId][previousSlot] !=
                previousFrame)
        {
            return g_remoteInputsByPlayer[remotePlayerId][previousSlot] &
                ROLLBACK_PREDICTABLE_INPUTS;
        }
    }
    return 0;
}

u16 PredictRemoteInput(u32 frame)
{
    return PredictRemoteInputForPlayer(frame, GetPrimaryRemotePlayerId());
}

bool GetRemoteInputForRollback(u32 frame, int remotePlayerId,
                               u16 *remoteInput)
{
    int slot = (int)(frame % INPUT_RING_SIZE);

    if (!remoteInput)
    {
        return false;
    }
    if (remotePlayerId < 0 ||
        remotePlayerId >= TH07_MULTI_MAX_PLAYERS)
    {
        return false;
    }
    if (g_remoteFramesByPlayer[remotePlayerId][slot] == frame)
    {
        *remoteInput = g_remoteInputsByPlayer[remotePlayerId][slot];
        return true;
    }
    if (g_predictedRemoteFramesByPlayer[remotePlayerId][slot] == frame)
    {
        *remoteInput =
            g_predictedRemoteInputsByPlayer[remotePlayerId][slot];
        return true;
    }
    return false;
}

bool SynchronizeRollbackFrame(
    u16 synchronizedPlayers[TH07_MULTI_MAX_PLAYERS])
{
    u32 currentFrame = g_frame;
    u32 targetFrame;
    int currentSlot;
    int targetSlot;
    u16 combinedInput = 0;
    u16 controls[TH07_MULTI_MAX_PLAYERS] = {0, 0, 0};
    int playerId;

    if (currentFrame < (u32)g_delay)
    {
        for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
        {
            synchronizedPlayers[playerId] = 0;
        }
        g_frame++;
        return true;
    }
    currentSlot = (int)(currentFrame % INPUT_RING_SIZE);
    targetFrame = currentFrame - (u32)g_delay;
    targetSlot = (int)(targetFrame % INPUT_RING_SIZE);
    if (g_localFrames[currentSlot] != currentFrame)
    {
        return false;
    }
    for (playerId = 0; playerId < g_playerCount; playerId++)
    {
        if (!IsInputRoutingSlotActive(playerId))
        {
            synchronizedPlayers[playerId] = 0;
            controls[playerId] = Netplay::INGAME_CONTROL_NONE;
        }
        else if (playerId == g_localPlayerSlot)
        {
            synchronizedPlayers[playerId] = g_localInputs[targetSlot];
            controls[playerId] = g_localControls[targetSlot];
        }
        else if (!GetRemoteInputForRollback(
                     targetFrame, playerId,
                     &synchronizedPlayers[playerId]))
        {
            return false;
        }
        else
        {
            controls[playerId] =
                g_remoteControlsByPlayer[playerId][targetSlot];
        }
    }
    for (; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        synchronizedPlayers[playerId] = 0;
    }
    ApplyLifecycleInputPolicy(targetFrame, synchronizedPlayers, controls);
    for (playerId = 0; playerId < g_playerCount; playerId++)
    {
        combinedInput |= synchronizedPlayers[playerId];
    }

    // Recalculate the metadata carried by the redundant input record after
    // the corrected state has been restored. The actual local controller
    // sample remains unchanged.
    g_localRng[currentSlot] = g_Rng.seed;
    g_localStateHash[currentSlot] = CalculateLogicalStateHash();
    RefreshDetailedStateHashesForSlot(currentSlot, currentFrame);
    if (IsSharedUiFrame() ||
        (combinedInput & SHARED_UI_INPUTS) != 0)
    {
        synchronizedPlayers[0] = combinedInput;
        for (playerId = 1; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
        {
            synchronizedPlayers[playerId] = 0;
        }
    }
    g_synchronizedControl = Netplay::INGAME_CONTROL_NONE;
    for (playerId = 0; playerId < g_playerCount; playerId++)
    {
        Netplay::InGameControl control = NormalizeControl(controls[playerId]);
        if (control != Netplay::INGAME_CONTROL_NONE)
        {
            g_synchronizedControl = control;
            break;
        }
    }
    ApplySynchronizedControl(Netplay::ConsumeSynchronizedControl());
    g_frame++;
    return true;
}

bool PerformPendingRollback()
{
    u32 mismatchFrame = g_rollbackEarliestFrame;
    u32 currentFrame = g_frame;
    u32 snapshotFrame;
    u32 replayFrameCount;
    int currentSlot;
    RollbackSnapshot *snapshot;
    LARGE_INTEGER replayStarted;
    LARGE_INTEGER replayFinished;
    LARGE_INTEGER replayFrequency;
    u32 replayTimeUs = 0;

    if (!g_rollbackEnabled || g_rollbackReplaying ||
        mismatchFrame == INVALID_FRAME)
    {
        return true;
    }
    // Also checked here, not only when saving: a stage can change between two
    // keyframes, and the restore path must not pick up a snapshot from the
    // previous stage in that window.
    InvalidateRollbackSnapshotsOnStageChange();
    if (g_rollbackEarliestFrame == INVALID_FRAME)
    {
        return true;
    }
    if (mismatchFrame > currentFrame)
    {
        // The corrected frame has not been simulated yet, so the real input
        // will simply be used when it is reached. Nothing to replay.
        g_rollbackEarliestFrame = INVALID_FRAME;
        return true;
    }
    if (!IsRollbackGameplayFrame())
    {
        // Menus, pauses, and stage transitions are not gameplay frames, so the
        // simulation is not advancing and no further divergence accumulates.
        // The correction MUST NOT be dropped here: discarding it silently left
        // the peers permanently out of sync, and because the RNG resync path
        // only repairs the seed, the diverged world state never recovered.
        // Hold it and replay once gameplay resumes.
        if (!g_rollbackDeferralLogged)
        {
            g_rollbackDeferralLogged = true;
            g_GameErrorContext.Log(
                "info : rollback correction deferred at frame %lu (pending %lu); not a gameplay frame\r\n",
                (unsigned long)currentFrame, (unsigned long)mismatchFrame);
        }
        return true;
    }
    snapshotFrame = mismatchFrame + (u32)g_delay;
    snapshotFrame -= snapshotFrame % ROLLBACK_SNAPSHOT_INTERVAL;
    if (snapshotFrame > currentFrame ||
        currentFrame - snapshotFrame >= ROLLBACK_HISTORY_FRAMES)
    {
        // Unrecoverable: the frame that needs correcting has aged out of the
        // keyframe history. Stalling is the safe failure - continuing would
        // silently bake in a divergence - but it has to be visible in the log
        // instead of only in the status text.
        if (!g_rollbackWindowExhaustedLogged)
        {
            g_rollbackWindowExhaustedLogged = true;
            g_GameErrorContext.Log(
                "error : rollback window exhausted frame %lu pending %lu snapshot %lu history %d; state cannot be repaired, press F8 to resynchronize\r\n",
                (unsigned long)currentFrame, (unsigned long)mismatchFrame,
                (unsigned long)snapshotFrame, ROLLBACK_HISTORY_FRAMES);
        }
        SetStatus("rollback window exhausted; waiting for input");
        return false;
    }
    snapshot = &g_rollbackSnapshots[GetRollbackSnapshotIndex(snapshotFrame)];
    if (snapshot->simulationFrame != snapshotFrame ||
        !RestoreRollbackSnapshot(snapshot))
    {
        if (!g_rollbackSnapshotMissingLogged)
        {
            g_rollbackSnapshotMissingLogged = true;
            g_GameErrorContext.Log(
                "error : rollback snapshot unavailable frame %lu pending %lu snapshot %lu stored %lu; state cannot be repaired, press F8 to resynchronize\r\n",
                (unsigned long)currentFrame, (unsigned long)mismatchFrame,
                (unsigned long)snapshotFrame,
                (unsigned long)snapshot->simulationFrame);
        }
        SetStatus("rollback snapshot unavailable; waiting for input");
        return false;
    }
    g_rollbackDeferralLogged = false;
    g_rollbackRestoredBombEffects += snapshot->bombEffectCount;

    g_rollbackEarliestFrame = INVALID_FRAME;
    g_frame = snapshotFrame;
    g_rollbackReplaying = true;
    replayFrameCount = 0;
    QueryPerformanceCounter(&replayStarted);
    while (g_frame < currentFrame)
    {
        u32 replayFrameBefore = g_frame;
        int replaySlot = (int)(g_frame % INPUT_RING_SIZE);
        if (IsRollbackSnapshotFrame(g_frame))
        {
            // Rebuild keyframes from the corrected timeline. Keeping stale
            // intermediate snapshots was one cause of later visual/state
            // corruption after several corrections in quick succession.
            SaveRollbackSnapshot(g_frame);
        }
        // Rebuild this frame's verification metadata from the corrected
        // timeline too. Only the frame the replay ends on used to be
        // refreshed, so every frame in between kept the hash it had when it
        // was first simulated from a wrong prediction. A peer that predicted
        // correctly stored the right value, and comparing the two reported a
        // divergence that had already been repaired.
        if (g_localFrames[replaySlot] == g_frame)
        {
            g_localRng[replaySlot] = g_Rng.seed;
            g_localStateHash[replaySlot] = CalculateLogicalStateHash();
            RefreshDetailedStateHashesForSlot(replaySlot, g_frame);
        }
        if (g_Chain.RunCalcChain() <= 0)
        {
            g_rollbackReplaying = false;
            g_GameErrorContext.Log(
                "info : rollback replay failed at frame %lu\r\n",
                (unsigned long)replayFrameBefore);
            SetStatus("rollback replay failed");
            return false;
        }
        if (g_frame <= replayFrameBefore)
        {
            g_rollbackReplaying = false;
            g_GameErrorContext.Log(
                "error : rollback replay made no progress at frame %lu\r\n",
                (unsigned long)g_frame);
            SetStatus("rollback replay stalled");
            return false;
        }
        replayFrameCount++;
    }
    QueryPerformanceCounter(&replayFinished);
    if (QueryPerformanceFrequency(&replayFrequency) &&
        replayFrequency.QuadPart > 0)
    {
        replayTimeUs = (u32)(
            (replayFinished.QuadPart - replayStarted.QuadPart) * 1000000 /
            replayFrequency.QuadPart);
    }
    g_rollbackReplaying = false;
    g_rollbackCount++;
    g_rollbackReplayFrames += replayFrameCount;
    if (replayFrameCount > g_rollbackMaxReplayFrames)
    {
        g_rollbackMaxReplayFrames = replayFrameCount;
    }
    g_rollbackReplayTimeUs += replayTimeUs;
    if (replayTimeUs > g_rollbackMaxReplayTimeUs)
    {
        g_rollbackMaxReplayTimeUs = replayTimeUs;
    }
    currentSlot = (int)(currentFrame % INPUT_RING_SIZE);
    if (g_localFrames[currentSlot] == currentFrame)
    {
        // The current frame was captured before the late packet triggered the
        // replay. Refresh its pre-step metadata so the normal diagnostics do
        // not compare the old predicted state with the corrected state.
        g_localRng[currentSlot] = g_Rng.seed;
        g_localStateHash[currentSlot] = CalculateLogicalStateHash();
        RefreshDetailedStateHashesForSlot(currentSlot, currentFrame);
    }
    if (g_rollbackCount <= 16)
    {
        g_GameErrorContext.Log(
            "info : rollback correction %lu mismatch %lu replay %lu time_us %lu effects %lu\r\n",
            (unsigned long)g_rollbackCount,
            (unsigned long)mismatchFrame,
            (unsigned long)replayFrameCount,
            (unsigned long)replayTimeUs,
            (unsigned long)snapshot->bombEffectCount);
    }
    if (IsRollbackPredictionFrame() && IsRollbackSnapshotFrame(g_frame))
    {
        SaveRollbackSnapshot(g_frame);
    }
    return g_frame == currentFrame;
}

void SendInputs(u32 newestFrame)
{
    NetPacket packet;
    int i;
    int relayIndex;
    InitializePacket(&packet, PACKET_INPUT);
    packet.newestFrame = newestFrame;
    for (i = 0; i < REDUNDANT_INPUT_COUNT; i++)
    {
        if (newestFrame >= (u32)i)
        {
            u32 frame = newestFrame - i;
            int slot = (int)(frame % INPUT_RING_SIZE);
            if (g_localFrames[slot] == frame)
            {
                packet.records[i].frame = frame;
                packet.records[i].input = g_localInputs[slot];
                packet.records[i].rngSeed = g_localRng[slot];
                packet.records[i].control =
                    (g_localControls[slot] & INPUT_RECORD_CONTROL_MASK) |
                    (g_localRollbackGameplay[slot]
                         ? INPUT_RECORD_ROLLBACK_GAMEPLAY
                         : 0);
            }
        }
    }
    for (i = 0; i < VERIFICATION_RECORD_COUNT; i++)
    {
        // Shifted back by the confirmation lag: a record still inside the
        // rollback window describes a frame this peer may yet re-simulate,
        // and sending it invites the receiver to compare a prediction
        // against its own replayed result.
        u32 confirmedNewest = newestFrame >= (u32)DETAILED_STATE_CONFIRM_LAG
            ? newestFrame - (u32)DETAILED_STATE_CONFIRM_LAG
            : 0;
        if (newestFrame >= (u32)DETAILED_STATE_CONFIRM_LAG &&
            confirmedNewest >= (u32)i)
        {
            u32 frame = confirmedNewest - (u32)i;
            int slot = (int)(frame % INPUT_RING_SIZE);
            if (g_localFrames[slot] == frame)
            {
                packet.verifications[i].frame = frame;
                packet.verifications[i].stateHash = g_localStateHash[slot];
                packet.verifications[i].bodyHash = g_localBodyHash[slot];
                packet.verifications[i].shotHash = g_localShotHash[slot];
                packet.verifications[i].enemyHash = g_traceEnemyHash[slot];
                packet.verifications[i].bulletHash = g_traceBulletHash[slot];
                packet.verifications[i].itemHash = g_traceItemHash[slot];
                packet.verifications[i].spellHash = g_localSpellHash[slot];
            }
        }
    }
    if (g_mode == Netplay::MODE_HOST)
    {
        int remotePlayerId;
        relayIndex = 0;
        for (remotePlayerId = 1;
             remotePlayerId < g_playerCount &&
             relayIndex < TH07_MULTI_MAX_GUESTS;
             remotePlayerId++)
        {
            if (Netplay::IsPlayerPermanentlyDeparted(
                    (u8)remotePlayerId))
            {
                continue;
            }
            packet.relaySlots[relayIndex] = (u8)remotePlayerId;
            for (i = 0; i < RELAY_INPUT_COUNT; i++)
            {
                if (newestFrame >= (u32)i)
                {
                    u32 frame = newestFrame - (u32)i;
                    int slot = (int)(frame % INPUT_RING_SIZE);
                    if (g_remoteFramesByPlayer[remotePlayerId][slot] ==
                        frame)
                    {
                        packet.relayRecords[relayIndex][i].frame = frame;
                        packet.relayRecords[relayIndex][i].input =
                            g_remoteInputsByPlayer[remotePlayerId][slot];
                        packet.relayRecords[relayIndex][i].control =
                            (g_remoteControlsByPlayer[remotePlayerId][slot] &
                             INPUT_RECORD_CONTROL_MASK) |
                            (g_remoteRollbackGameplayByPlayer
                                 [remotePlayerId][slot]
                                 ? INPUT_RECORD_ROLLBACK_GAMEPLAY
                                 : 0);
                    }
                }
            }
            relayIndex++;
        }
        packet.relaySlotCount = (u8)relayIndex;
    }
    SendPacketToAllPeers(packet);
}

Netplay::InGameControl NormalizeControl(u16 value)
{
    if (value <= Netplay::INGAME_CONTROL_P3_LEFT)
    {
        return (Netplay::InGameControl)value;
    }
    return Netplay::INGAME_CONTROL_NONE;
}

Netplay::InGameControl SelectSynchronizedControl(u16 local, u16 remote)
{
    Netplay::InGameControl localControl = NormalizeControl(local);
    Netplay::InGameControl remoteControl = NormalizeControl(remote);

    // Match TH06: when both peers request a control on the same frame, the
    // host wins. The guest still applies the exact same selected event.
    if (localControl != Netplay::INGAME_CONTROL_NONE &&
        remoteControl != Netplay::INGAME_CONTROL_NONE)
    {
        return g_mode == Netplay::MODE_HOST ? localControl : remoteControl;
    }
    return localControl != Netplay::INGAME_CONTROL_NONE ? localControl
                                                         : remoteControl;
}

void ApplySynchronizedControl(Netplay::InGameControl control)
{
    D3DXVECTOR3 itemPosition;
    i32 itemType;
    const char *itemName;

    if (control == Netplay::INGAME_CONTROL_NONE ||
        g_GameManager.globals == NULL || g_Supervisor.curState != 2)
    {
        return;
    }
    if (g_testSeconds > 0)
    {
        g_GameErrorContext.Log(
            "info : synchronized control %d at frame %lu\r\n",
            (int)control, (unsigned long)g_frame);
    }

    switch (control)
    {
    case Netplay::INGAME_CONTROL_P2_ABSENT:
    case Netplay::INGAME_CONTROL_P2_RESUME:
    case Netplay::INGAME_CONTROL_P2_LEFT:
    case Netplay::INGAME_CONTROL_P3_ABSENT:
    case Netplay::INGAME_CONTROL_P3_RESUME:
    case Netplay::INGAME_CONTROL_P3_LEFT:
        ApplyPlayerLifecycleTransition(control);
        break;
    case Netplay::INGAME_CONTROL_QUICK_QUIT:
        g_GameManager.isInPauseMenu = 0;
        g_GameManager.isInRetryMenu = 0;
        g_GameManager.isPaused = 0;
        g_Supervisor.curState = 1;
        SetStatus("control: quick quit");
        break;
    case Netplay::INGAME_CONTROL_QUICK_RESTART:
        g_GameManager.isInPauseMenu = 0;
        g_GameManager.isInRetryMenu = 0;
        g_GameManager.isPaused = 0;
        g_Supervisor.curState = 10;
        SetStatus("control: quick restart");
        break;
    case Netplay::INGAME_CONTROL_INF_LIFE:
        itemType = ITEM_LIFE;
        itemName = "life";
        goto spawn_control_item;
    case Netplay::INGAME_CONTROL_INF_BOMB:
        itemType = ITEM_BOMB;
        itemName = "bomb";
        goto spawn_control_item;
    case Netplay::INGAME_CONTROL_INF_POWER:
        itemType = ITEM_FULL_POWER;
        itemName = "full power";
    spawn_control_item:
        // Match TH06's control path: F2/F3/F4 drop one item at a
        // deterministic random position. Use TH07's current playfield
        // rectangle instead of TH06's hard-coded 384x448 coordinates.
        itemPosition.x = g_GameManager.arcadeRegionTopLeftPos.x +
                         g_Rng.GetRandomFloatInRange(
                             g_GameManager.arcadeRegionSize.x);
        itemPosition.y = g_GameManager.arcadeRegionTopLeftPos.y +
                         g_Rng.GetRandomFloatInRange(
                             g_GameManager.arcadeRegionSize.y);
        itemPosition.z = 0.0f;
        if (g_ItemManager.SpawnItem(&itemPosition, itemType, 0) == NULL)
        {
            g_GameErrorContext.Log(
                "error : control %s item spawn failed\r\n", itemName);
            SetStatus("control item spawn failed");
        }
        else
        {
            if (g_controlTestEnabled)
            {
                g_GameErrorContext.Log(
                    "info : control %s item spawned\r\n", itemName);
            }
            SetStatus("control item spawned");
        }
        break;
    case Netplay::INGAME_CONTROL_ADD_DELAY:
        Netplay::AdjustDelay(1);
        break;
    case Netplay::INGAME_CONTROL_DEC_DELAY:
        Netplay::AdjustDelay(-1);
        break;
    case Netplay::INGAME_CONTROL_INSANE_MODE:
        Netplay::ToggleInsaneMode();
        break;
    default:
        break;
    }
}

void ApplyAutomaticTestBomb(u16 *localPlayer1, u16 *localPlayer2)
{
    u32 frame;
    bool pulse;

    if (!g_autoBomb || !g_quickStartEnabled || g_quickStartPending ||
        g_GameManager.replay)
    {
        return;
    }
    frame = g_mode == Netplay::MODE_LOCAL ? g_testInputSyncLocalFrame : g_frame;
    // A one-frame pulse exercises the real bomb edge-trigger path without
    // turning the test into a permanent held-bomb input. Network peers pulse
    // their local lane on the same simulation frame; Local 2P alternates lanes.
    pulse = frame % 300 == 60;
    if (!pulse)
    {
        return;
    }
    if (g_mode == Netplay::MODE_LOCAL && (frame / 300) % 2 != 0)
    {
        *localPlayer2 |= TH_BUTTON_BOMB;
    }
    else
    {
        *localPlayer1 |= TH_BUTTON_BOMB;
    }
    if (g_autoBombPulseCount < 12)
    {
        g_GameErrorContext.Log(
            "info : auto bomb pulse at frame %lu\r\n",
            (unsigned long)frame);
    }
    g_autoBombPulseCount++;
}

void ApplyRollbackInputTest(u16 *localPlayer1, u16 *localPlayer2)
{
    u32 frame;
    u32 borderAge;
    u32 segment;
    u16 direction;
    bool bombPulse;

    if (!g_testRollbackInputEnabled || g_mode == Netplay::MODE_SINGLE ||
        !g_quickStartEnabled ||
        g_quickStartPending || g_GameManager.replay ||
        !g_GameManager.notInMenu)
    {
        return;
    }
    frame = g_frame;
    if (!g_testRollbackBorderActivated && g_Player2Active)
    {
        g_Player.ActivateBorder();
        g_Player2.ActivateBorder();
        g_testRollbackBorderActivated = true;
        g_GameErrorContext.Log(
            "info : rollback border test requested for both players at frame %lu\r\n",
            (unsigned long)frame);
    }

    // The first request can be queued while the two ships are spawning. Once
    // both are alive, retry the test-only request so the test clock starts on
    // a real active border instead of measuring the queued READY state.
    if (g_testRollbackBorderActivated &&
        g_testRollbackBorderStartFrame == INVALID_FRAME &&
        g_Player.playerState == PLAYER_STATE_ALIVE &&
        g_Player2.playerState == PLAYER_STATE_ALIVE &&
        (g_Player.hasBorder != BORDER_ACTIVE ||
         g_Player2.hasBorder != BORDER_ACTIVE))
    {
        g_Player.ActivateBorder();
    }

    // ActivateBorder() intentionally queues the request while either ship is
    // spawning. Start the test clock only after both local states are really
    // active; otherwise the old 25/55-frame assertions could fire before the
    // bomb edge was even eligible to break the shared border.
    if (g_testRollbackBorderActivated &&
        g_testRollbackBorderStartFrame == INVALID_FRAME &&
        g_Player.hasBorder == BORDER_ACTIVE &&
        g_Player2.hasBorder == BORDER_ACTIVE)
    {
        g_testRollbackBorderStartFrame = frame;
        g_GameErrorContext.Log(
            "info : rollback border test activated both players at frame %lu\r\n",
            (unsigned long)frame);
    }
    borderAge = g_testRollbackBorderStartFrame != INVALID_FRAME
        ? frame - g_testRollbackBorderStartFrame
        : 0;
    if (g_testRollbackBorderActivated &&
        g_testRollbackBorderStartFrame != INVALID_FRAME &&
        !g_testRollbackP1BorderVerified && borderAge >= 60)
    {
        g_testRollbackP1BorderVerified = true;
        if (g_Player.hasBorder != BORDER_NONE)
        {
            g_GameErrorContext.Log(
                "error : rollback P1 border break did not complete\r\n");
        }
        else
        {
            g_GameErrorContext.Log(
                "info : rollback P1 border break verified at frame %lu\r\n",
                (unsigned long)frame);
        }
    }
    if (g_testRollbackBorderActivated &&
        g_testRollbackBorderStartFrame != INVALID_FRAME &&
        !g_testRollbackP2BorderVerified && borderAge >= 90)
    {
        g_testRollbackP2BorderVerified = true;
        if (g_Player2.hasBorder != BORDER_NONE)
        {
            g_GameErrorContext.Log(
                "error : rollback P2 border break did not complete\r\n");
        }
        else
        {
            g_GameErrorContext.Log(
                "info : rollback P2 border break verified at frame %lu\r\n",
                (unsigned long)frame);
        }
    }
    segment = (frame / 45) % 4;
    if (g_mode == Netplay::MODE_HOST)
    {
        switch (segment)
        {
        case 0:
            direction = TH_BUTTON_RIGHT;
            break;
        case 1:
            direction = TH_BUTTON_DOWN;
            break;
        case 2:
            direction = TH_BUTTON_LEFT;
            break;
        default:
            direction = TH_BUTTON_UP;
            break;
        }
    }
    else
    {
        switch (segment)
        {
        case 0:
            direction = TH_BUTTON_LEFT;
            break;
        case 1:
            direction = TH_BUTTON_UP;
            break;
        case 2:
            direction = TH_BUTTON_RIGHT;
            break;
        default:
            direction = TH_BUTTON_DOWN;
            break;
        }
    }
    *localPlayer1 &= (u16)~(TH_BUTTON_DIRECTION | TH_BUTTON_FOCUS);
    *localPlayer1 |= direction | TH_BUTTON_SHOOT;
    if ((frame / 30) % 2 != 0)
    {
        *localPlayer1 |= TH_BUTTON_FOCUS;
    }

    // Keep the bomb edge independent on each peer. Rollback mode sends this
    // through the confirmed two-frame bomb lane while movement/shoot remain
    // zero-delay and predictable. The flag is test-only.
    bombPulse = g_mode == Netplay::MODE_HOST
        ? borderAge == 15 || frame % 150 == 30
        : borderAge == 45 || frame % 150 == 75;
    if (bombPulse)
    {
        *localPlayer1 |= TH_BUTTON_BOMB;
        if (g_autoBombPulseCount < 16)
        {
            g_GameErrorContext.Log(
                "info : rollback input bomb pulse at frame %lu mode %d\r\n",
                (unsigned long)frame, (int)g_mode);
        }
        g_autoBombPulseCount++;
    }
    (void)localPlayer2;
}

u32 GetTestRandomInputValue(u32 frame, u32 lane, u32 salt)
{
    u32 value = frame * 0x45d9f3b + lane * 0x9e3779b9 + salt;
    value ^= value >> 16;
    value *= 0x7feb352d;
    value ^= value >> 15;
    value *= 0x846ca68b;
    value ^= value >> 16;
    return value;
}

void ApplyPlayerLifecycleTransition(Netplay::InGameControl control)
{
    int playerId = GetLifecycleControlPlayerId(control);
    u8 playerBit;

    if (playerId < 1 || playerId >= g_playerCount)
    {
        return;
    }
    playerBit = (u8)(1 << playerId);
    if (IsAbsentLifecycleControl(control))
    {
        if (IsPlayerSlotActive((u8)playerId))
        {
            g_absentPlayerMask |= playerBit;
        }
        if (!g_rollbackReplaying)
        {
            g_GameErrorContext.Log(
                "info : P%d Stage 1 absence applied frame %lu active_mask 0x%02x\r\n",
                playerId + 1, (unsigned long)g_frame,
                (unsigned)GetActivePlayerMask());
            SetStatus("guest temporarily absent; continuing");
        }
        return;
    }
    if (IsResumeLifecycleControl(control))
    {
        g_absentPlayerMask &= (u8)~playerBit;
        if (!g_rollbackReplaying)
        {
            g_GameErrorContext.Log(
                "info : P%d resume applied frame %lu active_mask 0x%02x\r\n",
                playerId + 1, (unsigned long)g_frame,
                (unsigned)GetActivePlayerMask());
            SetStatus("guest resumed");
        }
        return;
    }
    if (IsLeftLifecycleControl(control))
    {
        i32 previousCount;
        i32 newCount;
        Player *player;
        if (!IsPlayerSlotActive((u8)playerId))
        {
            g_absentPlayerMask &= (u8)~playerBit;
            return;
        }
        previousCount = GetActivePlayerCount();
        player = GetPlayerById((u8)playerId);
        if (player)
        {
            memset(player->bullets, 0, sizeof(player->bullets));
            player->bombInfo.isInUse = 0;
            player->hasBorder = BORDER_NONE;
        }
        SetPlayerLives((u8)playerId, 0);
        SetPlayerBombs((u8)playerId, 0);
        SetPlayerPower((u8)playerId, 0);
        g_PlayerActive[playerId] = false;
        g_absentPlayerMask &= (u8)~playerBit;
        g_departedPlayerMask |= playerBit;
        {
            int ringSlot;
            for (ringSlot = 0; ringSlot < INPUT_RING_SIZE; ringSlot++)
            {
                g_predictedRemoteFramesByPlayer[playerId][ringSlot] =
                    INVALID_FRAME;
            }
        }
        newCount = GetActivePlayerCount();
        ApplyActivePlayerCountParameters(previousCount, newCount);
        g_connectedPlayerMask &= (u8)~playerBit;
        g_resyncAwaitingAckMask &= (u8)~playerBit;
        if (!g_rngMismatch && g_resyncAwaitingAckMask == 0)
        {
            g_resyncFrame = INVALID_FRAME;
        }
        if (g_mode == Netplay::MODE_HOST)
        {
            g_peerPresent[playerId] = false;
            g_hostPeerLifecycleStage[playerId] = HOST_PEER_LEFT;
        }
        g_connected = true;
        if (!g_rollbackReplaying)
        {
            g_GameErrorContext.Log(
                "info : P%d Stage 2 departure applied frame %lu active %d->%d mask 0x%02x boss_multiplier %.3f border_threshold %d\r\n",
                playerId + 1, (unsigned long)g_frame,
                previousCount, newCount, (unsigned)GetActivePlayerMask(),
                GetMultiplayerBossDamageMultiplier(),
                GetSharedBorderThreshold());
            SetStatus("guest left; contracted session continues");
        }
    }
}

// The bot sees far enough ahead to start moving around a pattern without
// spending a bomb.  Keep the sample interval coarse enough that this remains
// cheap on the original DX8 renderer.
const int TEST_EVASIVE_LOOKAHEAD_FRAMES = 48;
const int TEST_EVASIVE_SAMPLE_INTERVAL = 3;
const int TEST_EVASIVE_TARGET_LOOKAHEAD_FRAMES = 18;
const int TEST_EVASIVE_BOMB_TRIGGER_FRAMES = 9;
const f32 TEST_EVASIVE_BOMB_TRIGGER_CLEARANCE = 0.5f;
// Stay below the very top edge during ordinary movement. The bot can still
// reach the game's point-of-collection line, but it will not pin itself to
// the upper boundary while searching for a safe path.
const f32 TEST_EVASIVE_TOP_MARGIN = 32.0f;
const f32 TEST_EVASIVE_NO_THREAT = 1000000.0f;

void GetTestEvasiveCandidatePosition(Player *player, u16 direction,
                                     bool focus, int frames, f32 *x, f32 *y)
{
    int horizontal;
    int vertical;
    f32 speed;
    f32 horizontalSpeed;
    f32 verticalSpeed;
    f32 frameMultiplier;
    f32 left;
    f32 right;
    f32 top;
    f32 minimumY;
    f32 bottom;

    if (!player || !x || !y)
    {
        return;
    }

    horizontal = (direction & TH_BUTTON_RIGHT) != 0 ? 1 : 0;
    if ((direction & TH_BUTTON_LEFT) != 0)
    {
        horizontal--;
    }
    vertical = (direction & TH_BUTTON_DOWN) != 0 ? 1 : 0;
    if ((direction & TH_BUTTON_UP) != 0)
    {
        vertical--;
    }

    speed = focus ? player->shooterData->speedFocus
                  : player->shooterData->speed;
    if (horizontal != 0 && vertical != 0)
    {
        speed = focus ? player->shooterData->speedDiagonalFocus
                      : player->shooterData->speedDiagonal;
    }
    horizontalSpeed = (f32)horizontal * speed;
    verticalSpeed = (f32)vertical * speed;
    frameMultiplier = g_Supervisor.effectiveFramerateMultiplier;
    if (frameMultiplier <= 0.0f)
    {
        frameMultiplier = 1.0f;
    }

    *x = player->positionCenter.x +
        horizontalSpeed * (f32)frames * frameMultiplier;
    *y = player->positionCenter.y +
        verticalSpeed * (f32)frames * frameMultiplier;
    left = g_GameManager.playerMovementAreaTopLeftPos.x;
    top = g_GameManager.playerMovementAreaTopLeftPos.y;
    right = left + g_GameManager.playerMovementAreaSize.x;
    bottom = top + g_GameManager.playerMovementAreaSize.y;
    minimumY = top + TEST_EVASIVE_TOP_MARGIN;
    if (minimumY > bottom)
    {
        minimumY = top;
    }
    if (*x < left)
    {
        *x = left;
    }
    else if (*x > right)
    {
        *x = right;
    }
    if (*y < minimumY)
    {
        *y = minimumY;
    }
    else if (*y > bottom)
    {
        *y = bottom;
    }
}

void StoreRelayedLocalAuthority(const NetPacket &packet, int relayIndex)
{
    int i;
    u32 newestFrame = packet.newestFrame;

    for (i = 0; i < RELAY_INPUT_COUNT; i++)
    {
        const RelayInputRecord &record =
            packet.relayRecords[relayIndex][i];
        int slot;
        u16 authoritativeControl;
        bool changed;
        if (record.frame == INVALID_FRAME || record.frame > newestFrame ||
            newestFrame - record.frame >= RELAY_INPUT_COUNT ||
            (g_frame > record.frame &&
             g_frame - record.frame >= INPUT_RING_SIZE))
        {
            continue;
        }
        slot = (int)(record.frame % INPUT_RING_SIZE);
        if (g_localFrames[slot] != record.frame)
        {
            continue;
        }
        authoritativeControl =
            record.control & INPUT_RECORD_CONTROL_MASK;
        changed = g_localInputs[slot] != record.input ||
            g_localControls[slot] != authoritativeControl;
        g_localInputs[slot] = record.input;
        g_localControls[slot] = authoritativeControl;
        g_localRollbackGameplay[slot] =
            (record.control & INPUT_RECORD_ROLLBACK_GAMEPLAY) != 0 ? 1 : 0;
        if (changed && record.frame < g_frame && g_rollbackEnabled)
        {
            if (g_rollbackEarliestFrame == INVALID_FRAME ||
                record.frame < g_rollbackEarliestFrame)
            {
                g_rollbackEarliestFrame = record.frame;
            }
            if (g_testSeconds > 0)
            {
                g_GameErrorContext.Log(
                    "info : Host authority corrected local P%d input frame %lu\r\n",
                    g_localPlayerSlot + 1,
                    (unsigned long)record.frame);
            }
        }
    }
}

void StoreRelayedInputs(const NetPacket &packet, int relayIndex,
                        int remotePlayerId)
{
    int i;
    bool correctionDetected = false;
    u32 newestFrame = packet.newestFrame;

    if (relayIndex < 0 || relayIndex >= TH07_MULTI_MAX_GUESTS ||
        remotePlayerId < 0 || remotePlayerId >= g_playerCount ||
        newestFrame == INVALID_FRAME)
    {
        return;
    }
    if (remotePlayerId == g_localPlayerSlot)
    {
        StoreRelayedLocalAuthority(packet, relayIndex);
        return;
    }
    for (i = 0; i < RELAY_INPUT_COUNT; i++)
    {
        const RelayInputRecord &record =
            packet.relayRecords[relayIndex][i];
        u16 remoteControl = record.control & INPUT_RECORD_CONTROL_MASK;
        int slot;

        if (record.frame == INVALID_FRAME || record.frame > newestFrame ||
            newestFrame - record.frame >= RELAY_INPUT_COUNT ||
            (g_frame > record.frame &&
             g_frame - record.frame >= INPUT_RING_SIZE))
        {
            if (record.frame != INVALID_FRAME && record.frame <= newestFrame)
            {
                g_relayDroppedWindow++;
            }
            continue;
        }
        slot = (int)(record.frame % INPUT_RING_SIZE);
        if (g_frame > record.frame && g_frame - record.frame > g_relayMaxLag)
        {
            g_relayMaxLag = g_frame - record.frame;
        }
        g_relayAccepted++;
        if (g_remoteFramesByPlayer[remotePlayerId][slot] == record.frame)
        {
            // Relay history repeats each frame in up to RELAY_INPUT_COUNT
            // consecutive packets, so most arrivals are copies of a frame that
            // was already handled. Counting these as unmatched made a healthy
            // redundancy scheme look like a 92% failure rate.
            g_relayRedundant++;
        }
        else if (g_frame > record.frame &&
                 g_predictedRemoteFramesByPlayer[remotePlayerId][slot] !=
                     record.frame)
        {
            // First arrival, for a frame already simulated, with no prediction
            // recorded against it. Nothing compares the real input with what
            // was used, so no replay is scheduled. This is the case that would
            // leave a peer running on a guess it never revisits.
            g_relayNoPrediction++;
        }
        if (g_predictedRemoteFramesByPlayer[remotePlayerId][slot] ==
            record.frame)
        {
            if (g_predictedRemoteInputsByPlayer[remotePlayerId][slot] !=
                    record.input ||
                g_predictedRemoteControlsByPlayer[remotePlayerId][slot] !=
                    remoteControl)
            {
                correctionDetected = true;
                g_relayCorrections++;
                if (g_rollbackEarliestFrame == INVALID_FRAME ||
                    record.frame < g_rollbackEarliestFrame)
                {
                    g_rollbackEarliestFrame = record.frame;
                }
            }
            g_predictedRemoteFramesByPlayer[remotePlayerId][slot] =
                INVALID_FRAME;
        }
        g_remoteFramesByPlayer[remotePlayerId][slot] = record.frame;
        g_remoteInputsByPlayer[remotePlayerId][slot] = record.input;
        g_remoteRngByPlayer[remotePlayerId][slot] = 0;
        g_remoteControlsByPlayer[remotePlayerId][slot] = remoteControl;
        g_remoteStateHashByPlayer[remotePlayerId][slot] = 0;
        g_remotePlayerHashByPlayer[remotePlayerId][slot] = 0;
        g_remoteWorldHashByPlayer[remotePlayerId][slot] = 0;
        g_remoteSpellHashByPlayer[remotePlayerId][slot] = 0;
        g_remoteRollbackGameplayByPlayer[remotePlayerId][slot] =
            (record.control & INPUT_RECORD_ROLLBACK_GAMEPLAY) != 0 ? 1 : 0;
    }
    if (g_rollbackEnabled && correctionDetected &&
        g_rollbackEarliestFrame != INVALID_FRAME)
    {
        for (i = 0; i < INPUT_RING_SIZE; i++)
        {
            u32 predictedFrame =
                g_predictedRemoteFramesByPlayer[remotePlayerId][i];
            if (predictedFrame != INVALID_FRAME &&
                predictedFrame > g_rollbackEarliestFrame)
            {
                u16 refreshed = PredictRemoteInputForPlayer(
                    predictedFrame, remotePlayerId);
                if (refreshed !=
                    g_predictedRemoteInputsByPlayer[remotePlayerId][i])
                {
                    g_predictedRemoteInputsByPlayer[remotePlayerId][i] =
                        refreshed;
                    g_remoteInputsByPlayer[remotePlayerId][i] = refreshed;
                    g_rollbackPredictionRefreshFrames++;
                }
            }
        }
    }
}

f32 GetTestEvasiveBulletClearance(Player *player, Bullet *bullet,
                                  f32 playerX, f32 playerY, int frames)
{
    f32 bulletX;
    f32 bulletY;
    f32 velocityScale;
    f32 halfX;
    f32 halfY;
    f32 distanceX;
    f32 distanceY;

    if (!player || !bullet || bullet->sprites.grazeSize.x <= 0.0f ||
        bullet->sprites.grazeSize.y <= 0.0f)
    {
        return TEST_EVASIVE_NO_THREAT;
    }

    velocityScale = 1.0f;
    switch (bullet->state)
    {
    case BULLET_SPAWNING_FAST:
        velocityScale = 0.5f;
        break;
    case BULLET_SPAWNING_NORMAL:
        velocityScale = 0.4f;
        break;
    case BULLET_SPAWNING_SLOW:
        velocityScale = 1.0f / 3.0f;
        break;
    default:
        break;
    }
    bulletX = bullet->pos.x + bullet->velocity.x * velocityScale * frames;
    bulletY = bullet->pos.y + bullet->velocity.y * velocityScale * frames;
    halfX = bullet->sprites.grazeSize.x * 0.5f + player->hitboxSize.x + 4.0f;
    halfY = bullet->sprites.grazeSize.y * 0.5f + player->hitboxSize.y + 4.0f;
    distanceX = fabsf(playerX - bulletX) - halfX;
    distanceY = fabsf(playerY - bulletY) - halfY;
    if (distanceX <= 0.0f && distanceY <= 0.0f)
    {
        // A negative value lets the candidate selector prefer the path with
        // the least severe overlap if every direction is already occupied.
        return -1.0f - (distanceX < distanceY ? -distanceX : -distanceY);
    }
    if (distanceX < 0.0f)
    {
        distanceX = 0.0f;
    }
    if (distanceY < 0.0f)
    {
        distanceY = 0.0f;
    }
    return sqrtf(distanceX * distanceX + distanceY * distanceY);
}

bool IsTestEvasiveLaserHitboxActive(Laser *laser)
{
    int timer;

    if (!laser || !laser->inUse || laser->width <= 0.0f)
    {
        return false;
    }
    timer = laser->timer.GetCurrent();
    if (laser->state == LASER_SPAWNING)
    {
        return timer >= laser->hitboxStartTime;
    }
    if (laser->state == LASER_ACTIVE)
    {
        return true;
    }
    if (laser->state == LASER_DESPAWNING)
    {
        return timer < laser->hitboxEndTime;
    }
    return false;
}

f32 GetTestEvasiveLaserClearance(Player *player, Laser *laser,
                                 f32 playerX, f32 playerY, int frames)
{
    f32 sine;
    f32 cosine;
    f32 start;
    f32 end;
    f32 startLength;
    f32 frameMultiplier;
    f32 relativeX;
    f32 relativeY;
    f32 along;
    f32 perpendicular;
    f32 alongDistance;
    f32 radius;
    f32 outsidePerpendicular;

    if (!IsTestEvasiveLaserHitboxActive(laser))
    {
        return TEST_EVASIVE_NO_THREAT;
    }
    frameMultiplier = g_Supervisor.effectiveFramerateMultiplier;
    if (frameMultiplier <= 0.0f)
    {
        frameMultiplier = 1.0f;
    }
    start = laser->startOffset;
    end = laser->endOffset + laser->speed * (f32)frames * frameMultiplier;
    startLength = laser->startLength;
    if (startLength > 0.0f && end - start > startLength)
    {
        start = end - startLength;
    }
    if (start < 0.0f)
    {
        start = 0.0f;
    }
    if (end <= start)
    {
        return TEST_EVASIVE_NO_THREAT;
    }

    sine = sinf(laser->angle);
    cosine = cosf(laser->angle);
    relativeX = playerX - laser->pos.x;
    relativeY = playerY - laser->pos.y;
    along = relativeX * cosine + relativeY * sine;
    perpendicular = fabsf(relativeX * sine - relativeY * cosine);
    radius = laser->width * 0.5f +
        (player->hitboxSize.x > player->hitboxSize.y
             ? player->hitboxSize.x
             : player->hitboxSize.y) +
        5.0f;
    alongDistance = 0.0f;
    if (along < start)
    {
        alongDistance = start - along;
    }
    else if (along > end)
    {
        alongDistance = along - end;
    }
    outsidePerpendicular = perpendicular - radius;
    if (outsidePerpendicular < 0.0f)
    {
        outsidePerpendicular = 0.0f;
    }
    if (alongDistance == 0.0f && perpendicular <= radius)
    {
        return -1.0f - (radius - perpendicular);
    }
    return sqrtf(alongDistance * alongDistance +
                 outsidePerpendicular * outsidePerpendicular);
}

void EvaluateTestEvasivePath(Player *player, u16 direction, bool focus,
                             f32 *minimumClearance, int *minimumFrame)
{
    int sampleFrame;
    int bulletIndex;
    int laserIndex;
    f32 playerX;
    f32 playerY;
    f32 clearance;
    Bullet *bullet;
    Laser *laser;

    if (!minimumClearance || !minimumFrame)
    {
        return;
    }
    *minimumClearance = TEST_EVASIVE_NO_THREAT;
    *minimumFrame = TEST_EVASIVE_LOOKAHEAD_FRAMES + 1;
    for (sampleFrame = 0; sampleFrame <= TEST_EVASIVE_LOOKAHEAD_FRAMES;
         sampleFrame += TEST_EVASIVE_SAMPLE_INTERVAL)
    {
        GetTestEvasiveCandidatePosition(player, direction, focus, sampleFrame,
                                        &playerX, &playerY);
        bullet = g_BulletManager.bullets;
        for (bulletIndex = 0; bulletIndex < 1024; bulletIndex++, bullet++)
        {
            if (bullet->state == BULLET_INACTIVE ||
                bullet->state == BULLET_DESPAWN)
            {
                continue;
            }
            // Ignore bullets that cannot reach the local playfield during the
            // short prediction horizon. This keeps the bot cheap even in a
            // dense Stage 6 pattern.
            if (fabsf(bullet->pos.x - player->positionCenter.x) > 360.0f &&
                fabsf(bullet->pos.y - player->positionCenter.y) > 360.0f)
            {
                continue;
            }
            clearance = GetTestEvasiveBulletClearance(
                player, bullet, playerX, playerY, sampleFrame);
            if (clearance < *minimumClearance)
            {
                *minimumClearance = clearance;
                *minimumFrame = sampleFrame;
            }
        }

        laser = g_BulletManager.lasers;
        for (laserIndex = 0; laserIndex < 64; laserIndex++, laser++)
        {
            clearance = GetTestEvasiveLaserClearance(
                player, laser, playerX, playerY, sampleFrame);
            if (clearance < *minimumClearance)
            {
                *minimumClearance = clearance;
                *minimumFrame = sampleFrame;
            }
        }
    }
}

bool GetTestEvasiveTargetX(Player *player, f32 *targetX)
{
    Enemy *enemy;
    f32 bestScore;
    f32 playerY;
    f32 left;
    f32 right;
    int enemyIndex;
    bool found;

    if (!player || !targetX)
    {
        return false;
    }

    bestScore = -1000000000.0f;
    playerY = player->positionCenter.y;
    found = false;
    for (enemyIndex = 0; enemyIndex < 481; enemyIndex++)
    {
        enemy = &g_EnemyManager.enemies[enemyIndex];
        if (!enemy->active || enemy->life <= 0 || enemy->isProjectile ||
            enemy->hasNoCollision)
        {
            continue;
        }

        // Shots travel upward. Do not pull the bot toward an enemy that is
        // already below the player or an inactive off-screen remnant.
        if (enemy->position.y > playerY + 32.0f ||
            enemy->position.y < playerY - 720.0f)
        {
            continue;
        }

        // Bosses always win the target priority. Otherwise prefer the enemy
        // closest to the player's shot lane and vertically close enough that
        // moving underneath it will actually increase damage.
        {
            f32 verticalDistance = fabsf(playerY - enemy->position.y);
            f32 horizontalDistance =
                fabsf(player->positionCenter.x - enemy->position.x);
            f32 score = (enemy->isBoss ? 1000000.0f : 0.0f) +
                (enemy->isBoss && g_EnemyManager.spellcardInfo.isActive
                     ? 10000.0f
                     : 0.0f) -
                verticalDistance * 3.0f - horizontalDistance * 0.5f;
            if (!found || score > bestScore)
            {
                bestScore = score;
                *targetX = enemy->position.x;
                found = true;
            }
        }
    }

    if (!found)
    {
        return false;
    }
    left = g_GameManager.playerMovementAreaTopLeftPos.x;
    right = left + g_GameManager.playerMovementAreaSize.x;
    if (*targetX < left)
    {
        *targetX = left;
    }
    else if (*targetX > right)
    {
        *targetX = right;
    }
    return true;
}

bool GetTestEvasiveItemTargetX(Player *player, f32 *targetX)
{
    Item *item;
    f32 bestScore;
    f32 playerY;
    f32 pocY;
    f32 left;
    f32 right;
    int itemIndex;
    bool found;

    if (!player || !player->shooterData || !targetX)
    {
        return false;
    }

    bestScore = -1000000000.0f;
    playerY = player->positionCenter.y;
    pocY = player->shooterData->pocY;
    found = false;
    for (itemIndex = 0; itemIndex < 1101; itemIndex++)
    {
        f32 verticalDistance;
        f32 horizontalDistance;
        f32 priority;
        f32 score;

        item = &g_ItemManager.items[itemIndex];
        if (!item->isInUse || item->itemType == ITEM_NO_ITEM ||
            item->currentPosition.y <= pocY + 8.0f)
        {
            continue;
        }

        verticalDistance = fabsf(item->currentPosition.y - pocY);
        horizontalDistance =
            fabsf(player->positionCenter.x - item->currentPosition.x);
        priority = 0.0f;
        switch (item->itemType)
        {
        case ITEM_LIFE:
            priority = 1000.0f;
            break;
        case ITEM_BOMB:
            priority = 900.0f;
            break;
        case ITEM_FULL_POWER:
            priority = 800.0f;
            break;
        case ITEM_POWER_BIG:
            priority = 700.0f;
            break;
        default:
            priority = 100.0f;
            break;
        }
        score = priority - verticalDistance * 1.5f -
            horizontalDistance * 0.4f;
        if (!found || score > bestScore)
        {
            bestScore = score;
            *targetX = item->currentPosition.x;
            found = true;
        }
    }

    if (!found)
    {
        return false;
    }
    left = g_GameManager.playerMovementAreaTopLeftPos.x;
    right = left + g_GameManager.playerMovementAreaSize.x;
    if (*targetX < left)
    {
        *targetX = left;
    }
    else if (*targetX > right)
    {
        *targetX = right;
    }
    return true;
}

f32 GetTestEvasiveTargetError(Player *player, u16 direction, bool focus,
                              f32 targetX, bool collectItems)
{
    f32 candidateX;
    f32 candidateY;
    f32 targetY;
    f32 error;

    GetTestEvasiveCandidatePosition(player, direction, focus,
                                    TEST_EVASIVE_TARGET_LOOKAHEAD_FRAMES,
                                    &candidateX, &candidateY);
    error = fabsf(candidateX - targetX);
    if (collectItems && player->shooterData)
    {
        targetY = player->shooterData->pocY - 8.0f;
        error += fabsf(candidateY - targetY) * 0.75f;
    }
    return error;
}

u16 ChooseTestEvasiveInput(Player *player, u32 frame, u32 lane,
                           bool *useFocus, bool *useBomb)
{
    static const u16 directions[9] = {
        0,
        TH_BUTTON_UP,
        TH_BUTTON_DOWN,
        TH_BUTTON_LEFT,
        TH_BUTTON_RIGHT,
        TH_BUTTON_UP | TH_BUTTON_LEFT,
        TH_BUTTON_UP | TH_BUTTON_RIGHT,
        TH_BUTTON_DOWN | TH_BUTTON_LEFT,
        TH_BUTTON_DOWN | TH_BUTTON_RIGHT};
    f32 idleClearance;
    int idleFrame;
    bool focus;
    f32 bestClearance;
    int bestIndex;
    int bestFrame;
    int preferredIndex;
    int index;
    f32 candidateClearance;
    int candidateFrame;
    f32 candidateClearances[9];
    int candidateFrames[9];
    f32 targetX;
    f32 targetError;
    f32 bestTargetError;
    f32 targetSafetyLoss;
    bool hasTarget;
    bool collectItems;
    int targetIndex;
    u32 preferredValue;

    if (useFocus)
    {
        *useFocus = false;
    }
    if (useBomb)
    {
        *useBomb = false;
    }
    if (!player || !player->shooterData)
    {
        return 0;
    }

    EvaluateTestEvasivePath(player, 0, false, &idleClearance, &idleFrame);
    // Fast movement is useful when the playfield is clear; switch to focused
    // movement earlier near a predicted bullet so the bot can make smaller
    // corrections instead of waiting until the hitbox is almost occupied.
    focus = idleClearance < 96.0f ||
        (idleFrame <= 12 && idleClearance < 128.0f);
    bestClearance = -1000000.0f;
    bestIndex = 0;
    bestFrame = TEST_EVASIVE_LOOKAHEAD_FRAMES + 1;
    preferredValue = GetTestRandomInputValue(frame / 45, lane,
                                              0x6a09e667);
    preferredIndex = (int)(preferredValue % 9);
    for (index = 0; index < 9; index++)
    {
        EvaluateTestEvasivePath(player, directions[index], focus,
                                &candidateClearance, &candidateFrame);
        candidateClearances[index] = candidateClearance;
        candidateFrames[index] = candidateFrame;
        if (candidateClearance > bestClearance + 1.0f ||
            (fabsf(candidateClearance - bestClearance) <= 1.0f &&
             index == preferredIndex))
        {
            bestClearance = candidateClearance;
            bestIndex = index;
            bestFrame = candidateFrame;
        }
    }

    collectItems = GetTestEvasiveItemTargetX(player, &targetX);
    if (!collectItems)
    {
        hasTarget = GetTestEvasiveTargetX(player, &targetX);
    }
    else
    {
        // Item recovery takes priority over lining up shots. Once the item
        // stream reaches the point-of-collection line, enemy/boss alignment
        // resumes on the following frame.
        hasTarget = true;
    }
    if (hasTarget)
    {
        // In a clear pattern, allow the bot to choose the path that lines up
        // with the next enemy or boss. Once bullets are close, only paths
        // nearly as safe as the best path are eligible for target alignment.
        if (bestClearance > 128.0f)
        {
            targetSafetyLoss = 96.0f;
        }
        else if (bestClearance > 32.0f)
        {
            targetSafetyLoss = 24.0f;
        }
        else
        {
            targetSafetyLoss = 4.0f;
        }
        bestTargetError = TEST_EVASIVE_NO_THREAT;
        targetIndex = -1;
        for (index = 0; index < 9; index++)
        {
            if (candidateClearances[index] + targetSafetyLoss <
                bestClearance)
            {
                continue;
            }
            targetError = GetTestEvasiveTargetError(
                player, directions[index], focus, targetX, collectItems);
            if (targetIndex < 0 || targetError < bestTargetError - 1.0f ||
                (fabsf(targetError - bestTargetError) <= 1.0f &&
                 index == preferredIndex))
            {
                bestTargetError = targetError;
                targetIndex = index;
            }
        }
        if (targetIndex >= 0)
        {
            bestIndex = targetIndex;
            bestClearance = candidateClearances[bestIndex];
            bestFrame = candidateFrames[bestIndex];
        }
    }

    if (useFocus)
    {
        *useFocus = focus;
    }
    if (useBomb && !player->bombInfo.isInUse &&
        player->playerState == PLAYER_STATE_ALIVE &&
        player->respawnTimer != 0 && player->hasBorder == BORDER_NONE &&
        GetPlayerBombs((u8)lane) > 0 &&
        bestFrame <= TEST_EVASIVE_BOMB_TRIGGER_FRAMES &&
        bestClearance <= TEST_EVASIVE_BOMB_TRIGGER_CLEARANCE &&
        (frame + lane * 3) % 10 == 0)
    {
        *useBomb = true;
    }
    return directions[bestIndex];
}

void ApplyTestEvasiveInputForLane(u16 *input, u32 frame, u32 lane)
{
    Player *player;
    u16 direction;
    bool focus;
    bool bomb;
    f32 minimumY;

    if (!input)
    {
        return;
    }
    player = GetPlayerById((u8)lane);
    if (!player)
    {
        return;
    }
    direction = ChooseTestEvasiveInput(player, frame, lane, &focus, &bomb);

    // Do not let a safe-path tie keep pressing Up after the bot has reached
    // the normal upper movement band. Item collection uses the lower
    // point-of-collection line, so it does not need to sit against the top
    // boundary.
    minimumY = g_GameManager.playerMovementAreaTopLeftPos.y +
        TEST_EVASIVE_TOP_MARGIN;
    if (player->positionCenter.y <= minimumY &&
        (direction & TH_BUTTON_UP) != 0)
    {
        direction &= (u16)~TH_BUTTON_UP;
    }

    // The evasive driver owns the gameplay lane, but keeps Ctrl/Skip so an
    // unattended test can still dismiss dialogue. It never writes menu or
    // pause bits, which avoids background-window input leaking into a match.
    *input &= TH_BUTTON_SKIP;
    // Keep the skip edge available to the synchronized game lane. This is
    // harmless during gameplay and makes dialogue/message pauses advance even
    // when the bot is running without a foreground window.
    *input |= TH_BUTTON_SKIP;
    *input |= direction | TH_BUTTON_SHOOT;
    if (focus)
    {
        *input |= TH_BUTTON_FOCUS;
    }
    if (bomb)
    {
        *input |= TH_BUTTON_BOMB;
        if (g_testEvasiveInputBombPulseCount < 16)
        {
            g_GameErrorContext.Log(
                "info : evasive input danger bomb pulse at frame %lu lane %lu\r\n",
                (unsigned long)frame, (unsigned long)lane);
        }
        g_testEvasiveInputBombPulseCount++;
    }
}

// Which loadout the automated run picks for each slot. Three different
// characters so per-player ANM, shot and cut-in resources are all exercised
// rather than three copies of P1's.
i32 GetTestMenuTargetCharacter(int playerId)
{
    return playerId % 3;
}

i32 GetTestMenuTargetShot(int playerId)
{
    return playerId % 2;
}

void ApplyTestMenuInput(u16 *localPlayer1)
{
    MainMenu *menu;
    i32 selectingPlayer;
    i32 target;
    bool confirm = false;
    // Character select moves horizontally (MoveCursorHorizontal) and shot
    // select vertically (MoveCursorVertical). Sending the wrong axis leaves
    // the cursor parked and the sequence never advances.
    bool moveRight = false;
    bool moveDown = false;

    if (!g_testMenuInputEnabled || !localPlayer1 || g_GameManager.replay ||
        g_GameManager.notInMenu ||
        g_Supervisor.curState == TH07_SUPERVISOR_GAME_STATE)
    {
        return;
    }
    menu = FindActiveMainMenu();
    if (!menu)
    {
        return;
    }

    g_testMenuInputFrame++;
    // Menu handlers act on a press edge and animate between steps, so emit one
    // press per interval and leave the lane released in between. Holding the
    // button would be swallowed as a repeat and the sequence would stall.
    if (g_testMenuInputFrame % (u32)TEST_MENU_INPUT_INTERVAL != 0)
    {
        return;
    }

    // g_selectingLoadoutPlayerId is -1 while P1 chooses and 1 or 2 once the
    // shared sequence has moved on to a guest.
    selectingPlayer = g_selectingLoadoutPlayerId >= 1
        ? g_selectingLoadoutPlayerId : 0;

    switch (menu->gameState)
    {
    case STATE_PRE_INPUT:
    case STATE_NORMAL_SELECT_DIFFICULTY:
    case STATE_EXTRA_SELECT_DIFFICULTY:
        confirm = true;
        break;
    case STATE_NORMAL_SELECT_CHARACTER:
    case STATE_EXTRA_SELECT_CHARACTER:
        target = GetTestMenuTargetCharacter((int)selectingPlayer);
        if (menu->cursor != target)
        {
            moveRight = true;
        }
        else
        {
            confirm = true;
        }
        break;
    case STATE_NORMAL_SELECT_SHOTTYPE:
    case STATE_EXTRA_SELECT_SHOTTYPE:
        target = GetTestMenuTargetShot((int)selectingPlayer);
        if (menu->cursor != target)
        {
            moveDown = true;
        }
        else
        {
            confirm = true;
        }
        break;
    default:
        break;
    }

    if (menu->gameState != g_testMenuLastState)
    {
        g_testMenuLastState = menu->gameState;
        g_GameErrorContext.Log(
            "info : test menu input state %d sub %d selecting P%d cursor %d pending %d additional %d players %d\r\n",
            (int)menu->gameState, (int)menu->menuSubState,
            (int)selectingPlayer + 1, (int)menu->cursor,
            Netplay::GetNextPendingLoadoutPlayerId(),
            Netplay::ShouldSelectAdditionalPlayerLoadout() ? 1 : 0,
            g_playerCount);
    }

    if (moveRight)
    {
        *localPlayer1 |= TH_BUTTON_RIGHT;
    }
    else if (moveDown)
    {
        *localPlayer1 |= TH_BUTTON_DOWN;
    }
    else if (confirm)
    {
        g_testMenuConfirmCount++;
        *localPlayer1 |= TH_BUTTON_SELECTMENU;
    }
}

void ApplyTestEvasiveInput(u16 *localPlayer1, u16 *localPlayer2,
                           u16 *localPlayer3)
{
    u32 frame;
    u32 localP1Lane;

    if (!g_testEvasiveInputEnabled || !localPlayer1 ||
        g_GameManager.replay || !g_GameManager.notInMenu ||
        g_quickStartPending)
    {
        return;
    }
    frame = g_mode == Netplay::MODE_LOCAL ? g_testInputSyncLocalFrame
                                           : g_frame;
    localP1Lane = g_mode == Netplay::MODE_GUEST
        ? (u32)g_localPlayerSlot : 0;
    ApplyTestEvasiveInputForLane(localPlayer1, frame, localP1Lane);
    if (g_mode == Netplay::MODE_LOCAL)
    {
        ApplyTestEvasiveInputForLane(localPlayer2, frame, 1);
        if (Netplay::GetPlayerCount() >= 3)
        {
            ApplyTestEvasiveInputForLane(localPlayer3, frame, 2);
        }
    }
    if (!g_testEvasiveInputLogged)
    {
        g_testEvasiveInputLogged = true;
        g_GameErrorContext.Log(
            "info : test evasive input enabled (bullet avoidance/shot/slow/fast/bomb)\r\n");
    }
}

void ApplyTestRandomInputForLane(u16 *input, u32 frame, u32 lane)
{
    static const u16 directions[9] = {
        0,
        TH_BUTTON_UP,
        TH_BUTTON_DOWN,
        TH_BUTTON_LEFT,
        TH_BUTTON_RIGHT,
        TH_BUTTON_UP | TH_BUTTON_LEFT,
        TH_BUTTON_UP | TH_BUTTON_RIGHT,
        TH_BUTTON_DOWN | TH_BUTTON_LEFT,
        TH_BUTTON_DOWN | TH_BUTTON_RIGHT};
    u32 value;
    u32 movementValue;
    u16 direction;
    bool slow;
    bool shoot;
    bool bomb;

    if (!input)
    {
        return;
    }
    // Hold each random direction for several frames so the test exercises
    // real movement instead of producing a noisy one-frame key stream.
    value = GetTestRandomInputValue(frame / 15, lane, 0x13579bdf);
    movementValue = value >> 4;
    direction = directions[movementValue % 9];
    slow = ((value >> 12) & 3) != 0;
    shoot = ((value >> 18) & 7) != 0;
    bomb = (frame + lane * 37) % 180 == 45;

    // This mode owns the complete gameplay lane. Preserve only dialogue skip
    // from the test preset so a focused keyboard or stale DirectInput state
    // cannot inject pause/menu/debug edges into an unattended run.
    *input &= TH_BUTTON_SKIP;
    *input |= direction;
    if (slow)
    {
        *input |= TH_BUTTON_FOCUS;
    }
    if (shoot)
    {
        *input |= TH_BUTTON_SHOOT;
    }
    if (bomb)
    {
        *input |= TH_BUTTON_BOMB;
    }
}

void ApplyTestRandomInput(u16 *localPlayer1, u16 *localPlayer2,
                          u16 *localPlayer3)
{
    u32 frame;
    u32 localP1Lane;

    if (!g_testRandomInputEnabled || !localPlayer1 ||
        g_GameManager.replay || !g_GameManager.notInMenu ||
        g_quickStartPending)
    {
        return;
    }
    frame = g_mode == Netplay::MODE_LOCAL ? g_testInputSyncLocalFrame
                                           : g_frame;
    // The Guest's local lane becomes its negotiated P2/P3 slot after
    // synchronization. Give each slot its own deterministic input stream.
    localP1Lane = g_mode == Netplay::MODE_GUEST
        ? (u32)g_localPlayerSlot : 0;
    ApplyTestRandomInputForLane(localPlayer1, frame, localP1Lane);
    if (g_mode == Netplay::MODE_LOCAL)
    {
        ApplyTestRandomInputForLane(localPlayer2, frame, 1);
        if (Netplay::GetPlayerCount() >= 3)
        {
            ApplyTestRandomInputForLane(localPlayer3, frame, 2);
        }
    }
    if (!g_testRandomInputLogged)
    {
        g_testRandomInputLogged = true;
        g_GameErrorContext.Log(
            "info : test random input enabled (shot/slow/fast/bomb)\r\n");
    }
    if ((frame + localP1Lane * 37) % 180 == 45)
    {
        if (g_testRandomInputBombPulseCount < 16)
        {
            g_GameErrorContext.Log(
                "info : random input bomb pulse at frame %lu lane %lu\r\n",
                (unsigned long)frame, (unsigned long)localP1Lane);
        }
        g_testRandomInputBombPulseCount++;
    }
}

void ApplyDamageEventTestInput(u16 *localPlayer1, u16 *localPlayer2)
{
    u16 *giverInput = NULL;

    if (!g_testDamageEventsEnabled ||
        !Netplay::ShouldForceDamageTestTransfer(1))
    {
        return;
    }
    if (g_mode == Netplay::MODE_GUEST && g_localPlayerSlot == 1)
    {
        // This transfer test deliberately drives P2 only. A P3 Guest must
        // not inject that P2-only input into its own lane.
        giverInput = localPlayer1;
    }
    else if (g_mode == Netplay::MODE_LOCAL)
    {
        giverInput = localPlayer2;
    }
    if (!giverInput)
    {
        return;
    }
    // Exercise the real transfer rule: P2 must overlap P1, hold focus, stop
    // shooting, and charge for 90 consecutive frames. Keep a random/rollback
    // bomb edge from interrupting this deliberately narrow verification window.
    *giverInput &= (u16)~(TH_BUTTON_DIRECTION | TH_BUTTON_SHOOT |
                           TH_BUTTON_BOMB);
    *giverInput |= TH_BUTTON_FOCUS;
}

bool TryClaimFinalResourceBonus(u8 playerId, u8 activeMask,
                                u8 *appliedMask)
{
    u8 playerBit;
    if (!appliedMask || playerId == 0 ||
        playerId >= TH07_MULTI_MAX_PLAYERS)
    {
        return false;
    }
    playerBit = (u8)(1 << playerId);
    if ((activeMask & playerBit) == 0 ||
        (*appliedMask & playerBit) != 0)
    {
        return false;
    }
    *appliedMask |= playerBit;
    return true;
}

bool VerifyThreePlayerFinalResourceBonusClaims()
{
    u8 appliedMask = 0;
    u8 activeMask = 0x07;
    i32 claimCount[TH07_MULTI_MAX_PLAYERS] = {0, 0, 0};
    i32 pass;
    i32 playerId;

    // Run the same claim helper twice. P2 and P3 must each be admitted once,
    // and the second result-screen update must add neither slot again.
    for (pass = 0; pass < 2; pass++)
    {
        for (playerId = 1; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
        {
            if (TryClaimFinalResourceBonus(
                    (u8)playerId, activeMask, &appliedMask))
            {
                claimCount[playerId]++;
            }
        }
    }
    if (claimCount[1] == 1 && claimCount[2] == 1 &&
        appliedMask == 0x06)
    {
        g_GameErrorContext.Log(
            "info : three-player final resource bonus claims verified P2 1 P3 1 mask 0x%02x\r\n",
            (unsigned)appliedMask);
        return true;
    }
    g_GameErrorContext.Log(
        "error : three-player final resource bonus claims failed P2 %d P3 %d mask 0x%02x\r\n",
        claimCount[1], claimCount[2], (unsigned)appliedMask);
    return false;
}

void ApplyFinalMultiplayerScoreBonus()
{
    i32 playerBonus;
    i32 playerId;
    u8 activeMask;

    if (!g_GameManager.finished)
    {
        g_finalResourceBonusAppliedMask = 0;
        return;
    }
    if (!Netplay::IsMultiplayer() || g_GameManager.replay ||
        g_GameManager.globals == NULL)
    {
        return;
    }

    activeMask = GetActivePlayerMask();
    // Gui::UpdateGui already adds P1. Add each active sidecar slot once.
    for (playerId = 1; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        if (!TryClaimFinalResourceBonus(
                (u8)playerId, activeMask,
                &g_finalResourceBonusAppliedMask))
        {
            continue;
        }
        playerBonus = GetPlayerLives((u8)playerId) * 2000000 +
            GetPlayerBombs((u8)playerId) * 400000;
        switch (g_GameManager.difficulty)
        {
        case DIFF_EASY:
            playerBonus /= 2;
            break;
        case DIFF_HARD:
            playerBonus = playerBonus * 12 / 10;
            break;
        case DIFF_LUNATIC:
            playerBonus = playerBonus * 15 / 10;
            break;
        case DIFF_EXTRA:
        case DIFF_PHANTASM:
            playerBonus <<= 1;
            break;
        default:
            break;
        }
        if (g_GameManager.defaultCfg)
        {
            switch (g_GameManager.defaultCfg->lifeCount)
            {
            case 3:
                playerBonus = playerBonus * 5 / 10;
                break;
            case 4:
                playerBonus = (playerBonus << 1) / 10;
                break;
            default:
                break;
            }
        }
        g_GameManager.AddScore(playerBonus * 10);
    }
    g_GameManager.globals->guiScore = g_GameManager.globals->score;
    SetStatus("result: multiplayer resource bonuses added");
}

void ApplyRngResync()
{
    u32 appliedFrame = g_resyncFrame;

    // TH06's resync is a soft synchronization boundary rather than a full
    // pointer-safe memory snapshot. Keep the TH07 stage objects alive, reset
    // the deterministic generator on both peers, and ignore the delayed
    // pre-boundary samples until the new seed has reached the input window.
    g_Rng.SetSeed(0);
    g_Rng.seedBackup = 0;
    g_resyncIgnoreUntilFrame =
        g_frame + (u32)g_delay + 1;
    g_rngMismatch = false;
    g_lastResyncSendTick = 0;
    if (g_mode == Netplay::MODE_HOST)
    {
        // Keep broadcasting the boundary until the Guest confirms that it
        // also applied it. The existing remote-input timeout still handles a
        // peer that has disappeared completely.
        g_resyncAwaitingAckMask =
            GetActivePlayerMask() & (u8)~1;
        if (g_testSeconds > 0)
        {
            g_GameErrorContext.Log(
                "info : RNG resync applied frame %lu awaiting_mask 0x%02x\r\n",
                (unsigned long)appliedFrame,
                (unsigned)g_resyncAwaitingAckMask);
        }
        if (g_resyncAwaitingAckMask == 0)
        {
            g_resyncFrame = INVALID_FRAME;
        }
    }
    else
    {
        SendControl(CONTROL_RESYNC_ACK, appliedFrame, 1);
        if (g_testSeconds > 0)
        {
            g_GameErrorContext.Log(
                "info : RNG resync applied frame %lu ACK sent by P%d\r\n",
                (unsigned long)appliedFrame, g_localPlayerSlot + 1);
        }
        g_resyncFrame = INVALID_FRAME;
        g_resyncAwaitingAckMask = 0;
    }

    char status[160];
    sprintf(status, "RNG resynced at frame %lu",
            (unsigned long)appliedFrame);
    SetStatus(status);
}

void SendPendingResync()
{
    DWORD now;

    if ((!g_rngMismatch && g_resyncAwaitingAckMask == 0) ||
        g_resyncFrame == INVALID_FRAME || g_mode != Netplay::MODE_HOST)
    {
        return;
    }
    now = GetTickCount();
    if (now - g_lastResyncSendTick >= 100)
    {
        SendControl(CONTROL_RESYNC_REQUEST, g_resyncFrame,
                    g_initialRngSeed);
        g_lastResyncSendTick = now;
    }
}

bool AreRequiredRemoteInputsReady(u32 frame, bool allowPrediction)
{
    int playerId;
    int slot = (int)(frame % INPUT_RING_SIZE);
    for (playerId = 0; playerId < g_playerCount; playerId++)
    {
        if (!IsExpectedRemotePlayerId(playerId))
        {
            continue;
        }
        if (g_remoteFramesByPlayer[playerId][slot] != frame ||
            (!allowPrediction &&
             g_predictedRemoteFramesByPlayer[playerId][slot] == frame))
        {
            return false;
        }
    }
    return true;
}

u8 GetExactInputReadyMask(u32 frame)
{
    int playerId;
    int slot = (int)(frame % INPUT_RING_SIZE);
    u8 readyMask = (u8)(1 << g_localPlayerSlot);
    for (playerId = 0; playerId < g_playerCount; playerId++)
    {
        if (!IsExpectedRemotePlayerId(playerId))
        {
            continue;
        }
        if (g_remoteFramesByPlayer[playerId][slot] == frame &&
            g_predictedRemoteFramesByPlayer[playerId][slot] != frame)
        {
            readyMask |= (u8)(1 << playerId);
        }
    }
    return readyMask;
}

bool TryGetRemoteInput(u32 frame, bool allowPrediction)
{
    DWORD now;
    DWORD pollDeadline;
    DWORD inputTimeoutMs;
    bool remoteReady;
    bool startupBarrierWait;
    int playerId;
    int slot = (int)(frame % INPUT_RING_SIZE);

    if (g_connectionFailed)
    {
        return false;
    }
    PollPackets();
    UpdateHostPeerLifecycles();
    PrepareHostSyntheticInputs(frame);
    if (g_connectionFailed)
    {
        return false;
    }
    if (g_rollbackEnabled && g_rollbackEarliestFrame != INVALID_FRAME &&
        !PerformPendingRollback())
    {
        return false;
    }
    SendPendingResync();
    now = GetTickCount();
    startupBarrierWait = frame == 0 && !g_startupFrameBarrierLogged;
    inputTimeoutMs = startupBarrierWait ? STARTUP_INPUT_TIMEOUT_MS
                                        : REMOTE_INPUT_TIMEOUT_MS;
    if (g_connected && now - g_lastPingTick >= PING_INTERVAL_MS)
    {
        SendPing();
        g_lastPingTick = now;
    }
    remoteReady = AreRequiredRemoteInputsReady(frame, allowPrediction);

    // Prediction is useful only when it happens before waiting. The previous
    // ordering spent up to 16 ms in the poll loop first, so "delay 0"
    // rollback still felt almost exactly one frame late on a healthy route.
    if (!remoteReady && allowPrediction && g_rollbackEnabled &&
        IsRollbackPredictionFrame())
    {
        u32 oldestPredictedFrame = FindOldestPredictedFrame();
        if (oldestPredictedFrame == INVALID_FRAME ||
            frame < oldestPredictedFrame ||
            frame - oldestPredictedFrame <
                (u32)ROLLBACK_MAX_PREDICTION_FRAMES)
        {
            for (playerId = 0; playerId < g_playerCount; playerId++)
            {
                if (!IsExpectedRemotePlayerId(playerId) ||
                    g_remoteFramesByPlayer[playerId][slot] == frame)
                {
                    continue;
                }
                g_remoteInputsByPlayer[playerId][slot] =
                    PredictRemoteInputForPlayer(frame, playerId);
                g_remoteControlsByPlayer[playerId][slot] =
                    playerId == 0
                        ? (u16)GetScheduledLifecycleControl(frame)
                        : (u16)Netplay::INGAME_CONTROL_NONE;
                g_remoteStateHashByPlayer[playerId][slot] = 0;
                g_remotePlayerHashByPlayer[playerId][slot] = 0;
                g_remoteWorldHashByPlayer[playerId][slot] = 0;
                g_remoteSpellHashByPlayer[playerId][slot] = 0;
                g_predictedRemoteInputsByPlayer[playerId][slot] =
                    g_remoteInputsByPlayer[playerId][slot];
                g_predictedRemoteControlsByPlayer[playerId][slot] =
                    g_remoteControlsByPlayer[playerId][slot];
                g_predictedRemoteFramesByPlayer[playerId][slot] = frame;
                g_rollbackPredictedFrames++;
            }
            g_waitingForRemoteInput = false;
            g_waitStartedTick = 0;
            g_waitStatusTick = 0;
            return true;
        }
    }

    if (!remoteReady && g_connected)
    {
        pollDeadline = now + REMOTE_INPUT_POLL_BUDGET_MS;
        while (GetTickCount() < pollDeadline)
        {
            Sleep(1);
            PollPackets();
            if (g_connectionFailed)
            {
                break;
            }
            if (AreRequiredRemoteInputsReady(frame, allowPrediction))
            {
                remoteReady = true;
                break;
            }
        }
    }
    if (remoteReady)
    {
        now = GetTickCount();
        if (g_waitingForRemoteInput)
        {
            u32 stallMs = now - g_waitStartedTick;
            g_inputStallCount++;
            g_inputStallTotalMs += stallMs;
            if (stallMs > g_inputStallMaxMs)
            {
                g_inputStallMaxMs = stallMs;
            }
            // Count only waits after the synchronized prediction boundary.
            // The one exact-input frame used to align two differently timed
            // stage loads is startup coordination, not gameplay input latency.
            if (IsRollbackPredictionFrame())
            {
                g_gameplayInputStallCount++;
                g_gameplayInputStallTotalMs += stallMs;
                if (stallMs > g_gameplayInputStallMaxMs)
                {
                    g_gameplayInputStallMaxMs = stallMs;
                }
            }
            if (g_testSeconds > 0 && g_inputStallCount <= 32)
            {
                g_GameErrorContext.Log(
                    "info : input wait %lu ms at frame %lu (delay %d)\r\n",
                    (unsigned long)stallMs, (unsigned long)g_frame,
                    g_delay);
            }
        }
        g_waitingForRemoteInput = false;
        g_waitStartedTick = 0;
        g_waitStatusTick = 0;
        return true;
    }

    if (!g_waitingForRemoteInput)
    {
        g_waitingForRemoteInput = true;
        g_waitStartedTick = now;
        g_waitStatusTick = now;
        if (startupBarrierWait)
        {
            g_GameErrorContext.Log(
                "info : startup input barrier waiting up to %lu ms local P%d exact_mask 0x%02x required_mask 0x%02x\r\n",
                (unsigned long)inputTimeoutMs, g_localPlayerSlot + 1,
                (unsigned)GetExactInputReadyMask(frame),
                (unsigned)((1 << g_playerCount) - 1));
        }
    }
    if (now - g_lastInputSendTick >= 50)
    {
        SendInputs(g_frame);
        g_lastInputSendTick = now;
    }
    if (now - g_waitStatusTick >= 5000)
    {
        g_waitStatusTick = now;
        if (startupBarrierWait)
        {
            SetStatus("waiting for all players to finish loading");
            g_GameErrorContext.Log(
                "info : startup input barrier still waiting after %lu ms exact_mask 0x%02x required_mask 0x%02x\r\n",
                (unsigned long)(now - g_waitStartedTick),
                (unsigned)GetExactInputReadyMask(frame),
                (unsigned)((1 << g_playerCount) - 1));
        }
        else
        {
            SetStatus("waiting for remote input; retrying");
        }
    }
    if (now - g_waitStartedTick >= inputTimeoutMs)
    {
        if (startupBarrierWait)
        {
            SetStatus("startup synchronization timed out");
            g_GameErrorContext.Log(
                "info : startup input timed out after %lu ms exact_mask 0x%02x required_mask 0x%02x\r\n",
                (unsigned long)inputTimeoutMs,
                (unsigned)GetExactInputReadyMask(frame),
                (unsigned)((1 << g_playerCount) - 1));
        }
        if (g_mode == Netplay::MODE_GUEST && g_playerCount >= 3)
        {
            if (!startupBarrierWait)
            {
                SetStatus("Host connection lost; session ending");
                g_GameErrorContext.Log(
                    "info : Host input timed out after %lu ms; ending three-player session\r\n",
                    (unsigned long)inputTimeoutMs);
            }
            HandlePeerExit();
            return false;
        }
        g_connectionFailed = true;
        g_connected = false;
        g_waitingForRemoteInput = false;
        if (!startupBarrierWait)
        {
            SetStatus("connection lost; press F8 on both peers to reconnect");
            g_GameErrorContext.Log(
                "info : connection lost after %lu ms; press F8 on both peers to reconnect\r\n",
                (unsigned long)inputTimeoutMs);
        }
    }
    return false;
}

bool IsOptionBoundary(char value)
{
    return value == '\0' || value == ' ' || value == '\t';
}

const char *FindOption(const char *commandLine, const char *option)
{
    const char *found = commandLine;
    int optionLength = strlen(option);
    while ((found = strstr(found, option)) != NULL)
    {
        if ((found == commandLine || IsOptionBoundary(found[-1])) &&
            IsOptionBoundary(found[optionLength]))
        {
            return found;
        }
        found += optionLength;
    }
    return NULL;
}

bool HasOption(const char *commandLine, const char *option)
{
    return FindOption(commandLine, option) != NULL;
}

bool IsBlankCommandLine(const char *commandLine)
{
    while (*commandLine == ' ' || *commandLine == '\t')
    {
        commandLine++;
    }
    return *commandLine == '\0';
}

bool ParseUnsignedAfter(const char *commandLine, const char *option,
                        unsigned long *value, bool *valid)
{
    const char *found = FindOption(commandLine, option);
    char *end;
    if (!found)
    {
        return false;
    }
    found += strlen(option);
    while (*found == ' ' || *found == '\t')
    {
        found++;
    }
    *value = strtoul(found, &end, 0);
    *valid = end != found && IsOptionBoundary(*end);
    return true;
}

bool ParseWordAfter(const char *commandLine, const char *option,
                    char *value, int valueSize)
{
    const char *found = FindOption(commandLine, option);
    int length = 0;
    if (!found)
    {
        return false;
    }
    found += strlen(option);
    while (*found == ' ' || *found == '\t')
    {
        found++;
    }
    while (found[length] && !IsOptionBoundary(found[length]) &&
           length < valueSize - 1)
    {
        value[length] = found[length];
        length++;
    }
    value[length] = '\0';
    return length != 0;
}

bool ParseJoinHost(const char *commandLine, char *host, int hostSize)
{
    const char *found = FindOption(commandLine, "--join");
    int length = 0;
    if (!found)
    {
        return false;
    }
    found += strlen("--join");
    while (*found == ' ' || *found == '\t')
    {
        found++;
    }
    while (found[length] && found[length] != ' ' && found[length] != '\t' &&
           length < hostSize - 1)
    {
        host[length] = found[length];
        length++;
    }
    host[length] = '\0';
    return length != 0;
}

void ShowHelpAndExit()
{
    const char help[] =
        "th07_multi_net\r\n\r\n"
        "  (empty) / --connect-ui   open Host/Guest/Local launcher\r\n"
        "  --local                  local two/three-player\r\n"
        "  --host --port N          wait for P2\r\n"
        "  --join ADDRESS --port N  connect as P2\r\n"
        "  --single                 original single-player mode\r\n"
        "\r\n"
        "Useful options: --delay N, --rollback, --windowed, --no-save, --test,\r\n"
        "--fullscreen, --window-size 640x480|960x720|1280x960,\r\n"
        "--low-latency, --no-low-latency, --blt-prepare 0..16,\r\n"
        "--low-latency-spin,\r\n"
        "--test-full-run, --invincible, --no-invincible, --auto-shoot, --auto-skip,\r\n"
        "--auto-bomb, --no-controller,\r\n"
        "--test-random-input, --test-evasive-input, --test-damage-events,\r\n"
        "--test-resource-drops,\r\n"
        "--test-p2-features, --test-p3-features, --test-hash-trace,\r\n"
        "--test-boss-desync, --test-keep-stale-stage-snapshots,\r\n"
        "--three-player, --players 3,\r\n"
        "--difficulty, --character, --shot, --stage, --practice, --normal-stage,\r\n"
        "--p2-character, --p2-shot, --p3-character, --p3-shot,\r\n"
        "--seed, --test-seconds, --connect-timeout, --test-proximity,\r\n"
        "--test-life-transfer, --test-rollback-input.\r\n\r\n"
        "Do not launch the original th07.exe for multiplayer.";
    MessageBoxA(NULL, help, "th07_multi_net help", MB_OK | MB_ICONINFORMATION);
    ExitProcess(0);
}
void LogEndOfRunSummary()
{
    // Deliberately not gated on --test-seconds. Both callers are reachable
    // only from a netplay session, and gating them on test mode meant a real
    // session recorded none of this - which is exactly the case the boss
    // measurement was built to check.
    LowLatency::LogSummary();
    if (!g_inputTimingSummaryLogged)
    {
        u32 averageStall = g_inputStallCount != 0
            ? g_inputStallTotalMs / g_inputStallCount
            : 0;
        u32 averageGameplayStall = g_gameplayInputStallCount != 0
            ? g_gameplayInputStallTotalMs / g_gameplayInputStallCount
            : 0;
        char checkpoints[256];
        char *cursor = checkpoints;
        u32 emitted = g_bossTraceSamples < (u32)BOSS_CHECKPOINT_COUNT
            ? g_bossTraceSamples
            : (u32)BOSS_CHECKPOINT_COUNT;
        u32 index;
        checkpoints[0] = '\0';
        for (index = 0; index < emitted; index++)
        {
            u32 slot = (g_bossTraceSamples - emitted + index) %
                       (u32)BOSS_CHECKPOINT_COUNT;
            cursor += sprintf(cursor, " %lu:%08lx",
                              (unsigned long)g_bossCheckpointFrames[slot],
                              (unsigned long)g_bossCheckpointAccumulators[slot]);
        }
        g_inputTimingSummaryLogged = true;
        g_GameErrorContext.Log(
            "info : boss state samples %lu accumulator %08lx stage_invalidations %lu\r\n",
            (unsigned long)g_bossTraceSamples,
            (unsigned long)g_bossTraceAccumulator,
            (unsigned long)g_rollbackStageInvalidations);
        // Per-frame values, so peers that stopped at different frames can still
        // be compared on the samples they both took.
        g_GameErrorContext.Log("info : boss checkpoints%s\r\n", checkpoints);
        {
            u32 spellEmitted =
                g_spellCheckpointCount < (u32)SPELL_CHECKPOINT_COUNT
                    ? g_spellCheckpointCount
                    : (u32)SPELL_CHECKPOINT_COUNT;
            u32 spellIndex;
            for (spellIndex = 0; spellIndex < spellEmitted; spellIndex++)
            {
                u32 slot = (g_spellCheckpointCount - spellEmitted +
                            spellIndex) % (u32)SPELL_CHECKPOINT_COUNT;
                g_GameErrorContext.Log(
                    "info : spell checkpoint %lu info %08lx stage %08lx boss %08lx ecl %08lx\r\n",
                    (unsigned long)g_spellCheckpointFrames[slot],
                    (unsigned long)g_spellCheckpointSubHash[slot][0],
                    (unsigned long)g_spellCheckpointSubHash[slot][1],
                    (unsigned long)g_spellCheckpointSubHash[slot][2],
                    (unsigned long)g_spellCheckpointSubHash[slot][3]);
            }
        }
        g_GameErrorContext.Log(
            "info : relay accepted %lu redundant %lu dropped_window %lu no_prediction %lu corrections %lu max_lag %lu\r\n",
            (unsigned long)g_relayAccepted,
            (unsigned long)g_relayRedundant,
            (unsigned long)g_relayDroppedWindow,
            (unsigned long)g_relayNoPrediction,
            (unsigned long)g_relayCorrections,
            (unsigned long)g_relayMaxLag);
        g_GameErrorContext.Log(
            "info : state verify compared %lu transient %lu gate %lu horizon %lu local_slot %lu ignore %lu zero_hash %lu remote %lu\r\n",
            (unsigned long)g_dsCompared, (unsigned long)g_dsTransientCount,
            (unsigned long)g_dsSkipGate,
            (unsigned long)g_dsSkipHorizon, (unsigned long)g_dsSkipLocalSlot,
            (unsigned long)g_dsSkipIgnore, (unsigned long)g_dsSkipZeroHash,
            (unsigned long)g_dsSkipRemote);
        g_GameErrorContext.Log(
            "info : input timing delay %d stalls %lu avg_wait %lu ms max_wait %lu ms gameplay_stalls %lu gameplay_avg %lu ms gameplay_max %lu ms rtt %lu ms\r\n",
            g_delay, (unsigned long)g_inputStallCount,
            (unsigned long)averageStall, (unsigned long)g_inputStallMaxMs,
            (unsigned long)g_gameplayInputStallCount,
            (unsigned long)averageGameplayStall,
            (unsigned long)g_gameplayInputStallMaxMs,
            (unsigned long)g_lastRoundTripMs);
    }
    if (g_rollbackEverEnabled && !g_rollbackTimingSummaryLogged)
    {
        DWORD gameplayElapsedMs = g_rollbackGameplayStartedTick != 0
            ? GetTickCount() - g_rollbackGameplayStartedTick
            : 0;
        g_rollbackTimingSummaryLogged = true;
        g_GameErrorContext.Log(
            "info : rollback predicted %lu corrections %lu correction_permille %lu replay_frames %lu max_replay %lu replay_time_us %lu avg_replay_us %lu max_replay_us %lu effect_restores %lu max_bomb_effects %lu tail_refresh %lu gameplay_frames %lu gameplay_ms %lu\r\n",
            (unsigned long)g_rollbackPredictedFrames,
            (unsigned long)g_rollbackCount,
            (unsigned long)(g_rollbackGameplayFrames != 0
                                ? g_rollbackCount * 1000 /
                                      g_rollbackGameplayFrames
                                : 0),
            (unsigned long)g_rollbackReplayFrames,
            (unsigned long)g_rollbackMaxReplayFrames,
            (unsigned long)g_rollbackReplayTimeUs,
            (unsigned long)(g_rollbackCount != 0
                                ? g_rollbackReplayTimeUs /
                                      g_rollbackCount
                                : 0),
            (unsigned long)g_rollbackMaxReplayTimeUs,
            (unsigned long)g_rollbackRestoredBombEffects,
            (unsigned long)g_rollbackMaxBombEffects,
            (unsigned long)g_rollbackPredictionRefreshFrames,
            (unsigned long)g_rollbackGameplayFrames,
            (unsigned long)gameplayElapsedMs);
    }
}
} // namespace

void Netplay::DrawPlayer2ResourceIcons()
{
    DrawPlayer2ResourceIconsInternal();
}

bool Netplay::Initialize(const char *commandLine)
{
    int port;
    int modeCount;
    int requestedPlayerCount;
    bool hasJoin;
    bool hasJoinOption;
    bool hasHost;
    bool hasLocal;
    bool hasQuickSelection;
    bool isFullRunTest;
    bool isTestPreset;
    bool hasDelayOption;
    bool valid;
    unsigned long number;
    char word[32];
    char host[128];
    char caption[256];
    bool useConnectionUi;
    ConnectionUiSelection uiSelection;

    if (!commandLine)
    {
        commandLine = "";
    }
    g_startupCancelled = false;
    LoadDisplayPreferences();
    g_displayMode = DISPLAY_MODE_FROM_GAME_CONFIG;
    g_connectionUiLaunchMode = false;
    g_cliGuestStartBarrierEligible = false;
    g_audioPreferenceActive = false;
    g_bgmEnabled = true;
    g_seEnabled = true;
    LoadLowLatencyConfig();
    g_lowLatencyExplicit = HasOption(commandLine, "--low-latency");
    if (g_lowLatencyExplicit)
    {
        g_lowLatencyEnabled = true;
    }
    if (HasOption(commandLine, "--no-low-latency"))
    {
        g_lowLatencyEnabled = false;
    }
    if (HasOption(commandLine, "--low-latency-spin"))
    {
        g_lowLatencySpinWait = true;
    }
    if (HasOption(commandLine, "--low-latency-hybrid"))
    {
        g_lowLatencySpinWait = false;
    }
    if (HasOption(commandLine, "--dwm-flush"))
    {
        g_lowLatencyDwmFlush = true;
    }
    if (HasOption(commandLine, "--no-dwm-flush"))
    {
        g_lowLatencyDwmFlush = false;
    }
    if (ParseUnsignedAfter(commandLine, "--blt-prepare", &number, &valid))
    {
        if (!valid || number > 16)
        {
            SetStatus("--blt-prepare must be between 0 and 16");
            return false;
        }
        g_lowLatencyPrepareTimeMs = (int)number;
    }
    g_rollbackEnabled = HasOption(commandLine, "--rollback") &&
        !HasOption(commandLine, "--no-rollback");
    isFullRunTest = HasOption(commandLine, "--test-full-run");
    if (HasOption(commandLine, "--help") || HasOption(commandLine, "-h") ||
        HasOption(commandLine, "/?"))
    {
        ShowHelpAndExit();
    }
    ResetInputRings();
    memset(g_peerAddresses, 0, sizeof(g_peerAddresses));
    memset(g_peerPresent, 0, sizeof(g_peerPresent));
    g_connected = false;
    g_session = 0;
    g_initialRngSeed = 0;
    g_mode = MODE_SINGLE;
    g_playerCount = 1;
    g_localPlayerSlot = 0;
    g_absentPlayerMask = 0;
    g_connectedPlayerMask = 1;
    LoadLocalPlayerNameConfig();
    SetRemotePlayerName("Player2");
    g_resultDetached = false;
    g_resultReconnectAttempted = false;
    g_insaneMode = false;
    g_finalResourceBonusAppliedMask = 0;
    // Parse this before the optional connection UI opens so --no-save also
    // covers the UI's mod_config.ini write.
    g_localNoSave = HasOption(commandLine, "--test") || isFullRunTest ||
        HasOption(commandLine, "--test-damage-events") ||
        HasOption(commandLine, "--no-save");
    g_controlTestEnabled = HasOption(commandLine, "--test-controls");
    g_testRngMismatchEnabled = HasOption(commandLine,
                                          "--test-rng-mismatch");
    g_testStateMismatchEnabled = HasOption(commandLine,
                                            "--test-state-mismatch");
    g_testBossDesyncEnabled = HasOption(commandLine, "--test-boss-desync");
    g_testKeepStaleStageSnapshots =
        HasOption(commandLine, "--test-keep-stale-stage-snapshots");
    if (g_testKeepStaleStageSnapshots)
    {
        g_GameErrorContext.Log(
            "info : test stage snapshot invalidation disabled\r\n");
    }
    g_testResultReconnectEnabled = HasOption(commandLine,
                                              "--test-result-reconnect");
    g_testReplayBlockEnabled = HasOption(commandLine,
                                          "--test-replay-block");
    g_testUiSyncEnabled = HasOption(commandLine, "--test-ui-sync");
    g_testInputSyncEnabled = HasOption(commandLine, "--test-input-sync");
    g_testProximityEnabled = HasOption(commandLine, "--test-proximity");
    g_testLifeTransferEnabled = HasOption(commandLine,
                                           "--test-life-transfer");
    g_testDamageEventsEnabled = HasOption(commandLine,
                                           "--test-damage-events");
    g_testRollbackInputEnabled = HasOption(commandLine,
                                            "--test-rollback-input");
    g_testRandomInputEnabled = HasOption(commandLine,
                                          "--test-random-input");
    g_testEvasiveInputEnabled = HasOption(commandLine,
                                           "--test-evasive-input");
    if (g_testRandomInputEnabled && g_testEvasiveInputEnabled)
    {
        SetStatus("--test-random-input and --test-evasive-input cannot be combined");
        return false;
    }
    g_testMenuInputEnabled = HasOption(commandLine, "--test-menu-input");
    // --test-title-run is the whole unattended path the smoke tests never
    // covered: start at the title, walk the shared P1/P2/P3 loadout sequence,
    // then hand the ships to the evasive bots. Deliberately does not imply
    // quick start; skipping the menus is exactly what hid the P3 cut-in crash.
    if (HasOption(commandLine, "--test-title-run"))
    {
        g_testMenuInputEnabled = true;
        g_testEvasiveInputEnabled = true;
        g_testRandomInputEnabled = false;
        g_demoDisabled = true;
        g_localNoSave = true;
        g_noSave = true;
        g_forceWindowed = true;
        g_ignoreControllerInput = true;
        g_localIgnoreControllerInput = true;
    }
    g_testResourceDropsEnabled = HasOption(commandLine,
                                            "--test-resource-drops");
    g_testP2FeaturesEnabled = HasOption(commandLine,
                                        "--test-p2-features");
    g_testP3FeaturesEnabled = HasOption(commandLine,
                                        "--test-p3-features");
    g_testHashTraceEnabled = HasOption(commandLine,
                                       "--test-hash-trace");
    g_testStartedTick = 0;

    // A bare double-click should open the multiplayer launcher. The original
    // game treats an empty command line as single-player and rejects the
    // second process with its singleton mutex, which is especially confusing
    // when two people are trying to start a network game. Keep the original
    // single-player path available through the explicit --single switch.
    useConnectionUi = HasOption(commandLine, "--connect-ui") ||
        (IsBlankCommandLine(commandLine) &&
         !HasOption(commandLine, "--single"));

    isTestPreset = HasOption(commandLine, "--test") || isFullRunTest ||
        g_testRandomInputEnabled || g_testEvasiveInputEnabled ||
        g_testRollbackInputEnabled ||
        g_testResourceDropsEnabled || g_testP2FeaturesEnabled ||
        g_testP3FeaturesEnabled || g_testHashTraceEnabled ||
        g_testDamageEventsEnabled;
    g_invincible = (isTestPreset || HasOption(commandLine, "--invincible")) &&
        !g_testDamageEventsEnabled &&
        !HasOption(commandLine, "--no-invincible");
    // The native multiplayer launcher is not an original single-player title
    // session.  Disable the attract/demo timer for every launcher-launched
    // session, including a normal GUI Host with no test flags; otherwise a
    // Host left at the title while waiting for manual input can enter a demo
    // before the players start the match.
    g_demoDisabled = useConnectionUi || isTestPreset ||
        HasOption(commandLine, "--no-demo");
    g_autoShoot = isTestPreset || HasOption(commandLine, "--auto-shoot");
    g_autoSkip = isTestPreset || HasOption(commandLine, "--auto-skip");
    g_autoBomb = isFullRunTest || HasOption(commandLine, "--auto-bomb");
    g_localIgnoreControllerInput = isTestPreset ||
        HasOption(commandLine, "--no-controller");
    g_ignoreControllerInput = g_localIgnoreControllerInput;
    if (HasOption(commandLine, "--fullscreen") &&
        (HasOption(commandLine, "--windowed") ||
         HasOption(commandLine, "--window-size")))
    {
        SetStatus("--fullscreen cannot be combined with --windowed or --window-size");
        return false;
    }
    if (HasOption(commandLine, "--fullscreen"))
    {
        g_displayMode = DISPLAY_MODE_FULLSCREEN_640;
    }
    else if (HasOption(commandLine, "--window-size"))
    {
        if (!ParseWordAfter(commandLine, "--window-size", word,
                            sizeof(word)))
        {
            SetStatus("--window-size requires 640x480, 960x720, or 1280x960");
            return false;
        }
        if (_stricmp(word, "640x480") == 0)
        {
            g_displayMode = DISPLAY_MODE_WINDOW_640;
        }
        else if (_stricmp(word, "960x720") == 0)
        {
            g_displayMode = DISPLAY_MODE_WINDOW_960;
        }
        else if (_stricmp(word, "1280x960") == 0)
        {
            g_displayMode = DISPLAY_MODE_WINDOW_1280;
        }
        else
        {
            SetStatus("invalid --window-size value");
            return false;
        }
    }
    else if (isTestPreset || HasOption(commandLine, "--windowed"))
    {
        g_displayMode = DISPLAY_MODE_WINDOW_640;
    }
    g_forceWindowed = g_displayMode > DISPLAY_MODE_FULLSCREEN_640;
    g_localNoSave = isTestPreset || HasOption(commandLine, "--no-save");
    g_noSave = g_localNoSave;
    // Automated input modes own shot/bomb edges. Random mode deliberately
    // varies them; evasive mode holds shot and only pulses bomb in danger.
    if (g_testRandomInputEnabled || g_testEvasiveInputEnabled)
    {
        g_autoShoot = false;
        g_autoBomb = false;
    }
    if (HasOption(commandLine, "--no-auto-shoot"))
    {
        g_autoShoot = false;
    }
    if (HasOption(commandLine, "--no-auto-skip"))
    {
        g_autoSkip = false;
    }
    if (HasOption(commandLine, "--no-auto-bomb"))
    {
        g_autoBomb = false;
    }

    g_quickDifficulty = 1;
    g_quickCharacter = 0;
    g_quickShot = 0;
    g_quickStage = 1;
    g_p2Character = -1;
    g_p2Shot = -1;
    g_p3Character = -1;
    g_p3Shot = -1;
    g_p2LoadoutConfigured = false;
    g_p2LoadoutSelected = false;
    g_p3LoadoutConfigured = false;
    g_p3LoadoutSelected = false;
    g_p2LoadoutConfigured = g_p2Character >= 0 && g_p2Character <= 2 &&
        g_p2Shot >= 0 && g_p2Shot <= 1;
    // Damage/death coverage needs the normal shared two-life start. Practice
    // mode forces both pools to eight and would never reach spirit/transfer in
    // a bounded network smoke test.
    g_quickStartPractice = isTestPreset && !isFullRunTest &&
        !g_testDamageEventsEnabled;
    hasQuickSelection = false;

    if (HasOption(commandLine, "--difficulty"))
    {
        if (!ParseWordAfter(commandLine, "--difficulty", word, sizeof(word)))
        {
            SetStatus("--difficulty requires easy, normal, hard, or lunatic");
            return false;
        }
        if (_stricmp(word, "easy") == 0 || strcmp(word, "0") == 0)
        {
            g_quickDifficulty = 0;
        }
        else if (_stricmp(word, "normal") == 0 || strcmp(word, "1") == 0)
        {
            g_quickDifficulty = 1;
        }
        else if (_stricmp(word, "hard") == 0 || strcmp(word, "2") == 0)
        {
            g_quickDifficulty = 2;
        }
        else if (_stricmp(word, "lunatic") == 0 || strcmp(word, "3") == 0)
        {
            g_quickDifficulty = 3;
        }
        else
        {
            SetStatus("invalid --difficulty value");
            return false;
        }
        hasQuickSelection = true;
    }
    if (HasOption(commandLine, "--character"))
    {
        if (!ParseWordAfter(commandLine, "--character", word, sizeof(word)))
        {
            SetStatus("--character requires reimu, marisa, or sakuya");
            return false;
        }
        if (_stricmp(word, "reimu") == 0 || strcmp(word, "0") == 0)
        {
            g_quickCharacter = 0;
        }
        else if (_stricmp(word, "marisa") == 0 || strcmp(word, "1") == 0)
        {
            g_quickCharacter = 1;
        }
        else if (_stricmp(word, "sakuya") == 0 || strcmp(word, "2") == 0)
        {
            g_quickCharacter = 2;
        }
        else
        {
            SetStatus("invalid --character value");
            return false;
        }
        hasQuickSelection = true;
    }
    if (HasOption(commandLine, "--shot"))
    {
        if (!ParseWordAfter(commandLine, "--shot", word, sizeof(word)))
        {
            SetStatus("--shot requires a or b");
            return false;
        }
        if (_stricmp(word, "a") == 0 || strcmp(word, "0") == 0)
        {
            g_quickShot = 0;
        }
        else if (_stricmp(word, "b") == 0 || strcmp(word, "1") == 0)
        {
            g_quickShot = 1;
        }
        else
        {
            SetStatus("invalid --shot value");
            return false;
        }
        hasQuickSelection = true;
    }
    if (HasOption(commandLine, "--p2-character"))
    {
        if (!ParseWordAfter(commandLine, "--p2-character", word, sizeof(word)))
        {
            SetStatus("--p2-character requires reimu, marisa, or sakuya");
            return false;
        }
        if (_stricmp(word, "reimu") == 0 || strcmp(word, "0") == 0)
        {
            g_p2Character = 0;
        }
        else if (_stricmp(word, "marisa") == 0 || strcmp(word, "1") == 0)
        {
            g_p2Character = 1;
        }
        else if (_stricmp(word, "sakuya") == 0 || strcmp(word, "2") == 0)
        {
            g_p2Character = 2;
        }
        else
        {
            SetStatus("invalid --p2-character value");
            return false;
        }
    }
    if (HasOption(commandLine, "--p2-shot"))
    {
        if (!ParseWordAfter(commandLine, "--p2-shot", word, sizeof(word)))
        {
            SetStatus("--p2-shot requires a or b");
            return false;
        }
        if (_stricmp(word, "a") == 0 || strcmp(word, "0") == 0)
        {
            g_p2Shot = 0;
        }
        else if (_stricmp(word, "b") == 0 || strcmp(word, "1") == 0)
        {
            g_p2Shot = 1;
        }
        else
        {
            SetStatus("invalid --p2-shot value");
            return false;
        }
    }
    g_p2LoadoutConfigured = g_p2Character >= 0 && g_p2Character <= 2 &&
        g_p2Shot >= 0 && g_p2Shot <= 1;
    if (HasOption(commandLine, "--p3-character"))
    {
        if (!ParseWordAfter(commandLine, "--p3-character", word, sizeof(word)))
        {
            SetStatus("--p3-character requires reimu, marisa, or sakuya");
            return false;
        }
        if (_stricmp(word, "reimu") == 0 || strcmp(word, "0") == 0)
        {
            g_p3Character = 0;
        }
        else if (_stricmp(word, "marisa") == 0 || strcmp(word, "1") == 0)
        {
            g_p3Character = 1;
        }
        else if (_stricmp(word, "sakuya") == 0 || strcmp(word, "2") == 0)
        {
            g_p3Character = 2;
        }
        else
        {
            SetStatus("invalid --p3-character value");
            return false;
        }
    }
    if (HasOption(commandLine, "--p3-shot"))
    {
        if (!ParseWordAfter(commandLine, "--p3-shot", word, sizeof(word)))
        {
            SetStatus("--p3-shot requires a or b");
            return false;
        }
        if (_stricmp(word, "a") == 0 || strcmp(word, "0") == 0)
        {
            g_p3Shot = 0;
        }
        else if (_stricmp(word, "b") == 0 || strcmp(word, "1") == 0)
        {
            g_p3Shot = 1;
        }
        else
        {
            SetStatus("invalid --p3-shot value");
            return false;
        }
    }
    g_p3LoadoutConfigured = g_p3Character >= 0 && g_p3Character <= 2 &&
        g_p3Shot >= 0 && g_p3Shot <= 1;
    if (ParseUnsignedAfter(commandLine, "--stage", &number, &valid))
    {
        if (!valid || number < 1 || number > 6)
        {
            SetStatus("--stage must be between 1 and 6");
            return false;
        }
        g_quickStage = (int)number;
        g_quickStartPractice = true;
        hasQuickSelection = true;
    }
    if (HasOption(commandLine, "--practice"))
    {
        g_quickStartPractice = true;
        hasQuickSelection = true;
    }
    if (HasOption(commandLine, "--normal-stage"))
    {
        // --stage normally selects TH07's Practice route. Test runs that
        // need real stage transitions can explicitly request the normal
        // route while keeping the same deterministic quick-start setup.
        g_quickStartPractice = false;
        hasQuickSelection = true;
    }
    // Enabling any bot driver marks the run as a test preset, and a test
    // preset implies quick start. That is the opposite of what the menu
    // driver is for: it exists to walk the title and loadout screens, so it
    // has to veto the implication rather than inherit it.
    g_quickStartEnabled = (isTestPreset ||
        HasOption(commandLine, "--quick-start") || hasQuickSelection) &&
        !g_testMenuInputEnabled;
    g_quickStartPending = g_quickStartEnabled;

    g_seedConfigured = isTestPreset;
    g_configuredSeed = 1;
    if (ParseUnsignedAfter(commandLine, "--seed", &number, &valid))
    {
        if (!valid || number > 65535)
        {
            SetStatus("--seed must be between 0 and 65535");
            return false;
        }
        g_seedConfigured = true;
        g_configuredSeed = (u16)number;
    }

    g_testSeconds = 0;
    if (ParseUnsignedAfter(commandLine, "--test-seconds", &number, &valid))
    {
        if (!valid || number < 1 || number > 86400)
        {
            SetStatus("--test-seconds must be between 1 and 86400");
            return false;
        }
        g_testSeconds = (int)number;
    }
    g_connectTimeoutSeconds = DEFAULT_CONNECT_TIMEOUT_SECONDS;
    if (ParseUnsignedAfter(commandLine, "--connect-timeout", &number,
                            &valid))
    {
        if (!valid || number < 1 || number > 600)
        {
            SetStatus("--connect-timeout must be between 1 and 600");
            return false;
        }
        g_connectTimeoutSeconds = (int)number;
    }
    g_reconnectTestPending = HasOption(commandLine, "--test-reconnect");

    g_delay = DEFAULT_DELAY;
    hasDelayOption = ParseUnsignedAfter(commandLine, "--delay", &number,
                                         &valid);
    if (hasDelayOption)
    {
        if (!valid || number > 10)
        {
            SetStatus("--delay must be between 0 and 10");
            return false;
        }
        g_delay = (int)number;
    }
    if (g_rollbackEnabled && !hasDelayOption && !useConnectionUi)
    {
        // Prediction/rollback removes the fixed one-frame buffer. Keep the
        // old delay as the explicit fallback when rollback is not requested.
        g_delay = 0;
    }
    port = DEFAULT_PORT;
    if (ParseUnsignedAfter(commandLine, "--port", &number, &valid))
    {
        if (!valid || number < 1 || number > 65535)
        {
            SetStatus("--port must be between 1 and 65535");
            return false;
        }
        port = (int)number;
    }
    requestedPlayerCount = 2;
    if (ParseUnsignedAfter(commandLine, "--players", &number, &valid))
    {
        if (!valid || (number != 2 && number != 3))
        {
            SetStatus("--players must be 2 or 3");
            return false;
        }
        requestedPlayerCount = (int)number;
    }
    if (HasOption(commandLine, "--three-player") ||
        HasOption(commandLine, "--test-p3-features"))
    {
        requestedPlayerCount = 3;
    }
    if (requestedPlayerCount == 3)
    {
        // Three-player guest-to-guest traffic takes two hops. Prediction is
        // mandatory so ordinary movement never acquires that full delay.
        g_rollbackEnabled = true;
        g_rollbackEverEnabled = true;
        g_delay = 0;
    }

    if (useConnectionUi)
    {
        // TH06 establishes the socket and reports the matched peer inside the
        // launcher. Parse all gameplay options first so the Host's WELCOME
        // packet already carries the final authoritative settings.
        if (!RunConnectionUi(&uiSelection,
                             !HasOption(commandLine, "--no-rollback")))
        {
            g_startupCancelled = g_connectionUi.selection.cancelled;
            SetStatus("connection UI cancelled");
            return false;
        }
        g_displayMode = uiSelection.displayMode;
        g_forceWindowed =
            g_displayMode > DISPLAY_MODE_FULLSCREEN_640;
        g_audioPreferenceActive = true;
        g_bgmEnabled = uiSelection.bgmEnabled;
        g_showStagePlayerNames = uiSelection.showStageNames;
        g_showNetDiagnostics = uiSelection.showNetStats;
        g_seEnabled = uiSelection.seEnabled;
        requestedPlayerCount = uiSelection.playerCount;
        g_rollbackEnabled = uiSelection.rollback &&
            uiSelection.mode != Netplay::MODE_LOCAL &&
            !HasOption(commandLine, "--no-rollback");
        if (requestedPlayerCount == 3 &&
            uiSelection.mode != Netplay::MODE_LOCAL)
        {
            g_rollbackEnabled = true;
        }
        g_rollbackEverEnabled = g_rollbackEverEnabled || g_rollbackEnabled;
        port = uiSelection.port;
        g_delay = g_rollbackEnabled ? 0 : uiSelection.delay;
    }
    if (useConnectionUi && uiSelection.mode == Netplay::MODE_GUEST &&
        uiSelection.evasiveBot)
    {
        // The launcher checkbox is the GUI equivalent of
        // --test-evasive-input. Keep this driver local: the Host's P1 stays
        // under its own keyboard/pad, while this Guest owns only its
        // negotiated P2/P3 lane.
        g_testEvasiveInputEnabled = true;
        g_testRandomInputEnabled = false;
        g_localIgnoreControllerInput = true;
        g_ignoreControllerInput = true;
        g_autoShoot = false;
        g_autoBomb = false;
        g_demoDisabled = true;
        g_localNoSave = true;
        g_noSave = true;
    }

    hasLocal = HasOption(commandLine, "--local");
    hasHost = HasOption(commandLine, "--host");
    hasJoinOption = HasOption(commandLine, "--join");
    hasJoin = ParseJoinHost(commandLine, host, sizeof(host));
    if (useConnectionUi)
    {
        hasLocal = uiSelection.mode == Netplay::MODE_LOCAL;
        hasHost = uiSelection.mode == Netplay::MODE_HOST;
        hasJoinOption = uiSelection.mode == Netplay::MODE_GUEST;
        hasJoin = hasJoinOption;
        strncpy(host, uiSelection.host, sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
    }
    if (hasJoinOption && !hasJoin)
    {
        SetStatus("--join requires a host name or IP address");
        return false;
    }
    modeCount = (hasLocal ? 1 : 0) + (hasHost ? 1 : 0) +
        (hasJoinOption ? 1 : 0);
    if (modeCount > 1)
    {
        SetStatus("choose only one of --local, --host, or --join");
        return false;
    }

    if (hasLocal ||
        (isTestPreset && !hasHost && !hasJoinOption))
    {
        g_mode = MODE_LOCAL;
        g_playerCount = requestedPlayerCount;
        g_localPlayerSlot = 0;
        SetSessionPlayerName(0, g_localPlayerName);
        SetStatus(g_playerCount == 3
                      ? (g_invincible
                             ? "local three-player mode (invincible)"
                             : "local three-player mode")
                      : (g_invincible
                             ? "local two-player mode (invincible)"
                             : "local two-player mode"));
        return true;
    }
    if (hasHost)
    {
        g_mode = MODE_HOST;
        g_playerCount = requestedPlayerCount;
        g_localPlayerSlot = 0;
        SetSessionPlayerName(0, g_localPlayerName);
        if (g_playerCount == 3)
        {
            g_rollbackEnabled = true;
            g_rollbackEverEnabled = true;
            g_delay = 0;
        }
        if (useConnectionUi)
        {
            if (!g_connected || g_socket == INVALID_SOCKET ||
                !AreAllExpectedPeersConnected())
            {
                SetStatus("launcher closed before the Host matched a Guest");
                return false;
            }
            SetStatus("host connected");
            return true;
        }
        InitializeHostSession();
        if (!CreateSocket(port))
        {
            return false;
        }
        sprintf(caption,
                "th07_multi_net - Waiting for guest on UDP port %d", port);
        SetStatus(caption);
        return WaitForPeer(caption);
    }
    if (hasJoin)
    {
        g_mode = MODE_GUEST;
        if (useConnectionUi)
        {
            if (!g_connected || g_socket == INVALID_SOCKET ||
                !g_peerPresent[0] || g_localPlayerSlot < 1 ||
                g_localPlayerSlot >= g_playerCount)
            {
                SetStatus("launcher closed before the Guest matched a Host");
                return false;
            }
            // RunConnectionUi already accepted the Host's WELCOME packet.
            // Preserve its negotiated slot here: assigning every GUI Guest
            // back to P2 makes a real P3 send P2 input and leaves all peers
            // waiting forever for exact P3 frame-0 input.
            SetSessionPlayerName((u8)g_localPlayerSlot,
                                 g_localPlayerName);
            SetStatus("guest connected");
            return true;
        }
        g_playerCount = requestedPlayerCount;
        g_localPlayerSlot = 1;
        SetSessionPlayerName(1, g_localPlayerName);
        g_session = 0;
        if (!CreateSocket(0))
        {
            return false;
        }
        if (!ResolveAddress(host, port, &g_peerAddresses[0]))
        {
            SetStatus("host name could not be resolved");
            return false;
        }
        g_peerPresent[0] = true;
        sprintf(caption, "th07_multi_net - Connecting to %s:%d", host, port);
        SetStatus(caption);
        // A command-line Guest has no launcher window of its own. If the
        // Host is using the native GUI, WaitForPeer will hold this process at
        // the connection barrier until CONTROL_START_GAME_COMMIT arrives.
        g_cliGuestStartBarrierEligible = !useConnectionUi;
        return WaitForPeer(caption);
    }

    g_mode = MODE_SINGLE;
    g_playerCount = 1;
    g_localPlayerSlot = 0;
    SetStatus(g_invincible ? "single player (invincible)" : "single player");
    return true;
}

void Netplay::Shutdown()
{
    if (g_testHashTraceFile)
    {
        fclose(g_testHashTraceFile);
        g_testHashTraceFile = NULL;
    }
    if (g_netplayTraceFile)
    {
        fclose(g_netplayTraceFile);
        g_netplayTraceFile = NULL;
    }
    // The peer that closes the window never learns of a peer exit and never
    // reaches its own test deadline, so the two existing callers both miss it.
    // In the last real-line run that left exactly one of three peers with a
    // summary - the one that happened to be told the host had gone.
    if (IsNetworked())
    {
        LogEndOfRunSummary();
    }
    NotifyPeerExit();
    LowLatency::Shutdown();
    if (g_socket != INVALID_SOCKET)
    {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
    if (g_winsockStarted)
    {
        WSACleanup();
        g_winsockStarted = false;
    }
    g_connected = false;
    g_hasPeer = false;
}

Netplay::Mode Netplay::GetMode()
{
    return g_mode;
}

bool Netplay::IsMultiplayer()
{
    return g_mode != MODE_SINGLE;
}

bool Netplay::ShouldForceContentUnlocks()
{
    return IsMultiplayer();
}

bool Netplay::IsNetworked()
{
    return g_mode == MODE_HOST || g_mode == MODE_GUEST;
}

// The difficulty menu normally starts on whatever was last played. Two peers
// that last played different difficulties therefore open the menu with their
// cursors in different places, and because the menus synchronize presses
// rather than cursor positions, every later press lands somewhere else on
// each machine. Multiplayer starts from a fixed position instead.
i32 Netplay::GetInitialDifficultyCursor()
{
    if (IsMultiplayer())
    {
        return 1;
    }
    return g_Supervisor.cfg.defaultDifficulty;
}

bool Netplay::IsHost()
{
    return g_mode == MODE_HOST;
}

bool Netplay::IsConnected()
{
    return g_connected;
}

int Netplay::GetPlayerCount()
{
    return g_playerCount;
}

int Netplay::GetLocalPlayerSlot()
{
    return g_localPlayerSlot;
}

bool Netplay::IsPlayerTemporarilyAbsent(u8 playerId)
{
    return playerId < TH07_MULTI_MAX_PLAYERS &&
        (g_absentPlayerMask & (1 << playerId)) != 0;
}

bool Netplay::IsPlayerPermanentlyDeparted(u8 playerId)
{
    return playerId < TH07_MULTI_MAX_PLAYERS &&
        (g_departedPlayerMask & (1 << playerId)) != 0;
}

bool Netplay::IsWaitingForRemoteInput()
{
    bool gameplayState = g_GameManager.notInMenu &&
        g_Supervisor.curState == TH07_SUPERVISOR_GAME_STATE;
    return gameplayState && g_waitingForRemoteInput;
}

bool Netplay::IsConnectionFailed()
{
    return g_GameManager.notInMenu &&
        g_Supervisor.curState == TH07_SUPERVISOR_GAME_STATE &&
        g_connectionFailed;
}

bool Netplay::IsRollbackReplay()
{
    return g_rollbackReplaying;
}

bool Netplay::IsResyncing()
{
    return g_rngMismatch;
}

bool Netplay::LowLatencyEnabled()
{
    return g_lowLatencyEnabled &&
        (g_mode != MODE_SINGLE || g_lowLatencyExplicit);
}

bool Netplay::LowLatencySpinWait()
{
    return g_lowLatencySpinWait;
}

bool Netplay::LowLatencyDwmFlush()
{
    return g_lowLatencyDwmFlush;
}

int Netplay::GetLowLatencyPrepareTimeMs()
{
    return g_lowLatencyPrepareTimeMs;
}

bool Netplay::AllowsMultipleInstances()
{
    // The original singleton is only meaningful for direct single-player.
    // All multiplayer modes, including local two-player, must be able to
    // start without showing the original "二つは起動できません" dialog.
    return IsMultiplayer();
}

bool Netplay::IsInvincible()
{
    return g_invincible;
}

bool Netplay::IsDemoDisabled()
{
    return g_demoDisabled;
}

bool Netplay::ConsumeQuickStart()
{
    DWORD now;

    if (!g_quickStartPending)
    {
        return false;
    }
    if (!IsNetworked())
    {
        g_quickStartPending = false;
        return true;
    }

    // The old quick-start path let Host and Guest consume the menu shortcut
    // on whichever machine finished loading first.  That made their stage
    // RNG histories start at different logical frames.  Exchange a ready
    // marker first, then use one future network frame as the shared launch
    // boundary.  Input synchronization keeps g_frame aligned while both
    // peers remain in the title scene.
    PollPackets();
    now = GetTickCount();
    // READY used to be a single UDP datagram. Losing either Guest's one copy
    // left a three-player Host at the title screen forever even though frame
    // zero input synchronization had succeeded. Keep advertising readiness
    // until the Host publishes the shared launch boundary.
    if (g_quickStartFrame == INVALID_FRAME &&
        (!g_quickStartReadySent ||
         now - g_lastQuickStartSendTick >= 100))
    {
        SendControl(CONTROL_QUICK_START_READY, INVALID_FRAME, 1);
        g_quickStartReadySent = true;
        g_lastQuickStartSendTick = now;
        if (!g_quickStartRemoteReady)
        {
            SetStatus("waiting for peer quick-start readiness");
        }
    }
    if (g_quickStartFrame == INVALID_FRAME &&
        g_mode == Netplay::MODE_HOST && g_quickStartRemoteReady &&
        (g_quickStartReadyMask &
         (u8)(((1 << g_playerCount) - 1) & ~1)) ==
            (u8)(((1 << g_playerCount) - 1) & ~1))
    {
        g_quickStartFrame = g_frame + 20;
        g_lastQuickStartSendTick = 0;
        SendControl(CONTROL_QUICK_START, g_quickStartFrame, 1);
        g_GameErrorContext.Log(
            "info : quick-start boundary scheduled at frame %lu\r\n",
            (unsigned long)g_quickStartFrame);
    }
    if (g_mode == Netplay::MODE_HOST &&
        g_quickStartFrame != INVALID_FRAME &&
        now - g_lastQuickStartSendTick >= 100)
    {
        SendControl(CONTROL_QUICK_START, g_quickStartFrame, 1);
        g_lastQuickStartSendTick = now;
    }
    if (g_quickStartFrame == INVALID_FRAME ||
        g_frame < g_quickStartFrame)
    {
        return false;
    }
    g_quickStartPending = false;
    g_GameErrorContext.Log(
        "info : quick-start boundary reached at frame %lu\r\n",
        (unsigned long)g_frame);
    g_quickStartFrame = INVALID_FRAME;
    return true;
}

int Netplay::GetQuickStartDifficulty()
{
    return g_quickDifficulty;
}

int Netplay::GetQuickStartCharacter()
{
    return g_quickCharacter;
}

int Netplay::GetQuickStartShot()
{
    return g_quickShot;
}

int Netplay::GetQuickStartStage()
{
    return g_quickStage;
}

bool Netplay::IsQuickStartPractice()
{
    return g_quickStartPractice;
}

int Netplay::GetNextPendingLoadoutPlayerId()
{
    if (g_playerCount >= 2 &&
        !g_p2LoadoutConfigured && !g_p2LoadoutSelected)
    {
        return 1;
    }
    if (g_playerCount >= 3 &&
        !g_p3LoadoutConfigured && !g_p3LoadoutSelected)
    {
        return 2;
    }
    return -1;
}

bool Netplay::ShouldSelectAdditionalPlayerLoadout()
{
    if (!IsMultiplayer() || g_quickStartEnabled ||
        GetNextPendingLoadoutPlayerId() < 1)
    {
        return false;
    }
    return !IsNetworked() || g_connected;
}

void Netplay::BeginPlayerLoadoutSelection(u8 playerId)
{
    int *character;
    int *shot;

    if (!ShouldSelectAdditionalPlayerLoadout() ||
        playerId != GetNextPendingLoadoutPlayerId())
    {
        return;
    }
    character = playerId == 1 ? &g_p2Character : &g_p3Character;
    shot = playerId == 1 ? &g_p2Shot : &g_p3Shot;
    if (*character < 0 || *character > 2)
    {
        *character = 0;
    }
    if (*shot < 0 || *shot > 1)
    {
        *shot = 0;
    }
    g_GameErrorContext.Log(
        "info : interactive P%d loadout selection started (default %d/%d)\r\n",
        playerId + 1, *character, *shot);
    SetStatus(playerId == 1 ? "select P2 loadout" : "select P3 loadout");
}

void Netplay::CompletePlayerLoadoutSelection(u8 playerId, int character,
                                            int shot)
{
    if (playerId < 1 || playerId >= g_playerCount ||
        character < 0 || character > 2 || shot < 0 || shot > 1)
    {
        return;
    }
    if (playerId == 1)
    {
        g_p2Character = character;
        g_p2Shot = shot;
        g_p2LoadoutSelected = true;
    }
    else
    {
        g_p3Character = character;
        g_p3Shot = shot;
        g_p3LoadoutSelected = true;
    }
    g_GameErrorContext.Log(
        "info : interactive P%d loadout selected character %d shot %d\r\n",
        playerId + 1, character, shot);
    SetStatus(playerId == 1 ? "P2 loadout selected" : "P3 loadout selected");
}

void Netplay::CancelPlayerLoadoutSelection()
{
    g_p2LoadoutSelected = false;
    g_p3LoadoutSelected = false;
    g_selectingLoadoutPlayerId = -1;
    if (!g_p2LoadoutConfigured)
    {
        g_p2Character = -1;
        g_p2Shot = -1;
    }
    if (!g_p3LoadoutConfigured)
    {
        g_p3Character = -1;
        g_p3Shot = -1;
    }
}

void Netplay::ResetInteractiveLoadout()
{
    g_p2LoadoutSelected = false;
    g_p3LoadoutSelected = false;
    g_selectingLoadoutPlayerId = -1;
    g_interactiveP1Character = 0;
    g_interactiveP1Shot = 0;
    if (!g_p2LoadoutConfigured)
    {
        g_p2Character = -1;
        g_p2Shot = -1;
    }
    if (!g_p3LoadoutConfigured)
    {
        g_p3Character = -1;
        g_p3Shot = -1;
    }
}

int Netplay::GetPlayerCharacter(u8 playerId)
{
    if (playerId == 0)
    {
        return g_GameManager.character;
    }
    if (playerId == 1)
    {
        return g_p2Character >= 0 && g_p2Character <= 2
            ? g_p2Character : g_GameManager.character;
    }
    return g_p3Character >= 0 && g_p3Character <= 2
        ? g_p3Character : g_GameManager.character;
}

int Netplay::GetPlayerShot(u8 playerId)
{
    if (playerId == 0)
    {
        return g_GameManager.shotType;
    }
    if (playerId == 1)
    {
        return g_p2Shot >= 0 && g_p2Shot <= 1
            ? g_p2Shot : g_GameManager.shotType;
    }
    return g_p3Shot >= 0 && g_p3Shot <= 1
        ? g_p3Shot : g_GameManager.shotType;
}

bool Netplay::ShouldShowStagePlayerNames()
{
    return g_showStagePlayerNames;
}

bool Netplay::ShouldShowNetDiagnostics()
{
    return g_showNetDiagnostics;
}

const char *Netplay::GetPlayerName(u8 playerId)
{
    if (playerId < TH07_MULTI_MAX_PLAYERS)
    {
        return g_sessionPlayerNames[playerId];
    }
    return "Player";
}

bool Netplay::ShouldTintPlayer2()
{
    return IsMultiplayer() && GetPlayerCharacter(0) == GetPlayerCharacter(1);
}

bool Netplay::ShouldTintPlayer(u8 playerId)
{
    int other;
    if (!IsMultiplayer() || playerId == 0 || playerId >= g_playerCount)
    {
        return false;
    }
    for (other = 0; other < g_playerCount; other++)
    {
        if (other != playerId &&
            GetPlayerCharacter((u8)other) == GetPlayerCharacter(playerId))
        {
            return true;
        }
    }
    return false;
}

bool Netplay::IsAutoShootEnabled()
{
    return g_autoShoot;
}

bool Netplay::IsAutoSkipEnabled()
{
    return g_autoSkip;
}

bool Netplay::IsProximityTestEnabled()
{
    return g_testProximityEnabled;
}

void Netplay::ReportProximityTestResult(bool passed)
{
    if (!g_testProximityEnabled || g_testProximityVerified ||
        g_testProximityFailureReported)
    {
        return;
    }
    if (!passed)
    {
        return;
    }
    g_testProximityVerified = true;
    g_GameErrorContext.Log(
        "info : close-player display fade verified at alpha 55\r\n");
    SetStatus("test close-player display fade passed");
}

bool Netplay::IsLifeTransferTestEnabled()
{
    return g_testLifeTransferEnabled;
}

bool Netplay::IsLifeTransferTestVerified()
{
    return g_testLifeTransferVerified;
}

void Netplay::ReportLifeTransferTestResult(i32 recipient, i32 livesBefore,
                                           i32 livesAfter)
{
    i32 donor = recipient == 0 ? 1 : 0;

    if (!g_testLifeTransferEnabled || g_testLifeTransferVerified ||
        g_testLifeTransferFailureReported)
    {
        return;
    }
    if (recipient != 1 || livesAfter != livesBefore + 1 ||
        GetPlayerLives(donor) != 1)
    {
        g_testLifeTransferFailureReported = true;
        g_GameErrorContext.Log(
            "error : normal life transfer test failed (recipient P%d %d -> %d, giver lives %d)\r\n",
            recipient + 1, livesBefore, livesAfter,
            GetPlayerLives(donor));
        SetStatus("test normal life transfer failed");
        return;
    }
    g_testLifeTransferVerified = true;
    g_GameErrorContext.Log(
        "info : normal life transfer verified (P1 2 -> 1; P2 %d -> %d; targeted life collected)\r\n",
        livesBefore, livesAfter);
    SetStatus("test normal life transfer passed");
}

bool Netplay::IsDamageEventTestEnabled()
{
    return g_testDamageEventsEnabled;
}

bool Netplay::ShouldInitializeDamageEventTest()
{
    u32 frame;

    if (!g_testDamageEventsEnabled || !IsMultiplayer() ||
        !g_GameManager.notInMenu || g_GameManager.replay)
    {
        return false;
    }
    frame = g_mode == MODE_LOCAL ? g_testInputSyncLocalFrame : g_frame;
    return frame == 60;
}

bool Netplay::ShouldInjectDamageEvent(u8 playerId)
{
    u32 frame;

    if (!g_testDamageEventsEnabled || !IsMultiplayer() ||
        !g_GameManager.notInMenu || g_GameManager.replay)
    {
        return false;
    }
    frame = g_mode == MODE_LOCAL ? g_testInputSyncLocalFrame : g_frame;
    if (playerId == 0)
    {
        // Frame 181 deliberately lands on the Host rollback-input bomb edge.
        // Further attempts are spaced far enough apart for the real death and
        // invulnerability timers; an attempt is ignored by Player.cpp unless
        // the ship is in an active state. Repeating also covers post-revive
        // damage without making test progress depend on stage bullet density.
        return frame == 181 ||
            (frame >= 541 && (frame - 541) % 420 == 0);
    }
    // Keep P2 alive with one spare life so it can revive P1 through the normal
    // 90-frame proximity transfer after P1 enters spirit mode.
    return playerId == 1 && frame == 361;
}

bool Netplay::ShouldForceDamageTestTransfer(u8 giverId)
{
    if (!g_testDamageEventsEnabled || giverId != 1 || !IsMultiplayer() ||
        !g_GameManager.notInMenu || g_GameManager.replay)
    {
        return false;
    }
    // Start the 90-frame charge only after the actual death state machine has
    // entered spirit mode. This remains deterministic because both players and
    // their state are part of every rollback snapshot.
    return g_Player.playerState == PLAYER_STATE_SPIRIT;
}

void Netplay::ReportDamageEventHit(u8 playerId, i32 lives, i32 playerState)
{
    if (!g_testDamageEventsEnabled || playerId > 1 || IsRollbackReplay())
    {
        return;
    }
    g_testDamageHitCount[playerId]++;
    g_GameErrorContext.Log(
        "info : damage event P%d hit frame %lu lives %d state %d hit_count %lu\r\n",
        playerId + 1, (unsigned long)g_frame, lives, playerState,
        (unsigned long)g_testDamageHitCount[playerId]);
}

void Netplay::ReportDamageEventRespawn(u8 playerId, i32 livesBefore,
                                       i32 livesAfter)
{
    if (!g_testDamageEventsEnabled || IsRollbackReplay())
    {
        return;
    }
    g_GameErrorContext.Log(
        "info : damage event P%d respawn frame %lu lives %d -> %d\r\n",
        playerId + 1, (unsigned long)g_frame, livesBefore, livesAfter);
}

void Netplay::ReportDamageEventSpirit(u8 playerId)
{
    if (!g_testDamageEventsEnabled || IsRollbackReplay())
    {
        return;
    }
    g_testDamageSpiritSeen = true;
    g_GameErrorContext.Log(
        "info : damage event P%d entered spirit frame %lu\r\n",
        playerId + 1, (unsigned long)g_frame);
}

void Netplay::ReportDamageEventRevive(u8 giverId, u8 receiverId,
                                      i32 giverLivesBefore,
                                      i32 giverLivesAfter)
{
    if (!g_testDamageEventsEnabled || IsRollbackReplay())
    {
        return;
    }
    g_testDamageReviveSeen = true;
    g_GameErrorContext.Log(
        "info : damage event life transfer P%d -> P%d revived frame %lu giver_lives %d -> %d\r\n",
        giverId + 1, receiverId + 1, (unsigned long)g_frame,
        giverLivesBefore, giverLivesAfter);
    SetStatus("damage/death/life-transfer test passed");
}

bool Netplay::ForceWindowed()
{
    return g_forceWindowed;
}

void Netplay::ApplyAudioPreferences()
{
    if (!g_audioPreferenceActive)
    {
        return;
    }

    if (g_bgmEnabled)
    {
        // A saved TH07 configuration can already have BGM disabled. Restore
        // the normal data-backed mode when the launcher explicitly enables
        // BGM; this remains local to the current PC and is not synchronized.
        if (g_Supervisor.cfg.musicMode == MUSIC_OFF)
        {
            g_Supervisor.cfg.musicMode =
                GetFileAttributesA("thbgm.dat") != INVALID_FILE_ATTRIBUTES
                    ? MUSIC_WAV
                    : MUSIC_MIDI;
        }
    }
    else
    {
        g_Supervisor.cfg.musicMode = MUSIC_OFF;
    }
    g_Supervisor.cfg.playSounds = g_seEnabled ? 1 : 0;
}

bool Netplay::IsBgmEnabled()
{
    return !g_audioPreferenceActive || g_bgmEnabled;
}

bool Netplay::ForceFullscreen()
{
    return g_displayMode == DISPLAY_MODE_FULLSCREEN_640;
}

int Netplay::GetWindowClientWidth()
{
    if (g_displayMode == DISPLAY_MODE_WINDOW_960)
    {
        return 960;
    }
    if (g_displayMode == DISPLAY_MODE_WINDOW_1280)
    {
        return 1280;
    }
    return 640;
}

int Netplay::GetWindowClientHeight()
{
    if (g_displayMode == DISPLAY_MODE_WINDOW_960)
    {
        return 720;
    }
    if (g_displayMode == DISPLAY_MODE_WINDOW_1280)
    {
        return 960;
    }
    return 480;
}

bool Netplay::WasStartupCancelled()
{
    return g_startupCancelled;
}

bool Netplay::NoSave()
{
    return g_noSave;
}

void Netplay::StartTestTimer()
{
    LowLatency::Install();
    if (IsMultiplayer() && !g_rollbackMemoryLogged)
    {
        g_rollbackMemoryLogged = true;
        g_GameErrorContext.Log(
            "info : rollback snapshot bytes %lu count %d total %lu history %dF bomb_effect_limit %d packet_bytes %lu\r\n",
            (unsigned long)sizeof(RollbackSnapshot),
            ROLLBACK_SNAPSHOT_COUNT,
            (unsigned long)(sizeof(RollbackSnapshot) *
                            ROLLBACK_SNAPSHOT_COUNT),
            ROLLBACK_HISTORY_FRAMES,
            ROLLBACK_BOMB_EFFECT_SNAPSHOT_COUNT,
            (unsigned long)sizeof(NetPacket));
    }
    // Two local network peers cannot both own the foreground window. The
    // original render loop pauses when WM_ACTIVATEAPP marks a window
    // inactive, which would deadlock the input handshake on the background
    // peer. Network play is deterministic and already owns its input clock,
    // so keep its render/calculation loop alive after startup.
    if (IsNetworked())
    {
        InstallNetworkWindowHook();
        g_GameWindow.isAppActive = 1;
        if (!g_networkDrawGuardRegistered)
        {
            // Draw priorities place this just before GameManager and the
            // stage/HUD chains. A packet wait therefore leaves the last
            // complete backbuffer untouched instead of drawing half a frame.
            g_Chain.AddToDrawChain(&g_networkDrawGuardChain, 1);
            g_networkDrawGuardRegistered = true;
        }
        if (!g_controllerLaneLogged)
        {
            g_GameErrorContext.Log(
                "info : local keyboard/controller input routed to P%d lane\r\n",
                GetLocalPlayerSlot() + 1);
            g_controllerLaneLogged = true;
        }
    }
    if (IsMultiplayer() && !g_controllerConfigLogged)
    {
        char configPath[MAX_PATH];
        DWORD configPathLength = GetFullPathNameA(
            "th07.cfg", sizeof(configPath), configPath, NULL);
        ControllerMapping *mapping = &g_Supervisor.cfg.controllerMapping;
        if (configPathLength == 0 || configPathLength >= sizeof(configPath))
        {
            strcpy(configPath, "th07.cfg");
        }
        g_GameErrorContext.Log(
            "info : local controller config '%s' lane P%d present %d map shoot %d bomb %d focus %d menu %d up %d down %d left %d right %d skip %d axes %d %d\r\n",
            configPath, GetLocalPlayerSlot() + 1,
            g_Supervisor.controller ? 1 : 0,
            mapping->shootButton, mapping->bombButton,
            mapping->focusButton, mapping->menuButton,
            mapping->upButton, mapping->downButton,
            mapping->leftButton, mapping->rightButton,
            mapping->skipButton, g_Supervisor.cfg.padAxisX,
            g_Supervisor.cfg.padAxisY);
        g_controllerConfigLogged = true;
    }
    if (IsMultiplayer() && !g_loadoutSelectionChainsRegistered)
    {
        // GameManager is priority 2 and MainMenu is priority 3. Registering
        // this after Supervisor lets the selector run immediately before the
        // original menu without changing MainMenu.cpp's CP932 data or size.
        g_loadoutSelectionCalcChain.callback =
            (ChainCallback)UpdateInteractiveLoadoutSelection;
        g_loadoutSelectionDrawChain.callback =
            (ChainCallback)DrawInteractiveLoadoutPrompt;
        g_Chain.AddToCalcChain(&g_loadoutSelectionCalcChain, 2);
        g_Chain.AddToDrawChain(&g_loadoutSelectionDrawChain, 1);
        g_loadoutSelectionChainsRegistered = true;
    }
    if (g_testSeconds > 0 && g_testStartedTick == 0)
    {
        g_testStartedTick = GetTickCount();
        g_GameErrorContext.Log(
            "info : runtime test flags auto_shoot %d auto_skip %d auto_bomb %d random_input %d evasive_input %d damage_events %d invincible %d no_controller %d practice %d\r\n",
            g_autoShoot ? 1 : 0, g_autoSkip ? 1 : 0,
            g_autoBomb ? 1 : 0, g_testRandomInputEnabled ? 1 : 0,
            g_testEvasiveInputEnabled ? 1 : 0,
            g_testDamageEventsEnabled ? 1 : 0, g_invincible ? 1 : 0,
            g_ignoreControllerInput ? 1 : 0, g_quickStartPractice ? 1 : 0);
    }
}

bool Netplay::HasTestTimedOut()
{
    if (IsNetworked())
    {
        // Keep this asserted from the main loop as well as at startup: a
        // later WM_ACTIVATEAPP message may mark the background peer inactive
        // after the second process creates its window.
        g_GameWindow.isAppActive = 1;
    }
    bool timedOut = g_testSeconds > 0 && g_testStartedTick != 0 &&
                    GetTickCount() - g_testStartedTick >=
                        (DWORD)g_testSeconds * 1000;
    if (timedOut)
    {
        LogEndOfRunSummary();
    }
    if (timedOut && g_testUiSyncEnabled && !g_testUiSyncVerified &&
        !g_testUiSyncFailureReported)
    {
        g_testUiSyncFailureReported = true;
        int diagnosticSlot = (int)(g_frame % INPUT_RING_SIZE);
        g_GameErrorContext.Log(
            "error : shared UI input synchronization timed out (mode %d frame %lu, injected %d, connected %d, local %lu, remote %lu)\r\n",
            (int)g_mode, (unsigned long)g_frame,
            g_testUiSyncInjected ? 1 : 0, g_connected ? 1 : 0,
            (unsigned long)g_localFrames[diagnosticSlot],
            (unsigned long)g_remoteFrames[diagnosticSlot]);
    }
    if (timedOut && g_testInputSyncEnabled && !g_testInputSyncVerified &&
        !g_testInputSyncFailureReported)
    {
        g_testInputSyncFailureReported = true;
        g_GameErrorContext.Log(
            "error : P1/P2 input lane synchronization timed out (mode %d, local frame %lu, network frame %lu, injected %d)\r\n",
            (int)g_mode, (unsigned long)g_testInputSyncLocalFrame,
            (unsigned long)g_frame, g_testInputSyncInjected ? 1 : 0);
    }
    if (timedOut && g_testProximityEnabled && !g_testProximityVerified &&
        !g_testProximityFailureReported)
    {
        g_testProximityFailureReported = true;
        g_GameErrorContext.Log(
            "error : close-player display fade test timed out (mode %d, frame %lu)\r\n",
            (int)g_mode, (unsigned long)g_frame);
        SetStatus("test close-player display fade timed out");
    }
    if (timedOut && g_testLifeTransferEnabled && !g_testLifeTransferVerified &&
        !g_testLifeTransferFailureReported)
    {
        g_testLifeTransferFailureReported = true;
        g_GameErrorContext.Log(
            "error : normal life transfer test timed out (mode %d, frame %lu, local frame %lu)\r\n",
            (int)g_mode, (unsigned long)g_frame,
            (unsigned long)g_testInputSyncLocalFrame);
        SetStatus("test normal life transfer timed out");
    }
    if (timedOut && g_testDamageEventsEnabled &&
        !g_testDamageSummaryLogged && !g_testDamageFailureReported)
    {
        g_testDamageSummaryLogged = true;
        if (g_testDamageHitCount[0] < 3 || g_testDamageHitCount[1] < 1 ||
            !g_testDamageSpiritSeen || !g_testDamageReviveSeen)
        {
            g_testDamageFailureReported = true;
            g_GameErrorContext.Log(
                "error : damage event test incomplete P1_hits %lu P2_hits %lu spirit %d revive %d frame %lu\r\n",
                (unsigned long)g_testDamageHitCount[0],
                (unsigned long)g_testDamageHitCount[1],
                g_testDamageSpiritSeen ? 1 : 0,
                g_testDamageReviveSeen ? 1 : 0,
                (unsigned long)g_frame);
            SetStatus("damage event test incomplete");
        }
        else
        {
            g_GameErrorContext.Log(
                "info : damage event test passed P1_hits %lu P2_hits %lu spirit 1 revive 1 frame %lu\r\n",
                (unsigned long)g_testDamageHitCount[0],
                (unsigned long)g_testDamageHitCount[1],
                (unsigned long)g_frame);
        }
    }
    return timedOut;
}

int Netplay::GetDelay()
{
    return g_delay;
}

Netplay::InGameControl Netplay::ConsumeSynchronizedControl()
{
    Netplay::InGameControl control = g_synchronizedControl;
    g_synchronizedControl = Netplay::INGAME_CONTROL_NONE;
    return control;
}

void Netplay::AdjustDelay(int amount)
{
    g_delay += amount;
    if (g_delay < 0)
    {
        g_delay = 0;
    }
    else if (g_delay > 10)
    {
        g_delay = 10;
    }
    char status[160];
    sprintf(status, "control: target delay %d", g_delay);
    SetStatus(status);
}

void Netplay::ToggleInsaneMode()
{
    g_insaneMode = !g_insaneMode;
    if (g_insaneMode)
    {
        g_GameManager.rank.minRank = 63;
        g_GameManager.rank.maxRank = 64;
        g_GameManager.rank.rank = 64;
        SetStatus("control: insane mode on");
    }
    else
    {
        g_GameManager.InitializeRank();
        SetStatus("control: insane mode off");
    }
}

bool Netplay::IsInsaneMode()
{
    return g_insaneMode;
}

const char *Netplay::GetStatusText()
{
    return g_status;
}

u32 Netplay::GetRoundTripMs()
{
    return g_lastRoundTripMs;
}

u16 Netplay::GetInitialRngSeed(u16 fallbackSeed)
{
    if (IsNetworked() && g_connected)
    {
        return g_initialRngSeed;
    }
    if (g_seedConfigured)
    {
        return g_configuredSeed;
    }
    return fallbackSeed;
}

bool Netplay::ShouldCaptureLocalInput()
{
    int slot;

    if (!IsNetworked())
    {
        return true;
    }
    if (g_resultDetached)
    {
        return true;
    }
    if (!g_connected)
    {
        return false;
    }
    slot = (int)(g_frame % INPUT_RING_SIZE);
    return g_localFrames[slot] != g_frame;
}

int CountActiveItemsOfType(i32 itemType)
{
    int count = 0;
    int i;
    for (i = 0; i < 1100; i++)
    {
        if (g_ItemManager.items[i].isInUse &&
            g_ItemManager.items[i].itemType == itemType)
        {
            count++;
        }
    }
    return count;
}

int FindResourceDropPairSeparation(Item *first)
{
    f32 bestSeparation = 0.0f;
    int i;

    if (!first || !first->isInUse)
    {
        return 0;
    }
    for (i = 0; i < 1100; i++)
    {
        Item *candidate = &g_ItemManager.items[i];
        f32 yDifference;
        f32 separation;

        if (candidate == first || !candidate->isInUse ||
            candidate->itemType != first->itemType)
        {
            continue;
        }
        yDifference = candidate->currentPosition.y - first->currentPosition.y;
        if (yDifference < 0.0f)
        {
            yDifference = -yDifference;
        }
        if (yDifference > 0.01f)
        {
            continue;
        }
        separation = candidate->currentPosition.x - first->currentPosition.x;
        if (separation < 0.0f)
        {
            separation = -separation;
        }
        if (separation > bestSeparation)
        {
            bestSeparation = separation;
        }
    }
    return (int)(bestSeparation + 0.5f);
}

void RunResourceDropTest()
{
    D3DXVECTOR3 position;
    Item *lifeFirst;
    Item *bombFirst;
    int lifeBefore;
    int bombBefore;
    int lifeAfter;
    int bombAfter;
    int lifeSeparation;
    int bombSeparation;
    int expectedCount;
    int expectedSpan;

    if (!g_testResourceDropsEnabled || g_testResourceDropsVerified ||
        !Netplay::IsMultiplayer() || !g_GameManager.notInMenu ||
        !g_GameManager.globals || !g_AnmManager)
    {
        return;
    }
    position.x = 192.0f;
    position.y = 64.0f;
    position.z = 0.0f;
    lifeBefore = CountActiveItemsOfType(ITEM_LIFE);
    bombBefore = CountActiveItemsOfType(ITEM_BOMB);
    lifeFirst = g_ItemManager.SpawnEnemyDrop(&position, ITEM_LIFE, 0);
    lifeSeparation = FindResourceDropPairSeparation(lifeFirst);
    position.x += 24.0f;
    bombFirst = g_ItemManager.SpawnEnemyDrop(&position, ITEM_BOMB, 0);
    bombSeparation = FindResourceDropPairSeparation(bombFirst);
    lifeAfter = CountActiveItemsOfType(ITEM_LIFE);
    bombAfter = CountActiveItemsOfType(ITEM_BOMB);
    expectedCount = GetActivePlayerCount();
    expectedSpan = 32 * (expectedCount - 1);
    g_testResourceDropsVerified = true;
    if (lifeAfter - lifeBefore == expectedCount &&
        bombAfter - bombBefore == expectedCount &&
        lifeSeparation >= expectedSpan && bombSeparation >= expectedSpan)
    {
        g_GameErrorContext.Log(
            "info : multiplayer enemy resource drop test passed players %d life_delta %d bomb_delta %d life_separation %d bomb_separation %d\r\n",
            expectedCount,
            lifeAfter - lifeBefore, bombAfter - bombBefore,
            lifeSeparation, bombSeparation);
        SetStatus(expectedCount == 3
                      ? "enemy life/bomb drop x3 separated test passed"
                      : "enemy life/bomb drop x2 separated test passed");
    }
    else
    {
        g_GameErrorContext.Log(
            "error : multiplayer enemy resource drop test failed players %d life_delta %d bomb_delta %d life_separation %d bomb_separation %d expected_span %d\r\n",
            expectedCount,
            lifeAfter - lifeBefore, bombAfter - bombBefore,
            lifeSeparation, bombSeparation, expectedSpan);
        SetStatus("enemy life/bomb separated drop test failed");
    }
}

void RunAdditionalPlayerFeatureTestSetup(u8 targetPlayerId, bool enabled,
                                         bool *setup)
{
    D3DXVECTOR3 itemPosition;
    Item *testItem;
    Player *targetPlayer;
    i32 retainedPowerCount;
    i32 convertedCherryCount;
    i32 activeCount;
    i32 expectedItemCount;
    i32 playerId;
    i32 i;

    if (!enabled || *setup || g_mode != Netplay::MODE_LOCAL ||
        !IsPlayerSlotActive(targetPlayerId) ||
        !g_GameManager.notInMenu || !g_GameManager.globals ||
        !g_AnmManager)
    {
        return;
    }
    targetPlayer = GetPlayerById(targetPlayerId);
    activeCount = GetActivePlayerCount();
    if (!targetPlayer || !targetPlayer->shooterData)
    {
        return;
    }

    // The feature setup is called from the synchronized input path, which
    // can start a few frames before the normal spawn invulnerability has
    // finished.  ActivateBorder() intentionally queues the border while a
    // player is spawning, so spawning the diagnostic item at that point would
    // never exercise the P2 auto-collect path.  Wait for both local players
    // to reach a gameplay state and run the same setup once they are ready.
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        Player *player;
        if (!IsPlayerSlotActive((u8)playerId))
        {
            continue;
        }
        player = GetPlayerById((u8)playerId);
        if (!player || !player->shooterData ||
            player->playerState != PLAYER_STATE_ALIVE)
        {
            return;
        }
    }

    if (targetPlayerId == 2 && activeCount == 3)
    {
        f32 bossMultiplier = GetMultiplayerBossDamageMultiplier();
        if (fabs(bossMultiplier - 2.0f / 3.0f) < 0.00001f)
        {
            g_GameErrorContext.Log(
                "info : three-player boss damage multiplier verified %.6f\r\n",
                bossMultiplier);
        }
        else
        {
            g_GameErrorContext.Log(
                "error : three-player boss damage multiplier failed %.6f\r\n",
                bossMultiplier);
        }
        VerifyThreePlayerLifeTransferSelectionRules();
        VerifyThreePlayerFinalResourceBonusClaims();
    }

    // P1 is deliberately closer to the item but cannot trigger POC. P2 is
    // full-power and above its own POC line, so the multiplayer selector must
    // lock this item to P2 instead of choosing the nearest ship first.
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        if (IsPlayerSlotActive((u8)playerId))
        {
            SetPlayerPower((u8)playerId,
                           playerId == targetPlayerId ? 128 : 0);
        }
    }
    g_Player.positionCenter.x = 192.0f;
    g_Player.positionCenter.y =
        g_GameManager.playerMovementAreaTopLeftPos.y +
        g_GameManager.playerMovementAreaSize.y - 32.0f;
    targetPlayer->positionCenter.x = 240.0f;
    targetPlayer->positionCenter.y = targetPlayer->shooterData->pocY - 24.0f;
    itemPosition = g_Player.positionCenter;
    itemPosition.x += 4.0f;
    itemPosition.z = 0.0f;
    retainedPowerCount = 0;
    convertedCherryCount = 0;
    expectedItemCount = activeCount * 2;
    for (i = 0; i < expectedItemCount; i++)
    {
        testItem = g_ItemManager.SpawnItem(
            &itemPosition, ITEM_POWER_SMALL, 0);
        if (testItem->itemType == ITEM_POWER_SMALL)
        {
            retainedPowerCount++;
        }
        else if (testItem->itemType == ITEM_CHERRY)
        {
            convertedCherryCount++;
        }
        itemPosition.x += 4.0f;
    }
    if (retainedPowerCount == expectedItemCount - 2 &&
        convertedCherryCount == 2)
    {
        g_GameErrorContext.Log(
            "info : P%d multiplayer Power round-robin verified retained %d cherry %d players %d\r\n",
            targetPlayerId + 1, retainedPowerCount, convertedCherryCount,
            activeCount);
    }
    else
    {
        g_GameErrorContext.Log(
            "error : P%d multiplayer Power round-robin failed retained %d cherry %d players %d\r\n",
            targetPlayerId + 1, retainedPowerCount, convertedCherryCount,
            activeCount);
    }

    // Exercise the shared border path with P1 closer to the item. Both players
    // must enter the same border state, while ItemManager splits the automatic
    // collection stream between them.
    SetPlayerPower(targetPlayerId, 0);
    g_GameManager.cherryPlus =
        g_GameManager.globals->cherryStart + GetSharedBorderThreshold();
    ActivateSharedBorder();
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        Player *player;
        if (!IsPlayerSlotActive((u8)playerId))
        {
            continue;
        }
        player = GetPlayerById((u8)playerId);
        if (!player || player->hasBorder != BORDER_ACTIVE)
        {
            g_GameErrorContext.Log(
                "error : shared border activation test failed P%d state %d\r\n",
                playerId + 1, player ? (int)player->hasBorder : -1);
        }
    }
    g_GameErrorContext.Log(
        "info : shared border activation checked for %d active players threshold %d\r\n",
        activeCount, GetSharedBorderThreshold());
    for (i = 0; i < activeCount; i++)
    {
        g_ItemManager.SpawnItem(&itemPosition, ITEM_POINT, 0);
        itemPosition.x += 4.0f;
    }
    *setup = true;
    g_GameErrorContext.Log(
        "info : P%d feature test setup shared border and split auto-collect\r\n",
        targetPlayerId + 1);
}

void RunAdditionalPlayerFeatureTests()
{
    RunAdditionalPlayerFeatureTestSetup(
        1, g_testP2FeaturesEnabled, &g_testP2FeaturesSetup);
    RunAdditionalPlayerFeatureTestSetup(
        2, g_testP3FeaturesEnabled, &g_testP3FeaturesSetup);
}

void LogDeterministicStateTrace(
    u32 simulationFrame,
    const u16 inputs[TH07_MULTI_MAX_PLAYERS])
{
    u32 logicalHash;

    if (!g_testHashTraceEnabled || !inputs ||
        !g_GameManager.notInMenu ||
        g_Supervisor.curState != TH07_SUPERVISOR_GAME_STATE)
    {
        return;
    }
    logicalHash = CalculateLogicalStateHash();
    if (!g_testHashTraceFile)
    {
        g_testHashTraceFile = fopen("determinism_hashes.txt", "wb");
        if (!g_testHashTraceFile)
        {
            g_GameErrorContext.Log(
                "error : could not create determinism_hashes.txt\r\n");
            g_testHashTraceEnabled = false;
            return;
        }
        setvbuf(g_testHashTraceFile, NULL, _IOFBF, 65536);
    }
    fprintf(g_testHashTraceFile,
        "trace : stage %d stage_frame %d sim_frame %lu rng %u input %u/%u/%u logical %08lx\r\n",
        g_GameManager.currentStage, g_GameManager.framesThisStage,
        (unsigned long)simulationFrame, (unsigned)g_Rng.seed,
        (unsigned)inputs[0], (unsigned)inputs[1], (unsigned)inputs[2],
        (unsigned long)logicalHash);
}

bool Netplay::SynchronizeInputs(
    const u16 localPlayers[TH07_MULTI_MAX_PLAYERS], u16 rngSeed,
    u16 synchronizedPlayers[TH07_MULTI_MAX_PLAYERS])
{
    bool resyncApplied;
    bool ignoreRngComparison;
    u32 currentFrame;
    u32 targetFrame;
    u32 testFrame;
    int currentSlot;
    int targetSlot;
    u16 frameInputs[TH07_MULTI_MAX_PLAYERS] = {0, 0, 0};
    u16 frameControls[TH07_MULTI_MAX_PLAYERS] = {0, 0, 0};
    u16 combinedInput;
    int playerId;
    u16 localPlayer1;
    u16 localPlayer2;
    u16 localPlayer3;
    u16 *player1;
    u16 *player2;
    u16 *player3;

    if (!localPlayers || !synchronizedPlayers)
    {
        return false;
    }
    localPlayer1 = localPlayers[0];
    localPlayer2 = localPlayers[1];
    localPlayer3 = localPlayers[2];
    player1 = &synchronizedPlayers[0];
    player2 = &synchronizedPlayers[1];
    player3 = &synchronizedPlayers[2];
    *player3 = 0;
    if (g_rollbackReplaying)
    {
        return SynchronizeRollbackFrame(synchronizedPlayers);
    }
    RefreshRollbackPredictionState();
    ApplyFinalMultiplayerScoreBonus();
    RunResourceDropTest();
    RunAdditionalPlayerFeatureTests();
    *player1 = 0;
    *player2 = 0;
    InjectTestReplaySelection();
    BlockMultiplayerReplay();
    localPlayer1 = CaptureSafeP1Input(localPlayer1);
    if (g_mode == MODE_LOCAL)
    {
        localPlayer2 = CaptureConfiguredLocalP2Input(localPlayer2);
    }
    ArmInputAfterRelease(&localPlayer1, &localPlayer2);
    ApplyAutomaticTestBomb(&localPlayer1, &localPlayer2);
    ApplyTestMenuInput(&localPlayer1);
    ApplyRollbackInputTest(&localPlayer1, &localPlayer2);
    ApplyTestRandomInput(&localPlayer1, &localPlayer2, &localPlayer3);
    ApplyTestEvasiveInput(&localPlayer1, &localPlayer2, &localPlayer3);
    ApplyDamageEventTestInput(&localPlayer1, &localPlayer2);
    if (g_testP2FeaturesEnabled && g_mode == MODE_LOCAL &&
        g_GameManager.notInMenu)
    {
        localPlayer2 |= TH_BUTTON_SHOOT | TH_BUTTON_FOCUS;
        if (g_testInputSyncLocalFrame % 180 == 120)
        {
            localPlayer2 |= TH_BUTTON_BOMB;
        }
        else
        {
            localPlayer2 &= (u16)~TH_BUTTON_BOMB;
        }
    }
    if (g_testP3FeaturesEnabled && g_mode == MODE_LOCAL &&
        g_GameManager.notInMenu)
    {
        localPlayer3 |= TH_BUTTON_SHOOT | TH_BUTTON_FOCUS;
        if (g_testInputSyncLocalFrame % 180 == 150)
        {
            localPlayer3 |= TH_BUTTON_BOMB;
        }
        else
        {
            localPlayer3 &= (u16)~TH_BUTTON_BOMB;
        }
    }
    if (g_testSeconds > 0 && g_GameManager.notInMenu &&
        g_GameManager.currentStage != g_lastLoggedTestStage)
    {
        g_lastLoggedTestStage = g_GameManager.currentStage;
        g_GameErrorContext.Log(
            "info : test stage %d gameplay active\r\n",
            g_GameManager.currentStage);
    }
    testFrame = IsNetworked() ? g_frame : g_testInputSyncLocalFrame;
    if (g_testInputSyncEnabled && !g_testInputSyncInjected &&
        testFrame >= 120)
    {
        // Ignore whatever key happened to have focus while the visible
        // smoke-test windows were being arranged. The assertion below is
        // about lane routing, not about the desktop's current direction key.
        localPlayer1 &= (u16)~TH_BUTTON_DIRECTION;
        localPlayer2 &= (u16)~TH_BUTTON_DIRECTION;
        if (g_mode == MODE_LOCAL)
        {
            localPlayer1 |= TH_BUTTON_UP;
            localPlayer2 |= TH_BUTTON_DOWN;
        }
        else if (g_mode == MODE_HOST)
        {
            localPlayer1 |= TH_BUTTON_UP;
        }
        else if (g_mode == MODE_GUEST)
        {
            localPlayer1 |= TH_BUTTON_DOWN;
        }
        g_testInputSyncInjected = true;
        g_GameErrorContext.Log(
            "info : test P1/P2 input lanes injected at frame %lu\r\n",
            (unsigned long)testFrame);
    }
    if (g_testUiSyncEnabled && !g_testUiSyncInjected &&
        g_mode != Netplay::MODE_SINGLE && g_frame >= 120)
    {
        // Inject different directional buttons on the two peers while the
        // pause menu is active. The shared-UI path below must present both
        // buttons to both processes, just like TH06's is_in_UI input path.
        localPlayer1 |= g_mode == Netplay::MODE_HOST ? TH_BUTTON_UP
                                                     : TH_BUTTON_DOWN;
        g_testUiSyncUiFrame = true;
        g_testUiSyncInjected = true;
        g_GameErrorContext.Log(
            "info : test UI input injected at frame %lu\r\n",
            (unsigned long)g_frame);
    }
    if (g_testLifeTransferEnabled && g_mode == Netplay::MODE_LOCAL &&
        !g_testLifeTransferVerified && g_testInputSyncLocalFrame >= 120)
    {
        if (g_testInputSyncLocalFrame == 120)
        {
            g_GameErrorContext.Log(
                "info : normal life transfer test input armed at local frame %lu\r\n",
                (unsigned long)g_testInputSyncLocalFrame);
        }
        // Hold P1 focus and suppress shooting long enough for the 90-frame
        // TH06 life-transfer timer to expire. Player.cpp also places the two
        // local ships together for this test only.
        localPlayer1 &= (u16)~(TH_BUTTON_SHOOT | TH_BUTTON_DIRECTION);
        localPlayer1 |= TH_BUTTON_FOCUS;
        localPlayer2 &= (u16)~(TH_BUTTON_SHOOT | TH_BUTTON_DIRECTION);
    }
    if (ApplyResultConnectionPolicy())
    {
        // Result/title UI continues to run locally while the network session
        // is paused. No gameplay frame is advanced during this interval.
        *player1 = localPlayer1 | localPlayer2;
        *player2 = 0;
        *player3 = 0;
        return true;
    }
    if (g_mode == MODE_SINGLE)
    {
        g_synchronizedControl = CaptureLocalControl();
        ApplySynchronizedControl(ConsumeSynchronizedControl());
        *player1 = (IsSharedUiFrame() ||
                    HasSharedUiInput(localPlayer1, localPlayer2))
            ? localPlayer1 | localPlayer2
            : localPlayer1;
        *player2 = 0;
        *player3 = 0;
        g_testInputSyncLocalFrame++;
        return true;
    }
    if (g_mode == MODE_LOCAL)
    {
        g_synchronizedControl = CaptureLocalControl();
        ApplySynchronizedControl(ConsumeSynchronizedControl());
        if (IsSharedUiFrame() ||
            HasSharedUiInput(localPlayer1 | localPlayer3, localPlayer2))
        {
            *player1 = localPlayer1 | localPlayer2 | localPlayer3;
            *player2 = 0;
            *player3 = 0;
        }
        else
        {
            *player1 = localPlayer1;
            *player2 = localPlayer2;
            *player3 = Netplay::GetPlayerCount() >= 3 ? localPlayer3 : 0;
        }
        VerifyTestInputLanes(*player1, *player2, testFrame);
        LogDeterministicStateTrace(testFrame, synchronizedPlayers);
        g_testInputSyncLocalFrame++;
        return true;
    }
    if (g_reconnectTestPending && g_frame >= 120)
    {
        g_reconnectTestPending = false;
        if (!ReconnectToPeer())
        {
            return false;
        }
    }
    if (ConsumeReconnectRequest())
    {
        if (!ReconnectToPeer())
        {
            return false;
        }
    }
    if (!g_connected)
    {
        return false;
    }

    resyncApplied = false;
    if (g_rngMismatch && g_resyncFrame != INVALID_FRAME &&
        g_frame >= g_resyncFrame)
    {
        ApplyRngResync();
        rngSeed = g_Rng.seed;
        resyncApplied = true;
    }
    SendPendingResync();

    currentFrame = g_frame;
    PollPackets();
    UpdateHostPeerLifecycles();
    PrepareHostSyntheticInputs(currentFrame);
    if (!g_connected || g_connectionFailed)
    {
        return false;
    }
    currentSlot = (int)(currentFrame % INPUT_RING_SIZE);
    if (g_localFrames[currentSlot] != currentFrame)
    {
        g_localFrames[currentSlot] = currentFrame;
        g_localInputs[currentSlot] =
            ShouldForceNeutralPlayerInput(
                currentFrame, g_localPlayerSlot)
            ? 0 : localPlayer1;
        // No automated run carries a human press on a guest: the bot
        // drivers overwrite localPlayer1 after capture. Report what the
        // guest captured, what it stored, and which screen it was on, so
        // "the guest's menus do not respond" can be located rather than
        // guessed at.
        if (localPlayer1 != 0 && g_inputTraceCount < 40)
        {
            g_inputTraceCount++;
            NetplayTraceFile(
                "info : local input frame %lu slot %d captured 0x%04x stored 0x%04x armed %d shared %d notInMenu %d state %d pause %d\r\n",
                (unsigned long)currentFrame, g_localPlayerSlot,
                (unsigned)localPlayer1,
                (unsigned)g_localInputs[currentSlot],
                g_inputArmed ? 1 : 0, IsSharedUiFrame() ? 1 : 0,
                g_GameManager.notInMenu ? 1 : 0,
                (int)g_Supervisor.curState,
                g_GameManager.isInPauseMenu ? 1 : 0);
        }
        g_localRollbackGameplay[currentSlot] =
            IsRollbackGameplayFrame() ? 1 : 0;
        g_localRng[currentSlot] = rngSeed;
        g_localControls[currentSlot] = g_inputArmed
            ? (u16)CaptureLocalControl()
            : (u16)Netplay::INGAME_CONTROL_NONE;
        if (g_mode == MODE_HOST &&
            GetScheduledLifecycleControl(currentFrame) !=
                Netplay::INGAME_CONTROL_NONE)
        {
            g_localControls[currentSlot] =
                (u16)GetScheduledLifecycleControl(currentFrame);
        }
        g_localStateHash[currentSlot] = CalculateLogicalStateHash();
        RefreshDetailedStateHashesForSlot(currentSlot, currentFrame);
        LogSpellLifecycle(currentFrame);
        LogBossLifeTrace(currentFrame);
        LogPlayerLifecycleTrace(currentFrame);
        LogPlayerBodyTrace(currentFrame);
        if (g_testRngMismatchEnabled && g_mode == MODE_HOST &&
            !g_testRngMismatchInjected && currentFrame == 120)
        {
            g_localRng[currentSlot] ^= 1;
            g_testRngMismatchInjected = true;
        }
        if (g_testStateMismatchEnabled && g_mode == MODE_HOST &&
            !g_testStateMismatchInjected && currentFrame == 120)
        {
            g_localStateHash[currentSlot] ^= 0x13579bdf;
            g_testStateMismatchInjected = true;
        }
        SendInputs(currentFrame);
        g_lastInputSendTick = GetTickCount();
    }

    if (IsRollbackPredictionFrame() && currentFrame >= (u32)g_delay &&
        IsRollbackSnapshotFrame(currentFrame))
    {
        // This is the state immediately before the simulation step selected
        // by currentFrame. If the remote sample was predicted, this exact
        // point is the rollback anchor when the real packet arrives.
        SaveRollbackSnapshot(currentFrame);
    }

    if (currentFrame < (u32)g_delay)
    {
        // The remote input for these initial frames cannot be available yet.
        // Advancing with the local P1 input here makes the two peers simulate
        // different states during the warm-up period, and then applies that
        // same local input again once the fixed delay has elapsed. Keep the
        // warm-up deterministic; the captured inputs are consumed below after
        // their delay window has been filled.
        *player1 = 0;
        *player2 = 0;
        *player3 = 0;
        PollPackets();
        g_waitingForRemoteInput = false;
        g_frame++;
        return true;
    }

    targetFrame = currentFrame - g_delay;
    targetSlot = (int)(targetFrame % INPUT_RING_SIZE);
    if (!TryGetRemoteInput(targetFrame, IsRollbackPredictionFrame()))
    {
        return false;
    }
    if (targetFrame == 0 && !g_startupFrameBarrierLogged)
    {
        g_startupFrameBarrierLogged = true;
        g_GameErrorContext.Log(
            "info : synchronized startup barrier passed with exact %d-player frame 0 input\r\n",
            g_playerCount);
    }
    CompareConfirmedRollbackRng(currentFrame);
    CompareConfirmedDetailedState(currentFrame);

    if (g_rollbackEnabled && !g_rollbackPredictionActive &&
        IsRollbackGameplayFrame() && IsRollbackSnapshotFrame(currentFrame) &&
        g_localFrames[targetSlot] == targetFrame &&
        g_localRollbackGameplay[targetSlot])
    {
        bool allRemoteGameplayReady = true;
        for (playerId = 0; playerId < g_playerCount; playerId++)
        {
            if (!IsExpectedRemotePlayerId(playerId))
            {
                continue;
            }
            if (g_remoteFramesByPlayer[playerId][targetSlot] != targetFrame ||
                g_predictedRemoteFramesByPlayer[playerId][targetSlot] ==
                    targetFrame ||
                !g_remoteRollbackGameplayByPlayer[playerId][targetSlot])
            {
                allRemoteGameplayReady = false;
                break;
            }
        }
        if (allRemoteGameplayReady)
        {
        RollbackSnapshot *activationSnapshot;
        SaveRollbackSnapshot(currentFrame);
        activationSnapshot =
            &g_rollbackSnapshots[GetRollbackSnapshotIndex(currentFrame)];
        if (activationSnapshot->simulationFrame == currentFrame)
        {
            g_rollbackPredictionActive = true;
            g_GameErrorContext.Log(
                "info : rollback prediction synchronized at frame %lu\r\n",
                (unsigned long)currentFrame);
        }
        }
    }

    combinedInput = 0;
    for (playerId = 0; playerId < g_playerCount; playerId++)
    {
        if (!IsInputRoutingSlotActive(playerId))
        {
            frameInputs[playerId] = 0;
            frameControls[playerId] = Netplay::INGAME_CONTROL_NONE;
        }
        else if (playerId == g_localPlayerSlot)
        {
            frameInputs[playerId] = g_localInputs[targetSlot];
            frameControls[playerId] = g_localControls[targetSlot];
        }
        else
        {
            frameInputs[playerId] =
                g_remoteInputsByPlayer[playerId][targetSlot];
            frameControls[playerId] =
                g_remoteControlsByPlayer[playerId][targetSlot];
        }
    }
    ApplyLifecycleInputPolicy(targetFrame, frameInputs, frameControls);
    for (playerId = 0; playerId < g_playerCount; playerId++)
    {
        combinedInput |= frameInputs[playerId];
    }
    if (IsRollbackGameplayFrame() &&
        (combinedInput & TH_BUTTON_BOMB) != 0)
    {
        if (g_testSeconds > 0 && g_synchronizedBombPulseCount < 24)
        {
            g_GameErrorContext.Log(
                "info : rollback bomb edge frame %lu P1 %d P2 %d P3 %d\r\n",
                (unsigned long)targetFrame,
                (frameInputs[0] & TH_BUTTON_BOMB) != 0 ? 1 : 0,
                (frameInputs[1] & TH_BUTTON_BOMB) != 0 ? 1 : 0,
                (frameInputs[2] & TH_BUTTON_BOMB) != 0 ? 1 : 0);
        }
        g_synchronizedBombPulseCount++;
    }

    if (IsSharedUiFrame() || (combinedInput & SHARED_UI_INPUTS) != 0)
    {
        *player1 = combinedInput;
        if (combinedInput != 0 && g_sharedLaneTraceCount < 40)
        {
            g_sharedLaneTraceCount++;
            NetplayTraceFile(
                "info : shared lane frame %lu slot %d p0 0x%04x p1 0x%04x p2 0x%04x combined 0x%04x\r\n",
                (unsigned long)targetFrame, g_localPlayerSlot,
                (unsigned)frameInputs[0], (unsigned)frameInputs[1],
                (unsigned)frameInputs[2], (unsigned)combinedInput);
        }
        *player2 = 0;
        *player3 = 0;
    }
    else
    {
        *player1 = frameInputs[0];
        *player2 = frameInputs[1];
        *player3 = frameInputs[2];
    }
    VerifyTestInputLanes(*player1, *player2, targetFrame);
    if (g_testUiSyncEnabled && g_testUiSyncInjected &&
        !g_testUiSyncVerified && targetFrame >= 120)
    {
        if ((*player1 & (TH_BUTTON_UP | TH_BUTTON_DOWN)) ==
            (TH_BUTTON_UP | TH_BUTTON_DOWN))
        {
            g_testUiSyncVerified = true;
            g_GameErrorContext.Log(
                "info : test UI input verified at frame %lu\r\n",
                (unsigned long)targetFrame);
            SetStatus("test UI input sync passed");
        }
        else if (targetFrame >= 124 && !g_testUiSyncFailureReported)
        {
            g_testUiSyncFailureReported = true;
            g_GameErrorContext.Log(
                "error : shared UI input synchronization failed\r\n");
            SetStatus("test UI input sync failed");
        }
    }
    ignoreRngComparison = resyncApplied ||
        (g_resyncIgnoreUntilFrame != INVALID_FRAME &&
         targetFrame < g_resyncIgnoreUntilFrame);
    if (!g_rollbackEnabled && !g_rngMismatch && !ignoreRngComparison)
    {
        for (playerId = 0; playerId < g_playerCount; playerId++)
        {
            if (!IsExpectedRemotePlayerId(playerId) ||
                (g_mode == MODE_GUEST && playerId != 0) ||
                g_localStateHash[targetSlot] == 0 ||
                g_remoteStateHashByPlayer[playerId][targetSlot] == 0)
            {
                continue;
            }
            if (g_localRng[targetSlot] !=
                g_remoteRngByPlayer[playerId][targetSlot])
            {
                MarkRngMismatch(
                    targetFrame, g_localRng[targetSlot],
                    g_remoteRngByPlayer[playerId][targetSlot], true,
                    INVALID_FRAME);
                break;
            }
        }
    }
    if (g_testStateMismatchEnabled && !g_rollbackEnabled &&
        !g_rngMismatch && !ignoreRngComparison &&
        g_localStateHash[targetSlot] != 0 &&
        g_remoteStateHashByPlayer[GetPrimaryRemotePlayerId()][targetSlot] != 0 &&
        g_localStateHash[targetSlot] !=
            g_remoteStateHashByPlayer[GetPrimaryRemotePlayerId()][targetSlot])
    {
        MarkRngMismatch(targetFrame, g_localRng[targetSlot],
                        g_remoteRngByPlayer[GetPrimaryRemotePlayerId()]
                                           [targetSlot],
                        true, INVALID_FRAME);
        SetStatus("logical state mismatch; resync scheduled");
    }
    g_synchronizedControl = Netplay::INGAME_CONTROL_NONE;
    for (playerId = 0; playerId < g_playerCount; playerId++)
    {
        Netplay::InGameControl control =
            NormalizeControl(frameControls[playerId]);
        if (control != Netplay::INGAME_CONTROL_NONE)
        {
            g_synchronizedControl = control;
            break;
        }
    }
    ApplySynchronizedControl(ConsumeSynchronizedControl());
    if (IsRollbackGameplayFrame())
    {
        if (g_rollbackGameplayStartedTick == 0)
        {
            g_rollbackGameplayStartedTick = GetTickCount();
        }
        g_rollbackGameplayFrames++;
    }
    g_frame++;
    return true;
}

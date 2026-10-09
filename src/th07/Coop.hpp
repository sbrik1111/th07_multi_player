#pragma once

#include "GameManager.hpp"
#include "ZunMath.hpp"
#include "inttypes.hpp"

struct Player;
struct Enemy;
struct Item;
struct AnmVm;

Player *SeatPlayer(i32 seat);

i32 PlayerOnField(Player *player);

Player *AimTarget(Float3 *pos);

i32 PlayerCanBeHit(Player *player);

// 0 none, 1 grazed, 2 a bomb took it (*seat: who).
i32 CoopCheckGraze(Float3 *center, Float3 *size, i32 *seat);
// 2 a bomb took it first (*seat: whose), else 1 when it hit anyone.
i32 CoopKillbox(Float3 *center, Float3 *size, i32 *seat);
void CoopLaserHitbox(Float3 *center, Float3 *size, Float3 *origin, f32 rotation, i32 canGraze);

i32 CoopScaleDamage(Enemy *enemy, i32 damage);

i32 CoopItemCopies(i32 type, i32 boss);
f32 CoopItemCopyAngle(i32 k, i32 count);
void CoopSpawnDrop(Float3 *pos, i32 type, i32 state, i32 boss);
i32 CoopAllSeatsFullPower();

i32 PowerDropSeat(i32 itemIndex);

Player *CoopBorderOwner();
i32 CoopBorderActive();
// Call after every player is registered.
void CoopRestoreBorder();
void CoopEndBorderForDialogue();

i32 AnyBombInUse();
i32 AnyPlayerBusy();

Player *ItemCollector(Item *item);
i32 PlayerAutoCollects(Player *player);

// True when the player became a ghost (else th07's continue menu follows).
i32 CoopBecomeGhost(Player *player);
void CoopStepGhost(Player *player);
void CoopReviveGhost(Player *player);
i32 IsGhost(Player *player);
i32 CoopTestGhost(Player *player);

void CoopUpdateTransfers(Player *player);
void CoopDrawTransferPrompts();
void CoopDrawStageNames();
extern i32 g_CoopShowStageNames;

void CoopExtendFromPoints();
void CoopContinue();
void CoopKeepGhost(Player *player, i32 wasGhost);

extern i32 g_CoopTestStartStage;
extern i32 g_CoopTestStageClearFrame;
extern i32 g_CoopTestStageClearLast;

void CoopLog(const char *format, ...);
extern void (*g_CoopLogSink)(const char *line);

// Nonzero while the frame may still be undone by a rollback.
extern i32 g_FrameMayRollBack;

// The seat whose player is ticking (0 otherwise).
extern i32 g_CoopActiveSeat;

void CoopRefreshFadeSet();
u32 CoopPlayerAlpha(Player *player);
extern i32 g_CoopFadeActive;
extern i32 g_CoopViewSeat;

const char *CoopPlayerName(i32 seat);
extern const char *(*g_CoopNameSource)(i32 seat);

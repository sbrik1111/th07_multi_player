#pragma once

#include <d3d8.h>

#include "ResultScreen.hpp"
#include "Rng.hpp"
#include "Supervisor.hpp"
#include "ZunResult.hpp"
#include "inttypes.hpp"

struct ZunGlobals
{
    u32 guiScore;
    u32 score;
    u32 guiScoreDifference;
    u32 highScore;
    u8 highScoreNumContinues;
    // pad 3
    i32 grazeInStage;
    i32 grazeInTotal;
    i32 spellCardsCaptured;
    u8 numRetries;
    // pad 3
    i32 pointItemsCollectedThisStage;
    i32 pointItemsCollectedForExtend;
    i32 extendsFromPointItems;
    i32 nextNeededPointItemsForExtend;
    i32 rng1[7];
    // Per seat in GameManager::seats.
    f32 unusedDeaths;
    f32 rngFloat1[2];
    f32 unusedLivesRemaining;
    f32 rngFloat2[2];
    f32 unusedBombsRemaining;
    f32 unusedBombsUsed;
    f32 rngFloat3[3];
    f32 unusedCurrentPower;
    f32 rngFloat4[2];
    i32 cherryStart;
    i32 rng2[8];
    u32 curCsum;
    i32 csumAsSum;
    i32 csumData[5];
};
C_ASSERT(sizeof(ZunGlobals) == 0xc8);

struct Rank
{
    i32 rank;
    i32 maxRank;
    i32 minRank;
};

// The score, graze, point items, cherry, rank and continues are shared.
#define MAX_PLAYERS 4

struct SeatStock
{
    f32 lives;
    f32 bombs;
    f32 power;
    f32 bombsUsed;
    f32 deaths;
    i32 cherryPlus;
    i8 powerItemCountForScore;
    u8 character;
    u8 shotType;
    u8 shotTypeAndCharacter;
};

struct GameManager
{
    GameManager()
    {
        memset(this, 0, sizeof(GameManager));
        this->arcadeRegionTopLeftPos.x = 32.0f;
        this->arcadeRegionTopLeftPos.y = 16.0f;
        this->arcadeRegionSize.x = 384.0f;
        this->arcadeRegionSize.y = 448.0f;
        this->demoIdx = 2;
        this->phantasmUnlocked = 1;
    }

#pragma var_order(local_10, local_c)
    // FUNCTION: TH07 0x004012b0
    void RegenerateGameIntegrityCsum()
    {
        this->globals->rng1[2] = g_Rng.GetRandomU32InRange(100000) + 6543;
        this->globals->rng2[3] = g_Rng.GetRandomU32InRange(100000) + 6543;
        this->globals->curCsum = this->globals->rng1[2];

        this->globals->csumAsSum = ComputeGameIntegrityCsum();
        this->csumFloat = (f32)(this->globals->csumAsSum + this->globals->rng2[3]);
    }

    // FUNCTION: TH07 0x00404fe0
    i32 CheckGameIntegrity()
    {
#ifdef NON_MATCHING
        return 0;
#else
        // This is incredibly ugly but its the only way to get a match on this function
        return (this->globals->curCsum ==
                this->globals->rng1[2] + this->globals->csumData[2] *
                                             ((i32) & this->globals->curCsum - (i32)this->globals->rng1 +
                                                          sizeof(this->globals->csumData) + sizeof(GameConfiguration) * 2)) &&
                       (this->globals->csumAsSum + this->globals->rng2[3] ==
                        (i32)this->csumFloat)
                   ? 0
                   : 1;
#endif
    }

    // FUNCTION: TH07 0x0043b5c0
    void RerollRng()
    {
        this->globals->rng1[0] = g_Rng.GetRandomU32InRange(100000) + 6543;
        this->globals->rng1[1] = g_Rng.GetRandomU32InRange(100000) + 6543;
        this->globals->rng1[2] = g_Rng.GetRandomU32InRange(100000) + 6543;
        this->globals->rng1[3] = g_Rng.GetRandomU32InRange(100000) + 6543;
        this->globals->rng1[4] = g_Rng.GetRandomU32InRange(100000) + 6543;
        this->globals->rngFloat3[0] = g_Rng.GetRandomFloatInRange(100000.0f) + 6543.0f;
        this->globals->rngFloat3[1] = g_Rng.GetRandomFloatInRange(100000.0f) + 6543.0f;
        this->globals->rngFloat3[2] = g_Rng.GetRandomFloatInRange(100000.0f) + 6543.0f;
    }

    f32 &Lives(i32 seat)
    {
        return this->seats[seat].lives;
    }
    f32 &Bombs(i32 seat)
    {
        return this->seats[seat].bombs;
    }
    f32 &Power(i32 seat)
    {
        return this->seats[seat].power;
    }
    f32 &BombsUsed(i32 seat)
    {
        return this->seats[seat].bombsUsed;
    }
    f32 &Deaths(i32 seat)
    {
        return this->seats[seat].deaths;
    }
    i32 &CherryPlus(i32 seat)
    {
        return this->seats[seat].cherryPlus;
    }
    i8 &PowerItemCount(i32 seat)
    {
        return this->seats[seat].powerItemCountForScore;
    }
    u8 &Character(i32 seat)
    {
        return this->seats[seat].character;
    }
    u8 &ShotType(i32 seat)
    {
        return this->seats[seat].shotType;
    }
    u8 &ShotTypeAndCharacter(i32 seat)
    {
        return this->seats[seat].shotTypeAndCharacter;
    }
    void AddSeatStock(f32 &value, i32 amount)
    {
        if (CheckGameIntegrity())
        {
            NUKE_SUPERVISOR();
        }
        value += (f32)amount;
        RegenerateGameIntegrityCsum();
    }
    void SetSeatBombs(i32 seat, i32 amount)
    {
        this->Bombs(seat) = (f32)amount;
        this->globals->curCsum = this->globals->rng1[2];
        this->globals->csumAsSum = ComputeGameIntegrityCsum();
        this->csumFloat = (f32)(this->globals->csumAsSum + this->globals->rng2[3]);
    }
    i32 PlayerCount()
    {
        return this->playerCount;
    }

    void SetReplay(i32 replay)
    {
        this->replay = replay;
    }

    void AddScore(i32 score)
    {
        this->globals->score += score / 10;
    }

    i32 IsCherryAtMax()
    {
        return this->cherry >= this->cherryMax;
    }

    // FUNCTION: TH07 0x0042d657
    void ResetRegionsPos()
    {
        this->arcadeRegionTopLeftPos.x = 32.0f;
        this->arcadeRegionTopLeftPos.y = 16.0f;
        this->arcadeRegionSize.x = 384.0f;
        this->arcadeRegionSize.y = 448.0f;
        this->playerMovementAreaTopLeftPos.x = 8.0f;
        this->playerMovementAreaTopLeftPos.y = 16.0f;
        this->playerMovementAreaSize.x = 368.0f;
        this->playerMovementAreaSize.y = 416.0f;
    }

    static ZunResult RegisterChain();
    static void CutChain();

    static ZunResult AddedCallback(GameManager *arg);
    static ZunResult DeletedCallback(GameManager *arg);
    static u32 OnUpdate(GameManager *arg);
    static u32 OnDraw(GameManager *arg);

    static i32 ByteCsumAccumulator(u8 *param_1, i32 param_2);
    i32 ComputeGameIntegrityCsum();

    i32 HasReachedMaxClears(i32 shotType);
    i32 HasReachedMaxClearsAllShotTypes();
    i32 HasUnlockedPhantom(i32 shotType);
    i32 HasUnlockedPhantomAndMaxClears();

    void AddCherryPlus(i32 amount, i32 seat);
    void AddCherryGauge(i32 amount, i32 seat);
    void AddCherry(i32 amount);
    void ExtendSeat(i32 seat);

    void DecreaseSubrank(i32 amount);
    void IncreaseCherry(i32 amount);
    void IncreaseCherryMax(i32 amount);
    void IncreaseSubrank(i32 amount);
    void InitializeRank();
    static void InitializeRngAndCsum();
    i32 IsInBounds(f32 x, f32 y, f32 widthPx, f32 heightPx);

    static void DrawLoadingSprite();

    void *tmpBuffer;
    GameConfiguration *defaultCfg;
    ZunGlobals *globals;
    i8 isTimeStopped;
    i8 slowModeSlowActive;
    // pad 2
    i32 difficulty;
    u32 difficultyMask;
    struct Catk catk[SPELLCARD_COUNT];
    struct Catk catkAgain[SPELLCARD_COUNT];
    struct Clrd clrd[6];
    struct Pscr pscr[6][6][4];
    struct Plst plst;
    i32 isPaused;
    union {
        u32 flags;
        struct
        {
            u32 practice : 1;
            u32 demo : 1;
            u32 notInMenu : 1;
            u32 replay : 1;
            u32 finished : 1;
        };
    };
    u8 isInPauseMenu;
    u8 isInRetryMenu;
    u8 demoIdx;
    u8 replayStage;
    i32 demoFrames;
    char replayFilename[512];
    u16 stageRngSeed;
    // pad 2
    i32 framesThisStage;
    i32 currentStage;
    i32 unused_95f0;
    Float2 arcadeRegionTopLeftPos;
    Float2 arcadeRegionSize;
    Float2 playerMovementAreaTopLeftPos;
    Float2 playerMovementAreaSize;
    f32 csumFloat;
    i32 cherryMax;
    i32 cherry;
    i32 phantasmUnlocked;
    i32 playTimeAll; // ZUN name: PlayTimeAll
    u32 bulletLagTime;
    i32 maxRetries;
    Rank rank;
    i32 subrank;
    SeatStock seats[MAX_PLAYERS];
    i32 playerCount;
    u32 sessionSeed;
    u32 gamesStarted;
};
extern GameManager g_GameManager;

inline i32 PlayerCount()
{
    return g_GameManager.PlayerCount();
}

#include "ReplaySession.h"
#include "GameManager.hpp"
#include "Player.hpp"
#include "Coop.hpp"
#include "Supervisor.hpp"
#include <string.h>

namespace th07 { namespace replay {

StageState CaptureStage()
{
    const GameManager& g = g_GameManager;
    StageState s = {};
    s.stage = g.currentStage + 1;
    if (s.stage == 1 && g_CoopTestStartStage > 1 && !g.practice && g.difficulty < 4) s.stage = g_CoopTestStartStage;
    s.scene = g_Supervisor.curState;
    s.difficulty = g.difficulty;
    s.flags = g.flags;
    s.gamesStarted = g.gamesStarted;
    s.rngSeed = g_Rng.seed;
    s.rngBackup = g_Rng.seedBackup;
    s.rngCount = g_Rng.generationCount;
    static_assert(sizeof(s.globals) == sizeof(ZunGlobals), "replay globals");
    static_assert(sizeof(s.gameConfig) == sizeof(GameConfiguration), "replay configuration");
    if (g.globals) memcpy(s.globals, g.globals, sizeof(s.globals));
    if (g.defaultCfg) memcpy(s.gameConfig, g.defaultCfg, sizeof(s.gameConfig));
    for (int seat = 0; seat < MAX_PLAYERS; ++seat) {
        SeatState& stock = s.seats[seat];
        stock.lives = g_GameManager.Lives(seat);
        stock.bombs = g_GameManager.Bombs(seat);
        stock.power = g_GameManager.Power(seat);
        stock.bombsUsed = g_GameManager.BombsUsed(seat);
        stock.deaths = g_GameManager.Deaths(seat);
        stock.powerItems = g_GameManager.PowerItemCount(seat);
        stock.character = g_GameManager.Character(seat);
        stock.shot = g_GameManager.ShotType(seat);
        if (g_Players[seat].playerState == PLAYER_STATE_GHOST) s.ghosts |= 1u << seat;
        s.input[seat] = g_SeatGameInput[seat];
        s.lastInput[seat] = g_SeatLastGameInput[seat];
    }
    s.cherry = g.cherry;
    s.cherryMax = g.cherryMax;
    s.cherryPlus = g.cherryPlus;
    memcpy(s.rank, &g.rank, sizeof(s.rank));
    s.subrank = g.subrank;
    s.playTime = g.playTimeAll;
    s.maxRetries = g.maxRetries;
    s.paused = g.isPaused;
    s.timeStopped = g.isTimeStopped;
    s.slowActive = g.slowModeSlowActive;
    s.bulletLagTime = g.bulletLagTime;
    memcpy(s.regions, &g.arcadeRegionTopLeftPos, sizeof(s.regions));
    return s;
}

void RestoreStage(const StageState& s)
{
    GameManager& g = g_GameManager;
    g.currentStage = s.stage - 1;
    g_Supervisor.curState = s.scene;
    g.difficulty = s.difficulty;
    g.flags = s.flags;
    g.gamesStarted = s.gamesStarted;
    g_Rng.seed = (u16)s.rngSeed;
    g_Rng.seedBackup = (u16)s.rngBackup;
    g_Rng.generationCount = s.rngCount;
    if (!g.globals) g.globals = new ZunGlobals();
    if (!g.defaultCfg) g.defaultCfg = new GameConfiguration();
    memcpy(g.globals, s.globals, sizeof(s.globals));
    memcpy(g.defaultCfg, s.gameConfig, sizeof(s.gameConfig));
    for (int seat = 0; seat < MAX_PLAYERS; ++seat) {
        const SeatState& stock = s.seats[seat];
        g.Lives(seat) = stock.lives;
        g.Bombs(seat) = stock.bombs;
        g.Power(seat) = stock.power;
        g.BombsUsed(seat) = stock.bombsUsed;
        g.Deaths(seat) = stock.deaths;
        g.PowerItemCount(seat) = (i8)stock.powerItems;
        g.Character(seat) = (u8)stock.character;
        g.ShotType(seat) = (u8)stock.shot;
        g_Players[seat].playerState = s.ghosts & (1u << seat) ? PLAYER_STATE_GHOST : PLAYER_STATE_SPAWNING;
        g_SeatGameInput[seat] = s.input[seat];
        g_SeatLastGameInput[seat] = s.lastInput[seat];
    }
    // th07's (the dialogue's) buttons are seat 0's.
    g_CurFrameGameInput = s.input[0];
    g_LastFrameGameInput = s.lastInput[0];
    g.cherry = s.cherry;
    g.cherryMax = s.cherryMax;
    g.cherryPlus = s.cherryPlus;
    memcpy(&g.rank, s.rank, sizeof(s.rank));
    g.subrank = s.subrank;
    g.playTimeAll = s.playTime;
    g.maxRetries = s.maxRetries;
    g.isPaused = s.paused;
    g.isTimeStopped = (i8)s.timeStopped;
    g.slowModeSlowActive = (i8)s.slowActive;
    g.bulletLagTime = s.bulletLagTime;
    memcpy(&g.arcadeRegionTopLeftPos, s.regions, sizeof(s.regions));
}

} }

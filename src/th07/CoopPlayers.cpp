
#include "AsciiManager.hpp"
#include "Controller.hpp"
#include "Coop.hpp"
#include "Gui.hpp"
#include "Player.hpp"
#include "SoundPlayer.hpp"
#include <math.h>
#include <stdio.h>
#include <string.h>

extern i32 g_CoopTestGhostFrame;

static f32 GhostColumn(i32 seat)
{
    return g_GameManager.arcadeRegionSize.x / 2.0f + ((f32)seat - (f32)(PlayerCount() - 1) * 0.5f) * 48.0f;
}

static void SetPlayerPosition(Player *player, f32 x, f32 y)
{
    player->positionCenter.x = x;
    player->positionCenter.y = y;
    player->hitboxTopLeft = player->positionCenter - player->hitboxSize;
    player->hitboxBottomRight = player->positionCenter + player->hitboxSize;
    player->grazeTopLeft = player->positionCenter - player->grazeSize;
    player->grazeBottomRight = player->positionCenter + player->grazeSize;
    player->grabItemTopLeft = player->positionCenter - player->grabItemSize;
    player->grabItemBottomRight = player->positionCenter + player->grabItemSize;
    player->optionsPosition[0] = player->positionCenter;
    player->optionsPosition[1] = player->positionCenter;
}

i32 CoopBecomeGhost(Player *player)
{
    i32 others = 0;
    for (i32 seat = 0; seat < PlayerCount(); seat++)
    {
        if (seat != player->seat && !IsGhost(&g_Players[seat]))
        {
            others++;
        }
    }
    if (others == 0)
    {
        return 0;
    }
    // -1: a revival's life is the last one.
    g_GameManager.Lives(player->seat) = -1.0f;
    player->playerState = PLAYER_STATE_GHOST;
    player->invulnerabilityTimer = 0;
    player->playerSprite.scale.x = 1.0f;
    player->playerSprite.scale.y = 1.0f;
    player->playerSprite.color.color = 0xffffffff;
    player->playerSprite.blendMode = 0;
    g_AnmManager->SetAnmIdxAndExecuteScript(&player->playerSprite, 1024 + player->AnmShift());
    SetPlayerPosition(player, GhostColumn(player->seat), 400.0f);
    CoopLog("GHOST player=%d", player->seat);
    return 1;
}

void CoopStepGhost(Player *player)
{
    // The player's own timer, so a rollback restores it.
    i32 t = player->invulnerabilityTimer.GetCurrent();
    if (t < 0)
    {
        t = -t;
    }
    player->invulnerabilityTimer++;
    f32 phase = (f32)(t % 120) * (ZUN_PI * 2.0f / 120.0f);
    SetPlayerPosition(player, GhostColumn(player->seat), 400.0f + sinf(phase) * 6.0f);
}

i32 CoopTestGhost(Player *player)
{
    if (PlayerCount() < 2 || g_CoopTestGhostFrame < 0 || player->seat != PlayerCount() - 1 ||
        g_GameManager.framesThisStage != g_CoopTestGhostFrame || IsGhost(player))
    {
        return 0;
    }
    return CoopBecomeGhost(player);
}

void CoopReviveGhost(Player *player)
{
    i32 seat = player->seat;
    g_GameManager.Lives(seat) += 1.0f;
    g_GameManager.Power(seat) = 128.0f;
    g_GameManager.Bombs(seat) = player->shooterData->initialBombs;
    player->playerState = PLAYER_STATE_SPAWNING;
    player->positionCenter.x = g_GameManager.arcadeRegionSize.x / 2.0f;
    player->positionCenter.y = g_GameManager.arcadeRegionSize.y - 64.0f;
    player->positionCenter.z = 0.2f;
    player->invulnerabilityTimer = 0;
    player->playerSprite.scale.x = 3.0f;
    player->playerSprite.scale.y = 3.0f;
    g_AnmManager->SetAnmIdxAndExecuteScript(&player->playerSprite, 1024 + player->AnmShift());
    player->lifeGiveTimer = 0;
    player->lifeGiveTarget = 0;
    player->powerGiveTaps = 0;
    player->powerGiveWindow = 0;
    g_Gui.lifeDisplayUpdateFrames = 2;
    g_Gui.bombDisplayUpdateFrames = 2;
    g_Gui.powerDisplayUpdateFrames = 2;
    CoopLog("GHOST_REVIVE player=%d", seat);
}

void CoopExtendFromPoints()
{
    for (i32 seat = 0; seat < PlayerCount(); seat++)
    {
        Player *player = &g_Players[seat];
        if (IsGhost(player))
        {
            CoopReviveGhost(player);
            g_SoundPlayer.PlaySoundByIdx(SOUND_EXTEND, 0);
        }
        else
        {
            g_GameManager.ExtendSeat(seat);
        }
    }
    CoopLog("EXTEND seats=%d", PlayerCount());
}

void CoopContinue()
{
    for (i32 seat = 0; seat < PlayerCount(); seat++)
    {
        Player *player = &g_Players[seat];
        g_GameManager.Lives(seat) = g_GameManager.defaultCfg->lifeCount;
        g_GameManager.Bombs(seat) = player->shooterData->initialBombs;
        g_GameManager.Power(seat) = 0.0f;
    }
    for (i32 seat = 0; seat < PlayerCount(); seat++)
    {
        Player *player = &g_Players[seat];
        if (player->playerState == PLAYER_STATE_GHOST)
        {
            // The revival adds the life back.
            g_GameManager.Lives(seat) -= 1.0f;
            CoopReviveGhost(player);
            g_GameManager.Power(seat) = 0.0f;
        }
        player->lifeGiveTimer = 0;
        player->lifeGiveTarget = 0;
        player->powerGiveTaps = 0;
        player->powerGiveWindow = 0;
    }
    CoopLog("CONTINUE seats=%d", PlayerCount());
}

void CoopKeepGhost(Player *player, i32 wasGhost)
{
    if (wasGhost)
    {
        player->playerState = PLAYER_STATE_GHOST;
        player->invulnerabilityTimer = 30;
        CoopStepGhost(player);
        CoopLog("GHOST_KEPT player=%d", player->seat);
    }
}

#define TRANSFER_RANGE_SQ 400.0f
#define LIFE_CHARGE_FRAMES 90
#define POWER_TAPS 8
#define POWER_TAP_WINDOW 24
#define MAX_POWER 128

static i32 Playing(Player *player)
{
    return player->playerState == PLAYER_STATE_ALIVE || player->playerState == PLAYER_STATE_INVULNERABLE ||
           player->playerState == PLAYER_STATE_BORDER;
}

static i32 WithinTransferRange(Player *a, Player *b)
{
    f32 dx = a->positionCenter.x - b->positionCenter.x;
    f32 dy = a->positionCenter.y - b->positionCenter.y;
    return dx * dx + dy * dy <= TRANSFER_RANGE_SQ;
}

static i32 PowerGiveAmount()
{
    return MAX_POWER * 20 / 128;
}

static Player *PowerReceiver(Player *giver)
{
    Player *best = NULL;
    for (i32 seat = 0; seat < PlayerCount(); seat++)
    {
        Player *player = &g_Players[seat];
        if (player == giver || !Playing(player) || !WithinTransferRange(giver, player))
        {
            continue;
        }
        if ((i32)g_GameManager.Power(seat) >= MAX_POWER)
        {
            continue;
        }
        if (best == NULL || g_GameManager.Power(seat) < g_GameManager.Power(best->seat))
        {
            best = player;
        }
    }
    return best;
}

static Player *LifeReceiver(Player *giver)
{
    for (i32 seat = 0; seat < PlayerCount(); seat++)
    {
        Player *player = &g_Players[seat];
        if (player != giver && IsGhost(player) && WithinTransferRange(giver, player))
        {
            return player;
        }
    }
    Player *best = NULL;
    for (i32 seat = 0; seat < PlayerCount(); seat++)
    {
        Player *player = &g_Players[seat];
        if (player == giver || !Playing(player) || !WithinTransferRange(giver, player))
        {
            continue;
        }
        if ((i32)g_GameManager.Lives(seat) >= 8)
        {
            continue;
        }
        if (best == NULL || g_GameManager.Lives(seat) < g_GameManager.Lives(best->seat))
        {
            best = player;
        }
    }
    return best;
}

static void UpdatePowerTransfer(Player *giver)
{
    Player *receiver = PowerReceiver(giver);
    i32 amount = PowerGiveAmount();
    f32 &power = g_GameManager.Power(giver->seat);
    if (receiver == NULL || (i32)power < amount)
    {
        giver->powerGiveTaps = 0;
        giver->powerGiveWindow = 0;
        return;
    }
    if (giver->powerGiveWindow > 0 && --giver->powerGiveWindow == 0)
    {
        giver->powerGiveTaps = 0;
    }
    if (!giver->WasPressed(TH_BUTTON_SHOOT))
    {
        return;
    }
    giver->powerGiveTaps++;
    giver->powerGiveWindow = POWER_TAP_WINDOW;
    if (giver->powerGiveTaps < POWER_TAPS)
    {
        g_SoundPlayer.PlaySoundByIdx(SOUND_SELECT, 0);
        return;
    }
    giver->powerGiveTaps = 0;
    giver->powerGiveWindow = 0;
    f32 &to = g_GameManager.Power(receiver->seat);
    power -= amount;
    if (power < 0.0f)
    {
        power = 0.0f;
    }
    to += amount;
    if (to > MAX_POWER)
    {
        to = MAX_POWER;
    }
    g_Gui.powerDisplayUpdateFrames = 2;
    g_SoundPlayer.PlaySoundByIdx(SOUND_POWERUP, 0);
    CoopLog("POWER_GIVE from=%d to=%d amount=%d", giver->seat, receiver->seat, amount);
}

static void UpdateLifeTransfer(Player *giver)
{
    Player *receiver = LifeReceiver(giver);
    f32 &lives = g_GameManager.Lives(giver->seat);
    if (receiver == NULL || (i32)lives <= 0)
    {
        giver->lifeGiveTimer = 0;
        giver->lifeGiveTarget = 0;
        return;
    }
    if (giver->lifeGiveTarget != receiver->seat + 1)
    {
        giver->lifeGiveTimer = 0;
        giver->lifeGiveTarget = receiver->seat + 1;
    }
    if (giver->powerGiveTaps > 0)
    {
        giver->lifeGiveTimer = 0;
        return;
    }
    if (!giver->IsPressed(TH_BUTTON_FOCUS) || giver->IsPressed(TH_BUTTON_SHOOT))
    {
        giver->lifeGiveTimer = 0;
        giver->lifeGiveTarget = 0;
        return;
    }
    g_SoundPlayer.PlaySoundByIdx(SOUND_SELECT, 0);
    if (++giver->lifeGiveTimer < LIFE_CHARGE_FRAMES)
    {
        return;
    }
    giver->lifeGiveTimer = 0;
    giver->lifeGiveTarget = 0;
    lives -= 1.0f;
    if (IsGhost(receiver))
    {
        CoopReviveGhost(receiver);
    }
    else
    {
        g_GameManager.Lives(receiver->seat) += 1.0f;
    }
    g_Gui.lifeDisplayUpdateFrames = 2;
    g_SoundPlayer.PlaySoundByIdx(SOUND_EXTEND, 0);
    CoopLog("LIFE_GIVE from=%d to=%d", giver->seat, receiver->seat);
}

void CoopUpdateTransfers(Player *player)
{
    if (!Playing(player))
    {
        return;
    }
    UpdatePowerTransfer(player);
    UpdateLifeTransfer(player);
}

static void DrawPrompt(f32 fx, f32 fy, f32 scale, D3DCOLOR color, const char *text)
{
    Float3 pos;
    pos.x = g_GameManager.arcadeRegionTopLeftPos.x + fx -
            (f32)g_AsciiManager.fontSpacing * scale * (f32)strlen(text) * 0.5f;
    pos.y = g_GameManager.arcadeRegionTopLeftPos.y + fy;
    pos.z = 0.0f;
    g_AsciiManager.color = color;
    g_AsciiManager.scale.x = scale;
    g_AsciiManager.scale.y = scale;
    g_AsciiManager.AddString(&pos, text);
}

void CoopDrawTransferPrompts()
{
    D3DCOLOR color = g_AsciiManager.color;
    Float2 scale = g_AsciiManager.scale;
    i32 isGui = g_AsciiManager.isGui;
    g_AsciiManager.isGui = 0;
    for (i32 seat = 0; seat < PlayerCount(); seat++)
    {
        Player *player = &g_Players[seat];
        if (player->calcChain == NULL)
        {
            continue;
        }
        char text[16];
        if (player->lifeGiveTimer > 0)
        {
            sprintf_s(text, sizeof(text), "%d%%", player->lifeGiveTimer * 100 / LIFE_CHARGE_FRAMES);
            DrawPrompt(player->positionCenter.x, player->positionCenter.y - 22.0f - 8.0f, 0.6f, 0xffffff00, text);
        }
        if (player->powerGiveTaps >= 2)
        {
            sprintf_s(text, sizeof(text), "P %d/%d", player->powerGiveTaps, POWER_TAPS);
            DrawPrompt(player->positionCenter.x, player->positionCenter.y + 16.0f, 0.5f, 0xffa0ffa0, text);
        }
    }
    g_AsciiManager.color = color;
    g_AsciiManager.scale = scale;
    g_AsciiManager.isGui = isGui;
}

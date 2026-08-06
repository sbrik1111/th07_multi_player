#include "Player.hpp"

#include "AnmManager.hpp"
#include "AsciiManager.hpp"
#include "BombData.hpp"
#include "Chain.hpp"
#include "Controller.hpp"
#include "EffectManager.hpp"
#include "EnemyManager.hpp"
#include "FileSystem.hpp"
#include "GameErrorContext.hpp"
#include "GameManager.hpp"
#include "Gui.hpp"
#include "ItemManager.hpp"
#include "Netplay.hpp"
#include "Rng.hpp"
#include "SoundPlayer.hpp"
#include "Stage.hpp"
#include "ZunMath.hpp"
#include "d3dx8.h"
#include "dxutil.hpp"
#include "utils.hpp"

// GLOBAL: TH07 0x0049ecb0
ShtFunc1 g_ShtFireFuncs[6] = {
    NULL,
    ShtData::FireBulletDefault,
    ShtData::FireOrbBulletUnfocused,
    ShtData::FireOrbBulletFocused,
    ShtData::FireHomingBullet,
    ShtData::FireRotatingOrbBullet,
};

// GLOBAL: TH07 0x0049ecc8
ShtFunc2 g_ShtUpdateFuncs[6] = {
    NULL,
    ShtData::UpdateHomingBullet,
    ShtData::UpdateHomingBulletFocused,
    ShtData::UpdateUpwardAcceleratingBullet,
    ShtData::UpdateOrbLaser,
    ShtData::UpdatePlayerLaser,
};

// GLOBAL: TH07 0x0049ece0
ShtFunc3 g_ShtDrawFuncs[2] = {
    NULL,
    ShtData::DrawBulletWithTrail,
};

// GLOBAL: TH07 0x0049ece8
ShtFunc4 g_ShtHitFuncs[4] = {
    NULL,
    ShtData::OnMissileHit,
    ShtData::SpawnHitParticles,
    (ShtFunc4)0x00000001, // ZUN landmine: i guess bro
};

// GLOBAL: TH07 0x0049f530
const char *g_ShooterTable[6] = {
    // STRING: TH07 0x00496bb4
    "data/ply00a.sht",
    // STRING: TH07 0x00496ba4
    "data/ply00b.sht",
    // STRING: TH07 0x00496b94
    "data/ply01a.sht",
    // STRING: TH07 0x00496b84
    "data/ply01b.sht",
    // STRING: TH07 0x00496b74
    "data/ply02a.sht",
    // STRING: TH07 0x00496b64
    "data/ply02b.sht",
};

// GLOBAL: TH07 0x0049f548
const char *g_ShooterTableFocus[6] = {
    // STRING: TH07 0x00496b50
    "data/ply00as.sht",
    // STRING: TH07 0x00496b3c
    "data/ply00bs.sht",
    // STRING: TH07 0x00496b28
    "data/ply01as.sht",
    // STRING: TH07 0x00496b14
    "data/ply01bs.sht",
    // STRING: TH07 0x00496b00
    "data/ply02as.sht",
    // STRING: TH07 0x00496aec
    "data/ply02bs.sht",
};

// GLOBAL: TH07 0x004bdad8
Player g_Players[TH07_MULTI_MAX_PLAYERS];
bool g_PlayerActive[TH07_MULTI_MAX_PLAYERS] = {true, false, false};
i32 g_cherryMaxGrazeGrowth[TH07_MULTI_MAX_PLAYERS] = {0, 0, 0};
i32 g_cherryMaxBreakGrowth[TH07_MULTI_MAX_PLAYERS] = {0, 0, 0};

Player *GetPlayerById(u8 playerId)
{
    return playerId < TH07_MULTI_MAX_PLAYERS ? &g_Players[playerId] : NULL;
}

const Player *GetPlayerByIdConst(u8 playerId)
{
    return playerId < TH07_MULTI_MAX_PLAYERS ? &g_Players[playerId] : NULL;
}

bool IsPlayerSlotActive(u8 playerId)
{
    return playerId < TH07_MULTI_MAX_PLAYERS && g_PlayerActive[playerId];
}

u8 GetActivePlayerMask()
{
    u8 mask = 0;
    int playerId;
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        if (g_PlayerActive[playerId])
        {
            mask |= (u8)(1 << playerId);
        }
    }
    return mask;
}

i32 GetActivePlayerCount()
{
    i32 count = 0;
    int playerId;
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        if (g_PlayerActive[playerId])
        {
            count++;
        }
    }
    return count;
}

bool IsAnyActivePlayerBombing()
{
    i32 playerId;
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        if (IsPlayerSlotActive((u8)playerId) &&
            g_Players[playerId].bombInfo.isInUse)
        {
            return true;
        }
    }
    return false;
}

static bool g_lifeTransferTestSetupLogged = false;
static bool g_playerBulletAnmLogged[TH07_MULTI_MAX_PLAYERS] =
    {false, false, false};
static bool g_playerIdentityRepairLogged = false;
static bool g_sharedBorderTransition = false;

// Keep a game-over spirit close enough for the partner to revive it.  This is
// part of the synchronized gameplay state, so use one fixed deterministic
// speed for both axes and both players.
static const f32 PLAYER_SPIRIT_DRIFT_SPEED = 0.2f;

static bool IsSharedBorderParticipant(const Player *player)
{
    return player && player->playerState != PLAYER_STATE_ELIMINATED &&
        player->playerState != PLAYER_STATE_SPIRIT;
}

bool IsSharedBorderActive()
{
    int playerId;
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        if (g_PlayerActive[playerId] &&
            g_Players[playerId].hasBorder == BORDER_ACTIVE &&
            g_Players[playerId].playerState == PLAYER_STATE_BORDER)
        {
            return true;
        }
    }
    return false;
}

void ActivateSharedBorder()
{
    int playerId;
    if (g_sharedBorderTransition)
    {
        return;
    }

    g_sharedBorderTransition = true;
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        Player *player = &g_Players[playerId];
        if (g_PlayerActive[playerId] &&
            IsSharedBorderParticipant(player) &&
            player->hasBorder != BORDER_ACTIVE)
        {
            player->ActivateBorder();
        }
    }
    g_sharedBorderTransition = false;
}

static void ClearSharedBorderState(Player *player)
{
    if (!player)
    {
        return;
    }
    player->hasBorder = BORDER_NONE;
    player->playerState = PLAYER_STATE_INVULNERABLE;
    player->invulnerabilityTimer = 40;
    player->borderInvulnerabilityTime = 40;
    if (player->borderEffect)
    {
        player->borderEffect->inUseFlag = 0;
        player->borderEffect = NULL;
    }
}

static Player *GetSharedBorderOwner()
{
    i32 playerId;
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        Player *player = &g_Players[playerId];
        if (IsPlayerSlotActive((u8)playerId) &&
            player->hasBorder == BORDER_ACTIVE &&
            player->playerState == PLAYER_STATE_BORDER)
        {
            return player;
        }
    }
    return NULL;
}

Player *GetClosestActivePlayer(D3DXVECTOR3 *position)
{
    static u32 callsThisSecond = 0;
    static u32 switchesThisSecond = 0;
    static i32 previousTargetId = -1;
    static i32 lastLoggedFrame = -1;
    static i32 lastStage = -1;
    Player *closest = NULL;
    f32 closestDistance = 0.0f;
    int playerId;
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        Player *player = &g_Players[playerId];
        f32 dx;
        f32 dy;
        f32 distance;
        if (!g_PlayerActive[playerId] ||
            Netplay::IsPlayerTemporarilyAbsent((u8)playerId) ||
            (player->playerState != PLAYER_STATE_ALIVE &&
             player->playerState != PLAYER_STATE_INVULNERABLE &&
             player->playerState != PLAYER_STATE_BORDER))
        {
            continue;
        }
        dx = player->positionCenter.x - position->x;
        dy = player->positionCenter.y - position->y;
        distance = dx * dx + dy * dy;
        // Strictly-less keeps the lower slot id on an exact tie.
        if (!closest || distance < closestDistance)
        {
            closest = player;
            closestDistance = distance;
        }
    }
    closest = closest ? closest : &g_Player;
    if (Netplay::IsMultiplayer())
    {
        i32 targetId = closest->initParam;
        i32 stageFrame = g_GameManager.framesThisStage;
        callsThisSecond++;
        if (previousTargetId >= 0 && previousTargetId != targetId)
        {
            switchesThisSecond++;
        }
        previousTargetId = targetId;
        if (g_GameManager.currentStage != lastStage)
        {
            lastStage = g_GameManager.currentStage;
            lastLoggedFrame = -1;
            callsThisSecond = 1;
            switchesThisSecond = 0;
        }
        if (g_GameManager.notInMenu && stageFrame >= 0 &&
            stageFrame % 60 == 59 && stageFrame > lastLoggedFrame)
        {
            lastLoggedFrame = stageFrame;
            g_GameErrorContext.Log(
                "info : closest-player calls %lu target_switches %lu stage %d frame %d last P%d\r\n",
                (unsigned long)callsThisSecond,
                (unsigned long)switchesThisSecond,
                g_GameManager.currentStage, stageFrame, targetId + 1);
            callsThisSecond = 0;
            switchesThisSecond = 0;
        }
    }
    return closest;
}

static bool IsPlayerActiveForProximity(const Player *player)
{
    return player && IsPlayerSlotActive(player->initParam) &&
        !Netplay::IsPlayerTemporarilyAbsent(player->initParam) &&
        (player->playerState == PLAYER_STATE_ALIVE ||
         player->playerState == PLAYER_STATE_INVULNERABLE ||
         player->playerState == PLAYER_STATE_BORDER);
}

static bool IsPlayerActiveForLifeTransfer(const Player *player)
{
    return IsPlayerActiveForProximity(player);
}

static Player *SelectLifeTransferReceiver(const Player *giver)
{
    Player *best = NULL;
    bool bestIsSpirit = false;
    i32 bestLives = 0;
    i32 playerId;

    if (!giver || !IsPlayerSlotActive(giver->initParam))
    {
        return NULL;
    }
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        Player *candidate;
        bool isSpirit;
        bool canHoldLife;
        i32 lives;
        f32 dx;
        f32 dy;
        if (playerId == giver->initParam ||
            !IsPlayerSlotActive((u8)playerId))
        {
            continue;
        }
        candidate = &g_Players[playerId];
        isSpirit = candidate->playerState == PLAYER_STATE_SPIRIT;
        lives = GetPlayerLives((u8)playerId);
        canHoldLife = IsPlayerActiveForLifeTransfer(candidate) && lives < 8;
        if (!isSpirit && !canHoldLife)
        {
            continue;
        }
        dx = giver->positionCenter.x - candidate->positionCenter.x;
        dy = giver->positionCenter.y - candidate->positionCenter.y;
        if (dx * dx + dy * dy > 400.0f)
        {
            continue;
        }
        if (!best ||
            (isSpirit && !bestIsSpirit) ||
            (isSpirit == bestIsSpirit && lives < bestLives) ||
            (isSpirit == bestIsSpirit && lives == bestLives &&
             playerId < best->initParam))
        {
            best = candidate;
            bestIsSpirit = isSpirit;
            bestLives = lives;
        }
    }
    return best;
}

bool VerifyThreePlayerLifeTransferSelectionRules()
{
    Player *giver;
    Player *player2;
    Player *player3;
    D3DXVECTOR3 savedPosition2;
    D3DXVECTOR3 savedPosition3;
    i8 savedState2;
    i8 savedState3;
    i32 savedLives2;
    i32 savedLives3;
    Player *spiritWinner;
    Player *lowLifeWinner;
    Player *slotWinner;
    bool passed;

    if (GetActivePlayerCount() != 3 || !g_GameManager.globals)
    {
        g_GameErrorContext.Log(
            "error : three-player life transfer rule test requires three active players\r\n");
        return false;
    }

    giver = &g_Player;
    player2 = &g_Player2;
    player3 = &g_Player3;
    savedPosition2 = player2->positionCenter;
    savedPosition3 = player3->positionCenter;
    savedState2 = player2->playerState;
    savedState3 = player3->playerState;
    savedLives2 = GetPlayerLives(1);
    savedLives3 = GetPlayerLives(2);

    // Exercise the production selector with both candidates inside the real
    // 20-pixel radius. Restore every touched gameplay field before returning
    // so this diagnostic cannot perturb the synchronized run.
    player2->positionCenter = giver->positionCenter;
    player2->positionCenter.x += 10.0f;
    player3->positionCenter = giver->positionCenter;
    player3->positionCenter.x -= 10.0f;

    player2->playerState = PLAYER_STATE_ALIVE;
    player3->playerState = PLAYER_STATE_SPIRIT;
    SetPlayerLives(1, 0);
    SetPlayerLives(2, 7);
    spiritWinner = SelectLifeTransferReceiver(giver);

    player2->playerState = PLAYER_STATE_ALIVE;
    player3->playerState = PLAYER_STATE_ALIVE;
    SetPlayerLives(1, 2);
    SetPlayerLives(2, 1);
    lowLifeWinner = SelectLifeTransferReceiver(giver);

    SetPlayerLives(1, 1);
    SetPlayerLives(2, 1);
    slotWinner = SelectLifeTransferReceiver(giver);

    player2->positionCenter = savedPosition2;
    player3->positionCenter = savedPosition3;
    player2->playerState = savedState2;
    player3->playerState = savedState3;
    SetPlayerLives(1, savedLives2);
    SetPlayerLives(2, savedLives3);

    passed = spiritWinner == player3 && lowLifeWinner == player3 &&
        slotWinner == player2;
    if (passed)
    {
        g_GameErrorContext.Log(
            "info : three-player life transfer rules verified spirit P3 low-life P3 slot-tie P2\r\n");
    }
    else
    {
        g_GameErrorContext.Log(
            "error : three-player life transfer rules failed spirit P%d low-life P%d slot-tie P%d\r\n",
            spiritWinner ? spiritWinner->initParam + 1 : 0,
            lowLifeWinner ? lowLifeWinner->initParam + 1 : 0,
            slotWinner ? slotWinner->initParam + 1 : 0);
    }
    return passed;
}

static i32 SelectLowestLifeRecipient(u8 excludedPlayerId)
{
    i32 bestId = -1;
    i32 bestLives = 0;
    i32 playerId;
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        i32 lives;
        if (playerId == excludedPlayerId ||
            !IsPlayerSlotActive((u8)playerId) ||
            !IsPlayerActiveForLifeTransfer(&g_Players[playerId]))
        {
            continue;
        }
        lives = GetPlayerLives((u8)playerId);
        if (bestId < 0 || lives < bestLives)
        {
            bestId = playerId;
            bestLives = lives;
        }
    }
    return bestId;
}

static void AddLifeTransferPrompt(const Player *giver, bool charging)
{
    D3DXVECTOR3 position;
    Float2 savedScale;
    D3DCOLOR savedColor;
    i32 savedGui;
    i32 savedSelected;

    if (!charging)
    {
        return;
    }

    position = giver->positionCenter;
    position.x -= 14.0f;
    position.y -= 22.0f;
    position.z = 0.48f;
    savedScale = g_AsciiManager.scale;
    savedColor = g_AsciiManager.color;
    savedGui = g_AsciiManager.isGui;
    savedSelected = g_AsciiManager.isSelected;
    g_AsciiManager.scale.x = 0.6f;
    g_AsciiManager.scale.y = 0.6f;
    g_AsciiManager.color = 0xffffff00;
    g_AsciiManager.isGui = 1;
    g_AsciiManager.isSelected = 0;
    AsciiManager::AddFormatText(
        &g_AsciiManager, &position, "%d%%",
        giver->lifeGiveTimer * 100 / 90);
    g_AsciiManager.scale = savedScale;
    g_AsciiManager.color = savedColor;
    g_AsciiManager.isGui = savedGui;
    g_AsciiManager.isSelected = savedSelected;
}

// The opening seconds of a stage, while the title card is up and nothing is
// shooting yet. A fixed count rather than a hook into the banner's own
// lifetime: the banner is an ANM script whose visibility is not exposed as
// simulation state, and a label that lingers a moment past it costs nothing.
static const i32 STAGE_INTRO_NAME_FRAMES = 240;
static const f32 STAGE_INTRO_NAME_SCALE = 0.48f;

static bool IsStageIntroActive()
{
    return g_GameManager.notInMenu &&
        (i32)g_GameManager.framesThisStage < STAGE_INTRO_NAME_FRAMES;
}

// Drawn at the start of a stage because that is the one stretch with no
// bullets on screen: a label over the ship costs nothing there and answers
// "which one am I" without covering anything during play.
static void DrawStageIntroPlayerName(const Player *player)
{
    static const D3DCOLOR nameColors[TH07_MULTI_MAX_PLAYERS] = {
        0xffffffff, 0xffa0d0ff, 0xffa8ffa8
    };
    D3DXVECTOR3 position;
    Float2 savedScale;
    D3DCOLOR savedColor;
    i32 savedGui;
    i32 savedSelected;
    const char *name;
    size_t length;
    f32 labelWidth;

    if (!Netplay::IsMultiplayer() ||
        !Netplay::ShouldShowStagePlayerNames() ||
        player->initParam >= TH07_MULTI_MAX_PLAYERS ||
        !IsStageIntroActive())
    {
        return;
    }
    name = Netplay::GetPlayerName(player->initParam);
    if (!name || name[0] == '\0')
    {
        return;
    }
    length = strlen(name);
    // The ASCII font is 8 px wide before scaling.
    labelWidth = (f32)length * 8.0f * STAGE_INTRO_NAME_SCALE;
    position = player->positionCenter;
    position.x -= labelWidth * 0.5f;
    // The ships spawn on top of each other and fly apart over the first
    // second, so an unstaggered label is three names in the same place.
    position.y -= 22.0f + (f32)player->initParam * 9.0f;
    if (position.x < 2.0f)
    {
        position.x = 2.0f;
    }
    if (position.x + labelWidth > g_GameManager.arcadeRegionSize.x - 2.0f)
    {
        position.x = g_GameManager.arcadeRegionSize.x - 2.0f - labelWidth;
    }
    position.z = 0.48f;
    savedScale = g_AsciiManager.scale;
    savedColor = g_AsciiManager.color;
    savedGui = g_AsciiManager.isGui;
    savedSelected = g_AsciiManager.isSelected;
    g_AsciiManager.scale.x = STAGE_INTRO_NAME_SCALE;
    g_AsciiManager.scale.y = STAGE_INTRO_NAME_SCALE;
    g_AsciiManager.color = nameColors[player->initParam];
    g_AsciiManager.isGui = 1;
    g_AsciiManager.isSelected = 0;
    AsciiManager::AddFormatText(&g_AsciiManager, &position, "%s", name);
    g_AsciiManager.scale = savedScale;
    g_AsciiManager.color = savedColor;
    g_AsciiManager.isGui = savedGui;
    g_AsciiManager.isSelected = savedSelected;
}

static void DrawLifeTransferPrompt(const Player *giver)
{
    const Player *receiver;
    bool testFocus;

    if (GetActivePlayerCount() < 2 ||
        !IsPlayerActiveForLifeTransfer(giver))
    {
        return;
    }
    receiver = SelectLifeTransferReceiver(giver);
    if (!receiver)
    {
        return;
    }
    if (GetPlayerLives(giver->initParam) <= 0)
    {
        return;
    }
    testFocus = Netplay::IsLifeTransferTestEnabled() &&
        !Netplay::IsNetworked() && giver->initParam == 0 &&
        !Netplay::IsLifeTransferTestVerified();
    AddLifeTransferPrompt(
        giver, (giver->isFocus || testFocus) &&
                   !IS_PRESSED_PLAYER(giver, TH_BUTTON_SHOOT));
}

static void UpdateLifeTransfer(Player *giver)
{
    Player *receiver;
    bool receiverIsSpirit;
    bool testFocus;
    Item *lifeItem;
    i32 giverLivesBefore;

    if (GetActivePlayerCount() < 2 ||
        !IsPlayerActiveForLifeTransfer(giver))
    {
        giver->lifeGiveTimer = 0;
        giver->lifeGiveTargetToken = 0;
        return;
    }
    receiver = SelectLifeTransferReceiver(giver);
    if (!receiver)
    {
        giver->lifeGiveTimer = 0;
        giver->lifeGiveTargetToken = 0;
        return;
    }
    if (giver->lifeGiveTargetToken != receiver->initParam + 1)
    {
        // A different receiver becoming higher priority restarts the 90-frame
        // charge, as required for deterministic three-way transfer selection.
        giver->lifeGiveTimer = 0;
        giver->lifeGiveTargetToken = receiver->initParam + 1;
    }
    receiverIsSpirit = receiver->playerState == PLAYER_STATE_SPIRIT;
    testFocus = Netplay::IsLifeTransferTestEnabled() &&
        !Netplay::IsNetworked() && giver->initParam == 0 &&
        !Netplay::IsLifeTransferTestVerified();
    if ((giver->isFocus || testFocus) &&
        !IS_PRESSED_PLAYER(giver, TH_BUTTON_SHOOT))
    {
        // TH06 plays the proximity warning while the transfer is charging.
        g_SoundPlayer.PlaySoundByIdx(SOUND_21, 0);
        giver->lifeGiveTimer++;
        if (giver->lifeGiveTimer >= 90 &&
            GetPlayerLives(giver->initParam) > 0)
        {
            giver->lifeGiveTimer = 0;
            giver->lifeGiveTargetToken = 0;
            if (receiverIsSpirit)
            {
                giverLivesBefore = GetPlayerLives(giver->initParam);
                AddPlayerLives(giver->initParam, -1);
                receiver->playerState = PLAYER_STATE_INVULNERABLE;
                receiver->optionState = OPTION_UNFOCUSED;
                receiver->invulnerabilityTimer = 120;
                receiver->respawnTimer =
                    receiver->shooterData->initialRespawnTimer;
                receiver->bulletGracePeriod = 60;
                receiver->playerSprite.color.color = 0xffffffff;
                g_Gui.showLives = 2;
                g_SoundPlayer.PlaySoundByIdx(SOUND_EXTEND, 0);
                Netplay::ReportDamageEventRevive(
                    giver->initParam, receiver->initParam,
                    giverLivesBefore, GetPlayerLives(giver->initParam));
            }
            else
            {
                lifeItem = g_ItemManager.SpawnItem(
                    &giver->positionCenter, ITEM_LIFE,
                    GetLifeTransferSpawnState(receiver->initParam));
                if (lifeItem != &g_ItemManager.items[1100])
                {
                    AddPlayerLives(giver->initParam, -1);
                    g_Gui.showLives = 2;
                    g_SoundPlayer.PlaySoundByIdx(SOUND_25, 0);
                }
            }
        }
    }
    else
    {
        giver->lifeGiveTimer = 0;
        giver->lifeGiveTargetToken = 0;
    }
}

static bool IsProximityFadeTarget(const Player *player)
{
    if (!player || GetActivePlayerCount() < 2 ||
        !IsPlayerSlotActive(player->initParam))
    {
        return false;
    }
    if (!Netplay::IsNetworked())
    {
        return player->initParam != 0;
    }
    return player->initParam != Netplay::GetLocalPlayerSlot();
}

// TH06 hides the remote ship when the two ships overlap. Keep this as a
// draw-time effect so it cannot change the deterministic gameplay state or
// the original TH07 Player structure size.
static u8 CalculatePlayerOverlapAlpha(const Player *player)
{
    f32 dx;
    f32 dy;
    f32 distance;
    f32 closestDistanceSquared = 0.0f;
    i32 alpha;
    i32 playerId;
    bool foundOther = false;

    if (!player || !IsPlayerSlotActive(player->initParam) ||
        !IsPlayerActiveForProximity(player))
    {
        return 255;
    }
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        Player *other;
        f32 distanceSquared;
        if (playerId == player->initParam ||
            !IsPlayerSlotActive((u8)playerId))
        {
            continue;
        }
        other = &g_Players[playerId];
        if (!IsPlayerActiveForProximity(other))
        {
            continue;
        }
        dx = player->positionCenter.x - other->positionCenter.x;
        dy = player->positionCenter.y - other->positionCenter.y;
        distanceSquared = dx * dx + dy * dy;
        if (!foundOther || distanceSquared < closestDistanceSquared)
        {
            closestDistanceSquared = distanceSquared;
            foundOther = true;
        }
    }
    if (!foundOther)
    {
        return 255;
    }
    distance = sqrtf(closestDistanceSquared);
    if (distance >= 100.0f)
    {
        return 255;
    }
    if (distance < 50.0f)
    {
        distance = 50.0f;
    }
    alpha = (i32)(((distance - 50.0f) / 50.0f) * 200.0f) + 55;
    if (alpha < 0)
    {
        alpha = 0;
    }
    if (alpha > 255)
    {
        alpha = 255;
    }
    return (u8)alpha;
}

static u8 GetPlayerProximityAlpha(const Player *player)
{
    if (!IsProximityFadeTarget(player))
    {
        return 255;
    }
    return CalculatePlayerOverlapAlpha(player);
}

u8 GetPlayerOverlapAlpha(const Player *player)
{
    // Secondary focus circles are part of the shared playfield presentation.
    // Fade P2/P3 on every PC even when that slot is local; ship fading itself
    // remains remote-only above.
    if (GetActivePlayerCount() < 2 || !player || player->initParam == 0)
    {
        return 255;
    }
    return CalculatePlayerOverlapAlpha(player);
}

static u32 ApplyPlayerProximityAlpha(u32 color, const Player *player)
{
    u8 alpha = (u8)(color >> 24);
    u8 proximityAlpha = GetPlayerProximityAlpha(player);

    if (proximityAlpha < alpha)
    {
        alpha = proximityAlpha;
    }
    return (color & 0x00ffffff) | ((u32)alpha << 24);
}

// FUNCTION: TH07 0x0043bbd0
void DefaultFireBulletCallback(Player *player, PlayerBullet *bullet,
                               ShtEntry *shtEntry)
{
    if (shtEntry->option == 0)
    {
        bullet->pos = player->positionCenter;
    }
    else
    {
        bullet->pos = player->optionsPosition[shtEntry->option - 1];
    }
    *bullet->GetPosX() += shtEntry->offset.x;
    *bullet->GetPosY() += shtEntry->offset.y;
    bullet->pos.z = 0.495f;
    bullet->hitboxSize.x = shtEntry->hitboxSize.x;
    bullet->hitboxSize.y = shtEntry->hitboxSize.y;
    bullet->hitboxSize.z = 1.0f;
    bullet->angle = shtEntry->angle;
    bullet->speed = shtEntry->speed;
    bullet->velocity.x = cosf(shtEntry->angle) * shtEntry->speed;
    bullet->velocity.y = sinf(shtEntry->angle) * shtEntry->speed;
    bullet->timer = 0;
    bullet->bulletState2 = shtEntry->bulletState2;
    bullet->damage = shtEntry->damage;
    if (shtEntry->soundIdx >= 0)
    {
        g_SoundPlayer.PlaySoundByIdx(shtEntry->soundIdx, 0);
    }
    g_AnmManager->SetAnmIdxAndExecuteScript(
        &bullet->vm, GetPlayerAnmScript(player, shtEntry->anmFileIdx));
    if (player->initParam != 0 &&
        player->initParam < TH07_MULTI_MAX_PLAYERS &&
        !g_playerBulletAnmLogged[player->initParam])
    {
        g_playerBulletAnmLogged[player->initParam] = true;
        if (bullet->vm.anmFileIdx !=
            GetPlayerAnmScript(player, shtEntry->anmFileIdx))
        {
            g_GameErrorContext.Log(
                "error : P%d shot used wrong ANM script %d (expected %d)\r\n",
                player->initParam + 1,
                bullet->vm.anmFileIdx,
                GetPlayerAnmScript(player, shtEntry->anmFileIdx));
        }
        else
        {
            g_GameErrorContext.Log(
                "info : P%d shot ANM verified script %d\r\n",
                player->initParam + 1,
                bullet->vm.anmFileIdx);
        }
    }
}

// FUNCTION: TH07 0x0043bdc0
i32 ShtData::FireBulletDefault(Player *player, PlayerBullet *bullet,
                               i32 fireTime, ShtEntry *shtEntry)
{
    if (fireTime % shtEntry->fireInterval == shtEntry->fireOffset)
    {
        DefaultFireBulletCallback(player, bullet, shtEntry);
        return 1;
    }
    return 0;
}

// FUNCTION: TH07 0x0043be10
i32 ShtData::FireOrbBulletUnfocused(Player *player, PlayerBullet *bullet,
                                    i32 fireTime, ShtEntry *shtEntry)
{
    i32 fireOffset = shtEntry->fireOffset;

    if (player->timers[fireOffset].bullet)
    {
        if (player->shtEntries[fireOffset] != shtEntry)
        {
            player->timers[fireOffset].bullet->vm.pendingInterrupt = 1;
            player->timers[fireOffset].bullet = NULL;
        }
        return 0;
    }

    if (player->optionState != OPTION_UNFOCUSED)
    {
        return 0;
    }

    player->timers[fireOffset].timer = shtEntry->fireInterval;
    player->timers[fireOffset].bullet = bullet;
    bullet->timerIdx = fireOffset;
    bullet->optionId = (i16)shtEntry->option;
    bullet->offset.x = shtEntry->offset.x;
    bullet->offset.y = shtEntry->offset.y;
    DefaultFireBulletCallback(player, bullet, shtEntry);
    player->shtEntries[fireOffset] = shtEntry;
    return 1;
}

// FUNCTION: TH07 0x0043bf50
i32 ShtData::FireOrbBulletFocused(Player *player, PlayerBullet *bullet,
                                  i32 fireTime, ShtEntry *shtEntry)
{
    i32 fireOffset = shtEntry->fireOffset;

    if (player->timers[fireOffset].bullet)
    {
        if (player->shtEntries[fireOffset] != shtEntry)
        {
            player->timers[fireOffset].bullet->vm.pendingInterrupt = 1;
            player->timers[fireOffset].bullet = NULL;
        }
        return 0;
    }

    if (player->optionState != OPTION_FOCUSED)
    {
        return 0;
    }

    player->timers[fireOffset].timer = 999;
    player->timers[fireOffset].bullet = bullet;
    bullet->timerIdx = fireOffset;
    bullet->optionId = (i16)shtEntry->option;
    bullet->offset.x = shtEntry->offset.x;
    bullet->offset.y = shtEntry->offset.y;
    bullet->trailLength = shtEntry->fireInterval;
    DefaultFireBulletCallback(player, bullet, shtEntry);
    for (i32 i = 15; i >= 0; i--)
    {
        bullet->posHistory[i].x = -999.0f;
    }
    bullet->pos.x = -999.0f;
    player->shtEntries[fireOffset] = shtEntry;
    return 1;
}

#pragma var_order(speed, angle)
// FUNCTION: TH07 0x0043c0d0
i32 ShtData::FireHomingBullet(Player *player, PlayerBullet *bullet,
                              i32 fireTime, ShtEntry *shtEntry)
{
    f32 angle;
    f32 speed;

    if (fireTime % shtEntry->fireInterval == shtEntry->fireOffset)
    {
        DefaultFireBulletCallback(player, bullet, shtEntry);
        if (player->sakuyaTargetPosition.x > -100.0f)
        {
            angle = utils::AddNormalizeAngle(
                atan2f(player->sakuyaTargetPosition.y - bullet->pos.y,
                       player->sakuyaTargetPosition.x - bullet->pos.x),
                shtEntry->angle + 1.5707964f);
            speed = shtEntry->speed * 1.5f;
            AngleToVector((D3DXVECTOR3 *)&bullet->velocity, angle, speed);
            bullet->angle = angle;
        }
        return 1;
    }

    return 0;
}

#pragma var_order(speed, angle)
// FUNCTION: TH07 0x0043c1c0
i32 ShtData::FireRotatingOrbBullet(Player *player, PlayerBullet *bullet,
                                   i32 fireTime, ShtEntry *shtEntry)
{
    f32 angle;
    f32 speed;

    if (fireTime % shtEntry->fireInterval == shtEntry->fireOffset)
    {
        DefaultFireBulletCallback(player, bullet, shtEntry);
        angle = utils::AddNormalizeAngle(player->optionAngle,
                                         shtEntry->angle + 1.5707964f);
        speed = shtEntry->speed;
        AngleToVector((D3DXVECTOR3 *)&bullet->velocity, angle, speed);
        bullet->angle = angle;

        return 1;
    }

    return 0;
}

#pragma var_order(y, x, length)
i32 ShtData::UpdateHomingBullet(Player *player, PlayerBullet *bullet)
{
    f32 length;
    f32 x;
    f32 y;

    if (bullet->bulletState == 1)
    {
        if (player->positionOfLastEnemyHit.x > -100.0f &&
            bullet->timer.GetCurrent() < 40 &&
            bullet->timer.HasTicked())
        {
            x = player->positionOfLastEnemyHit.x - bullet->pos.x;
            y = player->positionOfLastEnemyHit.y - bullet->pos.y;
            length = sqrtf(x * x + y * y) / (bullet->speed / 4.0f);

            if (length < 1.0f)
            {
                length = 1.0f;
            }

            x = x / length + bullet->velocity.x;
            y = y / length + bullet->velocity.y;
            length = sqrtf(x * x + y * y);

            bullet->speed = length > 10.0f ? 10.0f : length;

            if (bullet->speed < 1.0f)
            {
                bullet->speed = 1.0f;
            }

            bullet->velocity.x = x * bullet->speed / length;
            bullet->velocity.y = y * bullet->speed / length;
        }
        else
        {
            if (bullet->speed < 10.0f)
            {
                bullet->speed = bullet->speed + 0.33333334f;
                x = bullet->velocity.x;
                y = bullet->velocity.y;
                length = sqrtf(x * x + y * y);
                bullet->velocity.x = x * bullet->speed / length;
                bullet->velocity.y = y * bullet->speed / length;
            }
        }
    }
    return 0;
}

#pragma var_order(y, x, length)
i32 ShtData::UpdateHomingBulletFocused(Player *player, PlayerBullet *bullet)
{
    f32 length;
    f32 x;
    f32 y;

    if (bullet->bulletState == 1)
    {
        if (player->positionOfLastEnemyHit.x > -100.0f &&
            bullet->timer.GetCurrent() < 40 &&
            bullet->timer.HasTicked())
        {
            x = player->positionOfLastEnemyHit.x - bullet->pos.x;
            y = player->positionOfLastEnemyHit.y - bullet->pos.y;
            length = sqrtf(x * x + y * y) / (bullet->speed / 4.0f);
            if (length < 1.0f)
            {
                length = 1.0f;
            }
            x = x / length + bullet->velocity.x;
            y = y / length + bullet->velocity.y;
            length = sqrtf(x * x + y * y);
            bullet->speed = length > 18.0f ? 18.0f : length;
            if (bullet->speed < 1.0f)
            {
                bullet->speed = 1.0f;
            }
            bullet->velocity.x = x * bullet->speed / length;
            bullet->velocity.y = y * bullet->speed / length;
        }
        else
        {
            if (bullet->speed < 18.0f)
            {
                bullet->speed = bullet->speed + 0.6f;
                x = bullet->velocity.x;
                y = bullet->velocity.y;
                length = sqrtf(x * x + y * y);
                bullet->velocity.x = x * bullet->speed / length;
                bullet->velocity.y = y * bullet->speed / length;
            }
        }
    }
    return 0;
}

// FUNCTION: TH07 0x0043c6b0
i32 ShtData::UpdateUpwardAcceleratingBullet(Player *player,
                                            PlayerBullet *bullet)
{
    if (bullet->bulletState == 1)
    {
        bullet->velocity.y =
            bullet->velocity.y - (g_Rng.GetRandomFloatInRange(0.1f) + 0.27f);
    }
    return 0;
}

// FUNCTION: TH07 0x0043c700
i32 ShtData::UpdateOrbLaser(Player *player, PlayerBullet *bullet)
{
    if (player->timers[bullet->timerIdx].bullet != bullet &&
        bullet->vm.isStopped)
    {
        bullet->vm.pendingInterrupt = 1;
    }
    if ((g_Gui.HasCurrentMsgIdx() || player->bombInfo.isInUse) &&
        20 < player->timers[bullet->timerIdx].timer.GetCurrent())
    {
        player->timers[bullet->timerIdx].timer = 20;
    }
    if (player->timers[bullet->timerIdx].timer <= 0)
    {
        player->timers[bullet->timerIdx].timer = 0;
        player->timers[bullet->timerIdx].bullet = NULL;
        bullet->bulletState = 0;
        return 1;
    }

    if (player->timers[bullet->timerIdx].timer <= 70 &&
        bullet->vm.isStopped)
    {
        bullet->vm.pendingInterrupt = 1;
    }
    bullet->pos = player->optionsPosition[bullet->optionId - 1];
    bullet->pos.x += bullet->offset.x;
    bullet->pos.z = 0.44f;
    if (player->playerState == PLAYER_STATE_DEAD)
    {
        return 1;
    }
    else
    {
        bullet->vm.scale.y = bullet->pos.y / 14.0f;
        bullet->hitboxSize.y = bullet->pos.y;
        bullet->pos.y = bullet->pos.y / 2.0f;
        return 0;
    }
}

// FUNCTION: TH07 0x0043c940
i32 ShtData::UpdatePlayerLaser(Player *player, PlayerBullet *bullet)
{
    i32 i;

    if (player->timers[bullet->timerIdx].bullet != bullet &&
        bullet->vm.isStopped)
    {
        bullet->vm.pendingInterrupt = 1;
    }
    if ((g_Gui.HasCurrentMsgIdx() || player->bombInfo.isInUse) &&
        20 < player->timers[bullet->timerIdx].timer.GetCurrent())
    {
        player->timers[bullet->timerIdx].timer = 20;
    }
    if (player->timers[bullet->timerIdx].timer <= 0)
    {
        player->timers[bullet->timerIdx].timer = 0;
        bullet->bulletState = 0;
        player->timers[bullet->timerIdx].bullet = NULL;
        return 1;
    }

    if (player->timers[bullet->timerIdx].timer <= 70 && bullet->vm.isStopped)
    {
        bullet->vm.pendingInterrupt = 1;
    }
    for (i = 0; i < bullet->trailLength; i++)
    {
        if (bullet->posHistory[i].x >= -900.0f)
        {
            player->bombDamageBoxes[i + 96].pos = bullet->posHistory[i];
            player->bombDamageBoxes[i + 96].lifetime = 1;
            player->bombDamageBoxes[i + 96].size = bullet->hitboxSize;
        }
    }
    for (i = 15; 0 < i; i--)
    {
        bullet->posHistory[i] = bullet->posHistory[i - 1];
    }
    bullet->posHistory[0] = bullet->pos;
    if (player->playerState == PLAYER_STATE_DEAD)
    {
        return 1;
    }
    else
    {
        bullet->pos = player->positionCenter;
        bullet->pos.x += bullet->offset.x;
        bullet->pos.z = 0.44f;
        bullet->vm.scale.y = (bullet->pos.y + 64.0f) / 14.0f;
        bullet->hitboxSize.y = player->positionCenter.y + 64.0f;
        bullet->pos.y = bullet->pos.y / 2.0f - 32.0f;
        return 0;
    }
}

// FUNCTION: TH07 0x0043ccb0
i32 ShtData::DrawBulletWithTrail(Player *player, PlayerBullet *bullet)
{
    i32 i;
    i32 origAlpha;

    origAlpha = bullet->vm.color.bytes.a;
    for (i = 0; i < bullet->trailLength; i++)
    {
        if (bullet->posHistory[i].x == -999.0f)
        {
            break;
        }

        bullet->vm.pos.x = bullet->posHistory[i].x;
        bullet->vm.pos.y = bullet->posHistory[i].y;
        bullet->vm.pos.z = bullet->posHistory[i].z;

        bullet->vm.color.bytes.a = origAlpha - origAlpha * i / bullet->trailLength;

        *bullet->GetVmPosX() += g_GameManager.arcadeRegionTopLeftPos.x;
        *bullet->GetVmPosY() += g_GameManager.arcadeRegionTopLeftPos.y;

        g_AnmManager->Draw(&bullet->vm);
    }
    bullet->vm.color.bytes.a = origAlpha;
    return 0;
}

// FUNCTION: TH07 0x0043cde0
i32 ShtData::OnMissileHit(Player *player, PlayerBullet *bullet,
                          D3DXVECTOR3 *pos)
{
    f32 angle;

    if (bullet->bulletState == 2)
    {
        if (bullet->timer.GetCurrent() % 2 != 0)
        {
            return 1;
        }
        bullet->damage = bullet->damage / 3;
        if (bullet->damage == 0)
        {
            bullet->damage = 1;
        }
        bullet->velocity.x *= 0.88f;
        bullet->velocity.y *= 0.88f;
    }
    else
    {
        angle = g_Rng.GetRandomFloatInRange(1.5707964f) - 2.3561945f;
        switch (bullet->vm.anmFileIdx -
                (player->initParam == 0
                     ? 0
                     : ANM_OFFSET_PLAYER2 - ANM_OFFSET_PLAYER))
        {
        case 1089:
            bullet->hitboxSize.x = 32.0f;
            bullet->hitboxSize.y = 32.0f;
            AngleToVector((D3DXVECTOR3 *)&bullet->velocity, angle, 4.0f);
            break;
        case 1090:
            bullet->hitboxSize.x = 42.0;
            bullet->hitboxSize.y = 42.0;
            AngleToVector((D3DXVECTOR3 *)&bullet->velocity, angle, 4.0f);
            break;
        case 1091:
            bullet->hitboxSize.x = 48.0f;
            bullet->hitboxSize.y = 48.0f;
            AngleToVector((D3DXVECTOR3 *)&bullet->velocity, angle, 4.0f);
            break;
        case 1092:
            bullet->hitboxSize.x = 56.0f;
            bullet->hitboxSize.y = 56.0f;
            AngleToVector((D3DXVECTOR3 *)&bullet->velocity, angle, 4.0f);
            break;
        case 1093:
            bullet->hitboxSize.x = 48.0f;
            bullet->hitboxSize.y = 48.0f;
            AngleToVector((D3DXVECTOR3 *)&bullet->velocity, angle, 6.0f);
            break;
        case 1094:
            bullet->hitboxSize.x = 64.0f;
            bullet->hitboxSize.y = 64.0f;
            AngleToVector((D3DXVECTOR3 *)&bullet->velocity, angle, 6.0f);
            break;
        case 1095:
            bullet->hitboxSize.x = 80.0f;
            bullet->hitboxSize.y = 80.0f;
            AngleToVector((D3DXVECTOR3 *)&bullet->velocity, angle, 6.0f);
            break;
        case 1096:
            bullet->hitboxSize.x = 96.0f;
            bullet->hitboxSize.y = 96.0f;
            AngleToVector((D3DXVECTOR3 *)&bullet->velocity, angle, 6.0f);
        }
    }
    if (bullet->timer.GetCurrent() % 6 == 0)
    {
        g_EffectManager.SpawnParticles(5, pos, 1, 0xffffffff);
    }
    return 0;
}

// FUNCTION: TH07 0x0043d0e0
i32 ShtData::SpawnHitParticles(Player *player, PlayerBullet *bullet,
                               D3DXVECTOR3 *pos)
{
    D3DXVECTOR3 particlePos;

    player->bombParticleTime++;
    if (player->bombParticleTime % 8 == 0)
    {
        particlePos = *pos;
        particlePos.x = bullet->pos.x;
        g_EffectManager.SpawnParticles(5, &particlePos, 1, 0xffffffff);
    }
    return 0;
}

#pragma var_order(i, level, bullet, ret, entry)
// FUNCTION: TH07 0x0043d160
void Player::SpawnBullets(Player *player, u32 timer)
{
    ShtEntry *entry;
    i32 ret;
    PlayerBullet *bullet;
    ShtLevel *level;
    i32 i;

    level = !player->isFocus ? &player->shooterData->levels
                             : &player->shooterDataFocus->levels;

    while (GetPlayerPower(player->initParam) >= level->requiredPower)
    {
        level++;
    }

    entry = level->entry;
    bullet = player->bullets;
    for (i = 0; i < 96; i++, bullet++)
    {
        if (bullet->bulletState != 0)
        {
            continue;
        }

    loop_with_goto_for_some_reason:
        if (entry->fireCallback)
        {
            ret = entry->fireCallback(player, bullet, timer, entry);
        }
        else
        {
            ret = ShtData::FireBulletDefault(player, bullet, timer, entry);
        }
        if (ret == 1)
        {
            bullet->vm.zWriteDisable = 1;
            bullet->bulletState = 1;
            bullet->shtEntry = entry;
            bullet->updateCallback = bullet->shtEntry->updateCallback;
            bullet->drawCallback = bullet->shtEntry->drawCallback;
            bullet->hitCallback = bullet->shtEntry->hitCallback;
        }
        entry++;
        if (entry->fireInterval < 0)
        {
            return;
        }

        if (!ret)
        {
            goto loop_with_goto_for_some_reason;
        }
    }
}

// FUNCTION: TH07 0x0043d2f0
void Player::UpdateShots()
{
    PlayerBullet *bullet;
    i32 i;

    if (this->optionState != OPTION_FOCUSED && this->timers[2].bullet)
    {
        this->timers[2].bullet->bulletState = 0;
        this->timers[2].bullet = NULL;
    }
    if (this->optionState != OPTION_UNFOCUSED)
    {
        if (this->timers[0].bullet)
        {
            this->timers[0].bullet->vm.pendingInterrupt = 1;
            this->timers[0].bullet = NULL;
        }
        if (this->timers[1].bullet)
        {
            this->timers[1].bullet->vm.pendingInterrupt = 1;
            this->timers[1].bullet = NULL;
        }
    }
    if (this->playerState == PLAYER_STATE_DEAD)
    {
        for (i = 0; i < 3; i++)
        {
            if (this->timers[i].bullet)
            {
                this->timers[i].bullet->bulletState = 0;
                this->timers[i].bullet = NULL;
            }
        }
    }
    for (i = 0; i < 3; i++)
    {
        if (!this->timers[i].bullet)
        {
            continue;
        }
        if (this->timers[i].timer.GetCurrent() > 0 &&
            this->timers[i].timer.GetCurrent() < 999)
        {
            this->timers[i].timer--;
        }
        if (this->fireBulletTimer.GetCurrent() < 0 &&
            this->timers[i].timer.GetCurrent() > 50)
        {
            this->timers[i].timer = 50;
        }
        if (this->timers[i].timer.GetCurrent() == 0)
        {
            this->timers[i].bullet = NULL;
        }
    }
    bullet = this->bullets;
    for (i = 0; i < 96; i++, bullet++)
    {
        if (bullet->bulletState == 0)
        {
            continue;
        }

        if (bullet->updateCallback &&
            bullet->updateCallback(this, bullet))
        {
            bullet->bulletState = 0;
            continue;
        }

        *bullet->GetPosX() +=
            bullet->velocity.x * g_Supervisor.effectiveFramerateMultiplier;
        *bullet->GetPosY() +=
            bullet->velocity.y * g_Supervisor.effectiveFramerateMultiplier;
        if (bullet->bulletState2 != 4 && bullet->bulletState2 != 5 &&
            !g_GameManager.IsInBounds(bullet->pos.x, bullet->pos.y,
                                      bullet->vm.sprite->widthPx,
                                      bullet->vm.sprite->heightPx))
        {
            bullet->bulletState = 0;
        }
        if (g_AnmManager->ExecuteScript(&bullet->vm))
        {
            bullet->bulletState = 0;
        }
        bullet->timer++;
    }
}

#pragma var_order(i, bullet)
// FUNCTION: TH07 0x0043d690
void Player::DrawBullets()
{
    PlayerBullet *bullet;
    i32 i;

    bullet = this->bullets;
    for (i = 0; i < 96; i++, bullet++)
    {
        if (bullet->bulletState != 1)
        {
            continue;
        }

        if (bullet->vm.autoRotate)
        {
            f32 angle = utils::AddNormalizeAngle(bullet->angle, 1.5707964f);
            bullet->vm.rotation.z = angle;
            bullet->vm.updateRotation = 1;
        }
        bullet->vm.pos.x = g_GameManager.arcadeRegionTopLeftPos.x + bullet->pos.x;
        bullet->vm.pos.y = g_GameManager.arcadeRegionTopLeftPos.y + bullet->pos.y;
        bullet->vm.pos.z = 0.4f;
        g_AnmManager->Draw(&bullet->vm);
        if (bullet->drawCallback)
        {
            bullet->drawCallback(this, bullet);
        }
    }
}

// FUNCTION: TH07 0x0043d790
void Player::DrawBulletExplosions()
{
    PlayerBullet *bullet;
    i32 i;

    bullet = this->bullets;
    for (i = 0; i < 96; i++, bullet++)
    {
        if (bullet->bulletState != 2)
        {
            continue;
        }

        if (bullet->vm.autoRotate)
        {
            f32 angle = utils::AddNormalizeAngle(bullet->angle, 1.5707964f);
            bullet->vm.rotation.z = angle;
            bullet->vm.updateRotation = 1;
        }
        bullet->vm.pos.x = g_GameManager.arcadeRegionTopLeftPos.x + bullet->pos.x;
        bullet->vm.pos.y = g_GameManager.arcadeRegionTopLeftPos.y + bullet->pos.y;
        bullet->vm.pos.z = 0.4f;
        g_AnmManager->Draw(&bullet->vm);
    }
}

// FUNCTION: TH07 0x0043d880
i32 Player::UpdateFireBulletTimer()
{
    if (this->fireBulletTimer.GetCurrent() < 0)
    {
        return 0;
    }
    if (this->fireBulletTimer.HasTicked() &&
        (!this->bombInfo.isInUse ||
         Netplay::GetPlayerCharacter(this->initParam) != CHAR_MARISA ||
         Netplay::GetPlayerShot(this->initParam) != 1))
    {
        SpawnBullets(this, this->fireBulletTimer.GetCurrent());
    }
    this->fireBulletTimer++;
    if (this->fireBulletTimer.GetCurrent() >= 30 ||
        this->playerState == PLAYER_STATE_DEAD ||
        this->playerState == PLAYER_STATE_SPAWNING)
    {

        this->fireBulletTimer = -1;
    }
    return 0;
}

// FUNCTION: TH07 0x0043d990
void Player::StartFireBulletTimer()
{
    if (this->fireBulletTimer.GetCurrent() < 0)
    {
        this->fireBulletTimer = 0;
    }
}

#pragma var_order(bullet, i, enemyBottomRight, bulletBottomRight, enemyTopLeft, damage, bulletTopLeft)
// FUNCTION: TH07 0x0043d9e0
i32 Player::CalcDamageToEnemy(D3DXVECTOR3 *center, D3DXVECTOR3 *size,
                              i32 *param_3)
{
    D3DXVECTOR3 bulletTopLeft;
    i32 damage;
    D3DXVECTOR3 enemyTopLeft;
    D3DXVECTOR3 bulletBottomRight;
    D3DXVECTOR3 enemyBottomRight;
    i32 i;
    PlayerBullet *bullet;

    damage = 0;
    if (!this->invulnerabilityTimer.HasTicked())
    {
        return 0;
    }

    enemyTopLeft.x = center->x - size->x * 0.5f;
    enemyTopLeft.y = center->y - size->y * 0.5f;
    enemyBottomRight.x = center->x + size->x * 0.5f;
    enemyBottomRight.y = center->y + size->y * 0.5f;

    bullet = this->bullets;
    if (param_3)
    {
        *param_3 = 0;
    }
    for (i = 0; i < 96; i++, bullet++)
    {
        if (bullet->bulletState == 0 ||
            (bullet->bulletState != 1 && bullet->bulletState2 != 3))
        {
            continue;
        }

        SetVecCorners(&bulletTopLeft, &bulletBottomRight, &bullet->pos, &bullet->hitboxSize);

        if (bulletTopLeft.y > enemyBottomRight.y ||
            bulletTopLeft.x > enemyBottomRight.x ||
            bulletBottomRight.y < enemyTopLeft.y ||
            bulletBottomRight.x < enemyTopLeft.x)
        {
            continue;
        }

        if (bullet->bulletState2 == 4 || bullet->bulletState2 == 5)
        {
            if (bullet->timer.current % 2 != 0)
            {
                continue;
            }
        }
        if (bullet->hitCallback &&
            bullet->hitCallback(this, bullet, center))
        {
            continue;
        }

        if (!this->bombInfo.isInUse)
        {
            damage += bullet->damage;
        }
        else
        {
            damage += bullet->damage / 3 != 0 ? bullet->damage / 3 : 1;
        }
        if (bullet->bulletState2 != 4 && bullet->bulletState2 != 5)
        {
            if (bullet->bulletState == 1)
            {
                g_AnmManager->SetAnmIdxAndExecuteScript(&bullet->vm, bullet->vm.anmFileIdx + 32);
                g_EffectManager.SpawnParticles(5, &bullet->pos, 1, 0xffffffff);
                bullet->pos.z = 0.1f;
            }
            bullet->bulletState = 2;
            if (bullet->bulletState2 != 3)
            {
                bullet->velocity.x /= 8.0f;
                bullet->velocity.y /= 8.0f;
            }
        }
    }
    for (i = 0; i < 112; i++)
    {
        if (this->bombDamageBoxes[i].size.x <= 0.0f)
        {
            continue;
        }

        bulletTopLeft = this->bombDamageBoxes[i].pos - this->bombDamageBoxes[i].size / 2.0f;
        bulletBottomRight = this->bombDamageBoxes[i].pos + this->bombDamageBoxes[i].size / 2.0f;

        if (bulletTopLeft.x > enemyBottomRight.x ||
            bulletBottomRight.x < enemyTopLeft.x ||
            bulletTopLeft.y > enemyBottomRight.y ||
            bulletBottomRight.y < enemyTopLeft.y)
        {
            continue;
        }

        damage += this->bombDamageBoxes[i].lifetime;
        this->bombDamageBoxes[i].damage += this->bombDamageBoxes[i].lifetime;
        this->bombParticleTime++;
        if (this->bombParticleTime % 4 == 0)
        {
            if (i < 96)
            {
                g_EffectManager.SpawnParticles(3, center, 1, 0xffffffff);
            }
            else
            {
                g_EffectManager.SpawnParticles(5, center, 1, 0xffffffff);
            }
        }
        if (this->bombInfo.isInUse && param_3)
        {
            *param_3 = 1;
        }
    }
    return damage;
}

#pragma var_order(bombTopLeft, bombY, bombX, i, bulletBottomRight, bulletTopLeft, bombProjectile, bombBottomRight)
// FUNCTION: TH07 0x0043e0a0
i32 Player::CheckBombGraze(D3DXVECTOR3 *center, D3DXVECTOR3 *size)
{
    BombClearBox *bombProjectile;
    i32 i;
    D3DXVECTOR3 bulletBottomRight;
    D3DXVECTOR3 bulletTopLeft;
    D3DXVECTOR3 bombBottomRight;
    D3DXVECTOR3 bombTopLeft;
    f32 bombY;
    f32 bombX;

    if (Netplay::IsPlayerTemporarilyAbsent(this->initParam))
    {
        return 0;
    }

    if (Netplay::IsMultiplayer() &&
        this->playerState != PLAYER_STATE_ALIVE &&
        this->playerState != PLAYER_STATE_INVULNERABLE &&
        this->playerState != PLAYER_STATE_BORDER)
    {
        return 0;
    }

    bombProjectile = this->bombClearBoxes;
    bulletTopLeft.x = center->x - size->x / 2.0f;
    bulletTopLeft.y = center->y - size->y / 2.0f;
    bulletBottomRight.x = center->x + size->x / 2.0f;
    bulletBottomRight.y = center->y + size->y / 2.0f;
    for (i = 0; i < 96; i++, bombProjectile++)
    {
        if (bombProjectile->pos.z != 0.0f)
        {
            bombTopLeft.x = bombProjectile->pos.x - bombProjectile->pos.z / 2.0f;
            bombTopLeft.y = bombProjectile->pos.y - bombProjectile->size.x / 2.0f;
            bombBottomRight.x = bombProjectile->pos.z / 2.0f + bombProjectile->pos.x;
            bombBottomRight.y = bombProjectile->size.x / 2.0f + bombProjectile->pos.y;
            if (!(bombTopLeft.x > bulletBottomRight.x ||
                  bombBottomRight.x < bulletTopLeft.x ||
                  bombTopLeft.y > bulletBottomRight.y ||
                  bombBottomRight.y < bulletTopLeft.y))
            {
                this->itemType = bombProjectile->itemType;
                return 2;
            }
        }
        else if (bombProjectile->size.y != 0.0) // double used here for some reason
        {
            bombX = center->x - bombProjectile->pos.x;
            bombY = center->y - bombProjectile->pos.y;
            if (bombX * bombX + bombY * bombY <
                bombProjectile->size.y * bombProjectile->size.y)
            {
                this->itemType = bombProjectile->itemType;
                return 2;
            }
        }
        else
        {
            continue; // ZUN bloat: this is completely pointless
        }
    }
    return 0;
}

#pragma var_order(killboxBottomRight, killboxTopLeft)
// FUNCTION: TH07 0x0043e260
i32 Player::CalcKillboxCollision(D3DXVECTOR3 *center, D3DXVECTOR3 *size)
{
    D3DXVECTOR3 killboxBottomRight;
    D3DXVECTOR3 killboxTopLeft;

    if (Netplay::IsPlayerTemporarilyAbsent(this->initParam))
    {
        return 0;
    }

    if (Netplay::IsMultiplayer() &&
        this->playerState != PLAYER_STATE_ALIVE &&
        this->playerState != PLAYER_STATE_INVULNERABLE &&
        this->playerState != PLAYER_STATE_BORDER)
    {
        return 0;
    }

    this->itemType = ITEM_POINT_BULLET;
    if (CheckBombGraze(center, size))
    {
        return 2;
    }
    if (Netplay::IsInvincible())
    {
        return 0;
    }

    killboxTopLeft.x = center->x - size->x / 2.0f;
    killboxTopLeft.y = center->y - size->y / 2.0f;
    killboxBottomRight.x = center->x + size->x / 2.0f;
    killboxBottomRight.y = center->y + size->y / 2.0f;
    if (this->hitboxTopLeft.x > killboxBottomRight.x ||
        this->hitboxTopLeft.y > killboxBottomRight.y ||
        this->hitboxBottomRight.x < killboxTopLeft.x ||
        this->hitboxBottomRight.y < killboxTopLeft.y)
    {
        return 0;
    }

    g_ReplayManager->replayEventFlags = g_ReplayManager->replayEventFlags | 2;
    if (this->playerState == PLAYER_STATE_BORDER)
    {
        this->BreakBorder(0);
        return 1;
    }
    if (this->playerState != PLAYER_STATE_ALIVE)
    {
        return 1;
    }

    g_GameManager.RerollRng();
    Die();
    return 1;
}

#pragma var_order(bulletBottomRight, bulletTopLeft)
// FUNCTION: TH07 0x0043e3b0
i32 Player::CheckGraze(D3DXVECTOR3 *center, D3DXVECTOR3 *size)
{
    D3DXVECTOR3 bulletBottomRight;
    D3DXVECTOR3 bulletTopLeft;

    if (Netplay::IsPlayerTemporarilyAbsent(this->initParam))
    {
        return 0;
    }

    if (Netplay::IsMultiplayer() &&
        this->playerState != PLAYER_STATE_ALIVE &&
        this->playerState != PLAYER_STATE_INVULNERABLE &&
        this->playerState != PLAYER_STATE_BORDER)
    {
        return 0;
    }

    this->itemType = ITEM_POINT_BULLET;

    if (CheckBombGraze(center, size))
    {
        return 2;
    }

    bulletTopLeft.x = center->x - size->x / 2.0f - 20.0f;
    bulletTopLeft.y = center->y - size->y / 2.0f - 20.0f;
    bulletBottomRight.x = center->x + size->x / 2.0f + 20.0f;
    bulletBottomRight.y = center->y + size->y / 2.0f + 20.0f;

    if (this->playerState == PLAYER_STATE_DEAD ||
        this->playerState == PLAYER_STATE_SPAWNING)
    {
        return 0;
    }

    if (this->grazeTopLeft.x > bulletBottomRight.x || this->grazeBottomRight.x < bulletTopLeft.x ||
        this->grazeTopLeft.y > bulletBottomRight.y || this->grazeBottomRight.y < bulletTopLeft.y)
    {
        return 0;
    }

    ScoreGraze(center);
    return 1;
}

#pragma var_order(itemBottomRight, itemTopLeft)
// FUNCTION: TH07 0x0043e4e0
i32 Player::CalcItemBoxCollision(D3DXVECTOR3 *center, D3DXVECTOR3 *size)
{
    D3DXVECTOR3 itemBottomRight;
    D3DXVECTOR3 itemTopLeft;

    if (Netplay::IsPlayerTemporarilyAbsent(this->initParam))
    {
        return 0;
    }

    if (this->playerState != PLAYER_STATE_ALIVE &&
        this->playerState != PLAYER_STATE_INVULNERABLE &&
        this->playerState != PLAYER_STATE_BORDER)
    {
        return 0;
    }

    memcpy(&itemTopLeft, &(*center - *size / 2.0f), sizeof(D3DXVECTOR3));
    memcpy(&itemBottomRight, &(*center + *size / 2.0f), sizeof(D3DXVECTOR3));

    if (this->grabItemTopLeft.x > itemBottomRight.x ||
        this->grabItemBottomRight.x < itemTopLeft.x ||
        this->grabItemTopLeft.y > itemBottomRight.y ||
        this->grabItemBottomRight.y < itemTopLeft.y)
    {
        return 0;
    }

    return 1;
}

#pragma var_order(playerRelativeTopLeft, laserBottomRight, laserTopLeft, playerRelativeBottomRight)
// FUNCTION: TH07 0x0043e6b0
i32 Player::CalcLaserHitbox(D3DXVECTOR3 *center, D3DXVECTOR3 *size,
                            D3DXVECTOR3 *origin, f32 rotation, i32 canGraze)
{
    D3DXVECTOR3 playerRelativeTopLeft;
    D3DXVECTOR3 playerRelativeBottomRight;
    D3DXVECTOR3 laserTopLeft;
    D3DXVECTOR3 laserBottomRight;

    if (Netplay::IsPlayerTemporarilyAbsent(this->initParam))
    {
        return 0;
    }

    if (Netplay::IsMultiplayer() &&
        this->playerState != PLAYER_STATE_ALIVE &&
        this->playerState != PLAYER_STATE_INVULNERABLE &&
        this->playerState != PLAYER_STATE_BORDER)
    {
        return 0;
    }

    laserTopLeft = this->positionCenter - *origin;
    utils::Rotate(&laserBottomRight, &laserTopLeft, rotation);
    laserBottomRight.z = 0;
    laserTopLeft = laserBottomRight + *origin;
    playerRelativeTopLeft = laserTopLeft - this->hitboxSize;
    playerRelativeBottomRight = laserTopLeft + this->hitboxSize;

    laserTopLeft = *center - *size / 2.0f;
    laserBottomRight = *center + *size / 2.0f;
    if (!(playerRelativeTopLeft.x > laserBottomRight.x ||
          playerRelativeBottomRight.x < laserTopLeft.x ||
          playerRelativeTopLeft.y > laserBottomRight.y ||
          playerRelativeBottomRight.y < laserTopLeft.y))
    {
        goto LASER_COLLISION;
    }

    if (!canGraze)
    {
        return 0;
    }

    laserTopLeft.x -= 48.0f;
    laserTopLeft.y -= 48.0f;
    laserBottomRight.x += 48.0f;
    laserBottomRight.y += 48.0f;
    if (playerRelativeTopLeft.x > laserBottomRight.x ||
        playerRelativeBottomRight.x < laserTopLeft.x ||
        playerRelativeTopLeft.y > laserBottomRight.y ||
        playerRelativeBottomRight.y < laserTopLeft.y)
    {
        return 0;
    }

    if (this->playerState == PLAYER_STATE_DEAD ||
        this->playerState == PLAYER_STATE_SPAWNING)
    {
        return 0;
    }

    ScoreGraze(&this->positionCenter);
    return 2;

LASER_COLLISION:
    if (Netplay::IsInvincible())
    {
        return 0;
    }
    g_ReplayManager->replayEventFlags = g_ReplayManager->replayEventFlags | 2;
    if (this->playerState == PLAYER_STATE_BORDER)
    {
        // this is already a member function of Player though
        this->BreakBorder(0);
        return 1;
    }
    if (this->playerState != PLAYER_STATE_ALIVE)
    {
        return 0;
    }

    g_GameManager.RerollRng();
    Die();
    return 1;
}

// FUNCTION: TH07 0x0043eb90
void Player::ScoreGraze(D3DXVECTOR3 *param_1)
{
    D3DXVECTOR3 grazePos;

    if (!this->bombInfo.isInUse)
    {
        if (g_GameManager.globals->grazeInStage < 9999)
        {
            g_GameManager.globals->grazeInStage++;
        }
        if (g_GameManager.globals->grazeInTotal < 999999)
        {
            g_GameManager.globals->grazeInTotal++;
        }
    }
    grazePos = (this->positionCenter + *param_1) / 2.0f;
    if (this->hasBorder == BORDER_ACTIVE)
    {
        if (this->isFocus)
        {
            g_EffectManager.SpawnParticles(8, &grazePos, 1, 0xffffffff);
        }
        else
        {
            g_EffectManager.SpawnParticles(8, &grazePos, 3, 0xffff8080);
        }
    }
    else
    {
        g_EffectManager.SpawnParticles(8, &grazePos, 1, 0xffffffff);
    }
    g_GameManager.IncreaseSubrank(6);
    g_Gui.showGraze = 2;
    g_SoundPlayer.PlaySoundByIdx(SOUND_GRAZE, 0);
    g_EnemyManager.spellcardInfo.grazeBonusScore =
        g_EnemyManager.spellcardInfo.grazeBonusScore + 2500 +
        (g_GameManager.cherry - g_GameManager.globals->cherryStart) / 1500 * 20;
    g_GameManager.AddScore(2000);
    if (this->hasBorder == BORDER_ACTIVE)
    {
        i32 grazeGrowth = this->isFocus ? 30 : 80;
        if (this->initParam >= 0 &&
            this->initParam < TH07_MULTI_MAX_PLAYERS)
        {
            g_cherryMaxGrazeGrowth[this->initParam] += grazeGrowth;
        }
        g_GameManager.IncreaseCherryMax(grazeGrowth);
        g_GameManager.IncreaseCherry(grazeGrowth);
    }
}

// FUNCTION: TH07 0x0043edc0
void Player::Die()
{
    if (Netplay::IsInvincible())
    {
        return;
    }
    Netplay::ReportDamageEventHit(
        this->initParam, GetPlayerLives(this->initParam), this->playerState);
    g_GameManager.RegenerateGameIntegrityCsum();
    g_EffectManager.SpawnEffect(12, &this->positionCenter,
                                GetPlayerEffectSlot(this, 3), 1,
                                0xff4040ff);
    g_EffectManager.SpawnParticles(6, &this->positionCenter, 16, 0xffffffff);
    this->playerState = PLAYER_STATE_DEAD;
    this->invulnerabilityTimer = 0;
    g_SoundPlayer.PlaySoundByIdx(SOUND_PICHUN, 0);
}

#pragma var_order(direction, verticalSpeed, horizontalSpeed, optionOffsetY, \
                  optionOffsetX, t, targetOffsetY, targetOffsetX, angleStep)
// FUNCTION: TH07 0x0043ee50
i32 Player::HandlePlayerInputs()
{
    f32 angleStep;
    f32 targetOffsetX;
    f32 targetOffsetY;
    f32 t;
    f32 optionOffsetX;
    f32 optionOffsetY;
    f32 horizontalSpeed;
    f32 verticalSpeed;
    PlayerDirection direction;

    horizontalSpeed = 0.0f;
    verticalSpeed = 0.0f;
    direction = this->playerDirection;
    this->playerDirection = MOVEMENT_NONE;

    if (IS_PRESSED_PLAYER(this, TH_BUTTON_UP))
    {
        this->playerDirection = MOVEMENT_UP;
        if (IS_PRESSED_PLAYER(this, TH_BUTTON_LEFT))
        {
            this->playerDirection = MOVEMENT_UP_LEFT;
        }
        if (IS_PRESSED_PLAYER(this, TH_BUTTON_RIGHT))
        {
            this->playerDirection = MOVEMENT_UP_RIGHT;
        }
    }
    else if (IS_PRESSED_PLAYER(this, TH_BUTTON_DOWN))
    {
        this->playerDirection = MOVEMENT_DOWN;
        if (IS_PRESSED_PLAYER(this, TH_BUTTON_LEFT))
        {
            this->playerDirection = MOVEMENT_DOWN_LEFT;
        }
        if (IS_PRESSED_PLAYER(this, TH_BUTTON_RIGHT))
        {
            this->playerDirection = MOVEMENT_DOWN_RIGHT;
        }
    }
    else
    {
        if (IS_PRESSED_PLAYER(this, TH_BUTTON_LEFT))
        {
            this->playerDirection = MOVEMENT_LEFT;
        }
        if (IS_PRESSED_PLAYER(this, TH_BUTTON_RIGHT))
        {
            this->playerDirection = MOVEMENT_RIGHT;
        }
    }

    if (IS_PRESSED_PLAYER(this, TH_BUTTON_FOCUS))
    {
        this->isFocus = 1;
        switch (this->playerDirection)
        {
        case MOVEMENT_RIGHT:
            horizontalSpeed = this->shooterData->speedFocus;
            break;
        case MOVEMENT_LEFT:
            horizontalSpeed = -this->shooterData->speedFocus;
            break;
        case MOVEMENT_UP:
            verticalSpeed = -this->shooterData->speedFocus;
            break;
        case MOVEMENT_DOWN:
            verticalSpeed = this->shooterData->speedFocus;
            break;
        case MOVEMENT_UP_LEFT:
            horizontalSpeed = -this->shooterData->speedDiagonalFocus;
            verticalSpeed = horizontalSpeed;
            break;
        case MOVEMENT_DOWN_LEFT:
            verticalSpeed = this->shooterData->speedDiagonalFocus;
            horizontalSpeed = -verticalSpeed;
            break;
        case MOVEMENT_UP_RIGHT:
            horizontalSpeed = this->shooterData->speedDiagonalFocus;
            verticalSpeed = -horizontalSpeed;
            break;
        case MOVEMENT_DOWN_RIGHT:
            horizontalSpeed = this->shooterData->speedDiagonalFocus;
            verticalSpeed = horizontalSpeed;
            break;
        }
    }
    else
    {
        this->isFocus = 0;
        switch (this->playerDirection)
        {
        case MOVEMENT_RIGHT:
            horizontalSpeed = this->shooterData->speed;
            break;
        case MOVEMENT_LEFT:
            horizontalSpeed = -this->shooterData->speed;
            break;
        case MOVEMENT_UP:
            verticalSpeed = -this->shooterData->speed;
            break;
        case MOVEMENT_DOWN:
            verticalSpeed = this->shooterData->speed;
            break;
        case MOVEMENT_UP_LEFT:
            horizontalSpeed = -this->shooterData->speedDiagonal;
            verticalSpeed = horizontalSpeed;
            break;
        case MOVEMENT_DOWN_LEFT:
            verticalSpeed = this->shooterData->speedDiagonal;
            horizontalSpeed = -verticalSpeed;
            break;
        case MOVEMENT_UP_RIGHT:
            horizontalSpeed = this->shooterData->speedDiagonal;
            verticalSpeed = -horizontalSpeed;
            break;
        case MOVEMENT_DOWN_RIGHT:
            horizontalSpeed = this->shooterData->speedDiagonal;
            verticalSpeed = horizontalSpeed;
            break;
        }
    }

    if (horizontalSpeed < 0.0f && this->previousHorizontalSpeed >= 0.0f)
    {
        g_AnmManager->SetAnmIdxAndExecuteScript(
            &this->playerSprite, GetPlayerAnmScript(this, 1025));
    }
    else if (horizontalSpeed == 0.0f && this->previousHorizontalSpeed < 0.0f)
    {
        g_AnmManager->SetAnmIdxAndExecuteScript(
            &this->playerSprite, GetPlayerAnmScript(this, 1026));
    }

    if (horizontalSpeed > 0.0f && this->previousHorizontalSpeed <= 0.0f)
    {
        g_AnmManager->SetAnmIdxAndExecuteScript(
            &this->playerSprite, GetPlayerAnmScript(this, 1027));
    }
    else if (horizontalSpeed == 0.0f && this->previousHorizontalSpeed > 0.0f)
    {
        g_AnmManager->SetAnmIdxAndExecuteScript(
            &this->playerSprite, GetPlayerAnmScript(this, 1028));
    }

    this->previousHorizontalSpeed = horizontalSpeed;
    this->previousVerticalSpeed = verticalSpeed;
    this->velocity.x = horizontalSpeed *
                       this->horizontalMovementSpeedMultiplierDuringBomb *
                       g_Supervisor.effectiveFramerateMultiplier;
    this->velocity.y = verticalSpeed *
                       this->verticalMovementSpeedMultiplierDuringBomb *
                       g_Supervisor.effectiveFramerateMultiplier;
    *GetPosCenterX() += this->velocity.x;
    *GetPosCenterY() += this->velocity.y;

    if (this->positionCenter.x < g_GameManager.playerMovementAreaTopLeftPos.x)
    {
        this->positionCenter.x = g_GameManager.playerMovementAreaTopLeftPos.x;
    }
    else if (this->positionCenter.x > g_GameManager.playerMovementAreaTopLeftPos.x + g_GameManager.playerMovementAreaSize.x)
    {
        this->positionCenter.x = g_GameManager.playerMovementAreaTopLeftPos.x + g_GameManager.playerMovementAreaSize.x;
    }

    if (this->positionCenter.y < g_GameManager.playerMovementAreaTopLeftPos.y)
    {
        this->positionCenter.y = g_GameManager.playerMovementAreaTopLeftPos.y;
    }
    else if (this->positionCenter.y > g_GameManager.playerMovementAreaTopLeftPos.y + g_GameManager.playerMovementAreaSize.y)
    {
        this->positionCenter.y = g_GameManager.playerMovementAreaTopLeftPos.y + g_GameManager.playerMovementAreaSize.y;
    }

    this->hitboxTopLeft = this->positionCenter - this->hitboxSize;
    this->hitboxBottomRight = this->positionCenter + this->hitboxSize;
    this->grazeTopLeft = this->positionCenter - this->grazeSize;
    this->grazeBottomRight = this->positionCenter + this->grazeSize;
    this->grabItemTopLeft = this->positionCenter - this->grabItemSize;
    this->grabItemBottomRight = this->positionCenter + this->grabItemSize;
    this->optionsPosition[0] = this->positionCenter;
    this->optionsPosition[1] = this->positionCenter;
    optionOffsetX = optionOffsetY = 0.0f;

    if (Netplay::GetPlayerCharacter(this->initParam) != CHAR_SAKUYA ||
        Netplay::GetPlayerShot(this->initParam) != 1)
    {
        switch (this->optionState)
        {
        case OPTION_HIDDEN:
            this->focusMovementTimer = 0;
            break;
        case OPTION_UNFOCUSED:
            optionOffsetX = 24.0f;
            this->focusMovementTimer = 0;
            if (this->isFocus)
            {
                this->optionState = OPTION_FOCUSING;
                this->focusEffect = g_EffectManager.SpawnEffect(
                    24, &this->positionCenter,
                    GetPlayerEffectSlot(this, 2), 1, 0xffffffff);
            }
            else
            {
                break;
            }
        CASE_OPTION_FOCUSING:
        case OPTION_FOCUSING:
            this->focusMovementTimer++;
            t = this->focusMovementTimer.AsFloat() / 8.0f;
            optionOffsetY = -32.0f + (1.0f - t) * 32.0f;
            t *= t;
            optionOffsetX = -16.0f * t + 24.0f;
            if (this->focusMovementTimer >= 8)
            {
                this->optionState = OPTION_FOCUSED;
            }
            if (!this->isFocus)
            {
                this->optionState = OPTION_UNFOCUSING;
                this->focusMovementTimer = 8 - this->focusMovementTimer.GetCurrent();
                if (this->focusEffect)
                {
                    this->focusEffect->vm.SetInterrupt(1);
                }
                goto CASE_OPTION_UNFOCUSING;
            }
            break;
        case OPTION_FOCUSED:
            optionOffsetX = 8.0f;
            optionOffsetY = -32.0f;
            this->focusMovementTimer = 0;
            if (!this->isFocus)
            {
                this->optionState = OPTION_UNFOCUSING;
                if (this->focusEffect)
                {
                    this->focusEffect->vm.SetInterrupt(1);
                }
                goto CASE_OPTION_UNFOCUSING;
            }
            break;
        CASE_OPTION_UNFOCUSING:
        case OPTION_UNFOCUSING:
            this->focusMovementTimer++;
            t = this->focusMovementTimer.AsFloat() / 8.0f;
            optionOffsetY = -32.0f + 32.0f * t;
            t *= t;
            t = 1.0f - t;
            optionOffsetX = -16.0f * t + 24.0f;
            if (this->focusMovementTimer >= 8)
            {
                this->optionState = OPTION_UNFOCUSED;
            }
            if (this->isFocus)
            {
                this->optionState = OPTION_FOCUSING;
                this->focusMovementTimer = 8 - this->focusMovementTimer.GetCurrent();
                this->focusEffect = g_EffectManager.SpawnEffect(
                    24, &this->positionCenter,
                    GetPlayerEffectSlot(this, 2), 1, 0xffffffff);
                goto CASE_OPTION_FOCUSING;
            }
        }
        this->optionsPosition[0].x -= optionOffsetX;
        this->optionsPosition[1].x += optionOffsetX;
        this->optionsPosition[0].y += optionOffsetY;
        this->optionsPosition[1].y += optionOffsetY;
    }
    else
    {
        switch (this->optionState)
        {
        case OPTION_HIDDEN:
            this->focusMovementTimer = 0;
            break;
        case OPTION_UNFOCUSED:
            optionOffsetX = cosf(this->optionAngle + 1.5707964f) * 24.0f;
            optionOffsetY = sinf(this->optionAngle + 1.5707964f) * 24.0f;
            this->focusMovementTimer = 0;
            if (this->isFocus)
            {
                this->optionState = OPTION_FOCUSING;
                this->focusEffect = g_EffectManager.SpawnEffect(
                    24, &this->positionCenter,
                    GetPlayerEffectSlot(this, 2), 1, 0xffffffff);
                goto CASE_OPTION_FOCUSING_2;
            }
            this->optionsPosition[0].x -= optionOffsetX;
            this->optionsPosition[1].x += optionOffsetX;
            this->optionsPosition[0].y -= optionOffsetY;
            this->optionsPosition[1].y += optionOffsetY;
            break;
        CASE_OPTION_FOCUSING_2:
        case OPTION_FOCUSING:
            if (!this->isFocus)
            {
                this->optionState = OPTION_UNFOCUSING;
                this->focusMovementTimer = 8 - this->focusMovementTimer.GetCurrent();
                if (this->focusEffect)
                {
                    this->focusEffect->vm.SetInterrupt(1);
                }
                goto CASE_OPTION_UNFOCUSING_2;
            }
            this->focusMovementTimer++;
            t = this->focusMovementTimer.AsFloat() / 8.0f;
            optionOffsetX = cosf(this->optionAngle + 1.5707964f) * 24.0f;
            optionOffsetY = sinf(this->optionAngle + 1.5707964f) * 24.0f;
            targetOffsetX = cosf(this->optionAngle + 0.22439948f) * 24.0f;
            targetOffsetY = sinf(this->optionAngle + 0.22439948f) * 24.0f;
            targetOffsetX = (targetOffsetX - optionOffsetX) * t + optionOffsetX;
            targetOffsetY = (targetOffsetY - optionOffsetY) * t + optionOffsetY;
            this->optionsPosition[1].x += targetOffsetX;
            this->optionsPosition[1].y += targetOffsetY;
            targetOffsetX = cosf(this->optionAngle - 0.22439948f) * 24.0f;
            targetOffsetY = sinf(this->optionAngle - 0.22439948f) * 24.0f;
            targetOffsetX = (targetOffsetX + optionOffsetX) * t - optionOffsetX;
            targetOffsetY = (targetOffsetY + optionOffsetY) * t - optionOffsetY;
            if (this->focusMovementTimer >= 8)
            {
                this->optionState = OPTION_FOCUSED;
            }
            this->optionsPosition[0].x += targetOffsetX;
            this->optionsPosition[0].y += targetOffsetY;
            break;
        case OPTION_FOCUSED:
            this->focusMovementTimer = 0;
            if (!this->isFocus)
            {
                this->optionState = OPTION_UNFOCUSING;
                if (this->focusEffect)
                {
                    this->focusEffect->vm.SetInterrupt(1);
                }
                goto CASE_OPTION_UNFOCUSING_2;
            }
            targetOffsetX = cosf(this->optionAngle + 0.22439948f) * 24.0f;
            targetOffsetY = sinf(this->optionAngle + 0.22439948f) * 24.0f;
            this->optionsPosition[1].x += targetOffsetX;
            this->optionsPosition[1].y += targetOffsetY;
            targetOffsetX = cosf(this->optionAngle - 0.22439948f) * 24.0f;
            targetOffsetY = sinf(this->optionAngle - 0.22439948f) * 24.0f;
            this->optionsPosition[0].x += targetOffsetX;
            this->optionsPosition[0].y += targetOffsetY;
            break;
        CASE_OPTION_UNFOCUSING_2:
        case OPTION_UNFOCUSING:
            if (this->isFocus)
            {
                this->optionState = OPTION_FOCUSING;
                this->focusMovementTimer = 8 - this->focusMovementTimer.GetCurrent();
                this->focusEffect = g_EffectManager.SpawnEffect(
                    24, &this->positionCenter,
                    GetPlayerEffectSlot(this, 2), 1, 0xffffffff);
                goto CASE_OPTION_FOCUSING_2;
            }
            this->focusMovementTimer++;
            t = 1.0f - this->focusMovementTimer.AsFloat() / 8.0f;
            optionOffsetX = cosf(this->optionAngle + 1.5707964f) * 24.0f;
            optionOffsetY = sinf(this->optionAngle + 1.5707964f) * 24.0f;
            targetOffsetX = cosf(this->optionAngle + 0.22439948f) * 24.0f;
            targetOffsetY = sinf(this->optionAngle + 0.22439948f) * 24.0f;
            targetOffsetX = (targetOffsetX - optionOffsetX) * t + optionOffsetX;
            targetOffsetY = (targetOffsetY - optionOffsetY) * t + optionOffsetY;
            this->optionsPosition[1].x += targetOffsetX;
            this->optionsPosition[1].y += targetOffsetY;
            targetOffsetX = cosf(this->optionAngle - 0.22439948f) * 24.0f;
            targetOffsetY = sinf(this->optionAngle - 0.22439948f) * 24.0f;
            targetOffsetX = (targetOffsetX + optionOffsetX) * t - optionOffsetX;
            targetOffsetY = (targetOffsetY + optionOffsetY) * t - optionOffsetY;
            if (this->focusMovementTimer >= 8)
            {
                this->optionState = OPTION_UNFOCUSED;
            }
            this->optionsPosition[0].x += targetOffsetX;
            this->optionsPosition[0].y += targetOffsetY;
            break;
        }
    }
    if (IS_PRESSED_PLAYER(this, TH_BUTTON_SHOOT) && !g_Gui.HasCurrentMsgIdx())
    {
        if (!g_GameManager.CheckGameIntegrity())
        {
            StartFireBulletTimer();
        }
        if (!IS_PRESSED_PLAYER(this, TH_BUTTON_FOCUS))
        {
            if (this->velocity.x != 0.0f)
            {
                angleStep = -(this->velocity.x / 4.0f) * ZUN_PI / 5.0f / 10.0f;
                this->optionAngle -= angleStep;
                if (this->optionAngle < -2.1991148f)
                {
                    this->optionAngle = -2.1991148f;
                }
                else if (this->optionAngle > -0.9424778f)
                {
                    this->optionAngle = -0.9424778f;
                }
            }
            else
            {
                if (fabsf(this->optionAngle - -1.5707964f) > 0.03141593f)
                {
                    angleStep = this->optionAngle < -1.5707964f
                                    ? 0.06283186f * g_Supervisor.effectiveFramerateMultiplier
                                    : -0.06283186f * g_Supervisor.effectiveFramerateMultiplier;
                    this->optionAngle += angleStep;
                }
                else
                {
                    this->optionAngle = -1.5707964f;
                }
            }
        }
    }
    return 0;
}

#pragma var_order(i, bomb)
// FUNCTION: TH07 0x00440940
void Player::UpdateBombProjectiles()
{
    BombClearBox *bomb;
    i32 i;

    for (i = 0; i < 112; i++)
    {
        this->bombDamageBoxes[i].size.x = 0.0f;
    }
    bomb = this->bombClearBoxes;
    for (i = 0; i < 96; i++, bomb++)
    {
        if (bomb->lifetime <= 0)
        {
            bomb->size.y = 0.0f;
            bomb->pos.z = 0.0f;
        }
        else
        {
            bomb->lifetime--;
            bomb->size.y += bomb->size.z;
        }
    }
}

// FUNCTION: TH07 0x004409f0
void Player::UpdateBorderAndBombState()
{
    if (this->hasBorder != BORDER_NONE && !this->bombInfo.isInUse &&
        IS_PRESSED_PLAYER(this, TH_BUTTON_BOMB))
    {
        BreakBorder(1);
        this->isBombing = 0;
        g_ItemManager.RemoveAllItems();
    }
    else
    {
        if (this->hasBorder == BORDER_READY)
        {
            ActivateBorder();
        }
        if (this->borderInvulnerabilityTime != 0)
        {
            this->borderInvulnerabilityTime--;
        }
        if (this->bombInfo.isInUse)
        {
            if (this->bombInfo.bombTimer.HasTicked())
            {
                PlayerBombInfo::SubtractCherryDrain(this->bombInfo.cherryDrain);
                g_Gui.showPoint = 2;
            }
            if (!this->bombInfo.isFocus)
            {
                this->bombInfo.bombCalc(this);
            }
            else
            {
                this->bombInfo.bombFocusCalc(this);
            }
        }
        else
        {
            if (!g_GameManager.CheckGameIntegrity() &&
                !g_Gui.HasCurrentMsgIdx() &&
                this->respawnTimer != 0 &&
                0 < GetPlayerBombs(this->initParam) &&
                this->borderInvulnerabilityTime == 0 &&
                IS_PRESSED_PLAYER(this, TH_BUTTON_BOMB))
            {
                g_ReplayManager->replayEventFlags |= 1;
                g_GameManager.AddBombsUsed(1);
                AddPlayerBombs(this->initParam, -1);
                g_Gui.showBombs = 2;
                this->bombInfo.isFocus = (i32)this->isFocus;
                this->bombInfo.isInUse = 1;
                this->isBombing = 1;
                this->bombInfo.bombTimer = 0;
                this->bombInfo.bombDuration = 999;
                if (!this->bombInfo.isFocus)
                {
                    this->bombInfo.bombCalc(this);
                }
                else
                {
                    this->bombInfo.bombFocusCalc(this);
                }
                g_EnemyManager.spellcardInfo.captureScore = 0;
                g_EnemyManager.spellcardInfo.isCapturing = 0;
                g_GameManager.DecreaseSubrank(200);
                g_EnemyManager.spellcardInfo.usedBomb =
                    g_EnemyManager.spellcardInfo.isActive;
                this->respawnTimer += 6;
                if (this->respawnTimer > this->shooterData->initialRespawnTimer)
                {
                    this->respawnTimer = this->shooterData->initialRespawnTimer;
                }
            }
            else
            {
                this->isBombing = 0;
            }
        }
    }
}

// FUNCTION: TH07 0x00440cf0
i32 Player::UpdateDeath()
{
    f32 invulnScale;
    i32 cherryPenalty;
    i32 playerLives;
    i32 recipientId;

    if (this->respawnTimer != 0)
    {
        if (this->hasBorder == BORDER_ACTIVE)
        {
            BreakBorder(0);
            return 0;
        }
        this->respawnTimer--;
        if (this->respawnTimer == 0)
        {
            g_ReplayManager->replayEventFlags |= 4;
            g_GameManager.powerItemCountForScore = 0;
            g_EnemyManager.spellcardInfo.captureScore = 0;
            g_EnemyManager.spellcardInfo.isCapturing = 0;
            g_GameManager.CheckGameIntegrityOnDeath(1);
            playerLives = GetPlayerLives(this->initParam);
            if (playerLives > 0)
            {
                if (GetPlayerPower(this->initParam) <= 16)
                {
                    SetPlayerPower(this->initParam, 0);
                }
                else
                {
                    AddPlayerPower(this->initParam, -16);
                }
                g_ItemManager.SpawnItem(&this->positionCenter, ITEM_POWER_BIG, 2);
                g_ItemManager.SpawnItem(&this->positionCenter, ITEM_POWER_SMALL, 2);
                g_ItemManager.SpawnItem(&this->positionCenter, ITEM_POWER_SMALL, 2);
                g_ItemManager.SpawnItem(&this->positionCenter, ITEM_POWER_SMALL, 2);
                g_ItemManager.SpawnItem(&this->positionCenter, ITEM_POWER_SMALL, 2);
                g_ItemManager.SpawnItem(&this->positionCenter, ITEM_POWER_SMALL, 2);
                g_Gui.showPower = 2;
                cherryPenalty =
                    (f32)(g_GameManager.cherry - g_GameManager.globals->cherryStart) *
                    this->shooterData->cherryPenaltyMultiplier;
                if (Netplay::GetPlayerCharacter(this->initParam) != CHAR_SAKUYA)
                {
                    if (cherryPenalty > 100000)
                    {
                        cherryPenalty = 100000;
                    }
                }
                else if (cherryPenalty > 60000)
                {
                    cherryPenalty = 60000;
                }
                cherryPenalty -= cherryPenalty % 10;
                g_GameManager.cherry -= cherryPenalty;
                g_Gui.showPoint = 2;
                g_ItemManager.ActivateAllItems();
            }
            else
            {
                SetPlayerPower(this->initParam, 0);
                g_ItemManager.SpawnItem(&this->positionCenter, ITEM_FULL_POWER, 2);
                g_ItemManager.SpawnItem(&this->positionCenter, ITEM_FULL_POWER, 2);
                g_ItemManager.SpawnItem(&this->positionCenter, ITEM_FULL_POWER, 2);
                g_ItemManager.SpawnItem(&this->positionCenter, ITEM_FULL_POWER, 2);
                g_ItemManager.SpawnItem(&this->positionCenter, ITEM_FULL_POWER, 2);
                g_Gui.showPower = 2;
            }
            g_GameManager.DecreaseSubrank(1600);
        }
    }
    else
    {
        invulnScale = this->invulnerabilityTimer.AsFloat() /
                      30.0f;
        this->playerSprite.scale.y = 3.0f * invulnScale + 1.0f;
        this->playerSprite.scale.x = 1.0f - 1.0f * invulnScale;
        this->playerSprite.color.color =
            (u32)(255.0f - this->invulnerabilityTimer.AsFloat() *
                               255.0f /
                               30.0f)
                << 24 |
            0xffffff;
        this->playerSprite.blendMode = 1;
        this->previousHorizontalSpeed = 0.0f;
        this->previousVerticalSpeed = 0.0f;
        if (this->invulnerabilityTimer.GetCurrent() >= 30)
        {
            this->playerState = PLAYER_STATE_SPAWNING;
            this->positionCenter.x =
                g_GameManager.arcadeRegionSize.x / 2.0f +
                (Netplay::GetPlayerCount() >= 3
                     ? ((i32)this->initParam - 1) * 48.0f
                     : (this->initParam == 0 ? -32.0f : 32.0f));
            this->positionCenter.y = g_GameManager.arcadeRegionSize.y - 64.0f;
            this->positionCenter.z = 0.2f;
            this->invulnerabilityTimer = 0;
            this->playerSprite.scale.x = 3.0f;
            this->playerSprite.scale.y = 3.0f;
            g_AnmManager->SetAnmIdxAndExecuteScript(
                &this->playerSprite, GetPlayerAnmScript(this, 1024));
            if (GetPlayerLives(this->initParam) <= 0)
            {
                // Multiplayer never enters TH07's retry/continue menu. A
                // player with no lives remains in Spirit mode so the partner
                // can still revive them through the normal life-transfer path.
                this->playerState = PLAYER_STATE_SPIRIT;
                this->optionState = OPTION_HIDDEN;
                this->isFocus = 0;
                this->lifeGiveTimer = 0;
                this->lifeGiveTargetToken = 0;
                this->bulletGracePeriod = 10;
                this->previousHorizontalSpeed =
                    (g_Rng.GetRandomU16() & 1) ? PLAYER_SPIRIT_DRIFT_SPEED
                                               : -PLAYER_SPIRIT_DRIFT_SPEED;
                this->previousVerticalSpeed =
                    (g_Rng.GetRandomU16() & 1) ? PLAYER_SPIRIT_DRIFT_SPEED
                                               : -PLAYER_SPIRIT_DRIFT_SPEED;
                SetPlayerBombs(this->initParam, 3);
                g_Gui.showBombs = 2;
                recipientId = SelectLowestLifeRecipient(this->initParam);
                if (recipientId >= 0)
                {
                    g_ItemManager.SpawnItem(
                        &this->positionCenter, ITEM_LIFE,
                        GetLifeTransferSpawnState((u8)recipientId));
                }
                this->playerSprite.color.color = 0x50ffffff;
                Netplay::ReportDamageEventSpirit(this->initParam);
                return 0;
            }
            else
            {
                playerLives = GetPlayerLives(this->initParam);
                AddPlayerLives(this->initParam, -1);
                Netplay::ReportDamageEventRespawn(
                    this->initParam, playerLives,
                    GetPlayerLives(this->initParam));
                g_Gui.showLives = 2;
                SetPlayerBombs(this->initParam,
                               (i32)this->shooterData->initialBombs);
                g_Gui.showBombs = 2;
                return 1;
            }
        }
    }
    return 0;
}

// FUNCTION: TH07 0x004411c0
void Player::Respawn()
{
    this->bulletGracePeriod = 60;
    f32 invulnScale = 1.0f - this->invulnerabilityTimer.AsFloat() /
                                 30.0f;
    this->playerSprite.scale.y = 2.0f * invulnScale + 1.0f;
    this->playerSprite.scale.x = 1.0f - 1.0f * invulnScale;
    this->playerSprite.blendMode = 1;
    this->verticalMovementSpeedMultiplierDuringBomb = 1.0f;
    this->horizontalMovementSpeedMultiplierDuringBomb = 1.0f;
    this->playerSprite.color.color =
        this->invulnerabilityTimer.GetCurrent() * 255 / 30 << 24 | 0xffffff;
    this->respawnTimer = 0;
    if (this->invulnerabilityTimer.GetCurrent() >= 30)
    {
        this->playerState = PLAYER_STATE_INVULNERABLE;
        this->playerSprite.scale.x = 1.0f;
        this->playerSprite.scale.y = 1.0f;
        this->playerSprite.color.color = 0xffffffff;
        this->playerSprite.blendMode = 0;
        this->invulnerabilityTimer = 240;
        this->respawnTimer = this->shooterData->initialRespawnTimer;
    }
}

// FUNCTION: TH07 0x00441330
void Player::UpdateState()
{
    ZunColor color;

    if (this->bulletGracePeriod != 0)
    {
        this->bulletGracePeriod--;
        g_BulletManager.RemoveAllBullets(0);
    }
    if (this->playerState == PLAYER_STATE_SPIRIT)
    {
        // Spirit mode is deliberately deterministic: no input is accepted,
        // the player drifts inside the lower playfield, and the remote peer
        // sees the same position through the normal input-delay simulation.
        this->playerSprite.color.color = 0x50ffffff;
        this->positionCenter.x += this->previousHorizontalSpeed;
        this->positionCenter.y += this->previousVerticalSpeed;
        if (this->positionCenter.x <
            g_GameManager.playerMovementAreaTopLeftPos.x)
        {
            this->positionCenter.x =
                g_GameManager.playerMovementAreaTopLeftPos.x;
            this->previousHorizontalSpeed =
                fabsf(this->previousHorizontalSpeed);
        }
        else if (this->positionCenter.x >
                 g_GameManager.playerMovementAreaTopLeftPos.x +
                     g_GameManager.playerMovementAreaSize.x)
        {
            this->positionCenter.x =
                g_GameManager.playerMovementAreaTopLeftPos.x +
                g_GameManager.playerMovementAreaSize.x;
            this->previousHorizontalSpeed =
                -fabsf(this->previousHorizontalSpeed);
        }
        if (this->positionCenter.y <
            g_GameManager.playerMovementAreaTopLeftPos.y + 300.0f)
        {
            this->positionCenter.y =
                g_GameManager.playerMovementAreaTopLeftPos.y + 300.0f;
            this->previousVerticalSpeed = fabsf(this->previousVerticalSpeed);
        }
        else if (this->positionCenter.y >
                 g_GameManager.playerMovementAreaTopLeftPos.y +
                     g_GameManager.playerMovementAreaSize.y - 32.0f)
        {
            this->positionCenter.y =
                g_GameManager.playerMovementAreaTopLeftPos.y +
                g_GameManager.playerMovementAreaSize.y - 32.0f;
            this->previousVerticalSpeed =
                -fabsf(this->previousVerticalSpeed);
        }
        return;
    }
    if (this->playerState == PLAYER_STATE_INVULNERABLE)
    {
        if (this->effect)
        {
            this->effect->pos1 = this->positionCenter;
        }
        this->invulnerabilityTimer--;
        if (this->invulnerabilityTimer.GetCurrent() <= 0)
        {
            if (this->effect)
            {
                this->effect->inUseFlag = 0;
                this->effect = NULL;
            }
            this->playerState = PLAYER_STATE_ALIVE;
            this->invulnerabilityTimer = 0;
            this->playerSprite.color.color = 0xffffffff;
        }
        else
        {
            if (this->invulnerabilityTimer.GetCurrent() % 8 < 2)
            {
                this->playerSprite.color.color = 0xff404040;
            }
            else
            {
                this->playerSprite.color.color = 0xffffffff;
            }
        }
    }
    else if (this->playerState == PLAYER_STATE_BORDER)
    {
        if (this->borderEffect)
        {
            this->borderEffect->pos1 = this->positionCenter;
        }
        // The lowest active slot owns the one shared gauge. Other player
        // animations count down independently but never overwrite it.
        Player *sharedBorderOwner = GetSharedBorderOwner();
        if (!sharedBorderOwner)
        {
            sharedBorderOwner = this;
        }
        if (sharedBorderOwner == this)
        {
            i32 borderCherryPlus = this->invulnerabilityTimer.GetCurrent() *
                GetSharedBorderThreshold() /
                this->borderTimer.GetCurrent();
            if (borderCherryPlus < 0)
            {
                borderCherryPlus = 0;
            }
            g_GameManager.cherryPlus =
                borderCherryPlus + g_GameManager.globals->cherryStart;
        }
        this->invulnerabilityTimer--;
        if (this->invulnerabilityTimer.GetCurrent() <= 0)
        {
            this->playerSprite.color.color = 0xffffffff;
            BreakBorderNaturally();
        }
        else
        {
            if (this->invulnerabilityTimer.GetCurrent() % 4 < 2)
            {
                this->playerSprite.color.color = 0xffff0000;
            }
            else
            {
                this->playerSprite.color.color = 0xffffffff;
            }
            color.bytes.a = 128;
            if (this->invulnerabilityTimer >= 510)
            {
                color.bytes.r = color.bytes.g = color.bytes.b =
                    128 -
                    (540 - this->invulnerabilityTimer.GetCurrent()) * 80 /
                        30;
            }
            else if (this->invulnerabilityTimer < 30)
            {
                color.bytes.r = color.bytes.g = color.bytes.b =
                    128 -
                    this->invulnerabilityTimer.GetCurrent() * 80 /
                        30;
            }
            else
            {
                color.bytes.r = color.bytes.g = color.bytes.b = 48;
            }
            g_Stage.SmoothBlendColor(color);
        }
    }
    else
    {
        this->invulnerabilityTimer++;
    }
}

// FUNCTION: TH07 0x00441670
void Player::BreakBorderNaturally()
{
    i32 cherryDiff;
    i32 playerId;

    if (!g_sharedBorderTransition && GetActivePlayerCount() > 1 &&
        IsSharedBorderActive())
    {
        g_sharedBorderTransition = true;
        // Score and extend the shared gauge once. Partners only clear local
        // state so P2/P3 cannot award duplicate natural-break bonuses.
        this->BreakBorderNaturally();
        for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
        {
            Player *other = &g_Players[playerId];
            if (other != this && IsPlayerSlotActive((u8)playerId) &&
                (other->hasBorder == BORDER_ACTIVE ||
                 other->playerState == PLAYER_STATE_BORDER))
            {
                ClearSharedBorderState(other);
            }
        }
        g_sharedBorderTransition = false;
        return;
    }

    if (this->initParam >= 0 && this->initParam < TH07_MULTI_MAX_PLAYERS)
    {
        g_cherryMaxBreakGrowth[this->initParam] += 10000;
    }
    g_GameManager.IncreaseCherryMax(10000);
    g_GameManager.IncreaseCherry(10000);
    cherryDiff = g_GameManager.cherry - g_GameManager.globals->cherryStart;
    cherryDiff *= 10;
    g_GameManager.AddScore(cherryDiff);
    g_Gui.ShowFullPowerMode(cherryDiff, 4);
    g_GameManager.cherryPlus = g_GameManager.globals->cherryStart;
    g_SoundPlayer.PlaySoundByIdx(SOUND_BORDER_BREAK, 0);
    if (this->playerState == PLAYER_STATE_SPAWNING)
    {
        this->playerSprite.scale.x = 1.0f;
        this->playerSprite.scale.y = 1.0f;
        this->playerSprite.color.color = 0xffffffff;
        this->playerSprite.blendMode = 0;
        this->invulnerabilityTimer = 240;
        this->respawnTimer = this->shooterData->initialRespawnTimer;
    }
    this->playerState = PLAYER_STATE_INVULNERABLE;
    this->invulnerabilityTimer = 40;
    this->borderInvulnerabilityTime = 40;
    this->hasBorder = BORDER_NONE;
    if (this->borderEffect)
    {
        this->borderEffect->inUseFlag = 0;
        this->borderEffect = NULL;
    }
}

#pragma var_order(i, bomb)
// FUNCTION: TH07 0x00441800
BombClearBox *Player::SpawnBombProjectile(D3DXVECTOR3 *centerPosition,
                                            f32 posZ, f32 size, i32 itemType)
{
    BombClearBox *bomb;
    i32 i;

    bomb = this->bombClearBoxes;
    for (i = 0; i < 95; i++, bomb++)
    {
        if (bomb->pos.z == 0.0f && bomb->size.y == 0.0f)
        {
            break;
        }
    }
    bomb->pos.x = centerPosition->x;
    bomb->pos.y = centerPosition->y;
    bomb->pos.z = posZ;
    bomb->size.x = size;
    bomb->lifetime = 0;
    bomb->itemType = itemType;
    return bomb;
}

#pragma var_order(i, bomb)
// FUNCTION: TH07 0x004418b0
BombClearBox *Player::SpawnBombEffect(D3DXVECTOR3 *pos, f32 sizeY, f32 sizeZ,
                                        i32 lifetime, i32 itemType)
{
    BombClearBox *bomb;
    i32 i;

    bomb = this->bombClearBoxes;
    for (i = 0; i < 95; i++, bomb++)
    {
        if (bomb->pos.z == 0.0f && bomb->size.y == 0.0f)
        {
            break;
        }
    }
    bomb->pos.x = pos->x;
    bomb->pos.y = pos->y;
    bomb->size.y = sizeY;
    bomb->size.z = sizeZ;
    bomb->lifetime = lifetime;
    bomb->itemType = itemType;
    return bomb;
}

// FUNCTION: TH07 0x00441960
void Player::ActivateBorder()
{
    Effect *spawnedEffect;

    if (!g_sharedBorderTransition && GetActivePlayerCount() > 1)
    {
        ActivateSharedBorder();
        return;
    }

    if (this->playerState == PLAYER_STATE_ELIMINATED)
    {
        return;
    }

    if (this->bombInfo.isInUse ||
        (!g_sharedBorderTransition && g_Gui.HasCurrentMsgIdx()))
    {
        this->hasBorder = BORDER_READY;
        return;
    }

    switch (this->playerState)
    {
    case PLAYER_STATE_SPAWNING:
    case PLAYER_STATE_INVULNERABLE:
        this->hasBorder = BORDER_READY;
        break;
    case PLAYER_STATE_DEAD:
        if (this->respawnTimer != 0)
        {
            BreakBorder(0);
            return;
        }

        this->hasBorder = BORDER_READY;
        break;
    default:
        this->invulnerabilityTimer = 540;
        this->borderTimer = this->invulnerabilityTimer;
        this->hasBorder = BORDER_ACTIVE;
        this->playerState = PLAYER_STATE_BORDER;
        if (this->borderEffect)
        {
            this->borderEffect->inUseFlag = 0;
        }
        if (this->effect)
        {
            this->effect->inUseFlag = 0;
            this->effect = NULL;
        }
        spawnedEffect = g_EffectManager.SpawnEffect(
            28, &this->positionCenter, GetPlayerEffectSlot(this, 4), 1,
            0xffffffff);
        spawnedEffect->vm.interpStartTimes[4] = 0;
        spawnedEffect->vm.interpEndTimes[4] = this->invulnerabilityTimer.GetCurrent();
        spawnedEffect->vm.interpModes[4] = 0;
        spawnedEffect->vm.scaleInterpInitial.y = 1.0f;
        spawnedEffect->vm.scaleInterpInitial.x = 1.0f;
        spawnedEffect->vm.scaleInterpFinal.x = 0.25f;
        spawnedEffect->vm.scaleInterpFinal.y = 0.25f;
        spawnedEffect->vm.intVars1[0] = this->invulnerabilityTimer.GetCurrent();
        spawnedEffect->vm.angleVel.z *= -1.0f;
        this->borderEffect = spawnedEffect;
        g_Gui.ShowFullPowerMode(0, 2);
        g_SoundPlayer.PlaySoundByIdx(SOUND_BORDER_ACTIVATE, 0);
        g_SoundPlayer.PlaySoundByIdx(SOUND_BORDER_ACTIVATE2, 0);
        g_ReplayManager->replayEventFlags |= 8;
        break;
    }
}

#pragma var_order(effect, i, angle)
// FUNCTION: TH07 0x00441bd0
void Player::BreakBorder(u32 unused)
{
    f32 angle;
    i32 i;
    i32 playerId;
    Effect *effect;

    if (!g_sharedBorderTransition && GetActivePlayerCount() > 1 &&
        IsSharedBorderActive())
    {
        g_sharedBorderTransition = true;
        this->BreakBorder(unused);
        for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
        {
            Player *other = &g_Players[playerId];
            if (other != this && IsPlayerSlotActive((u8)playerId) &&
                (other->hasBorder == BORDER_ACTIVE ||
                 other->playerState == PLAYER_STATE_BORDER))
            {
                // Only the player that was hit emits the break burst.
                ClearSharedBorderState(other);
            }
        }
        g_sharedBorderTransition = false;
        return;
    }

    if (this->borderEffect)
    {
        this->borderEffect->inUseFlag = 0;
        this->borderEffect = NULL;
    }
    effect = g_EffectManager.SpawnEffect(
        28, &this->positionCenter, GetPlayerEffectSlot(this, 4), 1,
        0xffffffff);
    effect->vm.interpStartTimes[4] = 0;
    effect->vm.interpEndTimes[4] = 30;
    effect->vm.interpModes[4] = 0;
    effect->vm.scaleInterpInitial.x = 0.0625f;
    effect->vm.scaleInterpInitial.y = 0.0625f;
    effect->vm.scaleInterpFinal.x = 1.3f;
    effect->vm.scaleInterpFinal.y = 1.3f;
    effect->vm.interpStartTimes[2] = 0;
    effect->vm.interpEndTimes[2] = 30;
    effect->vm.interpModes[2] = 1;
    effect->vm.colorInterpInitialColor.bytes.a = effect->vm.color.bytes.a;
    effect->vm.colorInterpFinalColor.bytes.a = 0;
    effect->vm.intVars1[0] = 30;
    this->borderEffect = effect;
    g_EnemyManager.spellcardInfo.captureScore = 0;
    g_EnemyManager.spellcardInfo.isCapturing = 0;
    this->hasBorder = BORDER_NONE;
    this->playerState = PLAYER_STATE_INVULNERABLE;
    this->invulnerabilityTimer = 40;
    this->borderInvulnerabilityTime = 40;
    g_GameManager.cherryPlus = g_GameManager.globals->cherryStart;
    SpawnBombEffect(&this->positionCenter, 32.0f, 16.0f, 50, 8);
    angle = -ZUN_PI;
    for (i = 0; i < 32; i++, angle += 0.19634955f)
    {
        effect = g_EffectManager.SpawnParticles(29, &this->positionCenter, 1,
                                                0xffffffff);
        effect->direction.x = cosf(angle);
        effect->direction.y = sinf(angle);
    }
    g_SoundPlayer.PlaySoundByIdx(SOUND_BOMB_MARISA_A_FOCUS, 0);
    g_SoundPlayer.PlaySoundByIdx(SOUND_BORDER_BREAK, 0);
    g_ReplayManager->replayEventFlags = g_ReplayManager->replayEventFlags | 0x10;
}

// FUNCTION: TH07 0x00441e80
void Player::UpdateUI()
{
    this->positionOfLastEnemyHit = D3DXVECTOR3(-999.0f, -999.0f, 0.0f);
    this->sakuyaTargetPosition = D3DXVECTOR3(-999.0f, -999.0f, 0.0f);
    this->targetingEnemy = 0;
    if (this->initParam != 0)
    {
        return;
    }
    if (this->positionCenter.y >= 400.0f)
    {
        if (g_AsciiManager.GetFadeState() != 2 &&
            this->positionCenter.x < 160.0f)
        {
            g_AsciiManager.cherryGauge.pendingInterrupt = 2;
            g_AsciiManager.uiFadeState = 2;
        }
        else if (g_AsciiManager.GetFadeState() == 2 &&
                 this->positionCenter.x > 160.0f)
        {
            g_AsciiManager.cherryGauge.pendingInterrupt = 3;
            g_AsciiManager.uiFadeState = 3;
        }
    }
    else if (g_AsciiManager.GetFadeState() == 2)
    {
        g_AsciiManager.cherryGauge.pendingInterrupt = 3;
        g_AsciiManager.uiFadeState = 3;
    }
}

// FUNCTION: TH07 0x00441fb0
u32 Player::OnUpdate(Player *arg)
{
    i32 expectedPlayerId = (i32)(arg - &g_Players[0]);
    if (expectedPlayerId >= 0 &&
        expectedPlayerId < TH07_MULTI_MAX_PLAYERS &&
        arg->initParam != expectedPlayerId)
    {
        arg->initParam = (u8)expectedPlayerId;
        if (!g_playerIdentityRepairLogged)
        {
            g_playerIdentityRepairLogged = true;
            g_GameErrorContext.Log(
                "info : repaired P%d player identity after chain/state restore\r\n",
            expectedPlayerId + 1);
        }
    }
    if (expectedPlayerId >= 0 &&
        expectedPlayerId < TH07_MULTI_MAX_PLAYERS &&
        !IsPlayerSlotActive((u8)expectedPlayerId))
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }
    if (g_GameManager.isTimeStopped)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }
    if (arg->playerState == PLAYER_STATE_ELIMINATED)
    {
        arg->UpdateShots();
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }
    arg->UpdateBombProjectiles();
    arg->UpdateBorderAndBombState();
    if (arg->initParam == 0 &&
        Netplay::ShouldInitializeDamageEventTest())
    {
        // GameManager applies its normal starting resources after player-chain
        // registration. Set the diagnostic stock on one synchronized gameplay
        // frame instead, so rollback replay repeats the same initialization.
        SetPlayerLives(0, 1);
        SetPlayerLives(1, 2);
        if (!Netplay::IsRollbackReplay())
        {
            g_GameErrorContext.Log(
                "info : damage event test resources initialized at frame 60 P1 lives 1 P2 lives 2\r\n");
        }
    }
    if (Netplay::ShouldInjectDamageEvent(arg->initParam) &&
        (arg->playerState == PLAYER_STATE_ALIVE ||
         arg->playerState == PLAYER_STATE_INVULNERABLE ||
         arg->playerState == PLAYER_STATE_BORDER))
    {
        arg->Die();
    }
    if (arg->playerState == PLAYER_STATE_DEAD)
    {
        if (arg->UpdateDeath())
        {
            goto WHAT;
        }
        else
        {
            goto WHY;
        }
    }
    if (arg->playerState == PLAYER_STATE_SPAWNING)
    {
    WHAT:
        arg->Respawn();
    }
WHY:
    arg->UpdateState();

    // The proximity smoke test deliberately keeps the two local players
    // close enough to exercise the TH06 display rule. It is test-only and
    // never runs for network peers or normal gameplay.
    if (Netplay::IsProximityTestEnabled() && !Netplay::IsNetworked() &&
        arg->initParam == 0 && IsPlayerActiveForProximity(arg) &&
        IsPlayerActiveForProximity(&g_Player2))
    {
        g_Player2.positionCenter = arg->positionCenter;
        g_Player2.positionCenter.x += 25.0f;
    }

    // The life-transfer smoke test uses the same gameplay path as normal
    // co-op, but keeps the local ships within TH06's 20-pixel radius.
    if (Netplay::IsLifeTransferTestEnabled() && !Netplay::IsNetworked() &&
        arg->initParam == 0 && IsPlayerActiveForLifeTransfer(arg) &&
        IsPlayerActiveForLifeTransfer(&g_Player2))
    {
        if (!g_lifeTransferTestSetupLogged)
        {
            g_lifeTransferTestSetupLogged = true;
            // Practice quick-start initializes both pools at the TH07 test
            // cap. Lower them only in this diagnostic so the real
            // "giver has a life, receiver is below 8" rule is exercised.
            SetPlayerLives(0, 2);
            SetPlayerLives(1, 1);
            g_GameErrorContext.Log(
                "info : normal life transfer test setup (p1 lives %d, p2 lives %d)\r\n",
                GetPlayerLives(0), GetPlayerLives(1));
        }
        g_Player2.positionCenter = arg->positionCenter;
        g_Player2.positionCenter.x += 10.0f;
    }

    // The network damage test still goes through the normal TH06-style
    // overlap/focus/no-shot transfer path. Only its positioning is scripted so
    // unattended Host/Guest runs can prove that a spirit is actually revived.
    if (Netplay::ShouldForceDamageTestTransfer(arg->initParam) &&
        arg->initParam == 1 && IsPlayerActiveForLifeTransfer(arg) &&
        g_Player.playerState == PLAYER_STATE_SPIRIT)
    {
        arg->positionCenter = g_Player.positionCenter;
        arg->positionCenter.x += 10.0f;
        arg->isFocus = 1;
    }

    UpdateLifeTransfer(arg);
    if (arg->playerState != PLAYER_STATE_DEAD &&
        arg->playerState != PLAYER_STATE_SPAWNING &&
        arg->playerState != PLAYER_STATE_SPIRIT)
    {
        arg->HandlePlayerInputs();
    }
    g_AnmManager->ExecuteScript(&arg->playerSprite);
    if (arg->optionState != OPTION_HIDDEN)
    {
        g_AnmManager->ExecuteScript(&arg->optionsSprite[0]);
        g_AnmManager->ExecuteScript(&arg->optionsSprite[1]);
    }
    arg->UpdateShots();
    arg->UpdateFireBulletTimer();
    arg->UpdateUI();
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

// FUNCTION: TH07 0x004420b0
u32 Player::OnDrawHighPrio(Player *arg)
{
    ZunColor color;
    u32 originalColor;
    u8 proximityAlpha;

    if (!IsPlayerSlotActive(arg->initParam))
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }

    arg->DrawBullets();
    if (arg->playerState == PLAYER_STATE_ELIMINATED)
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }
    if (arg->bombInfo.isInUse)
    {
        if (!arg->bombInfo.isFocus)
        {
            arg->bombInfo.draw(arg);
        }
        else
        {
            arg->bombInfo.drawFocus(arg);
        }
    }
    if (!g_GameManager.isInRetryMenu)
    {
        arg->playerSprite.pos.x =
            g_GameManager.arcadeRegionTopLeftPos.x + arg->positionCenter.x;
        arg->playerSprite.pos.y =
            g_GameManager.arcadeRegionTopLeftPos.y + arg->positionCenter.y;
        arg->playerSprite.pos.z = 0.0f;
        originalColor = arg->playerSprite.color.color;
        if (arg->initParam != 0 && Netplay::ShouldTintPlayer(arg->initParam))
        {
            arg->playerSprite.color.color =
                (originalColor & 0xff000000) | 0x0080ffff;
        }
        proximityAlpha = GetPlayerProximityAlpha(arg);
        if (Netplay::IsProximityTestEnabled() && arg->initParam != 0)
        {
            Netplay::ReportProximityTestResult(proximityAlpha == 55);
        }
        arg->playerSprite.color.color =
            ApplyPlayerProximityAlpha(arg->playerSprite.color.color, arg);
        if (Netplay::IsPlayerTemporarilyAbsent(arg->initParam))
        {
            arg->playerSprite.color.color =
                (arg->playerSprite.color.color & 0x00ffffff) | 0x50000000;
        }
        g_AnmManager->DrawNoRotation(&arg->playerSprite);
        arg->playerSprite.color.color = originalColor;
        DrawLifeTransferPrompt(arg);
        DrawStageIntroPlayerName(arg);
        if (Netplay::IsNetworked() && arg->initParam == 0)
        {
            D3DXVECTOR3 connectionStatusPos(40.0f, 32.0f, 0.0f);
            if (Netplay::IsConnected() && !Netplay::IsResyncing())
            {
                // A healthy connection has nothing to say, and this line sits
                // inside the playfield. Only the launcher's advanced section
                // asks for it; the trouble report below is always drawn,
                // because hiding that would hide a dropped peer.
                if (Netplay::ShouldShowNetDiagnostics())
                {
                    AsciiManager::AddFormatText(
                        &g_AsciiManager, &connectionStatusPos,
                        "NET %s RTT %lums D%d%s",
                        Netplay::IsHost() ? "H" : "G",
                        (unsigned long)Netplay::GetRoundTripMs(),
                        Netplay::GetDelay(),
                        Netplay::IsInsaneMode() ? " INSANE" : "");
                }
            }
            else
            {
                AsciiManager::AddFormatText(
                    &g_AsciiManager, &connectionStatusPos, "NET %s %s",
                    Netplay::IsHost() ? "H" : "G",
                    Netplay::GetStatusText());
            }
        }
        if (arg->optionState != OPTION_HIDDEN &&
            !Netplay::IsPlayerTemporarilyAbsent(arg->initParam) &&
            (arg->playerState == PLAYER_STATE_ALIVE ||
             arg->playerState == PLAYER_STATE_BORDER ||
             arg->playerState == PLAYER_STATE_INVULNERABLE))
        {
            arg->optionsSprite[0].pos.x =
                g_GameManager.arcadeRegionTopLeftPos.x + arg->optionsPosition[0].x;
            arg->optionsSprite[0].pos.y =
                g_GameManager.arcadeRegionTopLeftPos.y + arg->optionsPosition[0].y;
            arg->optionsSprite[0].pos.z = 0.0f;
            arg->optionsSprite[1].pos.x =
                g_GameManager.arcadeRegionTopLeftPos.x + arg->optionsPosition[1].x;
            arg->optionsSprite[1].pos.y =
                g_GameManager.arcadeRegionTopLeftPos.y + arg->optionsPosition[1].y;
            arg->optionsSprite[1].pos.z = 0.0f;
            g_AnmManager->Draw(&arg->optionsSprite[0]);
            g_AnmManager->Draw(&arg->optionsSprite[1]);
        }
    }
    if (arg->playerState == PLAYER_STATE_BORDER &&
        arg->invulnerabilityTimer.GetCurrent() > 0)
    {
        if (arg->invulnerabilityTimer.GetCurrent() % 4 < 2)
        {
            arg->playerSprite.color.color = 0xffff0000;
        }
        else
        {
            arg->playerSprite.color.color = 0xffffffff;
        }
        color.bytes.a = 128;
        if (arg->invulnerabilityTimer >= 510)
        {
            color.bytes.r = color.bytes.g = color.bytes.b =
                128 -
                (540 - arg->invulnerabilityTimer.GetCurrent()) * 80 /
                    30;
        }
        else if (arg->invulnerabilityTimer < 30)
        {
            color.bytes.r = color.bytes.g = color.bytes.b =
                128 -
                arg->invulnerabilityTimer.GetCurrent() * 80 /
                    30;
        }
        else
        {
            color.bytes.r = color.bytes.g = color.bytes.b = 48;
        }
        g_Stage.SmoothBlendColor(color);
    }
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

// FUNCTION: TH07 0x00442350
u32 Player::OnDrawLowPrio(Player *arg)
{
    if (!IsPlayerSlotActive(arg->initParam))
    {
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }
    arg->DrawBulletExplosions();
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

#pragma var_order(y, x)
// FUNCTION: TH07 0x00442370
f32 Player::AngleToPlayer(D3DXVECTOR3 *pos)
{
    f32 y;
    f32 x;

    x = this->positionCenter.x - pos->x;
    y = this->positionCenter.y - pos->y;
    if (y == 0.0f && x == 0.0f)
    {
        return 1.5707964f;
    }
    else
    {
        return atan2f(y, x);
    }
}

i32 GetPlayerAnmScript(const Player *player, i32 script)
{
    if (!player || player->initParam == 0)
    {
        return script;
    }
    return script +
        (player->initParam == 1 ? ANM_OFFSET_PLAYER2
                                : ANM_OFFSET_PLAYER3) -
            ANM_OFFSET_PLAYER;
}

i32 GetPlayerEffectSlot(const Player *player, i32 p1Slot)
{
    if (!player || player->initParam == 0)
    {
        return p1Slot;
    }
    // TH07 reserves special-effect slots 0..4 for its single player. Give
    // P2 a separate slot for every persistent player effect so focus, bomb,
    // death, and border animations cannot overwrite P1's effect (or each
    // other) in EffectManager::effects[400..408].
    i32 base = player->initParam == 1 ? 5 : 9;
    switch (p1Slot)
    {
    case 0:
        return base;
    case 2:
        return base + 1;
    case 3:
        return base + 2;
    case 4:
        return base + 3;
    default:
        return p1Slot;
    }
}

static i32 PlayerLoadoutIndex(const Player *player)
{
    i32 character = Netplay::GetPlayerCharacter(player->initParam);
    i32 shot = Netplay::GetPlayerShot(player->initParam);

    if (character < CHAR_REIMU || character > CHAR_SAKUYA)
    {
        character = CHAR_REIMU;
    }
    if (shot < 0 || shot > 1)
    {
        shot = 0;
    }
    return character * 2 + shot;
}

static const char *PlayerAnmPath(i32 character)
{
    switch (character)
    {
    case CHAR_MARISA:
        return "data/player01.anm";
    case CHAR_SAKUYA:
        return "data/player02.anm";
    case CHAR_REIMU:
    default:
        return "data/player00.anm";
    }
}

// FUNCTION: TH07 0x004423e0
ZunResult Player::AddedCallback(Player *arg)
{
    PlayerBullet *bullet;
    i32 i;
    i32 loadoutIndex;
    i32 playerCharacter;
    i32 playerAnmFile;
    i32 playerAnmOffset;

    loadoutIndex = PlayerLoadoutIndex(arg);
    playerCharacter = Netplay::GetPlayerCharacter(arg->initParam);
    playerAnmFile = arg->initParam == 0
        ? ANM_FILE_PLAYER
        : (arg->initParam == 1 ? ANM_FILE_PLAYER2 : ANM_FILE_PLAYER3);
    playerAnmOffset = arg->initParam == 0
        ? ANM_OFFSET_PLAYER
        : (arg->initParam == 1 ? ANM_OFFSET_PLAYER2 : ANM_OFFSET_PLAYER3);
    if (Netplay::IsMultiplayer())
    {
        g_GameErrorContext.Log(
            "info : P%d loadout character %d shot %d anm %d\r\n",
            arg->initParam + 1, playerCharacter,
            Netplay::GetPlayerShot(arg->initParam), playerAnmFile);
    }

    if (ShtData::LoadShtData(
            &arg->shooterData,
            g_ShooterTable[loadoutIndex]) != ZUN_SUCCESS)
    {
        return ZUN_ERROR;
    }

    if (ShtData::LoadShtData(
            &arg->shooterDataFocus,
            g_ShooterTableFocus[loadoutIndex]) !=
        ZUN_SUCCESS)
    {
        return ZUN_ERROR;
    }

    if ((u32)(g_Supervisor.curState != 3 && g_Supervisor.curState != 11 &&
              g_Supervisor.curState != 12))
    {
        if (g_AnmManager->LoadAnms(playerAnmFile,
                                   PlayerAnmPath(playerCharacter),
                                   playerAnmOffset) != ZUN_SUCCESS)
        {
            return ZUN_ERROR;
        }
    }
    g_AnmManager->SetAnmIdxAndExecuteScript(
        &arg->playerSprite, GetPlayerAnmScript(arg, 1024));
    if (Netplay::GetPlayerCount() >= 3)
    {
        arg->positionCenter.x = g_GameManager.arcadeRegionSize.x / 2.0f +
            ((i32)arg->initParam - 1) * 48.0f;
    }
    else
    {
        arg->positionCenter.x = g_GameManager.arcadeRegionSize.x / 2.0f +
            (arg->initParam == 0 ? -32.0f : 32.0f);
    }
    arg->positionCenter.y = g_GameManager.arcadeRegionSize.y - 64.0f;
    arg->positionCenter.z = 0.49f;
    arg->optionsPosition[0].z = 0.49f;
    arg->optionsPosition[1].z = 0.49f;

    // ZUN landmine: This loop goes for 128 iterations, but bombDamageBoxes has
    // only 112 elements, meaning that this causes UB. In practice this makes
    // some of it overflow into bombClearBoxes
    for (i = 0; i < 128; i++)
    {
        arg->bombDamageBoxes[i].size.x = 0.0f;
    }
    arg->hitboxSize.y = arg->shooterData->hitboxRadius / 2.0f;
    arg->hitboxSize.x = arg->hitboxSize.y;
    arg->hitboxSize.z = 5.0f;
    arg->grazeSize.y = arg->shooterData->grabItemRadius / 2.0f;
    arg->grazeSize.x = arg->grazeSize.y;
    arg->grazeSize.z = 5.0f;
    arg->grabItemSize.x = 12.0f;
    arg->grabItemSize.y = 12.0f;
    arg->grabItemSize.z = 5.0f;
    arg->playerDirection = MOVEMENT_NONE;
    arg->playerState = PLAYER_STATE_SPAWNING;
    arg->invulnerabilityTimer = 120;
    arg->optionState = OPTION_UNFOCUSED;
    g_AnmManager->SetAnmIdxAndExecuteScript(
        &arg->optionsSprite[0], GetPlayerAnmScript(arg, 1152));
    g_AnmManager->SetAnmIdxAndExecuteScript(
        &arg->optionsSprite[1], GetPlayerAnmScript(arg, 1153));
    bullet = arg->bullets;
    for (i = 0; i < 96; i++, bullet++)
    {
        bullet->bulletState = 0;
    }
    arg->fireBulletTimer = -1;
    arg->bombInfo.bombCalc = g_BombData[loadoutIndex].calc;
    arg->bombInfo.draw = g_BombData[loadoutIndex].draw;
    arg->bombInfo.bombFocusCalc =
        g_BombData[loadoutIndex].calcFocus;
    arg->bombInfo.drawFocus =
        g_BombData[loadoutIndex].drawFocus;
    arg->bombInfo.isInUse = 0;
    arg->optionAngle = -1.5707964f;
    arg->verticalMovementSpeedMultiplierDuringBomb = 1.0f;
    arg->horizontalMovementSpeedMultiplierDuringBomb = 1.0f;
    arg->respawnTimer = arg->shooterData->initialRespawnTimer;
    if (arg->initParam == 2 && Netplay::GetPlayerCount() >= 3)
    {
        ApplyActivePlayerCountParameters(2, 3);
    }
    if (arg->initParam == Netplay::GetPlayerCount() - 1 &&
        Netplay::GetPlayerCount() > 1 &&
        g_GameManager.cherryPlus >=
            g_GameManager.globals->cherryStart +
                GetSharedBorderThreshold())
    {
        g_GameManager.cherryPlus =
            g_GameManager.globals->cherryStart +
            GetSharedBorderThreshold();
        ActivateSharedBorder();
    }
    if (arg->initParam == 0)
    {
        if ((u32)(g_Supervisor.curState != 3 && g_Supervisor.curState != 11 &&
                  g_Supervisor.curState != 12))
        {
            g_AsciiManager.cherryGauge.pendingInterrupt = 1;
            g_AsciiManager.uiFadeState = 1;
        }
        g_AsciiManager.GetBossMarker(0)->pendingInterrupt = 2;
        g_AsciiManager.GetBossMarker(1)->pendingInterrupt = 2;
        g_AsciiManager.GetBossMarker(2)->pendingInterrupt = 2;
    }
    return ZUN_SUCCESS;
}

// FUNCTION: TH07 0x004428e0
ZunResult Player::DeletedCallback(Player *arg)
{
    if ((u32)(g_Supervisor.curState != 3 && g_Supervisor.curState != 11 &&
              g_Supervisor.curState != 12))
    {
        g_AnmManager->ReleaseAnm(
            arg->initParam == 0
                ? ANM_FILE_PLAYER
                : (arg->initParam == 1 ? ANM_FILE_PLAYER2
                                       : ANM_FILE_PLAYER3));
        if (arg->initParam == 0)
        {
            g_AsciiManager.cherryGauge.pendingInterrupt = 99;
            g_AsciiManager.uiFadeState = 99;
            g_AsciiManager.GetBossMarker(0)->pendingInterrupt = 99;
            g_AsciiManager.GetBossMarker(1)->pendingInterrupt = 99;
            g_AsciiManager.GetBossMarker(2)->pendingInterrupt = 99;
        }
    }
    SAFE_FREE(arg->shooterData);
    SAFE_FREE(arg->shooterDataFocus);
    return ZUN_SUCCESS;
}

// Multiplayer helper. P1 keeps the original TH07 selection and P2 may use an
// independently synchronized character/shot loadout without changing the
// original GameManager or ZunGlobals layouts.
static ZunResult RegisterOnePlayer(Player *mgr, u8 playerId)
{
    memset(mgr, 0, sizeof(Player));
    mgr->invulnerabilityTimer = 0;
    mgr->initParam = playerId;
    mgr->calcChain = g_Chain.CreateElem((ChainCallback)Player::OnUpdate);
    mgr->drawChain1 =
        g_Chain.CreateElem((ChainCallback)Player::OnDrawHighPrio);
    mgr->drawChain2 =
        g_Chain.CreateElem((ChainCallback)Player::OnDrawLowPrio);
    mgr->calcChain->arg = mgr;
    mgr->drawChain1->arg = mgr;
    mgr->drawChain2->arg = mgr;
    mgr->calcChain->addedCallback =
        (ChainLifecycleCallback)Player::AddedCallback;
    mgr->calcChain->deletedCallback =
        (ChainLifecycleCallback)Player::DeletedCallback;
    if (g_Chain.AddToCalcChain(mgr->calcChain, 8))
    {
        return ZUN_ERROR;
    }
    g_Chain.AddToDrawChain(mgr->drawChain1, 6);
    g_Chain.AddToDrawChain(mgr->drawChain2, 8);
    return ZUN_SUCCESS;
}

// FUNCTION: TH07 0x004429d0
ZunResult Player::RegisterChain(u32 param_1)
{
    (void)param_1;
    bool preserveResources = g_Supervisor.curState == 3;
    int playerId;

    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        g_PlayerActive[playerId] = playerId == 0 ||
            (Netplay::IsMultiplayer() && !g_GameManager.replay &&
             playerId < Netplay::GetPlayerCount() &&
             !Netplay::IsPlayerPermanentlyDeparted((u8)playerId));
    }
    if (RegisterOnePlayer(&g_Player, 0) != ZUN_SUCCESS)
    {
        return ZUN_ERROR;
    }
    for (playerId = 1; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        if (!g_PlayerActive[playerId])
        {
            continue;
        }
        if (!preserveResources)
        {
            ResetMultiplayerPlayerResources((u8)playerId);
        }
        if (RegisterOnePlayer(&g_Players[playerId], (u8)playerId) !=
            ZUN_SUCCESS)
        {
            return ZUN_ERROR;
        }
        g_Players[playerId].initParam = (u8)playerId;
        g_GameErrorContext.Log(
            "info : P%d same-character tint %s (P1 character %d, P%d character %d)\r\n",
            playerId + 1,
            Netplay::ShouldTintPlayer((u8)playerId) ? "enabled" : "disabled",
            Netplay::GetPlayerCharacter(0),
            playerId + 1, Netplay::GetPlayerCharacter((u8)playerId));
        g_GameErrorContext.Log(
            "info : P%d chain registered id %d preserve_resources %d\r\n",
            playerId + 1, (int)g_Players[playerId].initParam,
            preserveResources ? 1 : 0);
    }
    return ZUN_SUCCESS;
}

static void CutPlayerChains(Player *player)
{
    if (player->calcChain)
    {
        g_Chain.Cut(player->calcChain);
        player->calcChain = NULL;
    }
    if (player->drawChain1)
    {
        g_Chain.Cut(player->drawChain1);
        player->drawChain1 = NULL;
    }
    if (player->drawChain2)
    {
        g_Chain.Cut(player->drawChain2);
        player->drawChain2 = NULL;
    }
}

// FUNCTION: TH07 0x00442b10
void Player::CutChain()
{
    int playerId;
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        if (g_PlayerActive[playerId])
        {
            CutPlayerChains(&g_Players[playerId]);
        }
        g_PlayerActive[playerId] = playerId == 0;
    }
}

// FUNCTION: TH07 0x00442b70
ZunResult ShtData::LoadShtData(ShtData **data, const char *shtPath)
{
    ShtEntry *entry;
    i32 i;

    *data = (ShtData *)FileSystem::OpenFile(shtPath, 0);
    if (!*data)
    {
        return ZUN_ERROR;
    }

    for (i = 0; i < (i32)(u32)(*data)->entryCount; i++)
    {
        (&(*data)->levels)[i].entry =
            (ShtEntry *)((i32)(&(*data)->levels)[i].entry + (i32)*data);

        entry = (&(*data)->levels)[i].entry;
        while (entry->fireInterval >= 0)
        {
            entry->fireCallback = g_ShtFireFuncs[(i32)entry->fireCallback];
            entry->updateCallback =
                g_ShtUpdateFuncs[(i32)entry->updateCallback];
            entry->drawCallback = g_ShtDrawFuncs[(i32)entry->drawCallback];
            entry->hitCallback = g_ShtHitFuncs[(i32)entry->hitCallback];
            entry++;
        }
    }
    return ZUN_SUCCESS;
}

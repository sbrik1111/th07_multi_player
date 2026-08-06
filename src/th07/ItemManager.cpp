#include "ItemManager.hpp"

#include "AnmManager.hpp"
#include "AsciiManager.hpp"
#include "BulletManager.hpp"
#include "EffectManager.hpp"
#include "EnemyManager.hpp"
#include "GameErrorContext.hpp"
#include "GameManager.hpp"
#include "Gui.hpp"
#include "Netplay.hpp"
#include "Player.hpp"
#include "Rng.hpp"
#include "SoundPlayer.hpp"
#include "d3dx8.h"
#include "utils.hpp"

// GLOBAL: TH07 0x0049ecf8
i32 g_FullPowerScoreBonus[30] = {10, 20, 30, 40, 50, 60, 70, 80,
                                 90, 100, 200, 300, 400, 500, 600, 700,
                                 800, 900, 1000, 2000, 3000, 4000, 5000,
                                 6000, 7000, 8000, 9000, 10000, 11000, 12000};

// GLOBAL: TH07 0x0049ed74
i32 g_PowerLevels[9] = {8, 16, 32, 48, 64, 80, 96, 128, 999};

// GLOBAL: TH07 0x0049efa0
u8 g_ItemDropTable[32] = {0, 0, 1, 0, 1, 0, 0, 7, 1, 1, 0, 0, 7, 1, 1, 0, 1, 0,
                          1, 0, 1, 0, 1, 0, 1, 0, 7, 1, 1, 1, 0, 2};

// GLOBAL: TH07 0x00575c70
ItemManager g_ItemManager;

static const i8 LIFE_TRANSFER_TARGET_P2 = 2;
static const i8 LIFE_TRANSFER_TARGET_P1 = 3;
static const i8 LIFE_TRANSFER_TARGET_P3 = 8;
static const f32 MULTIPLAYER_RESOURCE_DROP_OFFSET = 16.0f;
static bool g_sharedP1AutoCollectLogged = false;
static bool g_sharedP2AutoCollectLogged = false;

static void GetSeparatedResourceDropPosition(const D3DXVECTOR3 *heading,
                                             i32 ordinal, i32 count,
                                             D3DXVECTOR3 *position)
{
    f32 centerX = heading->x;
    f32 halfSpan = MULTIPLAYER_RESOURCE_DROP_OFFSET * (count - 1);
    f32 maximumCenterX = g_GameManager.arcadeRegionSize.x - halfSpan;

    *position = *heading;
    if (centerX < halfSpan)
    {
        centerX = halfSpan;
    }
    if (centerX > maximumCenterX)
    {
        centerX = maximumCenterX;
    }
    position->x = centerX - halfSpan +
        ordinal * MULTIPLAYER_RESOURCE_DROP_OFFSET * 2.0f;
}
static const i8 AUTO_COLLECT_LEGACY = 1;
static const i8 AUTO_COLLECT_TARGET_P1 = 4;
static const i8 AUTO_COLLECT_TARGET_P2 = 5;
static const i8 AUTO_COLLECT_TARGET_P3 = 9;
static bool g_p2AutoCollectLogged = false;

static i8 GetLifeTransferMarker(u8 playerId)
{
    switch (playerId)
    {
    case 0:
        return LIFE_TRANSFER_TARGET_P1;
    case 1:
        return LIFE_TRANSFER_TARGET_P2;
    case 2:
        return LIFE_TRANSFER_TARGET_P3;
    default:
        return 0;
    }
}

static i8 GetAutoCollectMarker(u8 playerId)
{
    switch (playerId)
    {
    case 0:
        return AUTO_COLLECT_TARGET_P1;
    case 1:
        return AUTO_COLLECT_TARGET_P2;
    case 2:
        return AUTO_COLLECT_TARGET_P3;
    default:
        return 0;
    }
}

static i32 GetMarkerPlayerId(i8 marker)
{
    if (marker == LIFE_TRANSFER_TARGET_P1 ||
        marker == AUTO_COLLECT_TARGET_P1)
    {
        return 0;
    }
    if (marker == LIFE_TRANSFER_TARGET_P2 ||
        marker == AUTO_COLLECT_TARGET_P2)
    {
        return 1;
    }
    if (marker == LIFE_TRANSFER_TARGET_P3 ||
        marker == AUTO_COLLECT_TARGET_P3)
    {
        return 2;
    }
    return -1;
}

i32 GetLifeTransferSpawnState(u8 targetPlayerId)
{
    // Keep the legacy 3=P2 and 4=P1 states so existing diagnostics retain
    // their animation. State 5 is the equivalent throw toward P3.
    switch (targetPlayerId)
    {
    case 0:
        return 4;
    case 1:
        return 3;
    case 2:
        return 5;
    default:
        return 0;
    }
}

static bool IsItemTargetActive(const Player *player)
{
    return player && IsPlayerSlotActive(player->initParam) &&
        !Netplay::IsPlayerTemporarilyAbsent(player->initParam) &&
        (player->playerState == PLAYER_STATE_ALIVE ||
         player->playerState == PLAYER_STATE_INVULNERABLE ||
         player->playerState == PLAYER_STATE_BORDER);
}

static bool IsGenericAutoCollect(const Item *item)
{
    return item &&
        (item->autoCollect == AUTO_COLLECT_LEGACY ||
         item->autoCollect == AUTO_COLLECT_TARGET_P1 ||
         item->autoCollect == AUTO_COLLECT_TARGET_P2 ||
         item->autoCollect == AUTO_COLLECT_TARGET_P3);
}

static bool HasFixedItemTarget(const Item *item)
{
    return item &&
        (item->autoCollect == LIFE_TRANSFER_TARGET_P2 ||
         item->autoCollect == LIFE_TRANSFER_TARGET_P1 ||
         item->autoCollect == LIFE_TRANSFER_TARGET_P3 ||
         item->autoCollect == AUTO_COLLECT_TARGET_P1 ||
         item->autoCollect == AUTO_COLLECT_TARGET_P2 ||
         item->autoCollect == AUTO_COLLECT_TARGET_P3);
}

static bool CanPlayerStartAutoCollect(const Player *player)
{
    return IsItemTargetActive(player) && player->shooterData &&
        ((((i32)GetPlayerPower(player->initParam) >= 128 ||
           g_GameManager.difficulty >= DIFF_EXTRA) &&
          player->positionCenter.y < player->shooterData->pocY) ||
         player->hasBorder == BORDER_ACTIVE);
}

static Player *GetAutoCollectTarget(const Item *item)
{
    bool eligible[TH07_MULTI_MAX_PLAYERS];
    i32 eligibleIds[TH07_MULTI_MAX_PLAYERS];
    i32 eligibleCount = 0;
    i32 playerId;
    i32 itemIndex;
    Player *closest = NULL;
    f32 closestDistance = 0.0f;

    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        eligible[playerId] = IsPlayerSlotActive((u8)playerId) &&
            CanPlayerStartAutoCollect(&g_Players[playerId]);
        if (eligible[playerId])
        {
            eligibleIds[eligibleCount++] = playerId;
        }
    }

    // Whenever auto-collect applies to more than one player at once -- the
    // shared Shinra border, or several players at full power above the point
    // of collection -- divide the pickups evenly instead of handing them all
    // to the closest player. Fixed slot order plus the synchronized pool
    // index gives an equal share without consuming RNG.
    if (eligibleCount > 1)
    {
        itemIndex = item ? (i32)(item - g_ItemManager.items) : 0;
        return &g_Players[eligibleIds[itemIndex % eligibleCount]];
    }

    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        f32 dx;
        f32 dy;
        f32 distance;
        if (!eligible[playerId])
        {
            continue;
        }
        dx = g_Players[playerId].positionCenter.x - item->currentPosition.x;
        dy = g_Players[playerId].positionCenter.y - item->currentPosition.y;
        distance = dx * dx + dy * dy;
        if (!closest || distance < closestDistance)
        {
            closest = &g_Players[playerId];
            closestDistance = distance;
        }
    }
    return closest;
}

static Player *GetItemTargetPlayer(Item *item)
{
    i32 fixedId = item ? GetMarkerPlayerId(item->autoCollect) : -1;
    if (fixedId >= 0)
    {
        return &g_Players[fixedId];
    }
    return GetClosestActivePlayer(&item->currentPosition);
}

static i32 GetRoundRobinActivePlayerId(i32 itemIndex)
{
    i32 activeIds[TH07_MULTI_MAX_PLAYERS];
    i32 activeCount = 0;
    i32 playerId;
    for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
    {
        if (IsPlayerSlotActive((u8)playerId))
        {
            activeIds[activeCount++] = playerId;
        }
    }
    return activeCount > 0 ? activeIds[itemIndex % activeCount] : -1;
}

// FUNCTION: TH07 0x004325c0
void AngleToVector(D3DXVECTOR3 *vec, f32 angle, f32 speed)
{
    /* vec->x = cosf(angle) * speed;
     * vec->y = sinf(angle) * speed;
     */
    __asm {
        mov eax, vec
        fld [angle]
        fsincos
        fmul [speed]
        fstp float ptr [eax]
        fmul [speed]
        fstp float ptr [eax + 4]
    }
}

// FUNCTION: TH07 0x004325e0
void GameManager::AddCurrentPower(i32 amount)
{
    if (CheckGameIntegrity())
    {
        NUKE_SUPERVISOR();
    }
    this->globals->currentPower += (f32)amount;
    RegenerateGameIntegrityCsum();
}

// FUNCTION: TH07 0x00432630
ItemManager::ItemManager()
{
    i32 idk;

    UselessStack::FourBytes();
    UselessStack::FourBytes();
    UselessStack::FourBytes();
}

// FUNCTION: TH07 0x00432690
Item::Item()
{
}

#pragma var_order(i, item)
// FUNCTION: TH07 0x004326f0
Item *ItemManager::SpawnItem(D3DXVECTOR3 *heading, i32 itemType, i32 state)
{
    Item *item;
    i32 i;
    i32 powerTargetId;
    i32 transferTargetId;

    item = &this->items[this->nextIndex];
    if (itemType == ITEM_POWER_SMALL || itemType == ITEM_POWER_BIG)
    {
        if (!Netplay::IsMultiplayer())
        {
            if ((i32)GetPlayerPower(0) >= 128)
            {
                itemType = ITEM_CHERRY;
            }
        }
        else
        {
            // Assign each Power drop to an active slot in fixed round-robin
            // order. A drop becomes Cherry only when that slot is already at
            // MAX, so non-full P2/P3 keep receiving P even if a partner is
            // full. Item index and active mask are rollback state.
            powerTargetId = GetRoundRobinActivePlayerId(this->nextIndex);
            if (powerTargetId >= 0 && GetPlayerPower((u8)powerTargetId) >= 128)
            {
                itemType = ITEM_CHERRY;
            }
        }
    }
    for (i = 0; i < 1100; i++)
    {
        this->nextIndex++;

        if (item->isInUse)
        {
            if (this->nextIndex >= 1100)
            {
                this->nextIndex = 0;
                item = this->items;
            }
            else
            {
                item++;
            }
            continue;
        }
        if (this->nextIndex >= 1100)
        {
            this->nextIndex = 0;
        }
        item->isInUse = 1;
        item->currentPosition = *heading;
        item->startPosition.x = 0.0f;
        item->startPosition.y = -2.2f;
        item->startPosition.z = 0.0f;
        item->itemType = (u8)itemType;
        item->state = (u8)state;
        item->timer = 0;
        if (state == 2)
        {
            item->targetPosition.x = g_Rng.GetRandomFloatInRange(288.0f) + 48.0f;
            item->targetPosition.y = g_Rng.GetRandomFloatInRange(192.0f) - 64.0f;
            item->targetPosition.z = 0.0f;
            item->startPosition = item->currentPosition;
        }
        else if (state == 3 || state == 4 || state == 5)
        {
            // Match TH06multi's visible life handoff. Keep collision disabled
            // while the item rises for 20 frames, then home it to the other
            // player instead of letting the donor immediately recollect it.
            item->targetPosition = *heading;
            item->targetPosition.y -= 60.0f;
            item->targetPosition.z = 0.0f;
            item->startPosition = item->currentPosition;
        }
        g_AnmManager->SetAnmIdxAndExecuteScript(&item->sprite, itemType + 708);
        item->sprite.color.color = 0xffffffff;
        item->sprite.zWriteDisable = 1;
        transferTargetId = state == 3 ? 1 : (state == 4 ? 0 : 2);
        item->autoCollect = state >= 3 && state <= 5
            ? GetLifeTransferMarker((u8)transferTargetId)
            : 0;
        item->isOnscreen = 1;
        break;
    }

    return i < 1100 ? item : &this->items[1100];
}

Item *ItemManager::SpawnEnemyDrop(D3DXVECTOR3 *heading, i32 itemType,
                                  i32 state)
{
    i32 activeCount = GetActivePlayerCount();
    if (Netplay::IsMultiplayer() && activeCount > 1 &&
        (itemType == ITEM_LIFE || itemType == ITEM_BOMB))
    {
        D3DXVECTOR3 position;
        Item *first = &this->items[1100];
        i32 ordinal = 0;
        i32 playerId;

        // Emit one visibly separated copy per active slot. Slot order and
        // symmetric offsets are deterministic and consume no RNG.
        //
        // The copies are deliberately left unowned: any player may walk into
        // any of them. Reserving a copy for one slot made the other copies
        // uncollectible for everyone else, and the owner was also excluded
        // from auto-collect, so the extra drops were purely decorative.
        // Even distribution is handled by GetAutoCollectTarget when
        // auto-collect is active for more than one player at once.
        for (playerId = 0; playerId < TH07_MULTI_MAX_PLAYERS; playerId++)
        {
            Item *spawned;
            if (!IsPlayerSlotActive((u8)playerId))
            {
                continue;
            }
            GetSeparatedResourceDropPosition(
                heading, ordinal, activeCount, &position);
            spawned = SpawnItem(&position, itemType, state);
            if (ordinal == 0)
            {
                first = spawned;
            }
            ordinal++;
        }
        g_GameErrorContext.Log(
            "info : multiplayer enemy %s drop count %d separated %.0f px\r\n",
            itemType == ITEM_LIFE ? "life" : "bomb",
            activeCount, MULTIPLAYER_RESOURCE_DROP_OFFSET * 2.0f);
        return first;
    }
    return SpawnItem(heading, itemType, state);
}

#pragma var_order(i, itemTimerSecs, itemScore, playerAngle, local_20, itemAcquired, \
                  item, j, prevPowerIdx, k, prevPowerLevel2)
// FUNCTION: TH07 0x00432990
void ItemManager::OnUpdate()
{
    i32 prevPowerLevel2;
    i32 k;
    i32 prevPowerIdx;
    i32 j;
    Item *item;
    i32 itemAcquired;
    f32 playerAngle;
    i32 itemScore;
    f32 itemTimerSecs;
    i32 i;
    Player *targetPlayer;
    Player *autoCollectTarget;

    item = this->items;
    D3DXVECTOR3 local_20(0.0f, 0.0f, 16.0f);
    itemAcquired = 0;
    this->activeItemCount = 0;
    this->listTail = &this->listHead;
    this->listHead.next = NULL;

    for (i = 0; i < 1100; i++, item++)
    {
        if (!item->isInUse)
        {
            continue;
        }

        this->activeItemCount++;
        targetPlayer = GetItemTargetPlayer(item);
        if (HasFixedItemTarget(item) &&
            !IsItemTargetActive(targetPlayer))
        {
            item->autoCollect = 0;
            item->state = 0;
            targetPlayer = GetClosestActivePlayer(&item->currentPosition);
        }
        if (item->state == 2)
        {
            if (item->timer < 60)
            {
                itemTimerSecs = item->timer.AsFloat() / 60.0f;
                item->currentPosition = itemTimerSecs * item->targetPosition +
                                        item->startPosition *
                                            (1.0f - itemTimerSecs);
                goto check_collision;
            }
            else if (item->timer == 60)
            {
                item->startPosition = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
                item->state = 0;
            }
        }
        else if (item->state == 3 || item->state == 4 || item->state == 5)
        {
            if (item->timer < 20)
            {
                f32 throwTime = item->timer.AsFloat() / 20.0f;
                f32 throwEase = 1.0f -
                    powf(1.0f - throwTime, 1.5f);
                item->currentPosition =
                    throwEase * item->targetPosition +
                    item->startPosition * (1.0f - throwEase);
                goto check_collision;
            }
            else if (item->timer == 20)
            {
                item->startPosition = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
                item->state = 1;
            }
        }
        else
        {
            autoCollectTarget = item->state == 1 ||
                    (HasFixedItemTarget(item) &&
                     !IsSharedBorderActive())
                ? NULL
                : GetAutoCollectTarget(item);
            if (autoCollectTarget)
            {
                targetPlayer = autoCollectTarget;
                item->state = 1;
                if (IsSharedBorderActive() &&
                    targetPlayer->initParam == 0 &&
                    !g_sharedP1AutoCollectLogged)
                {
                    g_sharedP1AutoCollectLogged = true;
                    g_GameErrorContext.Log(
                        "info : shared border auto-collect P1 target verified\r\n");
                }
                if (IsSharedBorderActive() &&
                    targetPlayer->initParam != 0 &&
                    !g_sharedP2AutoCollectLogged)
                {
                    g_sharedP2AutoCollectLogged = true;
                    g_GameErrorContext.Log(
                        "info : shared border auto-collect P2 target verified\r\n");
                }
                if (item->autoCollect == 0)
                {
                    item->autoCollect =
                        GetAutoCollectMarker(targetPlayer->initParam);
                    if (targetPlayer->initParam != 0 &&
                        !g_p2AutoCollectLogged)
                    {
                        g_p2AutoCollectLogged = true;
                        g_GameErrorContext.Log(
                            "info : P2 top auto-collect target verified\r\n");
                    }
                }
            }
            if (item->state == 1)
            {
                if (targetPlayer->playerState != 1)
                {
                    playerAngle =
                        targetPlayer->AngleToPlayer(&item->currentPosition);
                    AngleToVector(&item->startPosition, playerAngle,
                                  targetPlayer->shooterData->itemCollectSpeed);
                    item->state = 1;
                }
                else
                {
                    item->startPosition.y = -0.5f;
                    item->state = 0;
                }
            }
            else
            {
                item->startPosition.x = 0.0f;
                item->startPosition.z = 0.0f;
                if (item->startPosition.y < -2.2f)
                {
                    item->startPosition.y = -2.2f;
                }
            }
        }
        item->currentPosition += item->startPosition * g_Supervisor.effectiveFramerateMultiplier;
        if (g_GameManager.arcadeRegionSize.y + 16.0f <= item->currentPosition.y)
        {
            item->isInUse = 0;
            g_GameManager.DecreaseSubrank(3);
            continue;
        }
        if (item->startPosition.y < 3.0f)
        {
            item->startPosition.y += 0.03f * g_Supervisor.effectiveFramerateMultiplier;
        }
        else
        {
            item->startPosition.y = 3.0f;
        }
    check_collision:
        targetPlayer = GetItemTargetPlayer(item);
        local_20.x = targetPlayer->shooterData->itemCollectRadius;
        local_20.y = targetPlayer->shooterData->itemCollectRadius;
        if (!(item->timer < 20 &&
              (item->state == 3 || item->state == 4 || item->state == 5)) &&
            targetPlayer->CalcItemBoxCollision(&item->currentPosition,
                                               &local_20))
        {
            g_ReplayManager->replayEventFlags |= 0x40;
            switch (item->itemType)
            {
            case ITEM_POWER_SMALL:
                if (GetPlayerPower(targetPlayer->initParam) >= 128)
                {
                    g_GameManager.powerItemCountForScore++;
                    if ((u32)g_GameManager.powerItemCountForScore >= 31)
                    {
                        g_GameManager.powerItemCountForScore = 30;
                    }
                    itemScore = g_FullPowerScoreBonus[g_GameManager.powerItemCountForScore];
                    g_GameManager.AddScore(itemScore);
                    g_AsciiManager.CreatePopup1(&item->currentPosition, itemScore, itemScore >= 12800 ? 0xffffff00 : 0xffffffff);
                }
                else
                {
                    j = 0;
                    while (GetPlayerPower(targetPlayer->initParam) >= g_PowerLevels[j])
                    {
                        j++;
                    }
                    prevPowerIdx = j;
                    g_GameManager.powerItemCountForScore = 0;
                    AddPlayerPower(targetPlayer->initParam, 1);
                    if (GetPlayerPower(targetPlayer->initParam) >= 128)
                    {
                        SetPlayerPower(targetPlayer->initParam, 128);
                        if (!g_EnemyManager.spellcardInfo.isActive)
                        {
                            g_BulletManager.RemoveAllBullets(1);
                        }
                        g_Gui.ShowFullPowerMode(0, 1);
                        this->DespawnAllItems(i);
                    }
                    g_GameManager.AddScore(10);
                    g_Gui.showPower = 2;
                    while (GetPlayerPower(targetPlayer->initParam) >= g_PowerLevels[j])
                    {
                        j++;
                    }
                    if (j != prevPowerIdx)
                    {
                        g_AsciiManager.CreatePopup1(&item->currentPosition, -1, 0xffffc0a0);
                        g_SoundPlayer.PlaySoundByIdx(SOUND_POWERUP, 0);
                    }
                    else
                    {
                        g_AsciiManager.CreatePopup1(&item->currentPosition, 10, 0xffffffff);
                    }
                }
                g_GameManager.IncreaseSubrank(1);
                break;
            case ITEM_POINT:
                itemScore =
                    item->currentPosition.y < targetPlayer->shooterData->pocY
                        ? 50000
                        : 50000 -
                              (i32)(item->currentPosition.y -
                                    targetPlayer->shooterData->pocY) *
                                  100;
                if (IsGenericAutoCollect(item))
                {
                    itemScore = 50000;
                }
                if (itemScore >= 50000)
                {
                    if (g_GameManager.cherry - g_GameManager.globals->cherryStart > 50000)
                    {
                        itemScore = g_GameManager.cherry - g_GameManager.globals->cherryStart;
                    }
                }
                else if (g_GameManager.cherry - g_GameManager.globals->cherryStart > 50000)
                {
                    itemScore += (g_GameManager.cherry - g_GameManager.globals->cherryStart - 50000) / 5;
                }
                itemScore -= itemScore % 10;
                g_AsciiManager.CreatePopup1(
                    &item->currentPosition, itemScore,
                    item->currentPosition.y < targetPlayer->shooterData->pocY ||
                            IsGenericAutoCollect(item)
                        ? 0xffffff00
                        : 0xffffffff);
                g_GameManager.AddScore(itemScore);
                g_GameManager.globals->pointItemsCollectedThisStage++;
                g_GameManager.globals->pointItemsCollectedForExtend++;
                g_Gui.showPoint = 2;
                if (item->currentPosition.y < 128.0f)
                {
                    g_GameManager.IncreaseSubrank(10);
                }
                else
                {
                    g_GameManager.IncreaseSubrank(3);
                }
                if (g_GameManager.globals->extendsFromPointItems >= 0)
                {
                    for (;;)
                    {
                        if (g_GameManager.difficulty < 4)
                        {
                            if (g_GameManager.globals->extendsFromPointItems < 3)
                            {
                                g_GameManager.globals->nextNeededPointItemsForExtend = g_GameManager.globals->extendsFromPointItems * 75 + 50;
                            }
                            else if (g_GameManager.globals->extendsFromPointItems < 5)
                            {
                                g_GameManager.globals->nextNeededPointItemsForExtend = (g_GameManager.globals->extendsFromPointItems - 3) * 150 + 300;
                            }
                            else
                            {
                                g_GameManager.globals->nextNeededPointItemsForExtend = (g_GameManager.globals->extendsFromPointItems - 5) * 200 + 800;
                            }
                        }
                        else if (g_GameManager.globals->extendsFromPointItems == 0)
                        {
                            g_GameManager.globals->nextNeededPointItemsForExtend = 200;
                        }
                        else if (g_GameManager.globals->extendsFromPointItems == 1)
                        {
                            g_GameManager.globals->nextNeededPointItemsForExtend = 500;
                        }
                        else
                        {
                            g_GameManager.globals->nextNeededPointItemsForExtend = (g_GameManager.globals->extendsFromPointItems - 2) * 500 + 800;
                        }

                        if (g_GameManager.globals->pointItemsCollectedForExtend >= g_GameManager.globals->nextNeededPointItemsForExtend)
                        {
                            ExtendAllPlayersFromPoints();
                            g_GameManager.globals->extendsFromPointItems++;
                            continue;
                        }
                        break;
                    }
                }
                break;
            case ITEM_POWER_BIG:
                if (GetPlayerPower(targetPlayer->initParam) >= 128)
                {
                    g_AsciiManager.CreatePopup1(&item->currentPosition, itemScore, itemScore >= 1000 ? 0xffffff00 : 0xffffffff);
                }
                else
                {
                    k = 0;
                    while (GetPlayerPower(targetPlayer->initParam) >= g_PowerLevels[k])
                    {
                        k++;
                    }
                    prevPowerLevel2 = k;
                    AddPlayerPower(targetPlayer->initParam, 8);
                    if (GetPlayerPower(targetPlayer->initParam) >= 128)
                    {
                        SetPlayerPower(targetPlayer->initParam, 128);
                        if (!g_EnemyManager.spellcardInfo.isActive)
                        {
                            g_BulletManager.RemoveAllBullets(1);
                        }
                        g_Gui.ShowFullPowerMode(0, 1);
                        this->DespawnAllItems(i);
                    }
                    g_Gui.showPower = 2;
                    g_GameManager.AddScore(10);
                    while (GetPlayerPower(targetPlayer->initParam) >= g_PowerLevels[k])
                    {
                        k++;
                    }
                    if (k != prevPowerLevel2)
                    {
                        g_AsciiManager.CreatePopup1(&item->currentPosition, -1, 0xffffc0a0);
                        g_SoundPlayer.PlaySoundByIdx(SOUND_POWERUP, 0);
                    }
                    else
                    {
                        g_AsciiManager.CreatePopup1(&item->currentPosition, 10, 0xffffffff);
                    }
                }
                break;
            case ITEM_BOMB:
                if (GetPlayerBombs(targetPlayer->initParam) < 8)
                {
                    AddPlayerBombs(targetPlayer->initParam, 1);
                    g_Gui.showBombs = 2;
                }
                g_GameManager.IncreaseSubrank(5);
                break;
            case ITEM_LIFE:
            {
                i32 recipient = targetPlayer->initParam;
                i32 livesBefore = GetPlayerLives(recipient);
                ExtendPlayerFromItem(targetPlayer->initParam);
                if (item->autoCollect == LIFE_TRANSFER_TARGET_P2 ||
                    item->autoCollect == LIFE_TRANSFER_TARGET_P1 ||
                    item->autoCollect == LIFE_TRANSFER_TARGET_P3)
                {
                    Netplay::ReportLifeTransferTestResult(
                        recipient, livesBefore, GetPlayerLives(recipient));
                }
                break;
            }
            case ITEM_FULL_POWER:
                if (GetPlayerPower(targetPlayer->initParam) < 128)
                {
                    g_BulletManager.RemoveAllBullets(1);
                    g_Gui.ShowFullPowerMode(0, 1);
                    g_SoundPlayer.PlaySoundByIdx(SOUND_POWERUP, 0);
                    g_AsciiManager.CreatePopup1(&item->currentPosition, -1, 0xffffc0a0);
                    this->DespawnAllItems(i);
                }
                SetPlayerPower(targetPlayer->initParam, 128);
                g_GameManager.AddScore(1000);
                g_AsciiManager.CreatePopup1(&item->currentPosition, 1000, 0xffffffff);
                g_Gui.showPower = 2;
                break;
            case ITEM_POINT_BULLET:
                if (!targetPlayer->isBombing)
                {
                    itemScore = g_GameManager.globals->grazeInTotal / 40 * 10 + 300;
                    if (itemScore <= 0)
                    {
                        itemScore = 10;
                    }
                }
                else
                {
                    itemScore = 100;
                }
                g_AsciiManager.CreatePopup2(&item->currentPosition, itemScore, -1);
                g_GameManager.AddScore(itemScore);
                if (!targetPlayer->bombInfo.isInUse)
                {
                    g_GameManager.AddCherryPlusForPlayer(
                        20, targetPlayer->initParam);
                }
                else if ((i & 1) == 0)
                {
                    g_GameManager.AddCherryPlusForPlayer(
                        10, targetPlayer->initParam);
                }
                else
                {
                    g_GameManager.AddCherry(10);
                }
                break;
            case ITEM_CHERRY_SMALL:
                g_GameManager.AddCherryPlusForPlayer(
                    30, targetPlayer->initParam);
                g_GameManager.AddCherry(70);
                break;
            case ITEM_CHERRY:
                if (g_GameManager.IsCherryAtMax())
                {
                    itemScore =
                        item->currentPosition.y <
                                    targetPlayer->shooterData->pocY ||
                                item->autoCollect
                            ? 50000
                            : 50000 -
                                  (i32)(item->currentPosition.y -
                                        targetPlayer->shooterData->pocY) *
                                      100;
                    itemScore -= itemScore % 10;
                    g_AsciiManager.CreatePopup1(
                        &item->currentPosition, itemScore,
                        item->currentPosition.y <
                                    targetPlayer->shooterData->pocY ||
                                item->autoCollect
                            ? 0xffffff00
                            : 0xffffffff);
                    g_GameManager.AddScore(itemScore);
                }
                itemScore = 1000;
                itemScore += g_GameManager.globals->spellCardsCaptured * 100;
                if (!g_GameManager.IsCherryAtMax())
                {
                    g_AsciiManager.CreatePopup1(&item->currentPosition, itemScore, 0xffff4040);
                }
                g_GameManager.AddCherryPlusForPlayer(
                    itemScore, targetPlayer->initParam);
                break;
            case ITEM_STAR:
                itemScore = g_GameManager.globals->grazeInTotal / 40 * 10 + 300;
                if (itemScore <= 0)
                {
                    itemScore = 10;
                }
                if (g_GameManager.IsCherryAtMax())
                {
                    g_AsciiManager.CreatePopup1(&item->currentPosition, itemScore, 0xffffffff);
                }
                g_GameManager.AddScore(itemScore);
                itemScore = 100;
                if (!g_GameManager.IsCherryAtMax())
                {
                    g_AsciiManager.CreatePopup1(&item->currentPosition, itemScore, 0xffff4040);
                }
                g_GameManager.AddCherryPlusForPlayer(
                    itemScore, targetPlayer->initParam);
                break;
            }
            item->isInUse = 0;
            itemAcquired = 1;
            continue;
        }
        else
        {
            item->timer++;
            if (item->sprite.currentInstruction)
            {
                g_AnmManager->ExecuteScript(&item->sprite);
            }
            this->listTail->next = item;
            item->next = NULL;
            this->listTail = item;
        }
    }
    if (itemAcquired)
    {
        g_SoundPlayer.PlaySoundByIdx(SOUND_21, 0);
    }
}

#pragma var_order(i, item)
// FUNCTION: TH07 0x00433a90
void ItemManager::RemoveAllItems()
{
    Item *item;
    i32 i;

    item = this->items;
    for (i = 0; i < 1100; i++, item++)
    {
        if (!item->isInUse)
        {
            continue;
        }

        item->state = 1;
        item->startPosition = D3DXVECTOR3(0.0f, -0.5f, 0.0f);
    }
}

#pragma var_order(i, item)
// FUNCTION: TH07 0x00433b20
void ItemManager::DespawnAllItems(i32 param_1)
{
    Item *item;
    i32 i;

    item = this->items;
    for (i = 0; i < 1100; i++, item++)
    {
        if (item->isInUse == 0 || i == param_1)
        {
            continue;
        }

        if (item->itemType == 0 || item->itemType == 2)
        {
            if (item->startPosition.y > -0.5f)
            {
                item->startPosition.x = 0.0f;
                item->startPosition.y = -0.5f;
                item->startPosition.z = 0.0f;
            }
            g_EffectManager.SpawnParticles(0, &item->currentPosition, 1, 0xffffffff);
            item->itemType = 7;
            g_AnmManager->SetAnmIdxAndExecuteScript(&item->sprite, 715);
        }
    }
}

#pragma var_order(i, item)
// FUNCTION: TH07 0x00433c40
void ItemManager::ActivateAllItems()
{
    Item *item;
    i32 i;

    item = this->items;
    for (i = 0; i < 1100; i++, item++)
    {
        if (item->isInUse != 1)
        {
            continue;
        }

        if (item->state == 1)
        {
            item->state = 0;
            item->startPosition.x = 0.0f;
            item->startPosition.y = -0.9f;
            item->startPosition.z = 0.0f;
        }
    }
}

#pragma var_order(local_8, item)
// FUNCTION: TH07 0x00433cd0
void ItemManager::OnDraw()
{
    Item *item;
    i32 local_8;

    item = this->listHead.next;
    while (item)
    {
        item->sprite.pos.x =
            g_GameManager.arcadeRegionTopLeftPos.x + item->currentPosition.x;
        item->sprite.pos.y =
            g_GameManager.arcadeRegionTopLeftPos.y + item->currentPosition.y;
        item->sprite.pos.z = 0.01f;
        if (item->currentPosition.y < -8.0f)
        {
            item->sprite.pos.y = 8.0f + g_GameManager.arcadeRegionTopLeftPos.y;
            if (item->isOnscreen)
            {
                g_AnmManager->SetActiveSprite(&item->sprite, item->itemType + 694);
                item->isOnscreen = 0;
                item->sprite.zWriteDisable = 1;
            }
            local_8 = 255 - (i32)((8.0f - item->currentPosition.y) * 255.0f / 128.0f);
            if (local_8 < 64)
            {
                local_8 = 64;
            }
            item->sprite.color.color =
                (item->sprite.color.color & 0xffffff) | local_8 << 24;
        }
        else
        {
            if (!item->isOnscreen)
            {
                g_AnmManager->SetActiveSprite(&item->sprite, item->itemType + 684);
                item->isOnscreen = 1;
                item->sprite.color.color = 0xffffffff;
                item->sprite.zWriteDisable = 1;
            }
        }
        g_AnmManager->Draw(&item->sprite);
        item = item->next;
    }
}

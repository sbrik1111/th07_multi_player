
#include "Coop.hpp"
#include "EnemyManager.hpp"
#include "ItemManager.hpp"
#include "Player.hpp"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

void (*g_CoopLogSink)(const char *line);

i32 g_FrameMayRollBack;
i32 g_CoopActiveSeat;
i32 g_CoopTestGhostFrame = -1;
i32 g_CoopTestStartStage = 1;
i32 g_CoopTestStageClearFrame = -1;
i32 g_CoopTestStageClearLast = 1;

void CoopLog(const char *format, ...)
{
    if (g_CoopLogSink == NULL)
    {
        return;
    }
    char line[640];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
    va_end(args);
    g_CoopLogSink(line);
}

Player *SeatPlayer(i32 seat)
{
    return &g_Players[seat >= 0 && seat < MAX_PLAYERS ? seat : 0];
}

static f32 DistanceSq(Player *player, Float3 *pos)
{
    f32 dx = player->positionCenter.x - pos->x;
    f32 dy = player->positionCenter.y - pos->y;
    return dx * dx + dy * dy;
}

i32 IsGhost(Player *player)
{
    return player != NULL && player->playerState == PLAYER_STATE_GHOST;
}

i32 PlayerOnField(Player *player)
{
    return player->playerState != PLAYER_STATE_DEAD && player->playerState != PLAYER_STATE_GHOST;
}

Player *AimTarget(Float3 *pos)
{
    Player *best = NULL;
    f32 bestDistance = 0.0f;
    i32 bestOnField = 0;
    for (i32 seat = 0; seat < PlayerCount(); seat++)
    {
        Player *player = &g_Players[seat];
        i32 onField = PlayerOnField(player);
        f32 distance = DistanceSq(player, pos);
        if (best == NULL || (onField && !bestOnField) || (onField == bestOnField && distance < bestDistance))
        {
            best = player;
            bestDistance = distance;
            bestOnField = onField;
        }
    }
    return best;
}

i32 PlayerCanBeHit(Player *player)
{
    return player->seat < PlayerCount() && !IsGhost(player);
}

i32 CoopCheckGraze(Float3 *center, Float3 *size, i32 *seat)
{
    *seat = 0;
    for (i32 s = 0; s < PlayerCount(); s++)
    {
        Player *player = &g_Players[s];
        if (!PlayerCanBeHit(player))
        {
            continue;
        }
        i32 r = player->CheckGraze(center, size);
        if (r != 0)
        {
            *seat = s;
            return r;
        }
    }
    return 0;
}

i32 CoopKillbox(Float3 *center, Float3 *size, i32 *seat)
{
    i32 result = 0;
    *seat = 0;
    for (i32 s = 0; s < PlayerCount(); s++)
    {
        Player *player = &g_Players[s];
        if (!PlayerCanBeHit(player))
        {
            continue;
        }
        i32 r = player->CalcKillboxCollision(center, size);
        if (r == 2)
        {
            *seat = s;
            return 2;
        }
        if (r == 1 && result == 0)
        {
            result = 1;
            *seat = s;
        }
    }
    return result;
}

void CoopLaserHitbox(Float3 *center, Float3 *size, Float3 *origin, f32 rotation, i32 canGraze)
{
    for (i32 s = 0; s < PlayerCount(); s++)
    {
        Player *player = &g_Players[s];
        if (!PlayerCanBeHit(player))
        {
            continue;
        }
        if (player->CalcLaserHitbox(center, size, origin, rotation, canGraze) == 2)
        {
            canGraze = 0;
        }
    }
}

i32 CoopScaleDamage(Enemy *enemy, i32 damage)
{
    i32 count = PlayerCount();
    if (count < 2 || damage <= 0)
    {
        return damage;
    }
    i32 boss = enemy->isBoss != 0;
    // By seats (2..4), boss / other
    static const i32 percent[3][2] = {
        {100, 75},
        {75, 66},
        {66, 50},
    };
    i32 p = percent[(count > 4 ? 4 : count) - 2][boss];
    return p == 100 ? damage : damage * p / 100;
}

i32 CoopItemCopies(i32 type, i32 boss)
{
    if (type == ITEM_LIFE || type == ITEM_BOMB)
    {
        return PlayerCount();
    }
    if (boss && (type == ITEM_POWER_SMALL || type == ITEM_POWER_BIG || type == ITEM_FULL_POWER))
    {
        return PlayerCount();
    }
    return 1;
}

f32 CoopItemCopyAngle(i32 k, i32 count)
{
    // Stacked copies would all go to the nearest seat.
    return -ZUN_PI / 2 + (f32)(2 * k - (count - 1)) * 0.35f;
}

i32 AnyBombInUse()
{
    for (i32 seat = 0; seat < PlayerCount(); seat++)
    {
        if (g_Players[seat].bombInfo.isInUse)
        {
            return 1;
        }
    }
    return 0;
}

i32 AnyPlayerBusy()
{
    for (i32 seat = 0; seat < PlayerCount(); seat++)
    {
        Player *player = &g_Players[seat];
        if (IsGhost(player))
        {
            continue;
        }
        if (player->bombInfo.isInUse || player->playerState != PLAYER_STATE_ALIVE)
        {
            return 1;
        }
    }
    return 0;
}

static i32 CanCollect(Player *player)
{
    return player->playerState == PLAYER_STATE_ALIVE || player->playerState == PLAYER_STATE_INVULNERABLE ||
           player->playerState == PLAYER_STATE_BORDER;
}

i32 PlayerAutoCollects(Player *player)
{
    return ((128.0 <= (f64)(i32)g_GameManager.Power(player->seat) || g_GameManager.difficulty >= 4) &&
            player->positionCenter.y < player->shooterData->pocY) ||
           player->hasBorder == BORDER_ACTIVE;
}

i32 PowerDropSeat(i32 itemIndex)
{
    i32 seats[MAX_PLAYERS];
    i32 count = 0;
    for (i32 seat = 0; seat < PlayerCount(); seat++)
        if (!IsGhost(&g_Players[seat]))
            seats[count++] = seat;
    return count ? seats[itemIndex % count] : 0;
}

Player *ItemCollector(Item *item)
{
    if (item->targetSeat >= 0 && item->targetSeat < PlayerCount())
    {
        Player *target = &g_Players[item->targetSeat];
        if (CanCollect(target))
            return target;
        // A dead recipient releases the gift so it cannot get stuck.
        item->targetSeat = -1;
        item->transfer = 0;
        item->state = 0;
        item->startPosition = Float3(0.0f, -0.5f, 0.0f);
    }

    i32 eligible[MAX_PLAYERS];
    i32 count = 0;
    Player *best = NULL;
    f32 bestDistance = 0.0f;
    for (i32 seat = 0; seat < PlayerCount(); seat++)
    {
        Player *player = &g_Players[seat];
        if (!CanCollect(player))
            continue;
        if (PlayerAutoCollects(player))
            eligible[count++] = seat;
        f32 distance = DistanceSq(player, &item->currentPosition);
        if (best == NULL || distance < bestDistance)
        {
            best = player;
            bestDistance = distance;
        }
    }
    if (count > 0)
    {
        best = &g_Players[eligible[(item - g_ItemManager.items) % count]];
        item->autoCollect = 1;
        // State 2: th07's death drop scatter.
        if (item->state != 2)
            item->state = 1;
    }
    if (best != NULL && item->state == 1)
        item->targetSeat = (i8)best->seat;
    return best != NULL ? best : &g_Players[0];
}

i32 CoopAllSeatsFullPower()
{
    for (i32 seat = 0; seat < PlayerCount(); seat++)
    {
        if ((i32)g_GameManager.Power(seat) < 128 && !IsGhost(&g_Players[seat]))
        {
            return 0;
        }
    }
    return 1;
}

void CoopSpawnDrop(Float3 *pos, i32 type, i32 state, i32 boss)
{
    i32 count = CoopItemCopies(type, boss);
    if (count <= 1)
    {
        g_ItemManager.SpawnItem(pos, type, state);
        return;
    }
    for (i32 k = 0; k < count; k++)
    {
        Item *item = g_ItemManager.SpawnItem(pos, type, state);
        item->startPosition.FromAngleMagnitude(CoopItemCopyAngle(k, count), 2.2f);
    }
    CoopLog("DROP_COPIES type=%d copies=%d boss=%d", type, count, boss);
}

const char *(*g_CoopNameSource)(i32 seat);

const char *CoopPlayerName(i32 seat)
{
    static const char *const defaults[4] = {"PLAYER1", "PLAYER2", "PLAYER3", "PLAYER4"};
    const char *name = g_CoopNameSource != NULL ? g_CoopNameSource(seat) : NULL;
    return name != NULL && name[0] != 0 ? name : defaults[seat & 3];
}

i32 g_CoopFadeActive;
i32 g_CoopViewSeat;

#define FADE_START 64.0f
#define FADE_FULL 24.0f
#define FADE_ALPHA 55
#define GHOST_ALPHA 128

u32 CoopPlayerAlpha(Player *player)
{
    if (player->playerState == PLAYER_STATE_GHOST)
    {
        return GHOST_ALPHA;
    }
    i32 view = g_CoopViewSeat;
    if (player->seat == view || player->playerState == PLAYER_STATE_DEAD)
    {
        return 255;
    }
    Player *mine = view >= 0 && view < PlayerCount() ? &g_Players[view] : NULL;
    if (mine == NULL || mine->playerState == PLAYER_STATE_DEAD)
    {
        return 255;
    }
    f32 dx = player->positionCenter.x - mine->positionCenter.x;
    f32 dy = player->positionCenter.y - mine->positionCenter.y;
    f32 span = dx * dx + dy * dy;
    if (span >= FADE_START * FADE_START)
    {
        return 255;
    }
    if (span <= FADE_FULL * FADE_FULL)
    {
        return FADE_ALPHA;
    }
    f32 apart = sqrtf(span);
    return (u32)((apart - FADE_FULL) * (255.0f - FADE_ALPHA) / (FADE_START - FADE_FULL) + FADE_ALPHA);
}

void CoopRefreshFadeSet()
{
    g_CoopFadeActive = 0;
}


#include "SimHash.hpp"
#include "BulletManager.hpp"
#include "Coop.hpp"
#include "EnemyManager.hpp"
#include "GameManager.hpp"
#include "Gui.hpp"
#include "ItemManager.hpp"
#include "Player.hpp"
#include "Rng.hpp"
#include "Supervisor.hpp"

extern const char *const g_SimHashPartNames[SIM_HASH_PART_COUNT] = {
    "rng", "globals", "stocks", "players", "shots", "bombs", "bullets",
    "lasers", "enemies", "items", "spell", "flow", "input",
};

namespace
{

struct Fnv
{
    u32 h;
    Fnv() : h(0x811c9dc5u)
    {
    }
    void Bytes(const void *data, u32 n)
    {
        const u8 *p = (const u8 *)data;
        for (u32 i = 0; i < n; i++)
        {
            h = (h ^ p[i]) * 0x01000193u;
        }
    }
    void U32(u32 v)
    {
        this->Bytes(&v, 4);
    }
    void I32(i32 v)
    {
        this->Bytes(&v, 4);
    }
    void F32(f32 v)
    {
        this->Bytes(&v, 4);
    }
    void Pos(const Float3 &p)
    {
        this->F32(p.x);
        this->F32(p.y);
        this->F32(p.z);
    }
};

void HashPlayer(Fnv &f, Player *p)
{
    f.Pos(p->positionCenter);
    f.I32(p->playerState);
    f.I32(p->invulnerabilityTimer.current);
    f.I32(p->respawnTimer);
    f.I32(p->isFocus);
    f.I32(p->optionState);
    f.I32(p->hasBorder);
    f.I32(p->fireBulletTimer.current);
    f.F32(p->optionAngle);
    f.Pos(p->optionsPosition[0]);
    f.Pos(p->optionsPosition[1]);
    f.I32(p->lifeGiveTimer);
    f.I32(p->powerGiveTaps);
}

void HashShots(Fnv &f, Player *p)
{
    for (i32 i = 0; i < ARRAY_SIZE_SIGNED(p->bullets); i++)
    {
        const PlayerBullet &b = p->bullets[i];
        if (b.bulletState == 0)
        {
            continue;
        }
        f.I32(i);
        f.I32(b.bulletState);
        f.Pos(b.pos);
        f.I32(b.damage);
    }
    for (i32 i = 0; i < ARRAY_SIZE_SIGNED(p->bombDamageBoxes); i++)
    {
        const BombProjectile &r = p->bombDamageBoxes[i];
        if (r.size.x <= 0.0f)
        {
            continue;
        }
        f.I32(i);
        f.Pos(r.pos);
        f.Pos(r.size);
    }
}

void HashBomb(Fnv &f, Player *p)
{
    f.I32(p->bombInfo.isInUse);
    f.I32(p->bombInfo.isFocus);
    f.I32(p->bombInfo.bombTimer.current);
    f.I32(p->bombInfo.bombDuration);
    f.I32(p->borderTimer.current);
    f.I32(p->borderInvulnerabilityTime);
}

}

u32 SimFrameHash(u32 *parts)
{
    Fnv part[SIM_HASH_PART_COUNT];

    part[SIM_HASH_RNG].U32(g_Rng.seed);
    part[SIM_HASH_RNG].U32(g_Rng.generationCount);

    if (g_GameManager.globals != NULL)
    {
        Fnv &f = part[SIM_HASH_GLOBALS];
        ZunGlobals *g = g_GameManager.globals;
        f.U32(g->score);
        f.I32(g->grazeInStage);
        f.I32(g->grazeInTotal);
        f.I32(g->spellCardsCaptured);
        f.I32(g->numRetries);
        f.I32(g->pointItemsCollectedThisStage);
        f.I32(g->pointItemsCollectedForExtend);
        f.I32(g->extendsFromPointItems);
        f.I32(g_GameManager.cherry);
        f.I32(g_GameManager.cherryMax);
        f.I32(g_GameManager.currentStage);
        f.I32(g_GameManager.difficulty);
        f.I32(g_GameManager.rank.rank);
        f.I32(g_GameManager.subrank);
        f.I32(g_GameManager.framesThisStage);
    }
    for (i32 seat = 0; seat < PlayerCount(); seat++)
    {
        Fnv &s = part[SIM_HASH_STOCKS];
        s.I32(g_GameManager.ShotTypeAndCharacter(seat));
        s.F32(g_GameManager.Lives(seat));
        s.F32(g_GameManager.Bombs(seat));
        s.F32(g_GameManager.Power(seat));
        s.I32(g_GameManager.CherryPlus(seat));
        s.I32(g_GameManager.PowerItemCount(seat));
        Player *player = &g_Players[seat];
        if (player->calcChain != NULL)
        {
            HashPlayer(part[SIM_HASH_PLAYERS], player);
            HashShots(part[SIM_HASH_SHOTS], player);
            HashBomb(part[SIM_HASH_BOMBS], player);
        }
        part[SIM_HASH_INPUT].U32(player->GameInput());
        part[SIM_HASH_INPUT].U32(player->LastGameInput());
    }

    {
        Fnv &f = part[SIM_HASH_BULLETS];
        f.I32(g_BulletManager.bulletCount);
        for (i32 i = 0; i < MAX_BULLETS; i++)
        {
            const Bullet &b = g_BulletManager.bullets[i];
            if (b.state == BULLET_INACTIVE)
            {
                continue;
            }
            f.I32(i);
            f.I32(b.state);
            f.I32(b.exFlags);
            f.Pos(b.pos);
            f.Pos(b.velocity);
        }
    }
    {
        Fnv &f = part[SIM_HASH_LASERS];
        for (i32 i = 0; i < ARRAY_SIZE_SIGNED(g_BulletManager.lasers); i++)
        {
            const Laser &l = g_BulletManager.lasers[i];
            if (!l.inUse)
            {
                continue;
            }
            f.I32(i);
            f.I32(l.state);
            f.Pos(l.pos);
            f.F32(l.angle);
            f.F32(l.startOffset);
            f.F32(l.endOffset);
        }
    }
    {
        Fnv &f = part[SIM_HASH_ENEMIES];
        f.I32(g_EnemyManager.enemyCountReal);
        for (i32 i = 0; i < MAX_ENEMIES; i++)
        {
            const Enemy &e = g_EnemyManager.enemies[i];
            if (!e.active)
            {
                continue;
            }
            f.I32(i);
            f.Pos(e.pos);
            f.I32(e.life);
            f.I32(e.flags1);
            f.I32(e.flags2);
            f.I32(e.timer.current);
        }
    }
    {
        Fnv &f = part[SIM_HASH_ITEMS];
        for (i32 i = 0; i < 1100; i++)
        {
            const Item &item = g_ItemManager.items[i];
            if (!item.isInUse)
            {
                continue;
            }
            f.I32(i);
            f.I32(item.state);
            f.I32(item.itemType);
            f.Pos(item.currentPosition);
            f.I32(item.collector);
        }
    }
    {
        Fnv &f = part[SIM_HASH_SPELL];
        const SpellcardInfo &s = g_EnemyManager.spellcardInfo;
        f.U32(s.isActive);
        f.U32(s.isCapturing);
        f.I32(s.captureScore);
        f.I32(s.spellcardIdx);
        f.U32(s.usedBomb);
    }
    {
        Fnv &f = part[SIM_HASH_FLOW];
        f.I32(g_Supervisor.wantedState);
        f.I32(g_Supervisor.curState);
        f.I32(g_GameManager.isInPauseMenu);
        f.I32(g_GameManager.isInRetryMenu);
        f.I32(g_Gui.HasCurrentMsgIdx());
    }

    Fnv all;
    for (i32 i = 0; i < SIM_HASH_PART_COUNT; i++)
    {
        if (parts != NULL)
        {
            parts[i] = part[i].h;
        }
        all.U32(part[i].h);
    }
    return all.h;
}

static u32 FloatBits(f32 value)
{
    return *(u32 *)&value;
}

void SimDumpState(u32 frame, u32 pass)
{
    CoopLog("DUMP_BEGIN frame=%u pass=%u", frame, pass);
    for (i32 seat = 0; seat < PlayerCount(); seat++)
    {
        Player *player = &g_Players[seat];
        CoopLog("DUMP_PLAYER frame=%u pass=%u seat=%d state=%d pos=%08X,%08X power=%d lives=%d bombs=%d", frame, pass,
                seat, player->playerState, FloatBits(player->positionCenter.x), FloatBits(player->positionCenter.y),
                (i32)g_GameManager.Power(seat), (i32)g_GameManager.Lives(seat), (i32)g_GameManager.Bombs(seat));
    }
    for (i32 i = 0; i < 1100; i++)
    {
        const Item &item = g_ItemManager.items[i];
        if (!item.isInUse)
        {
            continue;
        }
        CoopLog("DUMP_ITEM frame=%u pass=%u i=%d state=%d type=%d pos=%08X,%08X,%08X vel=%08X,%08X time=%d auto=%d "
                "collector=%d",
                frame, pass, i, item.state, item.itemType, FloatBits(item.currentPosition.x),
                FloatBits(item.currentPosition.y), FloatBits(item.currentPosition.z), FloatBits(item.startPosition.x),
                FloatBits(item.startPosition.y), item.timer.current, item.autoCollect, item.collector);
    }
    CoopLog("DUMP_END frame=%u pass=%u", frame, pass);
}

i32 SimCollectItems(SimItemRecord *out, i32 max)
{
    i32 n = 0;
    for (i32 i = 0; i < 1100 && n < max; i++)
    {
        const Item &item = g_ItemManager.items[i];
        if (!item.isInUse)
        {
            continue;
        }
        SimItemRecord &r = out[n++];
        r.index = i;
        r.state = item.state;
        r.type = item.itemType;
        r.pos_x = FloatBits(item.currentPosition.x);
        r.pos_y = FloatBits(item.currentPosition.y);
        r.pos_z = FloatBits(item.currentPosition.z);
        r.vel_x = FloatBits(item.startPosition.x);
        r.vel_y = FloatBits(item.startPosition.y);
        r.vel_z = FloatBits(item.startPosition.z);
        r.magnitude = 0;
        r.towards = 0;
        r.time = item.timer.current;
        r.intangible = 0;
        r.collector = item.collector;
    }
    return n;
}

#include "multi/Bot.h"

#include "BulletManager.hpp"
#include "Controller.hpp"
#include "EnemyManager.hpp"
#include "GameManager.hpp"
#include "Gui.hpp"
#include "ItemManager.hpp"
#include "Player.hpp"
#include "Supervisor.hpp"
#include "multi/MpConfig.h"

#include <intrin.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>
#include "multi/RuntimeData.h"

namespace th07 {
namespace bot {
namespace {

const int kSeats = 4;

float Abs(float v) { return v < 0.0f ? -v : v; }
float Min(float a, float b) { return a < b ? a : b; }
float Max(float a, float b) { return a > b ? a : b; }
float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
bool Finite(float v) { return v == v && v > -100000.0f && v < 100000.0f; }

// The player's movement area (Player::HandlePlayerInputs).
const float kMinX = 8.0f;
const float kMaxX = 376.0f;
const float kMinY = 16.0f;
const float kMaxY = 432.0f;
const float kMidX = 192.0f;

const unsigned short kShot = TH_BUTTON_SHOOT;
const unsigned short kBomb = TH_BUTTON_BOMB;
const unsigned short kFocus = TH_BUTTON_FOCUS;

// Sample t compares the player after t moves with hazards after t steps (the bot runs before
// the player and the bullets move).

const int kSamples = 9;
const int kSampleT[kSamples] = {0, 1, 2, 4, 8, 12, 18, 26, 34};
const int kGoalSample = 6;
const int kHorizon = 34;
const unsigned kDecisionEvery = 8;
const float kReplanBelow = 10.0f;
const unsigned kMapEvery = 32;

const int kMaxCircles = 64;
const int kBulletCandidates = 64;
const int kMaxSegs = 48;
const int kMapBulletStride = 8;
const int kMaxMapBullets = (0x7d1 + kMapBulletStride - 1) / kMapBulletStride + 8;
const int kItemStride = 4;
const int kNearCap = 24;
const int kSegNearCap = 8;
const int kMaxLead = 30;      // lockstep input delay
const int kHistory = 64;      // power of two
const int kLaserWarnAhead = 60;

struct Circle {
    float x[kSamples];
    float y[kSamples];
    float rad;
    float grow;
    short growT;
    short from;
    float grow2;
    short grow2From;
    unsigned char kind; // 0 bullet, 1 enemy body, 2 boss body
};

float Extra(const Circle& c, float t)
{
    float e = c.grow * Min(t, static_cast<float>(c.growT));
    if (c.grow2 > 0.0f && t > c.grow2From) {
        e += c.grow2 * (t - static_cast<float>(c.grow2From));
    }
    return e;
}

struct Seg {
    float ax[kSamples];
    float ay[kSamples];
    float bx[kSamples];
    float by[kSamples];
    float half;
    float grow;
    short harmFrom;
    short harmTo;   // exclusive
};

const int kCell = 32;
const int kGridW = 12;
const int kGridH = 13;
const unsigned kCoarseEvery = 64;

struct MapBullet {
    float x, y, vx, vy, radius;
};

struct Scratch {
    const Bullet* bulletPtr[kBulletCandidates];
    short bulletHeap[kBulletCandidates];
    float bulletScore[kBulletCandidates];
    int bulletCandidates;
    MapBullet mapBullet[kMaxMapBullets];
    int mapBullets;
    Circle circle[kMaxCircles];
    float circleScore[kMaxCircles];
    short circleHeap[kMaxCircles];
    int circles;
    Seg seg[kMaxSegs];
    float segScore[kMaxSegs];
    short segHeap[kMaxSegs];
    int segs;
    short near_[kSamples][kMaxCircles];
    float nearKey[kMaxCircles];
    short nearHeap[kNearCap];
    int nearCount[kSamples];
    int nearWork;
    short segNear[kSamples][kMaxSegs];
    int segNearCount[kSamples];
    float danger[kGridH][kGridW];
};
Scratch g_scratch;

struct SeatMemory {
    bool valid;
    unsigned lastFrame;
    unsigned short lastOutput;
    int lastAction;
    float targetX;
    float targetY;
    unsigned targetFrame;
    float lastWorst;
    int lastGoalMode;
    unsigned short history[kHistory]; // by frame % kHistory
    unsigned mapFrame;
    float danger[kGridH][kGridW];
    unsigned itemPhase;
};
SeatMemory g_memory[kSeats];

struct SeatStats {
    bool started;
    unsigned lastFrame;
    int lastState;
    int stage;
    unsigned frames;
    unsigned hits;
    unsigned misses;
    unsigned deathBombs;
    unsigned bombsPressed;
    unsigned releasesPressed;
    unsigned stageFrames;
    unsigned stageMisses;
    unsigned stageHits;
    unsigned stageDeathBombs;
    double sumY;
    unsigned yBins[28];
    unsigned high;
    unsigned bossFrames;
    unsigned bossHigh;
    unsigned bossClose;
    unsigned plans;
    unsigned long long planCycles;
    unsigned long long planMaxCycles;
    unsigned slowPlans;
    unsigned calls;
    unsigned long long callCycles;
    unsigned long long callMaxCycles;
    unsigned nextReport;
};
SeatStats g_stats[kSeats];

// No CRT heap here: the runtime may give it to the game.

char g_logPath[MAX_PATH];
int g_logState; // 0 unread, 1 file, 2 none
unsigned long long g_logCycles;

void LogLine(const char* text)
{
    const unsigned long long begin = __rdtsc();
    if (g_logState == 0) {
        const DWORD n = GetEnvironmentVariableA("TH07_BOT_LOG", g_logPath, sizeof(g_logPath));
        g_logState = n != 0 && n < sizeof(g_logPath) ? 1 : 2;
    }
    if (g_logState == 1) {
        HANDLE file = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            char line[512];
            const int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%s\r\n", text);
            DWORD written = 0;
            WriteFile(file, line, n > 0 ? static_cast<DWORD>(n) : static_cast<DWORD>(strlen(line)), &written, NULL);
            CloseHandle(file);
        }
    } else {
        mp::LogRaw(text);
    }
    g_logCycles += __rdtsc() - begin;
}

float GameSpeed()
{
    const float gs = g_Supervisor.effectiveFramerateMultiplier;
    return gs > 0.05f && gs <= 4.0f ? gs : 1.0f;
}

// 1 controllable, 4 hit (death-bomb window), 2 otherwise.
int BotState(const Player* p)
{
    switch (p->playerState) {
    case PLAYER_STATE_ALIVE:
    case PLAYER_STATE_INVULNERABLE:
    case PLAYER_STATE_BORDER:
        return 1;
    case PLAYER_STATE_DEAD:
        return p->respawnTimer != 0 ? 4 : 2;
    default:
        return 2;
    }
}

struct PlayerView {
    const Player* p;
    float x;
    float y;
    int state;
    int invuln;
    float hitR;
    float hh;
    float speed[2];
    float speedDiag[2];
    float scale;
};

const Player* SeatPlayer(int seat)
{
    if (seat < 0 || seat >= kSeats || seat >= PlayerCount()) {
        return NULL;
    }
    const Player* p = &g_Players[seat];
    return p->calcChain != NULL && p->shooterData != NULL ? p : NULL;
}

bool ReadPlayer(int seat, PlayerView* out)
{
    const Player* p = SeatPlayer(seat);
    if (p == NULL) {
        return false;
    }
    out->p = p;
    out->x = p->positionCenter.x;
    out->y = p->positionCenter.y;
    out->state = BotState(p);
    out->invuln = p->playerState == PLAYER_STATE_INVULNERABLE || p->playerState == PLAYER_STATE_BORDER
                      ? p->invulnerabilityTimer.current
                      : 0;
    if (!Finite(out->x) || !Finite(out->y)) {
        return false;
    }
    const float h = p->hitboxSize.x;
    out->hh = h > 0.25f && h < 32.0f ? h : 1.5f;
    out->hitR = out->hh;
    const ShtData* sht = p->shooterData;
    const float raw[4] = {sht->speed, sht->speedFocus, sht->speedDiagonal, sht->speedDiagonalFocus};
    const float fallback[4] = {4.0f, 2.0f, 2.83f, 1.41f};
    float v[4];
    for (int i = 0; i < 4; ++i) {
        v[i] = raw[i];
        if (!(v[i] > 0.2f && v[i] < 16.0f)) {
            v[i] = fallback[i];
        }
    }
    out->speed[0] = v[0];
    out->speed[1] = v[1];
    out->speedDiag[0] = v[2];
    out->speedDiag[1] = v[3];
    out->scale = GameSpeed();
    return true;
}

// 17 actions: 9 directions unfocused, then 8 moving directions focused.
const unsigned short kDirBits[9] = {0, 0x10, 0x20, 0x40, 0x80, 0x50, 0x90, 0x60, 0xA0};
const int kActions = 17;

int ActionDir(int action) { return action < 9 ? action : action - 8; }
bool ActionFocus(int action) { return action >= 9; }

void ActionVelocity(const PlayerView& pl, int action, float* vx, float* vy)
{
    const unsigned short bits = kDirBits[ActionDir(action)];
    const int dx = ((bits & 0x80u) != 0 ? 1 : 0) - ((bits & 0x40u) != 0 ? 1 : 0);
    const int dy = ((bits & 0x20u) != 0 ? 1 : 0) - ((bits & 0x10u) != 0 ? 1 : 0);
    const int f = ActionFocus(action) ? 1 : 0;
    const float s = (dx != 0 && dy != 0) ? pl.speedDiag[f] : pl.speed[f];
    *vx = static_cast<float>(dx) * s * pl.scale;
    *vy = static_cast<float>(dy) * s * pl.scale;
}

bool FartherSlot(int a, int b, const float* keys)
{
    return keys[a] > keys[b] || (keys[a] == keys[b] && a < b);
}

int KeepNearest(float score, float* keys, short* heap, int& count, int cap)
{
    if (count < cap) {
        const int slot = count++;
        keys[slot] = score;
        int pos = slot;
        while (pos > 0) {
            const int parent = (pos - 1) / 2;
            if (!FartherSlot(slot, heap[parent], keys)) break;
            heap[pos] = heap[parent];
            pos = parent;
        }
        heap[pos] = static_cast<short>(slot);
        return slot;
    }
    const int slot = heap[0];
    if (!(score < keys[slot])) return -1;
    keys[slot] = score;
    int pos = 0;
    while (pos * 2 + 1 < count) {
        int child = pos * 2 + 1;
        if (child + 1 < count && FartherSlot(heap[child + 1], heap[child], keys)) ++child;
        if (!FartherSlot(heap[child], slot, keys)) break;
        heap[pos] = heap[child];
        pos = child;
    }
    heap[pos] = static_cast<short>(slot);
    return slot;
}

int CircleSlot(float score)
{
    Scratch& s = g_scratch;
    return KeepNearest(score, s.circleScore, s.circleHeap, s.circles, kMaxCircles);
}
bool BulletShape(const Bullet* b, float* radius, bool* round)
{
    const float w = b->sprites.grazeSize.x;
    const float h = b->sprites.grazeSize.y;
    if (!(w > 0.0f) || !Finite(w) || !Finite(h)) {
        return false;
    }
    *round = false;
    *radius = Max(w, Abs(h)) * 0.5f;
    return *radius > 0.0f && *radius < 256.0f;
}

bool BulletLive(const Bullet* b)
{
    return b->state == BULLET_NORMAL || b->state == BULLET_SPAWNING_FAST || b->state == BULLET_SPAWNING_NORMAL ||
           b->state == BULLET_SPAWNING_SLOW;
}

bool PredictBullet(const Bullet* b, const PlayerView& pl, Circle* c, float* urgency)
{
    if (!BulletLive(b)) {
        return false;
    }
    float radius;
    bool round;
    if (!BulletShape(b, &radius, &round)) {
        return false;
    }
    const float gs = pl.scale;
    const float x0 = b->pos.x;
    const float y0 = b->pos.y;
    float vx = b->velocity.x * gs;
    float vy = b->velocity.y * gs;
    if (!Finite(x0) || !Finite(y0) || !Finite(vx) || !Finite(vy)) {
        return false;
    }
    const float speed = Abs(vx) + Abs(vy);
    const float reach = (speed + 8.0f) * kHorizon + radius + 16.0f;
    if (Abs(x0 - pl.x) > reach || Abs(y0 - pl.y) > reach) {
        return false;
    }
    float grow = 0.12f;
    int growT = kHorizon;
    float m = 1.0f;
    if (b->state != BULLET_NORMAL) {
        m = b->state == BULLET_SPAWNING_FAST ? 0.5f : (b->state == BULLET_SPAWNING_NORMAL ? 0.4f : 0.33f);
        grow += 0.05f * speed;
    }
    float grow2 = 0.0f;
    int changeAt = 1000;
    if (b->exFlags != 0) {
        grow2 = Max(0.3f + 0.25f * speed, 0.8f);
        changeAt = 1;
        grow += 0.2f;
    }
    c->rad = (radius + pl.hh) * 1.15f;
    c->grow = grow;
    c->growT = static_cast<short>(growT);
    c->from = 0;
    c->grow2 = changeAt <= kHorizon ? grow2 : 0.0f;
    c->grow2From = static_cast<short>(changeAt <= kHorizon ? changeAt : kHorizon);
    c->kind = 0;
    vx *= m;
    vy *= m;
    float best = 1000000.0f;
    for (int k = 0; k < kSamples; ++k) {
        const float t = static_cast<float>(kSampleT[k]);
        const float x = x0 + vx * t;
        const float y = y0 + vy * t;
        c->x[k] = x;
        c->y[k] = y;
        best = Min(best, Abs(x - pl.x) + Abs(y - pl.y) - c->rad - Extra(*c, t) - pl.speed[0] * 1.42f * t);
    }
    *urgency = best;
    return best < 48.0f;
}

void CollectBullets(const PlayerView& pl, unsigned mapPhase, bool sampleMap)
{
    Scratch& s = g_scratch;
    s.bulletCandidates = 0;
    s.mapBullets = 0;
    const float gs = pl.scale;
    for (int i = 0; i < MAX_BULLETS; ++i) {
        const Bullet* b = &g_BulletManager.bullets[i];
        if (!BulletLive(b)) continue;
        float radius;
        bool round;
        if (!BulletShape(b, &radius, &round)) continue;
        const float x = b->pos.x, y = b->pos.y;
        const float vx = b->velocity.x * gs, vy = b->velocity.y * gs;
        if (!Finite(x) || !Finite(y) || !Finite(vx) || !Finite(vy)) continue;
        if (sampleMap && s.mapBullets < kMaxMapBullets &&
            (i & (kMapBulletStride - 1)) == static_cast<int>(mapPhase % kMapBulletStride)) {
            MapBullet& m = s.mapBullet[s.mapBullets++];
            m.x = x;
            m.y = y;
            m.vx = vx;
            m.vy = vy;
            m.radius = radius;
        }
        const float dx = x - pl.x, dy = y - pl.y;
        const float score = Min(Abs(dx) + Abs(dy),
                                Min(Abs(dx + vx * 8.0f) + Abs(dy + vy * 8.0f) + 8.0f,
                                    Abs(dx + vx * 20.0f) + Abs(dy + vy * 20.0f) + 20.0f)) -
                            radius;
        if (score > 224.0f) continue;
        const int candidate = KeepNearest(score, s.bulletScore, s.bulletHeap, s.bulletCandidates, kBulletCandidates);
        if (candidate >= 0) s.bulletPtr[candidate] = b;
    }
    Circle tmp;
    for (int j = 0; j < s.bulletCandidates; ++j) {
        float urgency = 0.0f;
        if (!PredictBullet(s.bulletPtr[j], pl, &tmp, &urgency)) {
            continue;
        }
        const int slot = CircleSlot(urgency);
        if (slot < 0) {
            continue;
        }
        s.circle[slot] = tmp;
    }
}

struct WorldView {
    bool boss;
    float bossX;
    float bossY;
    bool aim;
    float aimX;
    int enemies;
};

void CollectEnemies(const PlayerView& pl, WorldView* w)
{
    w->boss = false;
    w->aim = false;
    w->enemies = 0;
    float bestAim = 1000000.0f;
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        const Enemy& e = g_EnemyManager.enemies[i];
        if (!e.active) continue;
        const float x = e.pos.x;
        const float y = e.pos.y;
        if (!Finite(x) || !Finite(y)) {
            continue;
        }
        ++w->enemies;
        const bool boss = e.isBoss && e.life > 0;
        if (boss && !w->boss) {
            w->boss = true;
            w->bossX = x;
            w->bossY = y;
        }
        const bool shootable = e.life > 0 && e.canDie && e.isHittable && e.canBeDamaged && !e.hasNoCollision;
        if (shootable && x > kMinX && x < kMaxX && y > 16.0f && y < pl.y - 24.0f) {
            const float score = boss ? -1000.0f : Abs(pl.y - y) * 1.5f + Abs(pl.x - x);
            if (score < bestAim) {
                bestAim = score;
                w->aim = true;
                w->aimX = x;
            }
        }
        if (!e.canDie || !e.hasContactHitbox || e.hasNoCollision || e.invisibleOnBomb) {
            continue;
        }
        const float bw = Max(Abs(e.hitboxSize.x), Abs(e.hitboxSize.y)) / 1.5f;
        if (!(bw > 0.0f) || bw > 512.0f) {
            continue;
        }
        float vx = Clamp(e.deltaPos.x, -8.0f, 8.0f);
        float vy = Clamp(e.deltaPos.y, -8.0f, 8.0f);
        if (!Finite(vx) || !Finite(vy)) {
            vx = vy = 0.0f;
        }
        const float reach = (Abs(vx) + Abs(vy) + 8.0f) * kHorizon + bw;
        if (Abs(x - pl.x) > reach || Abs(y - pl.y) > reach) {
            continue;
        }
        Circle c;
        const float b = bw * 0.5f;
        c.rad = (b + pl.hh) * 1.15f + (boss ? 12.0f : 4.0f);
        c.grow = boss ? 0.8f : 0.5f;
        c.growT = kHorizon;
        c.from = 0;
        c.grow2 = 0.0f;
        c.grow2From = kHorizon;
        c.kind = static_cast<unsigned char>(boss ? 2 : 1);
        float best = 1000000.0f;
        for (int k = 0; k < kSamples; ++k) {
            const float t = static_cast<float>(kSampleT[k]);
            c.x[k] = x + vx * t;
            c.y[k] = y + vy * t;
            const float d = Abs(c.x[k] - pl.x) + Abs(c.y[k] - pl.y) - c.rad - pl.speed[0] * 1.42f * t;
            best = Min(best, d);
        }
        if (best > 48.0f) {
            continue;
        }
        const int slot = CircleSlot(best);
        if (slot >= 0) {
            g_scratch.circle[slot] = c;
        }
    }
}

float PointSegDistance(float px, float py, float ax, float ay, float bx, float by);

float SegUrgency(const PlayerView& pl, const Seg& s)
{
    const float vmax = pl.speed[0] * 1.42f;
    float best = 1000000.0f;
    for (int k = 1; k < kSamples; ++k) {
        const int t = kSampleT[k];
        if (t < s.harmFrom || t >= s.harmTo) {
            continue;
        }
        const float d = PointSegDistance(pl.x, pl.y, s.ax[k], s.ay[k], s.bx[k], s.by[k]) - s.half - s.grow * t - vmax * t;
        best = Min(best, d);
    }
    if (best >= 1000000.0f) {
        const int k = kSamples - 1;
        best = PointSegDistance(pl.x, pl.y, s.ax[k], s.ay[k], s.bx[k], s.by[k]) - s.half;
    }
    return best;
}

void AddSegment(const PlayerView& pl, const Seg& s)
{
    Scratch& sc = g_scratch;
    const float urgency = SegUrgency(pl, s);
    if (urgency > 96.0f) {
        return;
    }
    const int slot = KeepNearest(urgency, sc.segScore, sc.segHeap, sc.segs, kMaxSegs);
    if (slot < 0) return;
    sc.seg[slot] = s;
}
float LaserHalf(float width, const PlayerView& pl)
{
    return Max(width * 0.25f, 0.5f) + pl.hh * 1.2f;
}

void CollectLasers(const PlayerView& pl)
{
    for (int i = 0; i < ARRAY_SIZE_SIGNED(g_BulletManager.lasers); ++i) {
        const Laser* L = &g_BulletManager.lasers[i];
        if (!L->inUse) {
            continue;
        }
        const int t = L->timer.current;
        int harmFrom = 0, harmTo = 10000;
        if (L->state == LASER_SPAWNING) {
            harmFrom = L->hitboxStartTime - t;
            harmTo = L->startTime - t + 1 + L->duration + L->hitboxEndTime;
        } else if (L->state == LASER_ACTIVE) {
            harmTo = L->duration - t + 1 + L->hitboxEndTime;
        } else {
            harmTo = L->hitboxEndTime - t + 1;
        }
        if (harmFrom < 0) harmFrom = 0;
        if (harmFrom > kLaserWarnAhead || harmTo <= 0) {
            continue;
        }
        const float angle = L->angle;
        const float px = L->pos.x, py = L->pos.y;
        const float a0 = L->startOffset, b0 = L->endOffset;
        const float speed = L->speed * pl.scale;
        if (!Finite(angle) || !Finite(px) || !Finite(py) || !Finite(a0) || !Finite(b0) || !Finite(L->width)) {
            continue;
        }
        const float dx = cosf(angle), dy = sinf(angle);
        Seg s;
        s.half = LaserHalf(L->width, pl);
        s.grow = 0.3f;
        s.harmFrom = static_cast<short>(harmFrom);
        s.harmTo = static_cast<short>(harmTo > 10000 ? 10000 : harmTo);
        bool any = false;
        for (int k = 0; k < kSamples; ++k) {
            const float tf = static_cast<float>(kSampleT[k]);
            float b = Clamp(b0 + (Finite(speed) ? speed * tf : 0.0f), 0.0f, 1200.0f);
            float a = a0;
            if (Finite(L->startLength) && b - a > L->startLength && L->startLength > 0.0f) {
                a = b - L->startLength;
            }
            a = Clamp(a, 0.0f, b);
            s.ax[k] = px + dx * a;
            s.ay[k] = py + dy * a;
            s.bx[k] = px + dx * b;
            s.by[k] = py + dy * b;
            any = any || b - a > 4.0f;
        }
        if (any) {
            AddSegment(pl, s);
        }
    }
}

// Closest approach of r(s) = r0 + (r1 - r0) s, s in [0,1].
float SweptDistance(float r0x, float r0y, float r1x, float r1y)
{
    const float wx = r1x - r0x, wy = r1y - r0y;
    const float ww = wx * wx + wy * wy;
    float s = 0.0f;
    if (ww > 1e-6f) {
        s = Clamp(-(r0x * wx + r0y * wy) / ww, 0.0f, 1.0f);
    }
    const float dx = r0x + wx * s, dy = r0y + wy * s;
    return sqrtf(dx * dx + dy * dy);
}

float PointSegDistanceSquared(float px, float py, float ax, float ay, float bx, float by)
{
    const float wx = bx - ax, wy = by - ay;
    const float ww = wx * wx + wy * wy;
    float s = 0.0f;
    if (ww > 1e-6f) {
        s = Clamp(((px - ax) * wx + (py - ay) * wy) / ww, 0.0f, 1.0f);
    }
    const float dx = ax + wx * s - px, dy = ay + wy * s - py;
    return dx * dx + dy * dy;
}

float PointSegDistance(float px, float py, float ax, float ay, float bx, float by)
{
    return sqrtf(PointSegDistanceSquared(px, py, ax, ay, bx, by));
}

float Cross(float ax, float ay, float bx, float by) { return ax * by - ay * bx; }

float SegSegDistanceSquared(float p0x, float p0y, float p1x, float p1y, float q0x, float q0y, float q1x, float q1y)
{
    const float d1 = Cross(q1x - q0x, q1y - q0y, p0x - q0x, p0y - q0y);
    const float d2 = Cross(q1x - q0x, q1y - q0y, p1x - q0x, p1y - q0y);
    const float d3 = Cross(p1x - p0x, p1y - p0y, q0x - p0x, q0y - p0y);
    const float d4 = Cross(p1x - p0x, p1y - p0y, q1x - p0x, q1y - p0y);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
        return 0.0f;
    }
    float d = PointSegDistanceSquared(p0x, p0y, q0x, q0y, q1x, q1y);
    d = Min(d, PointSegDistanceSquared(p1x, p1y, q0x, q0y, q1x, q1y));
    d = Min(d, PointSegDistanceSquared(q0x, q0y, p0x, p0y, p1x, p1y));
    d = Min(d, PointSegDistanceSquared(q1x, q1y, p0x, p0y, p1x, p1y));
    return d;
}

float CellX(int cx) { return Min(kMinX + (static_cast<float>(cx) + 0.5f) * kCell, kMaxX - 8.0f); }
float CellY(int cy) { return Min(kMinY + (static_cast<float>(cy) + 0.5f) * kCell, kMaxY - 8.0f); }

void Splat(float x, float y, float radius, float weight)
{
    Scratch& s = g_scratch;
    const float left = (x - radius - kMinX) / kCell;
    const float right = (x + radius - kMinX) / kCell;
    const float top = (y - radius - kMinY) / kCell;
    const float bottom = (y + radius - kMinY) / kCell;
    if (right < 0 || bottom < 0 || left >= kGridW || top >= kGridH) {
        return;
    }
    const int x0 = static_cast<int>(Max(left, 0.0f));
    const int x1 = static_cast<int>(Min(right, kGridW - 1.0f));
    const int y0 = static_cast<int>(Max(top, 0.0f));
    const int y1 = static_cast<int>(Min(bottom, kGridH - 1.0f));
    const float r2 = radius * radius;
    const __m128 xOffsets = _mm_set_ps(3.0f * kCell, 2.0f * kCell, 1.0f * kCell, 0.0f);
    const __m128 vx = _mm_set1_ps(x), vr = _mm_set1_ps(radius);
    const __m128 vr2 = _mm_set1_ps(r2), vw = _mm_set1_ps(weight);
    const __m128 one = _mm_set1_ps(1.0f);
    for (int cy = y0; cy <= y1; ++cy) {
        const float dy = CellY(cy) - y;
        int cx = x0;
        const __m128 dy2 = _mm_set1_ps(dy * dy);
        for (; cx + 3 <= x1 && cx + 3 < kGridW - 1; cx += 4) {
            const __m128 dx = _mm_sub_ps(_mm_add_ps(_mm_set1_ps(CellX(cx)), xOffsets), vx);
            const __m128 d2 = _mm_add_ps(_mm_mul_ps(dx, dx), dy2);
            const __m128 inside = _mm_cmplt_ps(d2, vr2);
            const __m128 value = _mm_mul_ps(vw, _mm_sub_ps(one, _mm_div_ps(_mm_sqrt_ps(d2), vr)));
            float* cells = &s.danger[cy][cx];
            _mm_storeu_ps(cells, _mm_add_ps(_mm_loadu_ps(cells), _mm_and_ps(inside, value)));
        }
        for (; cx <= x1; ++cx) {
            const float dx = CellX(cx) - x;
            const float d2 = dx * dx + dy * dy;
            if (d2 < r2) {
                s.danger[cy][cx] += weight * (1.0f - sqrtf(d2) / radius);
            }
        }
    }
}

void BuildDangerMap(const PlayerView& pl)
{
    Scratch& s = g_scratch;
    memset(s.danger, 0, sizeof(s.danger));
    const int times[2] = {12, 32};
    const float weights[2] = {1.75f, 1.5f};
    for (int i = 0; i < s.mapBullets; ++i) {
        const MapBullet& b = s.mapBullet[i];
        for (int k = 0; k < 2; ++k) {
            const float t = static_cast<float>(times[k]);
            Splat(b.x + b.vx * t, b.y + b.vy * t, b.radius + pl.hitR + 16.0f + t * 0.25f,
                  weights[k] * kMapBulletStride);
        }
    }
    for (int i = 0; i < s.segs; ++i) {
        const Seg& g = s.seg[i];
        if (g.harmTo <= 0 || g.harmFrom > kLaserWarnAhead) {
            continue;
        }
        int k = kSamples - 1;
        while (k > 1 && kSampleT[k] >= g.harmTo) {
            --k;
        }
        const float len = sqrtf((g.bx[k] - g.ax[k]) * (g.bx[k] - g.ax[k]) + (g.by[k] - g.ay[k]) * (g.by[k] - g.ay[k]));
        const int pieces = static_cast<int>(len / 24.0f) + 1;
        for (int p = 0; p <= pieces && p < 160; ++p) {
            const float u = static_cast<float>(p) / static_cast<float>(pieces);
            Splat(g.ax[k] + (g.bx[k] - g.ax[k]) * u, g.ay[k] + (g.by[k] - g.ay[k]) * u, g.half + 20.0f, 2.0f);
        }
    }
    for (int i = 0; i < s.circles; ++i) {
        const Circle& c = s.circle[i];
        if (c.kind != 0) {
            Splat(c.x[kSamples - 1], c.y[kSamples - 1], c.rad + (c.kind == 2 ? 40.0f : 20.0f),
                  c.kind == 2 ? 3.0f : 1.5f);
        }
    }
}

float DangerAt(float x, float y)
{
    int cx = static_cast<int>((x - kMinX) / kCell);
    int cy = static_cast<int>((y - kMinY) / kCell);
    cx = cx < 0 ? 0 : (cx >= kGridW ? kGridW - 1 : cx);
    cy = cy < 0 ? 0 : (cy >= kGridH ? kGridH - 1 : cy);
    return g_scratch.danger[cy][cx];
}

struct Goal {
    float aimX;
    bool hasAim;
    bool boss;
    float bossX;
    float bossY;
    float partnerX;
    float partnerY;
    bool partner;
    float itemX[8];
    float itemW[8];
    int items;
};
float ItemValue(int kind)
{
    switch (kind) {
    case ITEM_LIFE: return 3.0f;
    case ITEM_BOMB: return 2.6f;
    case ITEM_FULL_POWER: return 2.2f;
    case ITEM_POWER_BIG: return 1.2f;
    case ITEM_POWER_SMALL: return 0.8f;
    case ITEM_CHERRY: return 0.6f;
    case ITEM_POINT: return 0.35f;
    default: return 0.2f;
    }
}

void CollectItems(const PlayerView& pl, int seat, Goal* g)
{
    g->items = 0;
    const bool shared = PlayerCount() > 1;
    const int phase = static_cast<int>(g_memory[seat].itemPhase++ % kItemStride);
    for (int i = phase; i < 1100; i += kItemStride) {
        const Item& it = g_ItemManager.items[i];
        if (!it.isInUse || it.state != 0) {
            continue; // state 1 homes in, 2 scatters
        }
        if (shared && it.collector != seat) {
            continue;
        }
        const float x = it.currentPosition.x, y = it.currentPosition.y;
        if (!Finite(x) || !Finite(y) || x <= kMinX || x >= kMaxX || y <= 16.0f || y >= pl.y - 8.0f) {
            continue;
        }
        const float w = ItemValue(it.itemType);
        int slot = g->items < 8 ? g->items : -1;
        if (slot < 0) {
            int low = 0;
            for (int j = 1; j < 8; ++j) {
                if (g->itemW[j] < g->itemW[low]) low = j;
            }
            if (g->itemW[low] < w) slot = low;
        }
        if (slot >= 0) {
            g->itemX[slot] = x;
            g->itemW[slot] = w;
            if (slot == g->items) ++g->items;
        }
    }
}

float CellPreference(float x, float y, const Goal& g, int seat)
{
    float cost = 0.0f;
    if (y < 400.0f) {
        cost += (400.0f - y) * 0.05f;
    } else if (y > 408.0f) {
        cost += (y - 408.0f) * 0.04f;
    }
    if (g.boss) {
        if (y < 340.0f) {
            cost += (340.0f - y) * 0.25f;
        }
        const float dx = x - g.bossX, dy = y - g.bossY;
        const float d = sqrtf(dx * dx + dy * dy);
        if (d < 220.0f) {
            cost += (220.0f - d) * 0.08f;
        }
        if (d < 120.0f) {
            cost += (120.0f - d) * 0.3f;
        }
    }
    const float side = Abs(x - kMidX);
    if (side > 150.0f) {
        cost += (side - 150.0f) * 0.03f;
    }
    if (g.hasAim) {
        static const float kOffset[kSeats] = {-16.0f, 16.0f, -40.0f, 40.0f};
        const float want = g.aimX + kOffset[seat & 3];
        cost += Min(Abs(x - want), 160.0f) * 0.02f;
    }
    for (int i = 0; i < g.items; ++i) {
        const float d = Abs(x - g.itemX[i]);
        if (d < 56.0f) {
            cost -= g.itemW[i] * 0.25f * (1.0f - d / 56.0f);
        }
    }
    if (g.partner) {
        const float dx = x - g.partnerX, dy = y - g.partnerY;
        if (dx * dx + dy * dy < 36.0f * 36.0f) {
            cost += 0.4f;
        }
    }
    return cost;
}

void ChooseTarget(const PlayerView& pl, const Goal& g, int seat, SeatMemory& m, unsigned frame)
{
    float best = 1000000.0f;
    float bestX = pl.x, bestY = 408.0f;
    float previous = 1000000.0f;
    for (int cy = 0; cy < kGridH; ++cy) {
        const float y = CellY(cy);
        for (int cx = 0; cx < kGridW; ++cx) {
            const float x = CellX(cx);
            float cost = g_scratch.danger[cy][cx] * 2.5f + CellPreference(x, y, g, seat);
            const float dx = x - pl.x, dy = y - pl.y;
            const float dist = sqrtf(dx * dx + dy * dy);
            cost += dist * 0.004f;
            float way = 0.0f;
            for (int j = 1; j <= 3; ++j) {
                const float u = static_cast<float>(j) * 0.25f;
                way += DangerAt(pl.x + dx * u, pl.y + dy * u);
            }
            cost += way * 0.6f;
            if (cost < best) {
                best = cost;
                bestX = x;
                bestY = y;
            }
            if (m.targetFrame != 0xFFFFFFFFu && Abs(x - m.targetX) < 8.0f && Abs(y - m.targetY) < 8.0f) {
                previous = cost;
            }
        }
    }
    m.targetFrame = frame;
    if (previous < 1000000.0f) {
        const float jump = sqrtf((bestX - m.targetX) * (bestX - m.targetX) + (bestY - m.targetY) * (bestY - m.targetY));
        if (previous <= best + 2.0f + jump * 0.01f) {
            return;
        }
    }
    m.targetX = bestX;
    m.targetY = bestY;
}

struct Path {
    float x[kSamples];
    float y[kSamples];
};

void BuildNearLists(const PlayerView& pl)
{
    Scratch& s = g_scratch;
    s.nearWork = 0;
    const float vmax = Max(pl.speed[0], pl.speedDiag[0] * 1.4143f) * pl.scale;
    for (int k = 1; k < kSamples; ++k) {
        const float t = static_cast<float>(kSampleT[k]);
        const float reach = vmax * t + 13.0f;
        int n = 0;
        for (int i = 0; i < s.circles; ++i) {
            const Circle& c = s.circle[i];
            if (kSampleT[k] <= c.from) {
                continue;
            }
            const float d = PointSegDistance(pl.x, pl.y, c.x[k - 1], c.y[k - 1], c.x[k], c.y[k]);
            const float key = d - c.rad - Extra(c, t);
            if (key > reach) {
                continue;
            }
            const int slot = KeepNearest(key, s.nearKey, s.nearHeap, n, kNearCap);
            if (slot >= 0) s.near_[k][slot] = static_cast<short>(i);
        }
        s.nearCount[k] = n;
        s.nearWork += n;
        n = 0;
        for (int i = 0; i < s.segs; ++i) {
            const Seg& g = s.seg[i];
            if (kSampleT[k] < g.harmFrom || kSampleT[k - 1] + 1 >= g.harmTo) {
                continue;
            }
            const float sweep = Max(Abs(g.ax[k] - g.ax[k - 1]) + Abs(g.ay[k] - g.ay[k - 1]),
                                    Abs(g.bx[k] - g.bx[k - 1]) + Abs(g.by[k] - g.by[k - 1]));
            const float d = PointSegDistance(pl.x, pl.y, g.ax[k], g.ay[k], g.bx[k], g.by[k]);
            const float key = d - g.half - g.grow * t - sweep;
            if (key > reach) {
                continue;
            }
            const int slot = KeepNearest(key, s.nearKey, s.nearHeap, n, kSegNearCap);
            if (slot >= 0) s.segNear[k][slot] = static_cast<short>(i);
        }
        s.segNearCount[k] = n;
        s.nearWork += n * 3;
    }
}

float SafetyScore(float worst)
{
    return worst >= 0.0f ? Min(worst, 10.0f) * 6.0f : worst * 18.0f;
}

struct PlanScore {
    float distanceCost;
    float dangerCost;
    float keepBonus;
    float lowerBonus;
    float best;

    float Eval(float worst) const
    {
        return SafetyScore(worst) - distanceCost - dangerCost + keepBonus + lowerBonus;
    }
};

// The game tests once per frame after both moved: a one-frame interval is a point test.
float IntervalSafety(const Path& p, int k0, int k1, int skip, float worst, const PlanScore* bound)
{
    Scratch& s = g_scratch;
    for (int k = k0 + 1; k <= k1; ++k) {
        const int t = kSampleT[k];
        if (t <= skip) {
            continue;
        }
        const bool step = t - kSampleT[k - 1] == 1;
        const float soon = static_cast<float>(kSampleT[k - 1]) * 0.22f;
        const float tf = static_cast<float>(t);
        const int n = s.nearCount[k];
        for (int j = 0; j < n; ++j) {
            const Circle& c = s.circle[s.near_[k][j]];
            float d;
            if (step) {
                const float dx = p.x[k] - c.x[k], dy = p.y[k] - c.y[k];
                d = sqrtf(dx * dx + dy * dy);
            } else {
                d = SweptDistance(p.x[k - 1] - c.x[k - 1], p.y[k - 1] - c.y[k - 1], p.x[k] - c.x[k], p.y[k] - c.y[k]);
            }
            const float clear = d - c.rad - Extra(c, tf) - 1.0f + soon;
            if (clear < worst) {
                worst = clear;
            }
        }
        const int ns = s.segNearCount[k];
        for (int j = 0; j < ns; ++j) {
            const Seg& g = s.seg[s.segNear[k][j]];
            const float d = step ? PointSegDistance(p.x[k], p.y[k], g.ax[k], g.ay[k], g.bx[k], g.by[k])
                                 : sqrtf(Min(SegSegDistanceSquared(p.x[k - 1], p.y[k - 1], p.x[k], p.y[k], g.ax[k],
                                                                   g.ay[k], g.bx[k], g.by[k]),
                                             SegSegDistanceSquared(p.x[k - 1], p.y[k - 1], p.x[k], p.y[k],
                                                                   g.ax[k - 1], g.ay[k - 1], g.bx[k - 1], g.by[k - 1])));
            const float clear = d - g.half - g.grow * tf - 1.0f + soon;
            if (clear < worst) {
                worst = clear;
            }
        }
        if (bound != NULL && bound->Eval(worst) <= bound->best) {
            break;
        }
    }
    return worst;
}

float PlanDanger(float x, float y)
{
    const float side = 184.0f - Abs(x - kMidX);
    return DangerAt(x, y) * 2.0f + (side < 24.0f ? (24.0f - side) * 0.08f : 0.0f);
}

const float kTwoLegBelow = 4.0f;
const int kSplit = 4;

struct Plan {
    int action;
    float worst;
    float score;
};

// `lead` frames of input are already on their way.
Plan ChoosePlan(const PlayerView& pl, const SeatMemory& m, float targetX, float targetY, int lead,
                const unsigned short* committed)
{
    const int invuln = pl.invuln > 0 ? pl.invuln - 1 : 0;
    const int skip = invuln > lead ? invuln : lead;
    Path prefix;
    memset(&prefix, 0, sizeof(prefix));
    prefix.x[0] = pl.x;
    prefix.y[0] = pl.y;
    float startX = pl.x, startY = pl.y;
    int nextSample = 1;
    for (int t = 1; t <= lead && t <= kHorizon; ++t) {
        int action = 0;
        const unsigned short bits = committed[t - 1];
        for (int dir = 0; dir < 9; ++dir) {
            if (kDirBits[dir] == (bits & 0xF0u)) {
                action = dir == 0 ? 0 : ((bits & kFocus) != 0 ? dir + 8 : dir);
            }
        }
        float vx, vy;
        ActionVelocity(pl, action, &vx, &vy);
        startX = Clamp(startX + vx, kMinX, kMaxX);
        startY = Clamp(startY + vy, kMinY, kMaxY);
        if (nextSample < kSamples && t == kSampleT[nextSample]) {
            prefix.x[nextSample] = startX;
            prefix.y[nextSample] = startY;
            ++nextSample;
        }
    }
    int scoreSample = kGoalSample;
    while (scoreSample < kSamples - 1 && kSampleT[scoreSample] < lead + static_cast<int>(kDecisionEvery)) {
        ++scoreSample;
    }
    Plan best = {0, -1000000.0f, -1000000.0f};
    for (int order = 0; order < kActions; ++order) {
        const int a = order == 0 ? m.lastAction : (order <= m.lastAction ? order - 1 : order);
        float vx, vy;
        ActionVelocity(pl, a, &vx, &vy);
        Path path = prefix;
        for (int k = nextSample; k < kSamples; ++k) {
            const float t = static_cast<float>(kSampleT[k] - lead);
            path.x[k] = Clamp(startX + vx * t, kMinX, kMaxX);
            path.y[k] = Clamp(startY + vy * t, kMinY, kMaxY);
        }
        const float ex = path.x[scoreSample] - targetX, ey = path.y[scoreSample] - targetY;
        const PlanScore bound = {sqrtf(ex * ex + ey * ey) * 0.3f, PlanDanger(path.x[scoreSample], path.y[scoreSample]),
                                 a == m.lastAction ? 0.8f : 0.0f,
                                 (kDirBits[ActionDir(a)] & 0x20u) != 0 && startY < 400.0f ? 0.1f : 0.0f, best.score};
        if (bound.Eval(1000000.0f) <= best.score) continue;
        const float worst = IntervalSafety(path, 0, kSamples - 1, skip, 1000000.0f, &bound);
        const float score = bound.Eval(worst);
        if (score > best.score) {
            best.action = a;
            best.worst = worst;
            best.score = score;
        }
    }
    if (lead != 0 || best.worst >= kTwoLegBelow) {
        return best;
    }
    Path first[kActions];
    float firstWorst[kActions];
    for (int a = 0; a < kActions; ++a) {
        float vx, vy;
        ActionVelocity(pl, a, &vx, &vy);
        first[a] = prefix;
        for (int k = 1; k <= kSplit; ++k) {
            const float t = static_cast<float>(kSampleT[k]);
            first[a].x[k] = Clamp(startX + vx * t, kMinX, kMaxX);
            first[a].y[k] = Clamp(startY + vy * t, kMinY, kMaxY);
        }
        firstWorst[a] = IntervalSafety(first[a], 0, kSplit, skip, 1000000.0f, NULL);
    }
    for (int a = 0; a < kActions; ++a) {
        if (!(firstWorst[a] > best.worst)) {
            continue;
        }
        const float splitX = first[a].x[kSplit], splitY = first[a].y[kSplit];
        for (int b = 0; b < kActions; ++b) {
            if (b == a) {
                continue;
            }
            float vx, vy;
            ActionVelocity(pl, b, &vx, &vy);
            Path path = first[a];
            for (int k = kSplit + 1; k < kSamples; ++k) {
                const float t = static_cast<float>(kSampleT[k] - kSampleT[kSplit]);
                path.x[k] = Clamp(splitX + vx * t, kMinX, kMaxX);
                path.y[k] = Clamp(splitY + vy * t, kMinY, kMaxY);
            }
            const float ex = path.x[scoreSample] - targetX, ey = path.y[scoreSample] - targetY;
            const PlanScore bound = {sqrtf(ex * ex + ey * ey) * 0.3f + 0.3f,
                                     PlanDanger(path.x[scoreSample], path.y[scoreSample]),
                                     a == m.lastAction ? 0.8f : 0.0f, 0.0f, best.score};
            if (bound.Eval(firstWorst[a]) <= best.score) continue;
            const float worst = IntervalSafety(path, kSplit, kSamples - 1, skip, firstWorst[a], &bound);
            const float score = bound.Eval(worst);
            if (score > best.score) {
                best.action = a;
                best.worst = worst;
                best.score = score;
            }
        }
    }
    return best;
}
bool CanBomb(int seat)
{
    const Player* p = SeatPlayer(seat);
    return p != NULL && !p->bombInfo.isInUse && !g_Gui.HasCurrentMsgIdx() && p->respawnTimer != 0 &&
           p->borderInvulnerabilityTime == 0 && g_GameManager.globals != NULL &&
           (int)g_GameManager.Bombs(seat) > 0 && p->hasBorder == BORDER_NONE;
}

void ResetSeat(SeatMemory& m)
{
    memset(&m, 0, sizeof(m));
    m.lastAction = 0;
    m.targetFrame = 0xFFFFFFFFu;
    m.mapFrame = 0xFFFFFFFFu;
}

unsigned short Decide(unsigned frame, int seat, int lead)
{
    SeatMemory& m = g_memory[seat];
    const bool first = m.lastOutput == 0;
    const Player* player = SeatPlayer(seat);
    if (player == NULL) return kShot;
    const int state = BotState(player);
    if (state == 4) {
        bool pressed = false;
        for (int i = 1; i <= lead + 1 && !pressed; ++i) {
            pressed = (m.history[(frame - static_cast<unsigned>(i)) % kHistory] & kBomb) != 0;
        }
        return static_cast<unsigned short>(kShot | (!pressed && CanBomb(seat) ? kBomb : 0u));
    }
    if (state != 1) {
        return kShot;
    }
    if (frame % kDecisionEvery != 0 && !first && !(m.lastWorst < kReplanBelow)) {
        return static_cast<unsigned short>((m.lastOutput & ~kBomb) | kShot);
    }
    PlayerView pl;
    if (!ReadPlayer(seat, &pl)) return kShot;
    const unsigned long long begin = __rdtsc();

    Scratch& s = g_scratch;
    s.circles = 0;
    s.segs = 0;
    const bool stale = m.targetFrame == 0xFFFFFFFFu || frame - m.targetFrame >= kCoarseEvery || frame < m.targetFrame;
    const bool updateMap = stale || frame % kMapEvery == 0 || m.mapFrame == 0xFFFFFFFFu;
    WorldView world;
    CollectBullets(pl, frame / kMapEvery, updateMap);
    CollectEnemies(pl, &world);
    CollectLasers(pl);

    if (updateMap) {
        BuildDangerMap(pl);
        m.mapFrame = frame;
        memcpy(m.danger, s.danger, sizeof(m.danger));
    } else {
        // Several local bots share the scratch space.
        memcpy(s.danger, m.danger, sizeof(s.danger));
    }
    if (stale || DangerAt(m.targetX, m.targetY) > 1.5f) {
        Goal goal;
        memset(&goal, 0, sizeof(goal));
        goal.boss = world.boss;
        goal.bossX = world.bossX;
        goal.bossY = world.bossY;
        goal.hasAim = world.aim;
        goal.aimX = world.aim ? world.aimX : 0.0f;
        float nearest = 1e30f;
        for (int other = 0; other < PlayerCount() && other < kSeats; ++other) {
            PlayerView partner;
            if (other == seat || !ReadPlayer(other, &partner) || partner.state != 1) {
                continue;
            }
            const float d = (partner.x - pl.x) * (partner.x - pl.x) + (partner.y - pl.y) * (partner.y - pl.y);
            if (d < nearest) {
                nearest = d;
                goal.partner = true;
                goal.partnerX = partner.x;
                goal.partnerY = partner.y;
            }
        }
        CollectItems(pl, seat, &goal);
        ChooseTarget(pl, goal, seat, m, frame);
    }

    BuildNearLists(pl);
    unsigned short committed[kMaxLead];
    for (int t = 1; t <= lead; ++t) {
        committed[t - 1] = m.history[(frame - static_cast<unsigned>(lead) + static_cast<unsigned>(t) - 1u) % kHistory];
    }
    const Plan plan = ChoosePlan(pl, m, m.targetX, m.targetY, lead, committed);
    m.lastAction = plan.action;
    m.lastWorst = plan.worst;
    m.lastGoalMode = world.boss ? 1 : (world.aim ? 2 : 0);
    unsigned short out =
        static_cast<unsigned short>(kShot | kDirBits[ActionDir(plan.action)] | (ActionFocus(plan.action) ? kFocus : 0u));
    if (lead >= 7 && plan.worst < -1.0f && (m.lastOutput & kBomb) == 0 && CanBomb(seat)) {
        out = static_cast<unsigned short>(out | kBomb);
    }

    const unsigned long long cycles = __rdtsc() - begin;
    SeatStats& st = g_stats[seat];
    ++st.plans;
    st.planCycles += cycles;
    if (cycles > 15000000ull) {
        ++st.slowPlans;
    }
    if (cycles > st.planMaxCycles) {
        st.planMaxCycles = cycles;
    }
    if ((frame % 300u) == 0) {
        char line[256];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "BOT frame=%u seat=%d pos=(%.1f,%.1f) target=(%.1f,%.1f) goal=%d boss=%d hazards=%d/%d near=%d "
                    "worst=%.1f input=%02X",
                    frame, seat + 1, pl.x, pl.y, m.targetX, m.targetY, m.lastGoalMode, world.boss ? 1 : 0, s.circles,
                    s.segs, s.nearWork, plan.worst, static_cast<unsigned>(out));
        LogLine(line);
    }
    return out;
}

unsigned short Move(unsigned frame, int seat, int lead)
{
    SeatMemory& m = g_memory[seat];
    if (!m.valid || frame < m.lastFrame || frame > m.lastFrame + 60) {
        ResetSeat(m);
        m.valid = true;
    } else if (frame > m.lastFrame + 1) {
        for (unsigned f = m.lastFrame + 1; f < frame; ++f) {
            m.history[f % kHistory] = kShot;
        }
        m.lastOutput = 0;
    }
    const unsigned short out = Decide(frame, seat, lead < 0 ? 0 : (lead > kMaxLead ? kMaxLead : lead));
    m.history[frame % kHistory] = out;
    m.lastOutput = out;
    m.lastFrame = frame;
    return out;
}

void LogHit(int seat, unsigned frame, int stage, float x, float y)
{
    const Player* p = SeatPlayer(seat);
    const float hh = p != NULL ? p->hitboxSize.x : 1.5f;
    float bestB = 1e9f;
    unsigned bEx = 0;
    int bState = 0;
    float bSpeed = 0.0f, bRadius = 0.0f;
    for (int i = 0; i < MAX_BULLETS; ++i) {
        const Bullet* b = &g_BulletManager.bullets[i];
        if (b->state == BULLET_INACTIVE) continue;
        float radius;
        bool round;
        if (!BulletShape(b, &radius, &round)) continue;
        const float d = Max(Abs(b->pos.x - x), Abs(b->pos.y - y)) - radius - hh;
        if (d < bestB) {
            bestB = d;
            bEx = b->exFlags;
            bState = b->state;
            bSpeed = b->speed;
            bRadius = radius;
        }
    }
    float bestL = 1e9f;
    int lState = -1;
    for (int i = 0; i < ARRAY_SIZE_SIGNED(g_BulletManager.lasers); ++i) {
        const Laser* L = &g_BulletManager.lasers[i];
        if (!L->inUse) continue;
        const float ca = cosf(L->angle), sa = sinf(L->angle);
        const float d = PointSegDistance(x, y, L->pos.x + ca * L->startOffset, L->pos.y + sa * L->startOffset,
                                         L->pos.x + ca * L->endOffset, L->pos.y + sa * L->endOffset) -
                        L->width * 0.25f;
        if (d < bestL) {
            bestL = d;
            lState = L->state;
        }
    }
    char line[384];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "BOT_HIT seat=%d stage=%d frame=%u pos=(%.1f,%.1f) last_worst=%.1f bullet=%.1f(state=%d ex=%X "
                "speed=%.2f r=%.1f) laser=%.1f(state=%d)",
                seat + 1, stage, frame, x, y, g_memory[seat].lastWorst, bestB, bState, bEx, bSpeed, bRadius, bestL,
                lState);
    LogLine(line);
}

void Observe(unsigned frame, int seat, unsigned short output)
{
    SeatStats& st = g_stats[seat];
    const Player* player = SeatPlayer(seat);
    if (player == NULL) return;
    const int state = BotState(player);
    const float x = player->positionCenter.x, y = player->positionCenter.y;
    if (!Finite(x) || !Finite(y)) return;
    const int stage = g_GameManager.currentStage;
    if (!st.started) {
        memset(&st, 0, sizeof(st));
        st.started = true;
        st.lastState = state;
        st.stage = stage;
        st.nextReport = 1800;
        st.lastFrame = frame - 1;
    }
    if (stage != st.stage) {
        char line[256];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "BOT_STAGE seat=%d stage=%d frames=%u misses=%u hits=%u death_bombs=%u total_misses=%u next=%d",
                    seat + 1, st.stage, st.stageFrames, st.stageMisses, st.stageHits, st.stageDeathBombs, st.misses,
                    stage);
        LogLine(line);
        st.stage = stage;
        st.stageFrames = st.stageMisses = st.stageHits = st.stageDeathBombs = 0;
    }
    if (frame == st.lastFrame) {
        return;
    }
    const int before = st.lastState;
    if (before == 1 && state == 4) {
        ++st.hits;
        ++st.stageHits;
        LogHit(seat, frame, stage, x, y);
    }
    if (before != 2 && state == 2) {
        ++st.misses;
        ++st.stageMisses;
        char line[256];
        const SeatMemory& m = g_memory[seat];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "BOT_MISS seat=%d stage=%d frame=%u pos=(%.1f,%.1f) lives=%d last_worst=%.1f hazards=%d/%d",
                    seat + 1, stage, frame, x, y,
                    g_GameManager.globals != NULL ? (int)g_GameManager.Lives(seat) : 0, m.lastWorst, g_scratch.circles,
                    g_scratch.segs);
        LogLine(line);
    }
    if (before == 4 && state == 1) {
        ++st.deathBombs;
        ++st.stageDeathBombs;
    }
    st.lastState = state;
    st.lastFrame = frame;
    if ((output & kBomb) != 0) ++st.bombsPressed;
    if (state != 1) {
        return;
    }
    ++st.frames;
    ++st.stageFrames;
    st.sumY += y;
    int bin = static_cast<int>(y / 16.0f);
    bin = bin < 0 ? 0 : (bin > 27 ? 27 : bin);
    ++st.yBins[bin];
    if (y < 300.0f) ++st.high;
    if (g_memory[seat].lastGoalMode == 1) {
        ++st.bossFrames;
        if (y < 300.0f) ++st.bossHigh;
    }
    if (st.frames >= st.nextReport) {
        st.nextReport = st.frames + 1800;
        unsigned below = 0, p10 = 0;
        for (int i = 0; i < 28; ++i) {
            below += st.yBins[i];
            if (below * 10u >= st.frames) {
                p10 = static_cast<unsigned>(i * 16);
                break;
            }
        }
        char line[512];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "BOT_STATS seat=%d frames=%u hits=%u misses=%u death_bombs=%u bombs=%u y_mean=%.1f y_p10=%u "
                    "high=%.3f boss_frames=%u boss_high=%.3f plan_kcycles=%.0f/%.0f slow=%u plans=%u "
                    "frame_kcycles=%.1f call_kcycles_max=%.0f",
                    seat + 1, st.frames, st.hits, st.misses, st.deathBombs, st.bombsPressed,
                    st.frames != 0 ? st.sumY / st.frames : 0.0, p10,
                    st.frames != 0 ? static_cast<double>(st.high) / st.frames : 0.0, st.bossFrames,
                    st.bossFrames != 0 ? static_cast<double>(st.bossHigh) / st.bossFrames : 0.0,
                    st.plans != 0 ? static_cast<double>(st.planCycles) / st.plans / 1000.0 : 0.0,
                    static_cast<double>(st.planMaxCycles) / 1000.0, st.slowPlans, st.plans,
                    st.calls != 0 ? static_cast<double>(st.callCycles) / st.calls / 1000.0 : 0.0,
                    static_cast<double>(st.callMaxCycles) / 1000.0);
        LogLine(line);
    }
}

unsigned short DialogueMask(unsigned frame, int seat)
{
    if (mp::Cfg().testBotMash) {
        static const unsigned short kKeys[8] = {0, kShot, 0, kBomb, kFocus, kShot, 0x10, kShot | kFocus};
        const unsigned run = frame / (3u + static_cast<unsigned>(seat & 1));
        const unsigned idx = ((run * 2654435761u) ^ (seat != 0 ? 0x9E3779B9u : 0u)) >> 29;
        return kKeys[idx & 7];
    }
    if (seat != 0) {
        return 0;
    }
    return static_cast<unsigned short>(frame % 24u != 0 ? kShot : 0u);
}

}

unsigned short GameplayMask(unsigned frame, int seat, int lead)
{
    if (seat < 0 || seat >= kSeats) {
        return 0;
    }
    const unsigned long long begin = __rdtsc();
    const unsigned long long logBefore = g_logCycles;
    unsigned short out;
    if (g_Gui.HasCurrentMsgIdx()) {
        out = DialogueMask(frame, seat);
    } else if (SeatPlayer(seat) == NULL || ((mp::Cfg().testBotIdle >> seat) & 1u) != 0) {
        out = kShot;
    } else {
        out = Move(frame, seat, lead);
        if (mp::Cfg().testBotNoShot) {
            out = static_cast<unsigned short>(out & ~kShot);
        }
    }
    if (SeatPlayer(seat) != NULL) {
        Observe(frame, seat, out);
        SeatStats& st = g_stats[seat];
        const unsigned long long cycles = __rdtsc() - begin - (g_logCycles - logBefore);
        ++st.calls;
        st.callCycles += cycles;
        if (cycles > st.callMaxCycles) {
            st.callMaxCycles = cycles;
        }
    }
    return out;
}

bool SoloEnabled()
{
    static int s_solo = -1;
    if (s_solo < 0) {
        char value[8];
        const DWORD n = GetEnvironmentVariableA("TH07_BOT_SOLO", value, sizeof(value));
        s_solo = n == 1 && value[0] == '1' ? 1 : 0;
    }
    return s_solo != 0;
}

}
}

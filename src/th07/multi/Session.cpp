
#include "multi/Session.h"
#include "multi/SessionFrame.h"
#include "multi/MpConfig.h"
#include "multi/Net.h"
#include "multi/RollbackGame.h"
#include "AsciiManager.hpp"
#include "Bot.h"
#include "Controller.hpp"
#include "Coop.hpp"
#include "FrameInput.hpp"
#include "GameManager.hpp"
#include "GameWindow.hpp"
#include "Gui.hpp"
#include "Player.hpp"
#include "Rng.hpp"
#include "Supervisor.hpp"
#include <stdio.h>
#include <string.h>
#include "multi/RuntimeData.h"

extern i32 g_CoopTestGhostFrame;

enum
{
    SUPERVISOR_STATE_GAMEMANAGER = 2,
    SUPERVISOR_STATE_NEXT_STAGE = 3,
    SUPERVISOR_STATE_RESTART_FROM_BEGINNING = 10,
    SUPERVISOR_STATE_RESTART_STAGE = 11,
    SUPERVISOR_STATE_NEXT_STAGE_USELESS = 12,
};

namespace
{

bool g_shutdown;
unsigned g_localFrame;

void CoopLogToSeatLog(const char *line)
{
    th07::mp::LogRaw(line);
}

const char *NameOfSeat(i32 seat)
{
    return th07::mp::PlayerName(seat);
}

unsigned LocalDeviceButtons(int seat)
{
    if (th07::mp::UdpEnabled() || th07::mp::PlayerCount() == 1)
    {
        return ReadDeviceButtons(0);
    }
    if (seat == 0)
    {
        return ReadDeviceButtons(1);
    }
    return ReadJoypadButtonsOf(seat - 1);
}

int Playing()
{
    return g_Supervisor.wantedState == SUPERVISOR_STATE_GAMEMANAGER &&
           g_Supervisor.curState == SUPERVISOR_STATE_GAMEMANAGER && !g_GameManager.isInPauseMenu &&
           !g_GameManager.isInRetryMenu;
}

// In play the menus hear seat 0 and every seat's pause; otherwise every seat.
unsigned MenuMask(const unsigned *buttons, int count)
{
    int playing = Playing();
    unsigned mask = buttons[0];
    for (int seat = 1; seat < count; seat++)
    {
        mask |= playing ? (buttons[seat] & TH_BUTTON_MENU) : buttons[seat];
    }
    return mask;
}

unsigned TitleBotMask(unsigned frame)
{
    const th07::mp::Config &cfg = th07::mp::Cfg();
    int owner = cfg.testMenuSeat >= 0 ? cfg.testMenuSeat : 0;
    if (!cfg.testTitleBot || cfg.localSeat != owner || Playing())
    {
        return 0;
    }
    return frame % 20 == 0 ? TH_BUTTON_SHOOT : 0;
}

int RunFrame(const unsigned *buttons, int count, int draw)
{
    FrameInputs inputs = {};
    for (int seat = 0; seat < count && seat < MAX_PLAYERS; seat++)
    {
        inputs.held[seat] = (u16)buttons[seat];
    }
    inputs.menu = (u16)MenuMask(buttons, count);
    g_FrameInputs = &inputs;
    int status = RunLogicalFrame(draw);
    g_FrameInputs = NULL;
    return status;
}

}

void MpInitSession()
{
    const th07::mp::Config &cfg = th07::mp::Cfg();
    g_GameManager.playerCount = th07::mp::PlayerCount();
    static const u8 characters[MAX_PLAYERS] = {CHAR_REIMU, CHAR_MARISA, CHAR_SAKUYA, CHAR_REIMU};
    static const u8 shots[MAX_PLAYERS] = {0, 0, 0, 1};
    for (i32 seat = 0; seat < MAX_PLAYERS; seat++)
    {
        g_GameManager.Character(seat) = (u8)(cfg.testCharacters[seat] >= 0 ? cfg.testCharacters[seat] : characters[seat]);
        g_GameManager.ShotType(seat) = (u8)(cfg.testShotTypes[seat] >= 0 ? cfg.testShotTypes[seat] : shots[seat]);
    }
    g_GameManager.sessionSeed = cfg.sessionId;
    g_CoopTestGhostFrame = cfg.testGhostFrame != th07::mp::kNoFrame ? (i32)cfg.testGhostFrame : -1;
    if (cfg.mode == th07::mp::kUdp && cfg.rollback && !th07::rollback_game::Start())
    {
        th07::mp::Log("FAIL rollback could not start");
    }
    g_CoopLogSink = CoopLogToSeatLog;
    g_CoopNameSource = NameOfSeat;
    g_CoopViewSeat = cfg.mode == th07::mp::kUdp ? cfg.localSeat : 0;
    th07::mp::Log("SESSION mode=%s seat=%d players=%d rollback=%d delay=%u menu_delay=%u session=%08X",
                  cfg.mode == th07::mp::kUdp ? "udp" : "local", cfg.localSeat, cfg.playerCount, cfg.rollback ? 1 : 0,
                  cfg.artificialDelay, cfg.menuInputDelay, cfg.sessionId);
}

void MpRequestShutdown()
{
    g_shutdown = true;
}

int MpRunHostTick(int *present)
{
    *present = 0;
    if (g_shutdown)
    {
        return 0;
    }
    g_localFrame++;
    if (th07::mp::UdpEnabled())
    {
        return th07::net::RunHostTick(present);
    }
    const th07::mp::Config &cfg = th07::mp::Cfg();
    unsigned buttons[MAX_PLAYERS] = {};
    int count = th07::mp::PlayerCount();
    for (int seat = 0; seat < count; seat++)
    {
        buttons[seat] = LocalDeviceButtons(seat);
        if (seat == 0)
        {
            buttons[seat] |= TitleBotMask(g_localFrame);
        }
        if (cfg.testBot && Playing())
        {
            buttons[seat] = SessionBotMask(seat);
        }
    }
    int status = RunFrame(buttons, count, 1);
    *present = 1;
    return status;
}

unsigned short SessionEncodeButtons(unsigned buttons)
{
    return (unsigned short)(buttons & 0x7fff);
}

unsigned SessionDecodeButtons(unsigned short bits)
{
    return bits & 0x7fff;
}

int SessionRunFrame(const unsigned short *held, int count, int draw)
{
    unsigned buttons[MAX_PLAYERS] = {};
    for (int seat = 0; seat < count && seat < MAX_PLAYERS; seat++)
    {
        buttons[seat] = SessionDecodeButtons(held[seat]);
    }
    return RunFrame(buttons, count, draw);
}

unsigned short SessionBotMask(int seat)
{
    if (!Playing())
    {
        return 0;
    }
    const th07::mp::Config &cfg = th07::mp::Cfg();
    if (cfg.testGive == seat + 1)
    {
        i32 t = g_GameManager.framesThisStage;
        if (t >= 600 && t < 1200)
        {
            Player *self = &g_Players[seat];
            Player *target = NULL;
            f32 best = 1e30f;
            for (int other = 0; other < PlayerCount(); other++)
            {
                Player *p = &g_Players[other];
                if (other == seat || p->playerState == PLAYER_STATE_DEAD || p->playerState == PLAYER_STATE_SPAWNING)
                {
                    continue;
                }
                f32 dx = p->positionCenter.x - self->positionCenter.x;
                f32 dy = p->positionCenter.y - self->positionCenter.y;
                f32 d = dx * dx + dy * dy - (p->playerState == PLAYER_STATE_GHOST ? 1e6f : 0.0f);
                if (d < best)
                {
                    best = d;
                    target = p;
                }
            }
            unsigned out = TH_BUTTON_FOCUS;
            if (target != NULL)
            {
                f32 dx = target->positionCenter.x - self->positionCenter.x;
                f32 dy = target->positionCenter.y - self->positionCenter.y;
                out |= dx > 4.0f ? TH_BUTTON_RIGHT : dx < -4.0f ? TH_BUTTON_LEFT : 0;
                out |= dy > 4.0f ? TH_BUTTON_DOWN : dy < -4.0f ? TH_BUTTON_UP : 0;
            }
            return (unsigned short)out;
        }
    }
    return th07::bot::GameplayMask(g_localFrame, seat);
}

unsigned short SessionLocalInput(bool gameplay)
{
    const th07::mp::Config &cfg = th07::mp::Cfg();
    unsigned buttons = LocalDeviceButtons(cfg.localSeat) | TitleBotMask(g_localFrame);
    if (cfg.testBot && gameplay && Playing())
    {
        buttons = SessionBotMask(cfg.localSeat);
    }
    return SessionEncodeButtons(buttons);
}

int SessionGameplayActive()
{
    if (g_Supervisor.wantedState != SUPERVISOR_STATE_GAMEMANAGER ||
        g_Supervisor.curState != SUPERVISOR_STATE_GAMEMANAGER)
    {
        return 0;
    }
    for (int seat = 0; seat < PlayerCount(); seat++)
    {
        if (g_Players[seat].calcChain == NULL)
        {
            return 0;
        }
    }
    return 1;
}

int SessionGameplayExitPending()
{
    if (g_Supervisor.wantedState != SUPERVISOR_STATE_GAMEMANAGER)
    {
        return 1;
    }
    switch (g_Supervisor.curState)
    {
    case SUPERVISOR_STATE_GAMEMANAGER:
    case SUPERVISOR_STATE_NEXT_STAGE:
    case SUPERVISOR_STATE_NEXT_STAGE_USELESS:
    case SUPERVISOR_STATE_RESTART_FROM_BEGINNING:
    case SUPERVISOR_STATE_RESTART_STAGE:
        return 0;
    default:
        return 1;
    }
}

int SessionPredictionBlocked()
{
    const th07::mp::Config &cfg = th07::mp::Cfg();
    if (cfg.testPredictAlways)
    {
        return 0;
    }
    if (g_Supervisor.curState != g_Supervisor.wantedState ||
        g_Supervisor.wantedState != SUPERVISOR_STATE_GAMEMANAGER)
    {
        return 1; // the next frame loads and releases textures
    }
    if (g_GameManager.isInPauseMenu || g_GameManager.isInRetryMenu)
    {
        return 1;
    }
    if (cfg.talkConfirmed && g_Gui.HasCurrentMsgIdx())
    {
        return 1;
    }
    return 0;
}

namespace
{

const f32 kLineScale = 0.5f;

void DrawSessionLine(f32 y, const char *text, D3DCOLOR color)
{
    Float3 pos;
    pos.x = 630.0f - (f32)g_AsciiManager.fontSpacing * kLineScale * (f32)strlen(text);
    pos.y = y;
    pos.z = 0.0f;
    D3DCOLOR oldColor = g_AsciiManager.color;
    Float2 oldScale = g_AsciiManager.scale;
    g_AsciiManager.color = color;
    g_AsciiManager.scale.x = kLineScale;
    g_AsciiManager.scale.y = kLineScale;
    g_AsciiManager.AddString(&pos, text);
    g_AsciiManager.color = oldColor;
    g_AsciiManager.scale = oldScale;
}

D3DCOLOR RoundTripColor(unsigned tenths)
{
    return tenths >= 2000 ? 0xffff5050 : (tenths >= 1000 ? 0xffffff60 : 0xffffffff);
}

void WaitingText(char *text, size_t size, unsigned mask)
{
    int n = _snprintf_s(text, size, _TRUNCATE, "WAITING");
    for (int seat = 0; seat < MAX_PLAYERS && n > 0; seat++)
    {
        if (mask & (1u << seat))
        {
            n += _snprintf_s(text + n, size - n, _TRUNCATE, " %dP", seat + 1);
        }
    }
}

}

void MpDrawTitleSession()
{
    const th07::mp::Config &cfg = th07::mp::Cfg();
    char lines[8][48];
    D3DCOLOR colors[8];
    int count = 0;
    for (int seat = 0; seat < th07::mp::PlayerCount() && count < 8; seat++)
    {
        _snprintf_s(lines[count], sizeof(lines[count]), _TRUNCATE, "%dP %s", seat + 1, CoopPlayerName(seat));
        bool mine = cfg.mode != th07::mp::kUdp ? seat == 0 : seat == cfg.localSeat;
        colors[count++] = mine ? 0xffffffa0 : 0xffffffff;
    }
    th07::net::Status status = {};
    if (cfg.mode == th07::mp::kUdp)
    {
        th07::net::GetStatus(&status);
        if (cfg.rollback)
        {
            _snprintf_s(lines[count], sizeof(lines[count]), _TRUNCATE, "ROLLBACK ON  DELAY 0");
        }
        else
        {
            _snprintf_s(lines[count], sizeof(lines[count]), _TRUNCATE, "ROLLBACK OFF  DELAY %u", cfg.artificialDelay);
        }
        colors[count++] = 0xffffffff;
        if (status.waitingMask != 0)
        {
            WaitingText(lines[count], sizeof(lines[count]), status.waitingMask);
            colors[count++] = 0xffff5050;
        }
        if (status.hasRoundTrip)
        {
            unsigned tenths = (status.roundTripMicros + 50) / 100;
            if (tenths > 99999)
            {
                tenths = 99999;
            }
            _snprintf_s(lines[count], sizeof(lines[count]), _TRUNCATE, "%u.%ums", tenths / 10, tenths % 10);
            colors[count++] = RoundTripColor(tenths);
        }
    }
    else
    {
        _snprintf_s(lines[count], sizeof(lines[count]), _TRUNCATE, "LOCAL");
        colors[count++] = 0xffffffff;
    }
    f32 y = 470.0f - 9.0f * (f32)(count - 1);
    for (int i = 0; i < count; i++, y += 9.0f)
    {
        DrawSessionLine(y, lines[i], colors[i]);
    }
}

void MpDrawPlaySession()
{
    if (!th07::mp::UdpEnabled())
    {
        return;
    }
    th07::net::Status status = {};
    th07::net::GetStatus(&status);
    f32 y = 455.0f;
    if (status.hasRoundTrip)
    {
        unsigned tenths = (status.roundTripMicros + 50) / 100;
        if (tenths > 99999)
        {
            tenths = 99999;
        }
        char text[24];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%u.%ums", tenths / 10, tenths % 10);
        DrawSessionLine(y, text, RoundTripColor(tenths));
        y -= 9.0f;
    }
    if (status.waitingMask != 0)
    {
        char text[48];
        WaitingText(text, sizeof(text), status.waitingMask);
        DrawSessionLine(y, text, 0xffff5050);
    }
}

void MpDrawSelectLabels()
{
}

void MpLogBgmState()
{
}

#include "Coop.hpp"
#include "Player.hpp"

Player *CoopBorderOwner()
{
    for (int seat = 0; seat < PlayerCount(); ++seat) {
        Player& player = g_Players[seat];
        if (player.hasBorder == BORDER_ACTIVE && player.playerState == PLAYER_STATE_BORDER)
            return &player;
    }
    return NULL;
}

i32 CoopBorderActive()
{
    return CoopBorderOwner() != NULL;
}

void CoopRestoreBorder()
{
    if (g_GameManager.cherryPlus >= g_GameManager.globals->cherryStart + g_GameManager.BorderThreshold()) {
        g_GameManager.cherryPlus = g_GameManager.globals->cherryStart + g_GameManager.BorderThreshold();
        g_Players[0].ActivateBorder();
    }
}

void CoopEndBorderForDialogue()
{
    for (int seat = 0; seat < PlayerCount(); ++seat) {
        if (g_Players[seat].hasBorder != BORDER_NONE) {
            g_Players[seat].BreakBorderNaturally();
            return;
        }
    }
}

void Player::ActivateBorder()
{
    unsigned activated = 0;
    for (int seat = 0; seat < PlayerCount(); ++seat) {
        Player& player = g_Players[seat];
        if (!IsGhost(&player) && player.hasBorder != BORDER_ACTIVE) {
            player.ActivateBorderLocal();
            if (player.hasBorder == BORDER_ACTIVE) activated |= 1u << seat;
        }
    }
    if (activated)
        CoopLog("BORDER_START stage=%d frame=%d seats=%u threshold=%d", g_GameManager.currentStage,
                g_GameManager.framesThisStage, activated, g_GameManager.BorderThreshold());
}

void Player::ClearBorderLocal()
{
    hasBorder = BORDER_NONE;
    playerState = PLAYER_STATE_INVULNERABLE;
    invulnerabilityTimer = 40;
    borderInvulnerabilityTime = 40;
    if (borderEffect) {
        borderEffect->inUseFlag = 0;
        borderEffect = NULL;
    }
}

namespace {

void ClearPartners(Player *source)
{
    for (int seat = 0; seat < PlayerCount(); ++seat) {
        Player& player = g_Players[seat];
        if (&player != source &&
            (player.hasBorder == BORDER_ACTIVE || player.playerState == PLAYER_STATE_BORDER))
            player.ClearBorderLocal();
    }
}

}

void Player::BreakBorderNaturally()
{
    const bool shared = CoopBorderActive();
    BreakBorderNaturallyLocal();
    if (shared) ClearPartners(this);
    CoopLog("BORDER_END stage=%d frame=%d seat=%d natural=1 cherry=%d max=%d score=%u",
            g_GameManager.currentStage, g_GameManager.framesThisStage, seat,
            g_GameManager.cherry - g_GameManager.globals->cherryStart,
            g_GameManager.cherryMax - g_GameManager.globals->cherryStart,
            (unsigned)g_GameManager.globals->score);
}

void Player::BreakBorder(u32 unused)
{
    const bool shared = CoopBorderActive();
    BreakBorderLocal(unused);
    if (shared) ClearPartners(this);
    CoopLog("BORDER_END stage=%d frame=%d seat=%d natural=0", g_GameManager.currentStage,
            g_GameManager.framesThisStage, seat);
}

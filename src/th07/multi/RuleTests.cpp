#include "multi/RuleTests.h"
#include "multi/MpConfig.h"
#include "multi/RollbackGame.h"
#include "multi/RollbackHeap.h"
#include "multi/LocalKeyboard.h"
#include "Coop.hpp"
#include "Controller.hpp"
#include "EnemyManager.hpp"
#include "Gui.hpp"
#include "ItemManager.hpp"
#include "Player.hpp"
#include "SimHash.hpp"
#include "SoundPlayer.hpp"
#include "Supervisor.hpp"
#include <stdlib.h>
#include <string.h>
#include <dinput.h>

namespace {

void Place(int seat, float x, float y)
{
    Player& p = g_Players[seat];
    p.positionCenter = Float3(x, y, 0.0f);
    p.grabItemTopLeft = p.positionCenter - p.grabItemSize;
    p.grabItemBottomRight = p.positionCenter + p.grabItemSize;
}

void ResetFixture()
{
    memset(&g_ItemManager, 0, sizeof(g_ItemManager));
    g_Supervisor.effectiveFramerateMultiplier = 1.0f;
    for (int seat = 0; seat < PlayerCount(); ++seat) {
        Player& p = g_Players[seat];
        p.playerState = PLAYER_STATE_ALIVE;
        p.hasBorder = 0;
        p.lifeGiveTimer = p.lifeGiveTarget = p.powerGiveTaps = p.powerGiveWindow = 0;
        g_GameManager.Power(seat) = 0.0f;
        g_GameManager.Lives(seat) = 2.0f;
        Place(seat, 32.0f + seat * 96.0f, 360.0f);
    }
    g_GameManager.RegenerateGameIntegrityCsum();
}

void TickItems(int frames)
{
    for (int frame = 0; frame < frames; ++frame)
        g_ItemManager.OnUpdate();
}

}

bool RunCoopRuleTestsIfRequested()
{
    static bool done = false;
    if (done || !th07::mp::Cfg().testRules || g_GameManager.framesThisStage < 1 ||
        g_Supervisor.curState != 2 || g_Supervisor.wantedState != 2)
        return true;
    done = true;
    if (!th07::rollback_game::Enabled()) {
        th07::mp::Log("RULE_TEST FAIL requires UDP rollback");
        return false;
    }
    th07::rollback::heap::RuntimeScope runtime;
    auto* memory = th07::rollback_game::SharedArena();
    th07::rollback::Memory::Snapshot original;
    if (!memory->Capture(0, original))
        return false;
    const int sound = g_SoundSilenced;
    g_SoundSilenced = 1;
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks;
        failures += !ok;
        th07::mp::Log("RULE_TEST %s %s", ok ? "PASS" : "FAIL", name);
    };
    const int last = PlayerCount() - 1;
    Float3 drop(192.0f, 200.0f, 0.0f);

    check(!memory->Owns(g_AnmManager->spriteVertexBuffer), "render scratch lives outside rollback arena");
    check(g_AnmManager->spritesToDraw == 0, "snapshot boundary has no pending sprite batch");
    unsigned char* scratch = reinterpret_cast<unsigned char*>(g_AnmManager->spriteVertexBuffer);
    const unsigned lastByte = 49152 * sizeof(VertexTex1DiffuseXyzrhw) - 1;
    const unsigned char first = scratch[0], end = scratch[lastByte];
    scratch[0] ^= 0x5a;
    scratch[lastByte] ^= 0xa5;
    check(memory->Compare(original), "drawing scratch does not change saved simulation bytes");
    scratch[0] = first;
    scratch[lastByte] = end;

    const th07::input::KeyBinding fallback = {'I', DIK_I};
    auto q = th07::input::ParseKeyBinding("key_q", fallback);
    check(q.virtualKey == 'Q' && q.scanCode == DIK_Q, "custom letter binding");
    auto control = th07::input::ParseKeyBinding("rcontrol", fallback);
    check(control.virtualKey == VK_RCONTROL && control.scanCode == DIK_RCONTROL, "extended key binding");
    check(th07::input::ParseKeyBinding("invalid", fallback).scanCode == DIK_I, "invalid binding falls back");
    unsigned char keys[256] = {};
    keys[DIK_Q] = 0x80;
    check(th07::input::KeyBindingDown(q, keys, true), "DirectInput uses configured scan code");
    keys['Q'] = 0x80;
    check(th07::input::KeyBindingDown(q, keys, false), "keyboard fallback uses configured virtual key");
    check(!th07::input::KeyBindingDown(th07::input::ParseKeyBinding("none", fallback), keys, true),
          "disabled binding cannot press a button");

    ResetFixture();
    g_GameManager.Power(0) = 128;
    for (int i = 0; i < PlayerCount() * 2; ++i) {
        Item* item = g_ItemManager.SpawnItem(&drop, ITEM_POWER_SMALL, 0);
        check(item->itemType == (i % PlayerCount() == 0 ? ITEM_CHERRY : ITEM_POWER_SMALL),
              "power conversion follows drop slot");
    }
    g_ItemManager.DespawnAllItems(-1);
    check(g_ItemManager.items[1].itemType == ITEM_POWER_SMALL,
          "one MAX player does not convert another player's power");

    ResetFixture();
    CoopSpawnDrop(&drop, ITEM_LIFE, 0, 0);
    TickItems(40);
    float nearest = 1e9f;
    for (int a = 0; a < PlayerCount(); ++a)
        for (int b = a + 1; b < PlayerCount(); ++b) {
            const float dx = g_ItemManager.items[a].currentPosition.x - g_ItemManager.items[b].currentPosition.x;
            nearest = dx * dx < nearest ? dx * dx : nearest;
        }
    check(g_ItemManager.items[PlayerCount() - 1].isInUse && nearest >= 12.0f * 12.0f,
          "a drop's copies spread sideways");

    ResetFixture();
    const int seats = PlayerCount();
    g_GameManager.playerCount = 1;
    Item* power = g_ItemManager.SpawnItem(&drop, ITEM_POWER_BIG, 0);
    g_ItemManager.SpawnItem(&g_Players[0].positionCenter, ITEM_FULL_POWER, 0);
    TickItems(1);
    check(g_GameManager.Power(0) == 128 && power->itemType == ITEM_CHERRY,
          "single-player full-power item still converts existing drops");
    g_GameManager.playerCount = seats;

    ResetFixture();
    for (int seat = 0; seat < PlayerCount(); ++seat) {
        g_GameManager.Power(seat) = 128;
        Place(seat, 32.0f + 96.0f * seat, 80.0f);
    }
    for (int i = 0; i < PlayerCount() * 2; ++i) {
        Item* item = g_ItemManager.SpawnItem(&drop, ITEM_POINT, 0);
        check(ItemCollector(item)->seat == i % PlayerCount(), "simultaneous auto-collection is shared");
    }
    Place(last, 360.0f, 400.0f);
    check(ItemCollector(&g_ItemManager.items[last])->seat == last, "homing target remains stable");
    g_Players[last].playerState = PLAYER_STATE_GHOST;
    check(ItemCollector(&g_ItemManager.items[last])->seat != last, "lost target releases item");

    ResetFixture();
    Place(0, 180.0f, 350.0f);
    Place(last, 190.0f, 350.0f);
    g_GameManager.Power(0) = 40;
    g_SeatGameInput[0] = TH_BUTTON_SHOOT;
    g_SeatLastGameInput[0] = 0;
    for (int tap = 0; tap < 8; ++tap)
        CoopUpdateTransfers(&g_Players[0]);
    check(g_GameManager.Power(0) == 20 && g_GameManager.Power(last) == 0,
          "power debited on launch, credited on arrival");
    TickItems(20);
    check(g_GameManager.Power(last) == 0, "throw is not collected during launch");
    th07::rollback::Memory::Snapshot flying;
    check(memory->Capture(1, flying), "capture flying transfer");
    TickItems(60);
    check(g_GameManager.Power(last) == 20, "all twenty power reaches recipient");
    const u32 firstHash = SimFrameHash(NULL);
    check(memory->Restore(flying), "restore flying transfer");
    TickItems(60);
    check(SimFrameHash(NULL) == firstHash, "transfer replay has identical simulation state");

    ResetFixture();
    Place(0, 180.0f, 350.0f);
    Place(last, 190.0f, 350.0f);
    g_SeatGameInput[0] = TH_BUTTON_FOCUS;
    g_SeatLastGameInput[0] = 0;
    for (int frame = 0; frame < 90; ++frame)
        CoopUpdateTransfers(&g_Players[0]);
    check(g_GameManager.Lives(0) == 1 && g_GameManager.Lives(last) == 2,
          "life debited on launch, credited on arrival");
    TickItems(80);
    check(g_GameManager.Lives(last) == 3, "life item reaches recipient");

    ResetFixture();
    Place(0, 180.0f, 350.0f);
    Place(last, 190.0f, 350.0f);
    g_Players[last].playerState = PLAYER_STATE_GHOST;
    g_GameManager.Lives(last) = -1;
    for (int frame = 0; frame < 90; ++frame)
        CoopUpdateTransfers(&g_Players[0]);
    check(g_GameManager.Lives(last) == 0 && !IsGhost(&g_Players[last]), "life revives ghost directly");

    ResetFixture();
    Place(0, 180.0f, 350.0f);
    Place(last, 190.0f, 350.0f);
    for (int i = 0; i < 1100; ++i)
        g_ItemManager.items[i].isInUse = 1;
    g_GameManager.Power(0) = 40;
    g_SeatGameInput[0] = TH_BUTTON_SHOOT;
    for (int tap = 0; tap < 8; ++tap)
        CoopUpdateTransfers(&g_Players[0]);
    check(g_GameManager.Power(0) == 40 && g_GameManager.Power(last) == 0,
          "full pool cannot consume power gift");

    ResetFixture();
    for (int seat = 0; seat < PlayerCount(); ++seat) {
        g_Gui.ShowBombNamePortrait(1185, "TEST", seat);
        const int expected = seat ? ANM_OFFSET_COOP_FACE + (seat - 1) * 0x20 + 1 : 1185;
        check(g_Gui.impl->bombSpellcardPortrait.sprite == g_AnmManager->GetSprite(expected),
              "bomb portrait uses firing seat's face");
        check(g_Gui.impl->bombSpellcardPortrait.anmFileIdx == expected,
              "bomb portrait uses firing seat's animation");
        const int texture = g_Gui.impl->bombSpellcardPortrait.sprite->sourceFileIndex;
        check(texture >= 0 && texture < 264 && g_AnmManager->textures[texture] != NULL,
              "bomb portrait texture is loaded");
    }
    g_Gui.EndPlayerSpellcard(0);
    check(g_Gui.impl->bombSpellcardName.pendingInterrupt == 0,
          "another bomb ending does not dismiss current portrait");
    g_Gui.EndPlayerSpellcard(last);
    check(g_Gui.impl->bombSpellcardName.pendingInterrupt == 1,
          "own bomb ending dismisses current portrait");

    auto resetBorder = [&]() {
        ResetFixture();
        g_GameManager.cherry = g_GameManager.globals->cherryStart + 100000;
        g_GameManager.cherryMax = g_GameManager.globals->cherryStart + 300000;
        g_GameManager.cherryPlus = g_GameManager.globals->cherryStart;
        for (int seat = 0; seat < PlayerCount(); ++seat) {
            Player& p = g_Players[seat];
            if (p.borderEffect) p.borderEffect->inUseFlag = 0;
            p.borderEffect = NULL;
            p.bombInfo.isInUse = 0;
            p.borderInvulnerabilityTime = 0;
            p.invulnerabilityTimer = 0;
            p.respawnTimer = p.shooterData->initialRespawnTimer;
            memset(p.bombClearBoxes, 0, sizeof(p.bombClearBoxes));
        }
    };
    resetBorder();
    const int threshold = seats >= 3 ? 75000 : 50000;
    check(g_GameManager.BorderThreshold() == threshold, "shared border threshold follows player count");
    g_ItemManager.SpawnItem(&g_Players[last].positionCenter, ITEM_CHERRY_SMALL, 0);
    TickItems(1);
    check(g_GameManager.cherryPlus == g_GameManager.globals->cherryStart + 30 &&
          g_GameManager.cherry == g_GameManager.globals->cherryStart + 100100,
          "partner cherry pickup feeds the one shared gauge exactly once");
    g_GameManager.cherryPlus = g_GameManager.globals->cherryStart;
    g_GameManager.cherryPlus += threshold - 20;
    g_GameManager.AddCherryPlus(10);
    check(!CoopBorderActive() && g_GameManager.cherryPlus == g_GameManager.globals->cherryStart + threshold - 10,
          "shared border stays inactive below threshold");
    g_GameManager.AddCherryPlus(10);
    for (int seat = 0; seat < seats; ++seat) {
        Player& p = g_Players[seat];
        check(p.hasBorder == BORDER_ACTIVE && p.playerState == PLAYER_STATE_BORDER &&
              p.invulnerabilityTimer.GetCurrent() == 540, "shared threshold activates every live player for 540 frames");
        check(p.borderEffect && p.borderEffect->ownerSeat == seat, "border visual belongs to its player");
        for (int other = 0; other < seat; ++other)
            check(p.borderEffect != g_Players[other].borderEffect, "border visuals have independent fixed slots");
    }
    const int activeCherry = g_GameManager.cherry;
    g_GameManager.AddCherryPlus(100);
    check(g_GameManager.cherry == activeCherry + 100 &&
          g_GameManager.cherryPlus == g_GameManager.globals->cherryStart + threshold,
          "rewards during border increase shared cherry but not gauge");
    g_Players[last].invulnerabilityTimer = 100;
    g_Players[last].UpdateState();
    check(g_GameManager.cherryPlus == g_GameManager.globals->cherryStart + threshold,
          "partner countdown cannot overwrite shared gauge");
    g_Players[0].invulnerabilityTimer = 270;
    g_Players[0].UpdateState();
    check(g_GameManager.cherryPlus == g_GameManager.globals->cherryStart + threshold / 2,
          "first active player owns proportional shared countdown");

    g_Players[0].invulnerabilityTimer = 1;
    const unsigned scoreBeforeBorder = g_GameManager.globals->score;
    const int cherryBeforeBorder = g_GameManager.cherry;
    const int maxBeforeBorder = g_GameManager.cherryMax;
    th07::rollback::Memory::Snapshot endingBorder;
    check(memory->Capture(2, endingBorder), "capture shared border before natural expiry");
    g_Players[0].UpdateState();
    check(g_GameManager.cherryMax == maxBeforeBorder + 10000 &&
          g_GameManager.cherry == cherryBeforeBorder + 10000 &&
          g_GameManager.globals->score == scoreBeforeBorder +
              (cherryBeforeBorder + 10000 - g_GameManager.globals->cherryStart),
          "natural shared border awards cherry and score once");
    for (int seat = 0; seat < seats; ++seat)
        check(g_Players[seat].hasBorder == BORDER_NONE &&
              g_Players[seat].invulnerabilityTimer.GetCurrent() == 40 &&
              g_Players[seat].borderEffect == NULL, "natural expiry clears all active partners with 40-frame protection");
    const u32 borderHash = SimFrameHash(NULL);
    check(memory->Restore(endingBorder), "restore shared border before natural expiry");
    g_Players[0].UpdateState();
    check(SimFrameHash(NULL) == borderHash, "natural border replay has identical state and bonus");

    resetBorder();
    g_GameManager.AddCherryPlus(threshold);
    const unsigned manualScore = g_GameManager.globals->score;
    const int manualMax = g_GameManager.cherryMax;
    const int bombs = (int)g_GameManager.Bombs(last);
    g_SeatGameInput[last] = TH_BUTTON_BOMB;
    g_Players[last].UpdateBorderAndBombState();
    g_SeatGameInput[last] = 0;
    check(!CoopBorderActive() && g_GameManager.cherryPlus == g_GameManager.globals->cherryStart,
          "partner bomb button breaks shared border and resets gauge");
    check((int)g_GameManager.Bombs(last) == bombs && g_GameManager.globals->score == manualScore &&
          g_GameManager.cherryMax == manualMax, "manual border break consumes no bomb and grants no natural bonus");
    check(g_EnemyManager.spellcardInfo.captureScore == 0 && !g_EnemyManager.spellcardInfo.isCapturing,
          "manual border break cancels spell capture");
    for (int seat = 0; seat < seats; ++seat)
        check(g_Players[seat].hasBorder == BORDER_NONE &&
              (g_Players[seat].bombClearBoxes[0].lifetime != 0) == (seat == last),
              "only the breaking player emits the clearing burst");

    resetBorder();
    g_GameManager.AddCherryPlus(threshold);
    g_Players[last].Die();
    g_Players[last].UpdateDeath();
    check(!CoopBorderActive() && g_Players[last].playerState == PLAYER_STATE_INVULNERABLE &&
          g_GameManager.Lives(last) == 2, "hit during shared border protects the victim and ends all borders");

    resetBorder();
    g_Players[last].playerState = PLAYER_STATE_GHOST;
    g_Players[0].playerState = PLAYER_STATE_INVULNERABLE;
    g_GameManager.AddCherryPlus(threshold);
    check(g_Players[last].hasBorder == BORDER_NONE && g_Players[last].playerState == PLAYER_STATE_GHOST,
          "shared activation does not resurrect ghosts");
    check(g_Players[0].hasBorder == BORDER_READY, "invulnerable player queues shared border");
    g_Players[0].playerState = PLAYER_STATE_ALIVE;
    g_Players[0].ActivateBorder();
    check(g_Players[0].hasBorder == BORDER_ACTIVE && g_Players[last].hasBorder == BORDER_NONE,
          "queued border activates after protection without involving ghost");
    g_Players[0].ClearBorderLocal();
    g_GameManager.cherryPlus = g_GameManager.globals->cherryStart + threshold;
    g_Players[0].playerState = PLAYER_STATE_SPAWNING;
    CoopRestoreBorder();
    check(g_Players[0].hasBorder == BORDER_READY && g_Players[last].hasBorder == BORDER_NONE,
          "full shared gauge survives stage reentry while ghosts remain ghosts");

    resetBorder();
    g_Players[last].bombInfo.isInUse = 1;
    g_GameManager.AddCherryPlus(threshold);
    check(g_Players[last].hasBorder == BORDER_READY && g_Players[0].hasBorder == BORDER_ACTIVE,
          "bombing player queues border while ready partners activate");
    g_Players[last].bombInfo.isInUse = 0;
    g_Players[last].ActivateBorder();
    check(g_Players[last].hasBorder == BORDER_ACTIVE, "queued bombing player joins shared border");
    const int grazeMax = g_GameManager.cherryMax;
    g_Players[last].isFocus = 1;
    g_Players[last].ScoreGraze(&drop);
    g_Players[last].isFocus = 0;
    g_Players[last].ScoreGraze(&drop);
    check(g_GameManager.cherryMax == grazeMax + 30 + 80, "partner border graze keeps reference 30/80 cherry growth");

    resetBorder();
    g_GameManager.AddCherryPlus(threshold);
    const int dialogueMax = g_GameManager.cherryMax;
    CoopEndBorderForDialogue();
    CoopEndBorderForDialogue();
    check(!CoopBorderActive() && g_GameManager.cherryMax == dialogueMax + 10000,
          "dialogue finishes shared border once even when called again");

    resetBorder();
    g_Players[0].playerState = PLAYER_STATE_GHOST;
    g_GameManager.AddCherryPlus(threshold);
    check(CoopBorderActive() && CoopBorderOwner() == &g_Players[1] && g_Players[0].hasBorder == BORDER_NONE,
          "shared gauge and HUD remain active when first player is a ghost");

    resetBorder();
    g_GameManager.playerCount = 1;
    check(g_GameManager.BorderThreshold() == 50000, "single player retains 50000 threshold");
    g_GameManager.AddCherryPlus(50000);
    check(g_Players[0].hasBorder == BORDER_ACTIVE, "single player border still activates");
    g_Players[0].BreakBorderNaturally();
    check(!CoopBorderActive() && g_GameManager.cherryMax == g_GameManager.globals->cherryStart + 310000,
          "single player natural bonus remains unchanged");
    g_GameManager.playerCount = seats;

    check(memory->Restore(original), "restore original game state");
    g_SoundSilenced = sound;
    th07::mp::Log("RULE_TEST_RESULT checks=%d failures=%d players=%d", checks, failures, PlayerCount());
    return failures == 0;
}

#include "multi/RuleTests.h"
#include "multi/MpConfig.h"
#include "multi/RollbackGame.h"
#include "multi/RollbackHeap.h"
#include "multi/LocalKeyboard.h"
#include "Coop.hpp"
#include "Controller.hpp"
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

    check(memory->Restore(original), "restore original game state");
    g_SoundSilenced = sound;
    th07::mp::Log("RULE_TEST_RESULT checks=%d failures=%d players=%d", checks, failures, PlayerCount());
    return failures == 0;
}

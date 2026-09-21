#pragma once

#include "AnmManager.hpp"
#include "ZunResult.hpp"

#define TRANSITION_QUAD_ROWS 14
#define TRANSITION_QUAD_COLS 12

typedef enum GuiDisplayArg
{
    GUI_DISPLAY_HIDDEN = 0,
    GUI_DISPLAY_SHOWN = 1,
    GUI_DISPLAY_FULL_POWER = 1,
    GUI_DISPLAY_BORDER = 2,
    GUI_DISPLAY_CHERRY_MAX = 3,
    GUI_DISPLAY_BORDER_BONUS = 4,
} GuiDisplayArg;

// values from https://pytouhou.linkmauve.fr/doc/06/msg.xml
typedef enum MsgOpcode
{
    MSG_DELETE = 0,
    MSG_SHOW_PORTRAIT = 1,
    MSG_CHANGE_FACE = 2,
    MSG_DIALOGUE = 3,
    MSG_PAUSE = 4,
    MSG_SWITCH = 5,
    MSG_APPEAR_ENEMY = 6,
    MSG_MUSIC = 7,
    MSG_TEXT_INTRODUCE = 8,
    MSG_STAGERESULTS = 9,
    MSG_FREEZE = 10,
    MSG_NEXT_LEVEL = 11,
    MSG_FADEOUT_MUSIC = 12,
    MSG_ALLOW_SKIP = 13,
    MSG_FADE_IN_EFFECT = 14
} MsgOpcode;

struct MsgRawInstrArgPortrait
{
    i16 portraitIdx;
    i16 anmScriptIdx;
};

struct MsgRawInstrArgDialogue
{
    i16 textColor;
    i16 textLine;
    char text[5];
};

struct MsgRawInstrArgPause
{
    i32 duration;
};

struct MsgRawInstrArgSwitch
{
    i16 unkIdx;
    u8 interrupt;
};

struct MsgRawInstrArgMusic
{
    i32 musicIdx;
};

union MsgRawInstrArgs {
    MsgRawInstrArgPortrait portrait;
    MsgRawInstrArgDialogue dialogue;
    MsgRawInstrArgPause pause;
    MsgRawInstrArgSwitch msgSwitch;
    MsgRawInstrArgMusic music;
};

struct MsgRawInstr
{
    u16 time;
    u8 opcode;
    u8 argsize;
    MsgRawInstrArgs args;
};

struct MsgRawHeader
{
    i32 numInstrs;
    MsgRawInstr *instrs[1];
};

struct GuiFormattedText
{
    Float3 pos;
    i32 fmtArg;
    i32 displayArg;
    ZunTimer timer;
};

struct GuiMsgVm
{
    GuiMsgVm();

    MsgRawHeader *msgFile;
    MsgRawInstr *curInstr;
    i32 currentMsgIdx;
    ZunTimer timer;
    i32 framesElapsedDuringPause;
    AnmVm portraits[2];
    AnmVm dialogueLines[2];
    AnmVm introLines[2];
    D3DCOLOR textColorsA[4];
    D3DCOLOR textColorsB[4];
    u32 fontSize;
    u32 ignoreWaitCounter;
    u8 dialogueSkippable;
};

struct GuiImpl
{
    GuiImpl();

    ZunResult DrawDialogue();
    void MsgRead(i32 msgIdx);
    ZunResult RunMsg();

    AnmVm vms0[33];
    u8 bossHealthBarState;
    // pad 3
    AnmVm vms1[5];
    AnmVm bombSpellcardPortrait;
    AnmVm enemySpellcardPortrait;
    AnmVm bombSpellcardDecorLeft;
    AnmVm enemySpellcardRelated1;
    AnmVm bombSpellcardDecorRight;
    AnmVm enemySpellcardRelated2;
    AnmVm bombSpellcardName;
    AnmVm enemySpellcardName;
    AnmVm bombSpellcardNameBg;
    AnmVm enemySpellcardNameBg;
    AnmVm stageClearBg;
    AnmVm loadingSprite;
    AnmVm stageTransitionSnapshotVm;
    AnmVm captureBonusVm;
    AnmVm spellcardBonusIndicator;
    AnmVm transitionQuads[TRANSITION_QUAD_ROWS * TRANSITION_QUAD_COLS];
    i32 activeTransitionQuads;
    GuiMsgVm msg;
    // pad 3
    i32 finishedStage;
    i32 stageClearBonus;
    i32 transitionToScoreScreen;
    GuiFormattedText bonusScore;
    GuiFormattedText statusPopup;
    GuiFormattedText spellCardBonus;
    i32 clearPower;
    i32 clearPointItems;
    i32 clearCherryMax;
    i32 clearGraze;
    i32 bombPortraitSeat;
};
C_ASSERT(sizeof(GuiImpl) == 0x20a34);

struct Gui
{
    static ZunResult RegisterChain();
    static void CutChain();

    static ZunResult AddedCallback(Gui *arg);
    static ZunResult DeletedCallback(Gui *arg);
    static u32 OnUpdate(Gui *arg);
    static u32 OnDraw(Gui *arg);

    ZunResult ActualAddedCallback();
    void ClearActiveSprites();
    static void CopyEnemyNameTexture(i32 spriteIdx);
    void DrawGameScene();
    void DrawStageElements();
    void FreeMsgFile();
    i32 HasCurrentMsgIdx();
    i32 IsDialogueSkippable();
    i32 IsStageFinished();
    ZunResult LoadMsg(const char *filename);
    void MsgRead(i32 msgIdx);
    i32 MsgWait();

    void EndEnemySpellcard();
    void EndPlayerSpellcard(i32 seat);
    void ShowBombNamePortrait(i32 sprite, const char *name, i32 seat);
    void ShowBonusScore(i32 score);
    void ShowStatusPopup(i32 fmtArg, i32 popupType);
    void ShowSpellcard(i32 spellcardSprite, const char *spellcardName);
    void ShowSpellcardBonus(i32 fmtArg);
    void UpdateGui();

    void SetSpellcardSecondsRemaining(i32 seconds)
    {
        this->spellcardSecondsRemaining = seconds;
    }

    void SetBossHealth(i32 idx, f32 eased, f32 health)
    {
        this->bossHealthEased[idx] = eased;
        this->bossHealth[idx] = health;
    }

    void SetBossHealthBar(f32 amount)
    {
        this->bossHealthBar = amount;
    }

    bool BossPresent()
    {
        return this->bossPresent;
    }

    i32 frameCounter;
    union {
        u32 flags;
        struct
        {
            u32 lifeDisplayUpdateFrames : 2;
            u32 bombDisplayUpdateFrames : 2;
            u32 powerDisplayUpdateFrames : 2;
            u32 grazeDisplayUpdateFrames : 2;
            u32 pointDisplayUpdateFrames : 2;
        };
    };
    GuiImpl *impl;
    f32 bombNameBarLength;
    f32 spellcardBarLength;
    u32 bossHealthBarAlpha;
    i32 bossLifeMarkers;
    i32 spellcardSecondsRemaining;
    i32 lastSpellcardSecondsRemaining;
    bool bossPresent;
    // pad 3
    f32 bossHealthBar;
    f32 bossHealthBarEased;
    i32 unused_30;
    f32 bossHealth[8];
    f32 bossHealthEased[8];
    u32 bossColor[8];
};
C_ASSERT(sizeof(Gui) == 0x94);
extern Gui g_Gui;

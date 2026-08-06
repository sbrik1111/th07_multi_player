#pragma once

#define ANM_FILE_TEXT 0
#define ANM_FILE_ASCII 1
#define ANM_FILE_CAPTURE 4
#define ANM_FILE_STAGE_BG1 5
#define ANM_FILE_STAGE_BG2 6
#define ANM_FILE_STAGE_BG3 7
#define ANM_FILE_STAGE_BG4 8
#define ANM_FILE_STAGE_BG5 9
#define ANM_FILE_PLAYER 10
#define ANM_FILE_BULLETS 11
#define ANM_FILE_ENEMY 15
#define ANM_FILE_ENEMY2 16
#define ANM_FILE_EFFECTS 17
#define ANM_FILE_EFFECTS2 18
#define ANM_FILE_EFFECTS3 19
#define ANM_FILE_FRONT 21
#define ANM_FILE_LOADING 23
#define ANM_FILE_STAGE_TEXT 24
#define ANM_FILE_FACE 25
#define ANM_FILE_FACE2 40 // Keep P2 face blocks away from P1 face's child block.
// Past every existing id: LoadAnms walks a file's child chain into the
// following slots, so 41 sat inside FACE2's chain and overwrote it.
#define ANM_FILE_FACE3 50
#define ANM_FILE_FACE_STAGE 28
#define ANM_FILE_TITLE 32
#define ANM_FILE_RESULT 42
#define ANM_FILE_MUSIC 46
#define ANM_FILE_PLAYER2 47
#define ANM_FILE_PLAYER3 48
#define ANM_FILE_STAFF 49

#define ANM_OFFSET_ASCII 0x000
#define ANM_OFFSET_CHERRY_DIGIT 0x003
#define ANM_OFFSET_CHERRY_GAUGE 0x004
#define ANM_OFFSET_CHERRY_BORDER 0x005
#define ANM_OFFSET_BOSS_MARKER 0x006
#define ANM_OFFSET_RETRY_MENU 0x0fe
#define ANM_OFFSET_PAUSE_MENU 0x108
#define ANM_OFFSET_BULLETS 0x200
#define ANM_OFFSET_EFFECTS 0x2dc
#define ANM_OFFSET_EFFECTS2 0x2dd
#define ANM_OFFSET_EFFECTS3 0x2de
#define ANM_OFFSET_STAGE_BG1 0x300
#define ANM_OFFSET_STAGE_BG2 0x310
#define ANM_OFFSET_STAGE_BG3 0x320
#define ANM_OFFSET_STAGE_BG4 0x330
#define ANM_OFFSET_STAGE_BG5 0x340
#define ANM_OFFSET_PLAYER 0x400
#define ANM_OFFSET_PLAYER2 0x500
#define ANM_OFFSET_PLAYER3 0xa00
#define ANM_OFFSET_FACE 0x4a0
#define ANM_OFFSET_FACE2 0x5a0
// Each player's face block sits 0xa0 past its player block, so the bomb cut-in
// script index derived from the player offset lands inside it. P3 was missing
// this: GetPlayerAnmScript(player, 1185) produced 0xaa1 for P3, an index that
// nothing ever loaded, and handing that to SetActiveSprite left the VM with a
// negative sourceFileIndex that the texture lookup then dereferenced.
#define ANM_OFFSET_FACE3 0xaa0
#define ANM_OFFSET_FACE_STAGE 0x4ad
#define ANM_OFFSET_FRONT 0x600
#define ANM_OFFSET_STAFF 0x600
#define ANM_OFFSET_LOADING 0x61e
#define ANM_OFFSET_TEXT 0x700
#define ANM_OFFSET_CAPTURE 0x724
#define ANM_OFFSET_MENU_BG 0x724
#define ANM_OFFSET_STAGE_TEXT 0x800
#define ANM_OFFSET_TITLE 0x900
#define ANM_OFFSET_MUSIC 0x900
#define ANM_OFFSET_RESULT 0x900
#define ANM_OFFSET_ENEMY 0x900

#define ANM_SCRIPT_REIMU_A_BOMB_ARRAY 0x485
#define ANM_SCRIPT_REIMU_B_BOMB_ARRAY 0x489
#define ANM_SCRIPT_REIMU_B_FOCUS_BOMB_ARRAY 0x48d
#define ANM_SCRIPT_MARISA_A_BOMB_ARRAY 0x405
#define ANM_SCRIPT_MARISA_B_BOMB_ARRAY 0x40c
#define ANM_SCRIPT_MARISA_B_FOCUS_BOMB_ARRAY 0x408
#define ANM_SCRIPT_SAKUYA_A_BOMB_ARRAY 0x405
#define ANM_SCRIPT_SAKUYA_A_FOCUS_BOMB_ARRAY 0x407
#define ANM_SCRIPT_SAKUYA_B_BOMB_ARRAY 0x409
#define ANM_SCRIPT_SAKUYA_B_FOCUS_BOMB_ARRAY 0x40d

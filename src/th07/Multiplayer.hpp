#pragma once

// The original TH07 game state only contains one player.  Multiplayer keeps
// additional players in sidecar state, but all new multiplayer code uses one
// fixed slot count so P1/P2/P3 cannot silently diverge between subsystems.
const int TH07_MULTI_MAX_PLAYERS = 3;
const int TH07_MULTI_MAX_GUESTS = TH07_MULTI_MAX_PLAYERS - 1;

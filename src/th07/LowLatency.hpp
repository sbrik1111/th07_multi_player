#pragma once

namespace LowLatency
{
// Enables the safe-input wrapper on every run and initializes the
// update-before-draw frame loop for multiplayer low-latency mode.
void Install();
void LogSummary();
void Shutdown();
} // namespace LowLatency

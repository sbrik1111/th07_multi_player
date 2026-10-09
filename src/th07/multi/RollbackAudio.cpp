#include "RollbackAudio.h"
#include "RollbackMemory.h"
#include <cstdlib>

namespace th07 { namespace rollback { namespace audio {
namespace {
struct State {
    Wave waves[kWaves];
    Voice voices[kVoices];
} state;
}
void Voice::Start(Wave source, unsigned playFlags) {
    cursor = 0;
    wave = source;
    flags = playFlags;
    playing = wave.bytes && wave.bytesPerSecond;
}
void Voice::Tick() {
    if (!playing) return;
    cursor += wave.bytesPerSecond;
    const std::uint64_t end = std::uint64_t(wave.bytes) * 60;
    if (cursor < end) return;
    if (flags & 1) cursor %= end; // DSBPLAY_LOOPING
    else { cursor = end; playing = false; }
}
bool Voice::Stop() {
    const bool result = playing;
    playing = false;
    return result;
}
bool RegisterRoots(Memory& memory) {
    return memory.AddRegion(kRootKey, &state, sizeof(state));
}
void RegisterWave(unsigned id, unsigned bytes, unsigned bytesPerSecond) {
    if (id >= kWaves || !bytes || !bytesPerSecond) std::abort();
    state.waves[id].bytes = bytes;
    state.waves[id].bytesPerSecond = bytesPerSecond;
}
void StartVoice(unsigned id, unsigned wave, unsigned flags) {
    if (id >= kVoices || wave >= kWaves || !state.waves[wave].bytes) std::abort();
    state.voices[id].Start(state.waves[wave], flags);
}
bool StopVoice(unsigned id) {
    if (id >= kVoices) std::abort();
    return state.voices[id].Stop();
}
void Tick() { for (unsigned i = 0; i != kVoices; ++i) state.voices[i].Tick(); }

} } }

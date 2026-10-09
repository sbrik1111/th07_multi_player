#pragma once
#include <cstdint>

namespace th07 { namespace rollback {
class Memory;
namespace audio {

const unsigned kWaves = 0x43;
const unsigned kVoices = 78;
const std::uint32_t kRootKey = 0xF0000050u;

// Cursor units are bytes * 60: no rounding error per frame.
struct Wave {
    unsigned bytes, bytesPerSecond;
};
struct Voice {
    std::uint64_t cursor;
    Wave wave;
    unsigned flags;
    bool playing;
    void Start(Wave source, unsigned playFlags);
    void Tick();
    bool Stop();
};
bool RegisterRoots(Memory&);
void RegisterWave(unsigned id, unsigned bytes, unsigned bytesPerSecond);
void StartVoice(unsigned id, unsigned wave, unsigned flags);
bool StopVoice(unsigned id);
void Tick();

} } }

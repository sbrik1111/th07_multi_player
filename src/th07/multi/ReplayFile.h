#pragma once
#include <stdint.h>
#include <stdio.h>

namespace th07 { namespace replay {

const unsigned kReplayVersion = 1;
const unsigned kHashParts = 13;
const unsigned kHashInterval = 16;
const unsigned kHasHash = 1;
const unsigned kEnd = 2;

#pragma pack(push, 1)
struct Settings {
    uint32_t players, viewSeat, session;
    int32_t characters[4], shots[4];
    uint32_t startStage, clearFrame, clearLast, ghostFrame, keepAlive, arena;
    char names[4][16];
    uint8_t gameConfig[56];
    uint8_t executable[32], gameData[32];
};
struct Header {
    char magic[8];
    uint32_t version, bytes;
    Settings settings;
    uint32_t difficulty, shot;
    char date[6];
    uint16_t reserved;
    uint32_t checksum;
};
struct Frame {
    uint32_t index, segment, frame, flags;
    uint16_t held[4];
    uint32_t hash, parts[kHashParts];
    uint32_t checksum;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 288, "replay header layout");
static_assert(sizeof(Frame) == 84, "replay frame layout");

uint32_t Checksum(const void* data, size_t bytes, uint32_t seed = 2166136261u);
Header MakeHeader(const Settings& settings);
bool ValidHeader(const Header& header);

class Stream {
protected:
    FILE* file_ = nullptr;
    const char* error_ = nullptr;
    uint32_t count_ = 0, segment_ = 0, next_ = 0, rolling_ = 2166136261u;
    bool Fail(const char* error);
    bool Accept(const Frame& frame);
public:
    ~Stream();
    const char* Error() const { return error_; }
    uint32_t Count() const { return count_; }
    void Close();
};

class Writer : public Stream {
public:
    // Never overwrites an existing replay.
    bool Open(const wchar_t* path, const Header& header);
    bool Append(Frame frame);
    bool UpdateHeader(Header header);
    bool Finish();
};

class Reader : public Stream {
    Header header_ = {};
    bool complete_ = false, ended_ = false;
public:
    bool Open(const wchar_t* path);
    const Header& Info() const { return header_; }
    // 1 frame, 0 end, -1 invalid data.
    int Next(Frame& frame);
    bool Complete() const { return complete_; }
};

} }

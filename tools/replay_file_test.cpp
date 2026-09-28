#include "multi/ReplayFile.h"
#include "multi/RollbackNetcode.h"
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

using namespace th07;
static int checks;
static void Check(bool ok, const char* name)
{
    ++checks;
    if (!ok) { fprintf(stderr, "FAIL %s\n", name); exit(1); }
}
static replay::Header Header()
{
    replay::Settings s = {};
    s.players = 4;
    s.startStage = s.clearLast = 1;
    s.clearFrame = s.ghostFrame = UINT32_MAX;
    return replay::MakeHeader(s);
}
static replay::Frame Frame(unsigned segment, unsigned frame)
{
    replay::Frame f = {};
    f.segment = segment;
    f.frame = frame;
    if (frame % replay::kHashInterval == 0) f.flags = replay::kHasHash;
    for (int seat = 0; seat < 4; ++seat) f.held[seat] = (unsigned short)(frame + seat);
    return f;
}
static std::vector<unsigned char> Read(const wchar_t* path)
{
    FILE* f = _wfopen(path, L"rb");
    Check(f != nullptr, "read fixture");
    fseek(f, 0, SEEK_END);
    std::vector<unsigned char> bytes(ftell(f));
    rewind(f);
    Check(fread(bytes.data(), 1, bytes.size(), f) == bytes.size(), "read bytes");
    fclose(f);
    return bytes;
}
static void Write(const wchar_t* path, const std::vector<unsigned char>& bytes)
{
    FILE* f = _wfopen(path, L"wb");
    Check(f && fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size(), "write fixture");
    fclose(f);
}

int main()
{
    DeleteFileW(L"record.mpr");
    replay::Header header = Header();
    Check(replay::ValidHeader(header), "valid header");
    {
        replay::Writer writer;
        Check(writer.Open(L"record.mpr", header), "create recording");
        for (unsigned segment = 0; segment < 5; ++segment)
            for (unsigned frame = 0; frame < 41; ++frame)
                Check(writer.Append(Frame(segment, frame)), "append ordered inputs");
        header.difficulty = 4;
        Check(writer.UpdateHeader(header), "update summary");
        Check(writer.Finish(), "finish recording");
        Check(writer.Count() == 205, "written count");
    }
    {
        replay::Writer writer;
        Check(!writer.Open(L"record.mpr", header), "never overwrite");
        replay::Reader reader;
        Check(reader.Open(L"record.mpr"), "open recording");
        Check(reader.Info().difficulty == 4, "summary retained");
        replay::Frame f;
        unsigned count = 0;
        while (reader.Next(f) == 1) {
            Check(f.index == count && f.segment == count / 41 && f.frame == count % 41, "replay frame identity");
            for (unsigned seat = 0; seat < 4; ++seat) Check(f.held[seat] == f.frame + seat, "all seats retained");
            ++count;
        }
        Check(count == 205 && reader.Complete() && !reader.Error(), "complete replay");
    }
    auto bytes = Read(L"record.mpr");
    {
        auto bad = bytes;
        bad[sizeof(replay::Header) + offsetof(replay::Frame, held)] ^= 1;
        Write(L"corrupt.mpr", bad);
        replay::Reader reader;
        replay::Frame f;
        Check(reader.Open(L"corrupt.mpr") && reader.Next(f) == -1, "corrupt input rejected");
    }
    {
        auto bad = bytes;
        bad[8] ^= 1;
        Write(L"version.mpr", bad);
        replay::Reader reader;
        Check(!reader.Open(L"version.mpr"), "unsupported header rejected");
    }
    {
        auto bad = bytes;
        auto* f = reinterpret_cast<replay::Frame*>(bad.data() + sizeof(replay::Header));
        f->frame = 17;
        f->checksum = replay::Checksum(f, offsetof(replay::Frame, checksum));
        Write(L"gap.mpr", bad);
        replay::Reader reader;
        replay::Frame out;
        Check(reader.Open(L"gap.mpr") && reader.Next(out) == -1, "frame gaps rejected");
    }
    {
        auto shortFile = bytes;
        shortFile.resize(sizeof(replay::Header) + sizeof(replay::Frame) * 20 + 11);
        Write(L"interrupted.mpr", shortFile);
        replay::Reader reader;
        replay::Frame f;
        Check(reader.Open(L"interrupted.mpr"), "interrupted header");
        unsigned count = 0;
        while (reader.Next(f) == 1) ++count;
        Check(count == 20 && !reader.Complete() && !reader.Error(), "recover only complete frames");
    }
    {
        auto trailing = bytes;
        trailing.push_back(0);
        Write(L"trailing.mpr", trailing);
        replay::Reader reader;
        replay::Frame f;
        Check(reader.Open(L"trailing.mpr"), "trailing header");
        int status;
        while ((status = reader.Next(f)) == 1) {}
        Check(status == -1, "trailing garbage rejected");
    }
    {
        netcode::Timeline t;
        netcode::Inputs inputs;
        Check(t.Initialize(0, 8, 4), "timeline init");
        for (unsigned frame = 0; frame < 6; ++frame) {
            Check(t.SampleLocal((unsigned short)(frame + 1)), "local sample");
            Check(t.Advance([](unsigned, const netcode::Inputs&, bool) { return 1; }), "predict");
            Check(!t.ReadConfirmed(frame, inputs), "prediction never exported");
        }
        for (unsigned frame = 0; frame < 6; ++frame)
            for (unsigned seat = 1; seat < 4; ++seat)
                Check(t.Receive(seat, frame, (unsigned short)(frame + seat + 1)), "late input");
        Check(t.Dirty() == 0 && !t.ReadConfirmed(0, inputs), "dirty frames never exported");
        Check(t.Repair([](unsigned frame) { return frame; }, [](unsigned, const netcode::Inputs&, bool) { return 1; }), "repair");
        for (unsigned frame = 0; frame < 6; ++frame) {
            Check(t.ReadConfirmed(frame, inputs), "corrected frame exported");
            for (unsigned seat = 0; seat < 4; ++seat)
                Check(inputs.held[seat] == frame + seat + 1, "corrected seat input");
        }
        Check(!t.ReadConfirmed(6, inputs), "unsimulated frame excluded");
    }
    printf("PASS replay format and confirmed inputs: %d checks\n", checks);
    return 0;
}

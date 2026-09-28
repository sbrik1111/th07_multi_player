#include "ReplayFile.h"
#include <stddef.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

namespace th07 { namespace replay {

uint32_t Checksum(const void* data, size_t bytes, uint32_t seed)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) seed = (seed ^ p[i]) * 16777619u;
    return seed;
}

Header MakeHeader(const Settings& settings)
{
    Header h = {};
    memcpy(h.magic, "T07MPR1", 8);
    h.version = kReplayVersion;
    h.bytes = sizeof(h);
    h.settings = settings;
    h.difficulty = UINT32_MAX;
    h.checksum = Checksum(&h, offsetof(Header, checksum));
    return h;
}

bool ValidHeader(const Header& h)
{
    if (memcmp(h.magic, "T07MPR1", 8) || h.version != kReplayVersion || h.bytes != sizeof(h) ||
        h.checksum != Checksum(&h, offsetof(Header, checksum))) return false;
    const Settings& s = h.settings;
    if (s.players < 1 || s.players > 4 || s.viewSeat >= s.players || s.startStage < 1 || s.startStage > 6 ||
        s.clearLast > 8 || s.keepAlive > 1 || s.arena > 1 || h.shot >= 6 ||
        (h.difficulty != UINT32_MAX && h.difficulty >= 6) || h.reserved || h.date[5]) return false;
    for (int seat = 0; seat < 4; ++seat) {
        if (s.characters[seat] < -1 || s.characters[seat] > 2 || s.shots[seat] < -1 || s.shots[seat] > 1 ||
            !memchr(s.names[seat], 0, sizeof(s.names[seat]))) return false;
    }
    return true;
}

bool Stream::Fail(const char* error)
{
    if (!error_) error_ = error;
    return false;
}

Stream::~Stream() { Close(); }
void Stream::Close()
{
    if (file_ && fclose(file_) != 0) Fail("replay close failed");
    file_ = nullptr;
}

bool Stream::Accept(const Frame& f)
{
    if (f.index != count_ || count_ == UINT32_MAX || f.flags & ~kHasHash)
        return Fail("invalid replay frame index/flags");
    if (f.segment == segment_) {
        if (f.frame != next_) return Fail("replay frame gap");
    } else if (count_ && f.segment == segment_ + 1 && !f.frame) {
        segment_ = f.segment;
        next_ = 0;
    } else return Fail("replay segment gap");
    if (f.frame == UINT32_MAX || ((f.flags & kHasHash) != 0) != (f.frame % kHashInterval == 0))
        return Fail("invalid replay hash interval");
    for (unsigned seat = 0; seat < 4; ++seat)
        if (f.held[seat] & 0x8000) return Fail("invalid replay input");
    ++count_;
    ++next_;
    rolling_ = Checksum(&f.checksum, sizeof(f.checksum), rolling_);
    return true;
}

bool Writer::Open(const wchar_t* path, const Header& header)
{
    if (file_ || error_ || !ValidHeader(header)) return Fail("invalid replay header");
    int fd = -1;
    if (_wsopen_s(&fd, path, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _SH_DENYWR, _S_IREAD | _S_IWRITE))
        return Fail("cannot create replay file");
    file_ = _wfdopen(fd, L"wb");
    if (!file_) { _close(fd); return Fail("cannot open replay stream"); }
    // Each append reaches the OS even if the process is killed.
    setvbuf(file_, nullptr, _IONBF, 0);
    return fwrite(&header, sizeof(header), 1, file_) == 1 || Fail("cannot write replay header");
}

bool Writer::Append(Frame f)
{
    if (!file_ || error_) return false;
    f.index = count_;
    f.checksum = Checksum(&f, offsetof(Frame, checksum));
    if (!Accept(f)) return false;
    return fwrite(&f, sizeof(f), 1, file_) == 1 || Fail("cannot write replay input");
}

bool Writer::UpdateHeader(Header header)
{
    if (!file_ || error_) return false;
    header.checksum = Checksum(&header, offsetof(Header, checksum));
    if (!ValidHeader(header) || _fseeki64(file_, 0, SEEK_SET) ||
        fwrite(&header, sizeof(header), 1, file_) != 1 || _fseeki64(file_, 0, SEEK_END))
        return Fail("cannot update replay summary");
    return true;
}

bool Writer::Finish()
{
    if (!file_) return !error_;
    if (!error_) {
        Frame end = {};
        end.index = count_;
        end.segment = segment_;
        end.frame = next_;
        end.flags = kEnd;
        end.hash = rolling_;
        end.checksum = Checksum(&end, offsetof(Frame, checksum));
        if (fwrite(&end, sizeof(end), 1, file_) != 1) Fail("cannot finish replay");
    }
    Close();
    return !error_;
}

bool Reader::Open(const wchar_t* path)
{
    if (file_ || error_) return Fail("replay reader already used");
    if (_wfopen_s(&file_, path, L"rb") || !file_) return Fail("cannot open replay");
    if (fread(&header_, sizeof(header_), 1, file_) != 1 || !ValidHeader(header_))
        return Fail("invalid or unsupported replay header");
    return true;
}

int Reader::Next(Frame& frame)
{
    if (error_ || !file_) return -1;
    if (ended_) return 0;
    size_t bytes = fread(&frame, 1, sizeof(frame), file_);
    if (bytes != sizeof(frame)) {
        if (ferror(file_)) { Fail("cannot read replay"); return -1; }
        ended_ = true;
        return 0;
    }
    if (frame.checksum != Checksum(&frame, offsetof(Frame, checksum))) {
        Fail("replay checksum mismatch");
        return -1;
    }
    if (frame.flags == kEnd) {
        if (frame.index != count_ || frame.segment != segment_ || frame.frame != next_ || frame.hash != rolling_ ||
            fgetc(file_) != EOF || ferror(file_)) {
            Fail("invalid replay end");
            return -1;
        }
        ended_ = complete_ = true;
        return 0;
    }
    if (!Accept(frame)) return -1;
    for (unsigned seat = header_.settings.players; seat < 4; ++seat)
        if (frame.held[seat]) { Fail("input for inactive replay seat"); return -1; }
    return 1;
}

} }

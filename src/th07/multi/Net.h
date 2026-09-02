#pragma once

namespace th07 {
namespace net {

// 0 quit, -1 restart, else go on. *present: a frame was drawn.
int RunHostTick(int* present);
unsigned short TestBotMask(int seat);

struct Status {
    bool hasRoundTrip;
    unsigned roundTripMicros;
    unsigned waitingMask;
};
void GetStatus(Status* out);

}
}

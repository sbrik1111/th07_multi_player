#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "FramePacing.hpp"

namespace th07 { namespace launcher {
bool LowLatencyEnabled() { return true; }
} namespace mp {
void Log(const char*, ...) {}
} }

namespace {
unsigned checks;
void Require(bool result, const char* reason)
{
    ++checks;
    if (!result) {
        fprintf(stderr, "FAIL: %s\n", reason);
        exit(1);
    }
}
}

int main()
{
    const double interval = 1.0 / 60.0;
    th07::frame::Schedule schedule;
    double deadline = schedule.Deadline(10.0, interval);
    Require(fabs(deadline - (10.0 + interval - 0.001)) < 1e-9, "input starts 1 ms before the presentation target");
    Require(schedule.Deadline(10.001, interval) == deadline, "message handling preserves the pending deadline");
    schedule.Finish(-1.0);
    Require(fabs(schedule.Deadline(deadline, interval) - deadline - interval) < 1e-9,
            "a skipped display frame advances the target once");
    schedule.Reset();
    for (unsigned i = 0; i < 30; ++i) schedule.Finish(i == 12 ? 0.100 : 0.003);
    Require(fabs(schedule.PrepareSeconds() - 0.00325) < 1e-9, "use recent work plus margin, ignoring loading spikes");
    for (unsigned i = 0; i < 30; ++i) schedule.Finish(0.0001);
    Require(fabs(schedule.PrepareSeconds() - 0.003) < 1e-9, "reduce the preparation budget gradually");
    for (unsigned i = 0; i < 30; ++i) schedule.Finish(0.008);
    Require(fabs(schedule.PrepareSeconds() - 0.008) < 1e-9, "cap the adaptive preparation budget at 8 ms");
    deadline = schedule.Deadline(11.0, 0.004);
    Require(fabs(deadline - 11.0) < 1e-9, "fast playback cannot start before this frame");
    schedule.Retry();
    Require(schedule.Deadline(11.002, interval) == 11.002, "network retries are immediate");
    Require(schedule.Deadline(11.003, interval) == 11.003, "repeated network retries do not wait a whole frame");
    schedule.Finish(0.002);
    Require(fabs(schedule.Deadline(12.0, interval) - (12.0 + interval - 0.008)) < 1e-9,
            "resume a fresh presentation target after network recovery");
    Require(fabs(schedule.Deadline(13.0, interval) - (13.0 + interval - 0.008)) < 1e-9,
            "long stalls do not cause catch-up bursts");
    schedule.Reset();
    Require(schedule.PrepareSeconds() == 0.001, "a device reset restores the initial preparation budget");

    MSG message = {};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    const double past = th07::frame::Now() - 1.0;
    Require(th07::frame::WaitUntil(past), "an elapsed deadline does not wait");

    for (unsigned i = 0; i < 4; ++i) {
        const double deadline = th07::frame::Now() + 0.005;
        Require(th07::frame::WaitUntil(deadline), "a timer reaches its deadline");
        Require(th07::frame::Now() >= deadline, "input is not sampled before its frame deadline");
    }

    Require(PostThreadMessageW(GetCurrentThreadId(), WM_APP, 7, 0) != FALSE, "post a window-thread message");
    const double beforeMessage = th07::frame::Now();
    Require(!th07::frame::WaitUntil(beforeMessage + 1.0), "pending input wakes the message pump");
    Require(th07::frame::Now() - beforeMessage < 0.5, "a pending message does not wait for the frame");
    Require(PeekMessageW(&message, nullptr, WM_APP, WM_APP, PM_REMOVE) != FALSE && message.wParam == 7,
            "the frame wait does not consume the application's message");

    th07::frame::WaitForNetwork();
    th07::frame::Shutdown();
    DWORD handlesBefore = 0;
    Require(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore) != FALSE, "read handle count");
    for (unsigned i = 0; i < 8; ++i) {
        Require(th07::frame::WaitUntil(th07::frame::Now() + 0.003), "the timer can restart after shutdown");
        th07::frame::Shutdown();
    }
    DWORD handlesAfter = 0;
    Require(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter) != FALSE, "read final handle count");
    Require(handlesAfter == handlesBefore, "timer restarts do not leak handles");
    printf("PASS frame pacing: %u checks\n", checks);
}

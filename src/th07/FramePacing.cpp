#include "FramePacing.hpp"

#include <windows.h>
#include <intrin.h>

#include "multi/Launcher.h"
#include "multi/MpConfig.h"
#include "multi/RuntimeData.h"

namespace th07 { namespace frame {
namespace {

HANDLE g_timer;
bool g_timerAttempted;
Schedule g_schedule;
bool g_active;
double g_workStarted, g_submitted;
int g_measure = -1;
double g_started, g_presentStarted, g_frameLate;
double g_lateTotal, g_lateMax, g_workTotal, g_presentTotal;
unsigned g_frames;

bool Measuring()
{
    if (g_measure < 0) {
        char value[8] = {};
        g_measure = GetEnvironmentVariableA("TH07_FRAME_TIMING", value, sizeof(value)) && value[0] == '1';
    }
    return g_measure != 0;
}

HANDLE Timer()
{
    if (!g_timerAttempted) {
        g_timerAttempted = true;
        typedef HANDLE (WINAPI *CreateTimer)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
        const auto create = reinterpret_cast<CreateTimer>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                        "CreateWaitableTimerExW"));
        if (create) g_timer = create(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        const bool highResolution = g_timer != nullptr;
        if (!g_timer) g_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        mp::Log("FRAME_TIMER high_resolution=%u available=%u", highResolution ? 1u : 0u, g_timer ? 1u : 0u);
    }
    return g_timer;
}

bool WaitSeconds(double seconds)
{
    HANDLE timer = Timer();
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(seconds * 10000000.0);
        if (due.QuadPart == 0) due.QuadPart = -1;
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            const DWORD result = MsgWaitForMultipleObjectsEx(1, &timer, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            return result != WAIT_OBJECT_0 + 1;
        }
    }
    const DWORD milliseconds = static_cast<DWORD>(seconds * 1000.0);
    return MsgWaitForMultipleObjectsEx(0, nullptr, milliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE) != WAIT_OBJECT_0;
}

}

Schedule::Schedule() { Reset(); }

void Schedule::Reset()
{
    next_ = -1.0;
    interval_ = 1.0 / 60.0;
    prepare_ = 0.001;
    recentMax_ = 0.0;
    samples_ = 0;
    retry_ = false;
}

double Schedule::Deadline(double now, double interval)
{
    const bool changed = interval != interval_;
    interval_ = interval;
    if (retry_) return now;
    if (next_ < 0.0 || changed || now > next_ + interval_)
        next_ = now + interval_;
    return next_ - (prepare_ < interval_ ? prepare_ : interval_);
}

void Schedule::Finish(double workSeconds)
{
    if (workSeconds >= 0.0) {
        // 30-frame peak plus 0.25 ms, ignoring loading spikes.
        if (workSeconds <= 0.008 && workSeconds > recentMax_)
            recentMax_ = workSeconds;
        if (++samples_ >= 30) {
            double desired = recentMax_ + 0.00025;
            if (desired < 0.001) desired = 0.001;
            if (desired > 0.008) desired = 0.008;
            if (desired > prepare_) prepare_ = desired;
            else if (prepare_ > desired + 0.0005) prepare_ -= 0.00025;
            recentMax_ = 0.0;
            samples_ = 0;
        }
    }
    if (next_ >= 0.0) next_ += interval_;
    retry_ = false;
}

void Schedule::Retry()
{
    next_ = -1.0;
    retry_ = true;
}

double Now()
{
    static LARGE_INTEGER frequency;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}

bool WaitUntil(double deadline)
{
    for (;;) {
        const double remaining = deadline - Now();
        if (remaining <= 0.0) return true;
        if (remaining > 0.001) {
            if (!WaitSeconds(remaining - 0.001)) return false;
        } else {
            if (GetQueueStatus(QS_ALLINPUT) & (QS_ALLINPUT << 16)) return false;
            YieldProcessor();
        }
    }
}

void WaitForNetwork()
{
    g_active = false;
    g_schedule.Retry();
    WaitSeconds(0.00025);
}

bool WaitForFrame(double interval)
{
    const double deadline = g_schedule.Deadline(Now(), interval);
    if (!WaitUntil(deadline)) return false;
    g_active = true;
    g_workStarted = Now();
    g_submitted = 0.0;
    Begin(deadline);
    return true;
}

void Complete(bool presented)
{
    if (g_active)
        g_schedule.Finish(presented && g_submitted >= g_workStarted ? g_submitted - g_workStarted : -1.0);
    g_active = false;
}

void Unpaced()
{
    g_schedule.Reset();
    g_active = false;
}

void Begin(double deadline)
{
    if (!Measuring()) return;
    g_started = Now();
    g_frameLate = g_started > deadline ? g_started - deadline : 0.0;
}

void PresentStarted()
{
    if (Measuring()) g_presentStarted = Now();
}

void Presented(IDirect3DDevice8*, bool succeeded)
{
    const bool measure = Measuring();
    const double submitted = (measure || g_active) ? Now() : 0.0;
    if (succeeded && g_active) g_submitted = submitted;
    if (!succeeded || !measure || !g_started) return;
    g_lateTotal += g_frameLate;
    if (g_frameLate > g_lateMax) g_lateMax = g_frameLate;
    g_workTotal += g_presentStarted - g_started;
    g_presentTotal += submitted - g_presentStarted;
    if (++g_frames == 300) {
        mp::Log("FRAME_TIMING lowlatency=%u frames=%u wake_avg_us=%u wake_max_us=%u "
                "work_avg_us=%u present_avg_us=%u prepare_us=%u",
                launcher::LowLatencyEnabled() ? 1u : 0u, g_frames,
                static_cast<unsigned>(g_lateTotal * 1000000.0 / g_frames),
                static_cast<unsigned>(g_lateMax * 1000000.0),
                static_cast<unsigned>(g_workTotal * 1000000.0 / g_frames),
                static_cast<unsigned>(g_presentTotal * 1000000.0 / g_frames),
                static_cast<unsigned>(g_schedule.PrepareSeconds() * 1000000.0));
        g_frames = 0;
        g_lateTotal = g_lateMax = g_workTotal = g_presentTotal = 0.0;
    }
    g_started = 0.0;
}

void Reset()
{
    Unpaced();
    g_started = g_presentStarted = 0.0;
    g_lateTotal = g_lateMax = g_workTotal = g_presentTotal = 0.0;
    g_frames = 0;
}

void Shutdown()
{
    if (g_timer) CloseHandle(g_timer);
    g_timer = nullptr;
    g_timerAttempted = false;
    Reset();
}

} }

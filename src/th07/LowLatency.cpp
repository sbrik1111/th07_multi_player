#include "LowLatency.hpp"

#include <d3d8.h>
#include <mmsystem.h>
#include <string.h>
#include <windows.h>

#include "AnmManager.hpp"
#include "Chain.hpp"
#include "Controller.hpp"
#include "GameErrorContext.hpp"
#include "GameWindow.hpp"
#include "Netplay.hpp"
#include "SoundPlayer.hpp"
#include "Supervisor.hpp"

extern i32 g_FrameCount;

namespace
{
const int LOW_LATENCY_TARGET_FPS = 60;

// The original Visual Studio .NET Platform SDK predates these declarations,
// although the functions are present on every supported Windows version.
typedef HANDLE(WINAPI *CreateWaitableTimerAFunc)(
    LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
typedef BOOL(WINAPI *SetWaitableTimerFunc)(
    HANDLE, const LARGE_INTEGER *, LONG, void *, void *, BOOL);
typedef HRESULT(WINAPI *DwmFlushFunc)();

bool g_lowLatencyInstalled = false;
bool g_inputFixLogged = false;
bool g_timerPeriodActive = false;
bool g_clockInitialized = false;
bool g_remoteRetryActive = false;
HANDLE g_waitableTimer = NULL;
SetWaitableTimerFunc g_setWaitableTimer = NULL;
HMODULE g_dwmModule = NULL;
DwmFlushFunc g_dwmFlush = NULL;
__int64 g_perfFrequency = 0;
__int64 g_frameTicks = 0;
__int64 g_nextPresentTick = 0;
__int64 g_workStartTick = 0;
__int64 g_totalWorkTicks = 0;
__int64 g_maxWorkTicks = 0;
__int64 g_steadyWorkTicks = 0;
__int64 g_adaptivePrepareTicks = 0;
__int64 g_recentMaxWorkTicks = 0;
u32 g_renderedFrames = 0;
u32 g_steadyWorkFrames = 0;
u32 g_recentWorkFrames = 0;
u32 g_deadlineMisses = 0;
u32 g_clockResyncs = 0;
u32 g_lastSummaryFrame = 0xffffffff;

bool IsGameFocused()
{
    return !g_GameWindow.window ||
           GetForegroundWindow() == g_GameWindow.window;
}

bool KeyDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

// Source-level equivalent of OpenInputLagPatch's FixInputGlitching option.
// It never consumes a failed DirectInput stack buffer and it also prevents a
// background network peer from treating text typed into another window as
// gameplay input.
u16 SafeGetInput()
{
    u16 buttons = 0;

    if (!IsGameFocused())
    {
        return 0;
    }
    if (KeyDown(VK_UP) || KeyDown(VK_NUMPAD8))
        buttons |= TH_BUTTON_UP;
    if (KeyDown(VK_DOWN) || KeyDown(VK_NUMPAD2))
        buttons |= TH_BUTTON_DOWN;
    if (KeyDown(VK_LEFT) || KeyDown(VK_NUMPAD4))
        buttons |= TH_BUTTON_LEFT;
    if (KeyDown(VK_RIGHT) || KeyDown(VK_NUMPAD6))
        buttons |= TH_BUTTON_RIGHT;
    if (KeyDown(VK_NUMPAD7))
        buttons |= TH_BUTTON_UP_LEFT;
    if (KeyDown(VK_NUMPAD9))
        buttons |= TH_BUTTON_UP_RIGHT;
    if (KeyDown(VK_NUMPAD1))
        buttons |= TH_BUTTON_DOWN_LEFT;
    if (KeyDown(VK_NUMPAD3))
        buttons |= TH_BUTTON_DOWN_RIGHT;
    if (KeyDown(VK_HOME))
        buttons |= TH_BUTTON_HOME;
    if (KeyDown('D'))
        buttons |= TH_BUTTON_D;
    if (KeyDown('Z'))
        buttons |= TH_BUTTON_SHOOT;
    if (KeyDown('X'))
        buttons |= TH_BUTTON_BOMB;
    if (KeyDown(VK_SHIFT))
        buttons |= TH_BUTTON_FOCUS;
    if (KeyDown(VK_ESCAPE))
        buttons |= TH_BUTTON_MENU;
    if (KeyDown(VK_CONTROL))
        buttons |= TH_BUTTON_SKIP;
    if (KeyDown('Q'))
        buttons |= TH_BUTTON_Q;
    if (KeyDown('S'))
        buttons |= TH_BUTTON_S;
    if (KeyDown('R'))
        buttons |= TH_BUTTON_RESET;
    if (KeyDown(VK_RETURN))
        buttons |= TH_BUTTON_ENTER;

    // Netplay's final safe-input pass adds the DirectInput controller once.
    // If DirectInput setup failed entirely, preserve the legacy joystick
    // fallback here because that pass intentionally keeps this return value.
    return g_Supervisor.keyboard
        ? buttons
        : Controller::GetControllerInput(buttons);
}

void ResetClock()
{
    g_clockInitialized = false;
    g_nextPresentTick = 0;
    g_workStartTick = 0;
}

void SpinUntil(__int64 target)
{
    LARGE_INTEGER now;
    do
    {
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < target);
}

void WaitUntil(__int64 target)
{
    LARGE_INTEGER now;
    __int64 remaining;
    __int64 oneMillisecond;

    QueryPerformanceCounter(&now);
    remaining = target - now.QuadPart;
    if (remaining <= 0)
    {
        return;
    }
    if (!Netplay::LowLatencySpinWait())
    {
        oneMillisecond = g_perfFrequency / 1000;
        if (remaining > oneMillisecond)
        {
            __int64 coarseTicks = remaining - oneMillisecond;
            if (g_waitableTimer)
            {
                LARGE_INTEGER dueTime;
                dueTime.QuadPart = (__int64)(
                    (double)coarseTicks * -10000000.0 /
                    (double)g_perfFrequency);
                if (dueTime.QuadPart < 0 &&
                    g_setWaitableTimer &&
                    g_setWaitableTimer(g_waitableTimer, &dueTime, 0, NULL,
                                       NULL, FALSE))
                {
                    WaitForSingleObject(g_waitableTimer, INFINITE);
                }
            }
            else
            {
                DWORD milliseconds = (DWORD)(
                    (double)coarseTicks * 1000.0 /
                    (double)g_perfFrequency);
                if (milliseconds > 0)
                {
                    Sleep(milliseconds);
                }
            }
        }
    }
    SpinUntil(target);
}

void BeginLowLatencyFrame()
{
    LARGE_INTEGER now;
    __int64 prepareTicks;
    __int64 updateTick;

    QueryPerformanceCounter(&now);
    if (!g_clockInitialized ||
        now.QuadPart > g_nextPresentTick + g_frameTicks)
    {
        g_nextPresentTick = now.QuadPart + g_frameTicks;
        g_clockInitialized = true;
        g_clockResyncs++;
    }
    prepareTicks =
        g_perfFrequency * Netplay::GetLowLatencyPrepareTimeMs() / 1000;
    if (g_adaptivePrepareTicks > prepareTicks)
    {
        prepareTicks = g_adaptivePrepareTicks;
    }
    if (prepareTicks > g_frameTicks)
    {
        prepareTicks = g_frameTicks;
    }
    updateTick = g_nextPresentTick - prepareTicks;
    WaitUntil(updateTick);
    QueryPerformanceCounter(&now);
    g_workStartTick = now.QuadPart;
}

void FinishLowLatencyFrame(bool presented, bool resyncAfter)
{
    LARGE_INTEGER now;
    __int64 workTicks;
    __int64 basePrepareTicks;
    __int64 desiredPrepareTicks;
    __int64 quarterMillisecond;
    __int64 maximumAdaptiveTicks;

    QueryPerformanceCounter(&now);
    if (presented && g_workStartTick != 0)
    {
        workTicks = now.QuadPart - g_workStartTick;
        g_totalWorkTicks += workTicks;
        if (workTicks > g_maxWorkTicks)
        {
            g_maxWorkTicks = workTicks;
        }
        // Tune to the smallest recent update-to-present budget that still
        // reaches the target. Ignore loading spikes above 8 ms: starting an
        // ordinary frame that early would add more latency than it saves.
        maximumAdaptiveTicks = g_perfFrequency * 8 / 1000;
        if (workTicks <= maximumAdaptiveTicks &&
            workTicks > g_recentMaxWorkTicks)
        {
            g_recentMaxWorkTicks = workTicks;
        }
        if (workTicks <= maximumAdaptiveTicks)
        {
            g_steadyWorkTicks += workTicks;
            g_steadyWorkFrames++;
        }
        g_recentWorkFrames++;
        if (g_recentWorkFrames >= 30)
        {
            quarterMillisecond = g_perfFrequency / 4000;
            basePrepareTicks =
                g_perfFrequency * Netplay::GetLowLatencyPrepareTimeMs() /
                1000;
            desiredPrepareTicks = g_recentMaxWorkTicks + quarterMillisecond;
            if (desiredPrepareTicks < basePrepareTicks)
            {
                desiredPrepareTicks = basePrepareTicks;
            }
            if (desiredPrepareTicks > maximumAdaptiveTicks)
            {
                desiredPrepareTicks = maximumAdaptiveTicks;
            }
            if (desiredPrepareTicks > g_adaptivePrepareTicks)
            {
                g_adaptivePrepareTicks = desiredPrepareTicks;
            }
            else if (g_adaptivePrepareTicks >
                     desiredPrepareTicks + quarterMillisecond * 2)
            {
                g_adaptivePrepareTicks -= quarterMillisecond;
            }
            g_recentMaxWorkTicks = 0;
            g_recentWorkFrames = 0;
        }
    }
    if (presented)
    {
        g_renderedFrames++;
        if (!resyncAfter && now.QuadPart >
            g_nextPresentTick + g_perfFrequency / 4000)
        {
            g_deadlineMisses++;
        }
    }
    g_workStartTick = 0;
    if (resyncAfter)
    {
        ResetClock();
    }
    else
    {
        g_nextPresentTick += g_frameTicks;
    }
}

RenderResult RenderLowLatency(GameWindow *window)
{
    i32 chainResult;
    bool wasRemoteRetry;
    LARGE_INTEGER now;

    if (!window->isAppActive)
    {
        ResetClock();
        g_remoteRetryActive = false;
        Sleep(16);
        return RENDER_RESULT_KEEP_RUNNING;
    }

    wasRemoteRetry = g_remoteRetryActive;
    if (wasRemoteRetry)
    {
        // Once a calculation yielded for network input, poll it at timer
        // resolution instead of scheduling each retry one display frame out.
        QueryPerformanceCounter(&now);
        g_workStartTick = now.QuadPart;
    }
    else
    {
        BeginLowLatencyFrame();
    }

    // PCB originally draws the previous state and samples input afterwards.
    // Updating first lets this input sample appear in the frame that is about
    // to be presented, removing one complete frame of local input latency.
    g_AnmManager->Flush();
    g_Supervisor.viewport.X = 0;
    g_Supervisor.viewport.Y = 0;
    g_Supervisor.viewport.Width = 640;
    g_Supervisor.viewport.Height = 480;
    g_Supervisor.d3dDevice->SetViewport(&g_Supervisor.viewport);

    chainResult = g_Chain.RunCalcChain();
    g_SoundPlayer.ProcessQueues();
    if (!chainResult)
    {
        return RENDER_RESULT_EXIT_SUCCESS;
    }
    if (chainResult == -1)
    {
        return RENDER_RESULT_EXIT_ERROR;
    }
    if (Netplay::IsWaitingForRemoteInput())
    {
        // Retry as soon as the packet arrives; do not quantize the retry to
        // the next 60 Hz presentation target and do not draw partial state.
        if (!g_remoteRetryActive)
        {
            ResetClock();
        }
        g_remoteRetryActive = true;
        Sleep(1);
        return RENDER_RESULT_KEEP_RUNNING;
    }
    g_remoteRetryActive = false;

    if (window->curFrame < 0)
    {
        window->curFrame++;
        FinishLowLatencyFrame(false, wasRemoteRetry);
        return RENDER_RESULT_KEEP_RUNNING;
    }

    g_Supervisor.d3dDevice->BeginScene();
    g_AnmManager->ResetVertexBuffer();
    g_Supervisor.fogEnabled = 255;
    g_Supervisor.DisableFog();
    g_Chain.RunDrawChain();
    g_AnmManager->Flush();
    g_Supervisor.d3dDevice->SetTexture(0, NULL);
    g_Supervisor.d3dDevice->EndScene();
    GameWindow::Present();
    window->curFrame = 0;
    g_FrameCount++;
    FinishLowLatencyFrame(true, wasRemoteRetry);
    // Direct3D8 has no SetMaximumFrameLatency. In windowed mode DwmFlush is
    // the closest native queue bound: it does not return until the currently
    // pending DirectX surface update has reached the compositor. Calling it
    // after the timing sample keeps the adaptive budget focused on CPU/GPU
    // preparation time rather than the intentional composition wait.
    if (g_dwmFlush)
    {
        if (SUCCEEDED(g_dwmFlush()))
        {
            // Phase the next 60 Hz target from the composition that actually
            // consumed this frame. Otherwise DwmFlush can return after the
            // old free-running target and make every following frame look
            // late even though the displayed cadence is stable.
            QueryPerformanceCounter(&now);
            g_nextPresentTick = now.QuadPart + g_frameTicks;
            g_clockInitialized = true;
        }
    }
    return RENDER_RESULT_KEEP_RUNNING;
}
} // namespace

void LowLatency::Install()
{
    LARGE_INTEGER frequency;
    HMODULE kernel32;
    CreateWaitableTimerAFunc createWaitableTimer;

    if (!g_inputFixLogged)
    {
        g_inputFixLogged = true;
        g_GameErrorContext.Log(
            "info : OpenInputLagPatch safe input fix enabled\r\n");
    }
    if (!Netplay::LowLatencyEnabled() ||
        g_Supervisor.cfg.frameskipConfig != 0 || g_lowLatencyInstalled)
    {
        if (Netplay::LowLatencyEnabled() &&
            g_Supervisor.cfg.frameskipConfig != 0)
        {
            g_GameErrorContext.Log(
                "info : low latency loop disabled because frameskip is enabled\r\n");
        }
        return;
    }
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    {
        g_GameErrorContext.Log(
            "error : low latency loop needs QueryPerformanceCounter\r\n");
        return;
    }
    g_perfFrequency = frequency.QuadPart;
    g_frameTicks = g_perfFrequency / LOW_LATENCY_TARGET_FPS;
    g_adaptivePrepareTicks =
        g_perfFrequency * Netplay::GetLowLatencyPrepareTimeMs() / 1000;
    kernel32 = GetModuleHandleA("kernel32.dll");
    createWaitableTimer = kernel32
        ? (CreateWaitableTimerAFunc)GetProcAddress(
              kernel32, "CreateWaitableTimerA")
        : NULL;
    g_setWaitableTimer = kernel32
        ? (SetWaitableTimerFunc)GetProcAddress(kernel32,
                                               "SetWaitableTimer")
        : NULL;
    if (createWaitableTimer && g_setWaitableTimer)
    {
        g_waitableTimer = createWaitableTimer(NULL, TRUE, NULL);
    }
    if (Netplay::LowLatencyDwmFlush() &&
        !Netplay::ForceFullscreen() &&
        (g_Supervisor.cfg.windowed || Netplay::ForceWindowed()))
    {
        g_dwmModule = LoadLibraryA("dwmapi.dll");
        if (g_dwmModule)
        {
            g_dwmFlush =
                (DwmFlushFunc)GetProcAddress(g_dwmModule, "DwmFlush");
        }
    }
    if (timeBeginPeriod(1) == TIMERR_NOERROR)
    {
        g_timerPeriodActive = true;
    }
    g_lowLatencyInstalled = true;
    ResetClock();
    g_GameErrorContext.Log(
        "info : OpenInputLagPatch low latency loop enabled (update-before-draw, prepare %d ms, wait %s, compositor_queue %s)\r\n",
        Netplay::GetLowLatencyPrepareTimeMs(),
        Netplay::LowLatencySpinWait() ? "spin" : "hybrid",
        g_dwmFlush ? "bounded" : "default");
}

void LowLatency::LogSummary()
{
    double averageMs;
    double maximumMs;
    double steadyAverageMs;

    if (!g_lowLatencyInstalled || g_renderedFrames == 0 ||
        g_lastSummaryFrame == g_renderedFrames)
    {
        return;
    }
    averageMs = (double)g_totalWorkTicks * 1000.0 /
        (double)g_perfFrequency / (double)g_renderedFrames;
    maximumMs = (double)g_maxWorkTicks * 1000.0 /
        (double)g_perfFrequency;
    steadyAverageMs = g_steadyWorkFrames != 0
        ? (double)g_steadyWorkTicks * 1000.0 /
              (double)g_perfFrequency / (double)g_steadyWorkFrames
        : 0.0;
    g_lastSummaryFrame = g_renderedFrames;
    g_GameErrorContext.Log(
        "info : low latency timing frames %lu update_to_present_avg %.3f ms steady_avg %.3f ms max %.3f ms adaptive_prepare %.3f ms deadline_misses %lu clock_resyncs %lu\r\n",
        (unsigned long)g_renderedFrames, averageMs, steadyAverageMs, maximumMs,
        (double)g_adaptivePrepareTicks * 1000.0 /
            (double)g_perfFrequency,
        (unsigned long)g_deadlineMisses, (unsigned long)g_clockResyncs);
}

void LowLatency::Shutdown()
{
    if (g_waitableTimer)
    {
        CloseHandle(g_waitableTimer);
        g_waitableTimer = NULL;
    }
    if (g_timerPeriodActive)
    {
        timeEndPeriod(1);
        g_timerPeriodActive = false;
    }
    g_dwmFlush = NULL;
    if (g_dwmModule)
    {
        FreeLibrary(g_dwmModule);
        g_dwmModule = NULL;
    }
}

RenderResult GameWindow::Render()
{
    if (Netplay::IsConnectionFailed())
    {
        // Do not enter either renderer after a confirmed network timeout. The
        // original renderer clears before its calculation step, while the
        // low-latency renderer clears after it; skipping at this shared entry
        // point preserves the last complete backbuffer in both modes.
        Sleep(1);
        return RENDER_RESULT_KEEP_RUNNING;
    }
    if (!g_lowLatencyInstalled || !Netplay::LowLatencyEnabled() ||
        g_Supervisor.cfg.frameskipConfig != 0)
    {
        return OriginalRender();
    }
    return RenderLowLatency(this);
}

u16 Controller::GetInput()
{
    return SafeGetInput();
}

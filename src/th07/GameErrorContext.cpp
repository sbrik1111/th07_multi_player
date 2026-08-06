#include "GameErrorContext.hpp"

#include <stdio.h>
#include <string.h>

namespace
{
bool IsPriorityLogLine(const char *line, size_t length)
{
    static const char *patterns[] = {
        "error :",
        "info : RNG mismatch",
        "info : detailed state mismatch",
        "info : RNG resync",
        "info : Host input timed out",
        "info : rollback replay failed",
        "info : low latency timing",
        "info : input timing delay",
        "info : rollback predicted",
        // End-of-run measurements. They are written last, so without this they
        // are the first thing dropped once the buffer is full.
        "info : boss state samples",
        "info : boss checkpoints",
        "info : relay accepted",
        "info : state verify compared",
        // One-shot events whose absence would otherwise read as "it never
        // happened". Both fire a handful of times per run at most.
        // Stage progress. A run long enough to reach stage 5 produces far more
        // log than the 8 KiB buffer holds, and without these the summary
        // cannot say how far the session actually got - which turns "no
        // divergence" into a claim about an unknown amount of gameplay.
        "info : test stage",
        "info : rollback history invalidated",
        // A machine property, logged only when it changes or is corrected. If
        // a driver is moving how the peers round floats, this is the only line
        // that says so, and it is written early enough to be recycled
        // otherwise.
        "info : fpu control word",
        "info : point extend awarded",
        "info : stage clear bonus",
        "info : cherry max growth",
        "info : cherry at stage end",
        "info : cherry max out of range",
        "info : divergence resources",
        "info : divergence spell",
        "info : controller suppression",
        "info : local controller input",
        "info : spell checkpoint"
    };
    size_t i;
    for (i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++)
    {
        size_t patternLength = strlen(patterns[i]);
        if (length >= patternLength &&
            memcmp(line, patterns[i], patternLength) == 0)
        {
            return true;
        }
    }
    return false;
}

bool IsDiscardableNetplayLogLine(const char *line, size_t length)
{
    static const char *patterns[] = {
        "info : rollback bomb edge",
        "info : random input bomb pulse",
        "info : rollback correction",
        "info : rollback test deferred packet poll",
        // These diagnostics are useful for a short sample, but a three-player
        // full-stage test produces enough of them to fill TH07's fixed 8 KiB
        // error buffer before Stage 2. Preserve lifecycle, hash, resync and
        // failure lines by recycling the oldest high-frequency samples.
        "info : closest-player calls",
        // Periodic samples. The end-of-run summaries are written last and are
        // not discardable, so they are dropped outright if these have already
        // filled the buffer - which is how several diagnostic runs ended up
        // with no summary at all.
        "info : hash trace",
        "info : boss trace",
        // Once the peers have parted, the seed-only resync re-fires every 20
        // to 30 frames for the rest of the session and buries everything
        // else, including the end-of-run summaries. The first occurrence is
        // reported separately by the non-discardable "simulation diverged"
        // error, so recycling the repeats loses nothing.
        "info : RNG mismatch",
        "info : RNG resync applied",
        "info : RNG resync ACK",
        "info : input wait",
        "info : evasive input danger bomb pulse",
        "info : spell lifecycle",
        "info : rollback state frame",
        "info : rollback prediction suspended",
        "info : rollback prediction synchronized"
    };
    size_t i;
    for (i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++)
    {
        size_t patternLength = strlen(patterns[i]);
        if (length >= patternLength &&
            memcmp(line, patterns[i], patternLength) == 0)
        {
            return true;
        }
    }
    return false;
}

bool RemoveOldestDiscardableLogLine(GameErrorContext *context)
{
    char *line = context->m_Buffer;
    while (line < context->m_BufferEnd)
    {
        char *lineEnd = strstr(line, "\r\n");
        char *next;
        size_t length;
        size_t removeLength;
        if (!lineEnd)
        {
            return false;
        }
        next = lineEnd + 2;
        length = (size_t)(lineEnd - line);
        if (IsDiscardableNetplayLogLine(line, length))
        {
            removeLength = (size_t)(next - line);
            memmove(line, next,
                    (size_t)(context->m_BufferEnd - next) + 1);
            context->m_BufferEnd -= removeLength;
            return true;
        }
        line = next;
    }
    return false;
}

bool MakeLogRoom(GameErrorContext *context, size_t size)
{
    while (context->m_BufferEnd + size >= context->m_Buffer + 0x1fff)
    {
        if (!RemoveOldestDiscardableLogLine(context))
        {
            return false;
        }
    }
    return true;
}

bool RemoveOldestInformationalLogLine(GameErrorContext *context)
{
    char *line = context->m_Buffer;
    while (line < context->m_BufferEnd)
    {
        char *lineEnd = strstr(line, "\r\n");
        char *next;
        size_t length;
        size_t removeLength;
        if (!lineEnd)
        {
            return false;
        }
        next = lineEnd + 2;
        length = (size_t)(lineEnd - line);
        if (length >= 6 && memcmp(line, "info :", 6) == 0 &&
            !IsPriorityLogLine(line, length))
        {
            removeLength = (size_t)(next - line);
            memmove(line, next,
                    (size_t)(context->m_BufferEnd - next) + 1);
            context->m_BufferEnd -= removeLength;
            return true;
        }
        line = next;
    }
    return false;
}

bool MakePriorityLogRoom(GameErrorContext *context, size_t size)
{
    while (context->m_BufferEnd + size >= context->m_Buffer + 0x1fff)
    {
        if (!RemoveOldestInformationalLogLine(context))
        {
            return false;
        }
    }
    return true;
}
} // namespace

// GLOBAL: TH07 0x00624210
GameErrorContext g_GameErrorContext;

// FUNCTION: TH07 0x004315f0
const char *GameErrorContext::Log(const char *fmt, ...)
{
    char tmp[8192];
    size_t tmpSize;
    va_list args;

    va_start(args, fmt);
    vsprintf(tmp, fmt, args);
    tmpSize = strlen(tmp);
    bool hasRoom = MakeLogRoom(this, tmpSize);
    if (!hasRoom && IsPriorityLogLine(tmp, tmpSize))
    {
        hasRoom = MakePriorityLogRoom(this, tmpSize);
    }
    if (hasRoom)
    {
        strcpy(this->m_BufferEnd, tmp);

        this->m_BufferEnd += tmpSize;
        *this->m_BufferEnd = '\0';
    }
    va_end(args);
    return fmt;
}

// FUNCTION: TH07 0x00431730
const char *GameErrorContext::Fatal(const char *fmt, ...)
{
    char tmp[512];
    size_t tmpSize;
    va_list args;

    va_start(args, fmt);
    vsprintf(tmp, fmt, args);
    tmpSize = strlen(tmp);
    bool hasRoom = MakeLogRoom(this, tmpSize);
    if (!hasRoom)
    {
        hasRoom = MakePriorityLogRoom(this, tmpSize);
    }
    if (hasRoom)
    {
        strcpy(this->m_BufferEnd, tmp);
        this->m_BufferEnd += tmpSize;
        *this->m_BufferEnd = '\0';
    }
    va_end(args);
    this->m_ShowMessageBox = true;
    return fmt;
}

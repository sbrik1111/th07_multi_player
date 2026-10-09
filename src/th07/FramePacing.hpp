#pragma once

struct IDirect3DDevice8;

namespace th07 { namespace frame {

class Schedule {
public:
    Schedule();
    double Deadline(double now, double interval);
    void Finish(double workSeconds);
    void Retry();
    void Reset();
    double PrepareSeconds() const { return prepare_; }
private:
    double next_, interval_, prepare_, recentMax_;
    unsigned samples_;
    bool retry_;
};

double Now();
bool WaitUntil(double deadline);
bool WaitForFrame(double interval);
void WaitForNetwork();
void Complete(bool presented);
void Unpaced();
void Begin(double deadline);
void PresentStarted();
void Presented(IDirect3DDevice8* device, bool succeeded);
void Reset();
void Shutdown();

} }

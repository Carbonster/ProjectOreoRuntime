#include "diagnostics.h"

#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <string>

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>

#include "log.h"

#pragma comment(lib, "Psapi.lib")

namespace diagnostics {

namespace {

// Two seconds at 60 Hz: readable, and a stutter still shows in the worst case.
constexpr int kSamples = 120;
constexpr const char* kModel = "oreo_diagnostics";

class CostHistory {
public:
    void add(double value) {
        last_ = value;
        samples_[index_] = value;
        index_ = (index_ + 1) % kSamples;
        if (filled_ < kSamples) ++filled_;
    }
    bool empty() const { return filled_ == 0; }
    double last() const { return last_; }
    double average() const {
        if (filled_ == 0) return 0.0;
        double total = 0.0;
        for (int i = 0; i < filled_; ++i) total += samples_[i];
        return total / filled_;
    }
    double worst() const {
        double top = 0.0;
        for (int i = 0; i < filled_; ++i) if (samples_[i] > top) top = samples_[i];
        return top;
    }

private:
    double samples_[kSamples] = {};
    double last_ = 0.0;
    int index_ = 0;
    int filled_ = 0;
};

CostHistory g_cpu, g_gpu;
volatile LONG64 g_wireMessages = 0, g_wireBytes = 0;   // pipe thread writes, render thread reads

enum Row { CpuLast, CpuAverage, CpuWorst, GpuLast, GpuAverage, GpuWorst, ProcessMB, PipeMessages,
           PipeKB, Documents, RowCount };

struct RowSpec {
    const char* variable;
    const char* label;
};
const RowSpec kRows[RowCount] = {
    {"cpu", "CPU MS"},           {"cpu_avg", "CPU MS AVG"},   {"cpu_max", "CPU MS MAX"},
    {"gpu", "GPU MS"},           {"gpu_avg", "GPU MS AVG"},   {"gpu_max", "GPU MS MAX"},
    {"process_mb", "PROCESS MB"}, {"pipe_msgs", "PIPE MSG/S"}, {"pipe_kb", "PIPE KB/S"},
    {"documents", "DOCUMENTS VISIBLE"},
};

struct Panel {
    std::string values[RowCount];
    Rml::DataModelHandle model;
    Rml::ElementDocument* document = nullptr;
    bool failed = false;
};
Panel g_panel;

bool enabled() {
    static bool value = false;
    static ULONGLONG checked = 0;
    ULONGLONG now = GetTickCount64();
    if (checked != 0 && now - checked < 2000) return value;
    checked = now;

    char appdata[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return value = false;
    std::string path = std::string(appdata) + "\\ProjectOreo\\overlay.ini";
    // A word, like every other true/false setting here; GetPrivateProfileInt would read "true" as 0.
    char text[16] = {0};
    GetPrivateProfileStringA("Debug", "DebugHotkeys", "false", text, sizeof(text), path.c_str());
    return value = _stricmp(text, "true") == 0 || _stricmp(text, "1") == 0;
}

std::string format(const char* pattern, double value) {
    char text[32];
    snprintf(text, sizeof(text), pattern, value);
    return text;
}

void format_costs(const CostHistory& history, Row first) {
    if (history.empty()) {
        for (int i = 0; i < 3; ++i) g_panel.values[first + i] = "n/a";
        return;
    }
    g_panel.values[first] = format("%.3f", history.last());
    g_panel.values[first + 1] = format("%.3f", history.average());
    g_panel.values[first + 2] = format("%.3f", history.worst());
}

// Four times a second: nobody reads faster.
bool refresh(int documentsVisible) {
    static ULONGLONG lastTick = 0;
    static LONG64 lastMessages = 0, lastBytes = 0;
    ULONGLONG now = GetTickCount64();
    if (lastTick != 0 && now - lastTick < 250) return false;
    double perSecond = lastTick == 0 ? 0.0 : 1000.0 / (double)(now - lastTick);
    lastTick = now;

    LONG64 messages = InterlockedCompareExchange64(&g_wireMessages, 0, 0);
    LONG64 bytes = InterlockedCompareExchange64(&g_wireBytes, 0, 0);
    double messageRate = (double)(messages - lastMessages) * perSecond;
    double byteRate = (double)(bytes - lastBytes) * perSecond;
    lastMessages = messages;
    lastBytes = bytes;

    PROCESS_MEMORY_COUNTERS memory{};
    double processMB = 0.0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
        processMB = (double)memory.WorkingSetSize / (1024.0 * 1024.0);

    format_costs(g_cpu, CpuLast);
    format_costs(g_gpu, GpuLast);
    g_panel.values[ProcessMB] = format("%.1f", processMB);
    g_panel.values[PipeMessages] = format("%.0f", messageRate);
    g_panel.values[PipeKB] = format("%.1f", byteRate / 1024.0);
    g_panel.values[Documents] = format("%.0f", (double)documentsVisible);
    return true;
}

std::string panel_rml() {
    std::string rows;
    for (const RowSpec& row : kRows)
        rows += std::string("<div class='row'><span class='label'>") + row.label +
                "</span><span class='value'>{{" + row.variable + "}}</span></div>";
    return "<rml><head><style>"
           "body { position: absolute; top: 20px; right: 20px; width: 300px; padding: 10px 14px;"
           " font-family: Roboto;"
           " background-color: #04070ceb; border: 1.5px #7ddcff; border-radius: 8px;"
           " font-size: 14px; color: #e6f0fa; }"
           "h1 { display: block; color: #7ddcff; font-size: 20px; margin-bottom: 14px; }"
           ".row { display: block; height: 20px; }"
           ".label { color: #96aabe; }"
           ".value { position: absolute; right: 14px; }"
           "</style></head><body data-model='" + std::string(kModel) + "'><h1>OVERLAY</h1>" + rows +
           "</body></rml>";
}

bool create_panel(Rml::Context* context) {
    Rml::DataModelConstructor model = context->CreateDataModel(kModel);
    if (!model) return false;
    for (int i = 0; i < RowCount; ++i)
        model.BindFunc(kRows[i].variable, [i](Rml::Variant& out) { out = g_panel.values[i]; });
    g_panel.model = model.GetModelHandle();
    g_panel.document = context->LoadDocumentFromMemory(panel_rml(), "[overlay diagnostics]");
    return g_panel.document != nullptr;
}

}  // namespace

void record_cpu_ms(double ms) { g_cpu.add(ms); }
void record_gpu_ms(double ms) { g_gpu.add(ms); }
void count_wire_bytes(long long bytes) { InterlockedAdd64(&g_wireBytes, bytes); }
void count_wire_message() { InterlockedIncrement64(&g_wireMessages); }

void update(Rml::Context* context, int documentsVisible) {
    bool on = enabled();
    if (on && !g_panel.document && !g_panel.failed) {
        g_panel.failed = !create_panel(context);
        if (g_panel.failed) LOG_ERROR("diagnostics: could not build the panel.");
    }
    if (!g_panel.document) return;
    if (on != g_panel.document->IsVisible()) {
        if (on) g_panel.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        else g_panel.document->Hide();
    }
    if (on && refresh(documentsVisible)) g_panel.model.DirtyAllVariables();
}

}  // namespace diagnostics

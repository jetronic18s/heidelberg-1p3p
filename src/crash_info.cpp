#include "crash_info.h"

#include <cstdio>
#include <cstdint>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_cpu.h>
#include <esp_cpu_utils.h>
#include <esp_debug_helpers.h>
#include <esp_memory_utils.h>
#include <esp_private/panic_internal.h>
#include <soc/soc_caps.h>
#include <xtensa_context.h>

// Linked with -Wl,--wrap=esp_panic_handler: the panic handler calls
// __wrap_esp_panic_handler(), which records the crash and then runs the
// original handler (__real_esp_panic_handler).
// g_exc_frames, g_panic_abort and g_panic_abort_details come from
// panic_internal.h. abort() (assert, ESP_ERROR_CHECK, stack overflow,
// uncaught C++ exception) sets g_panic_abort before it raises the panic;
// the original handler only then replaces reason and description.
extern "C" void __real_esp_panic_handler(panic_info_t *info);

// Change it with the layout of CrashLog: older records are then dropped.
static constexpr uint32_t kMagic = 0x43525332; // "CRS2"
static constexpr int kDepth = 12;
static constexpr int kSlots = 4;

struct CrashInfo {
    int32_t core;
    char reason[48];
    char description[96];
    uint32_t addr;
    char task[SOC_CPU_CORES_NUM][16];
    uint32_t pcs[SOC_CPU_CORES_NUM][kDepth];
};

// The last kSlots crashes; next is the slot the next crash goes to.
struct CrashLog {
    uint32_t magic;
    uint32_t next;
    uint32_t count;
    CrashInfo slots[kSlots];
};

static RTC_NOINIT_ATTR CrashLog s_log;

static void IRAM_ATTR copyText(char *dest, size_t size, const char *src)
{
    size_t i = 0;
    if (src) {
        for (; i + 1 < size && src[i]; i++) dest[i] = src[i];
    }
    dest[i] = 0;
}

static void IRAM_ATTR recordBacktrace(const void *f, uint32_t *pcs)
{
    for (int i = 0; i < kDepth; i++) pcs[i] = 0;
    if (!f) return;
    const XtExcFrame *xt = (const XtExcFrame *)f;
    esp_backtrace_frame_t frame = { .pc = (uint32_t)xt->pc, .sp = (uint32_t)xt->a1, .next_pc = (uint32_t)xt->a0, .exc_frame = xt };
    int n = 0;
    pcs[n++] = esp_cpu_process_stack_pc(frame.pc);
    // A wild stack pointer would fault here and lose the record.
    if (!esp_stack_ptr_is_sane(frame.sp)) return;
    while (n < kDepth && frame.next_pc != 0 && esp_backtrace_get_next_frame(&frame)) {
        pcs[n++] = esp_cpu_process_stack_pc(frame.pc);
    }
}

// A second panic inside the panic handler (IDF allows one) would find
// g_panic_abort still set and record the first abort text again: keep only
// the first record.
static bool s_recording = false;

extern "C" void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    if (s_recording) {
        __real_esp_panic_handler(info);
        return;
    }
    s_recording = true;
    if (s_log.magic != kMagic || s_log.next >= kSlots) {
        s_log.next = 0;
        s_log.count = 0;
        s_log.magic = kMagic;
    }
    CrashInfo &crash = s_log.slots[s_log.next];
    // Count the crash and keep its reason before walking any stack: should
    // the walk fault, the record is still there.
    // A reused slot must not show the old crash's text under the new count.
    crash.reason[0] = 0;
    crash.description[0] = 0;
    s_log.next = (s_log.next + 1) % kSlots;
    s_log.count++;
    crash.core = info->core;
    if (g_panic_abort) {
        copyText(crash.reason, sizeof(crash.reason), "abort");
        copyText(crash.description, sizeof(crash.description), g_panic_abort_details);
    } else {
        copyText(crash.reason, sizeof(crash.reason), info->reason);
        copyText(crash.description, sizeof(crash.description), info->description);
    }
    crash.addr = (uint32_t)info->addr;
    for (int c = 0; c < SOC_CPU_CORES_NUM; c++) {
        crash.pcs[c][0] = 0;
        crash.task[c][0] = 0;
    }
    for (int c = 0; c < SOC_CPU_CORES_NUM; c++) {
        TaskHandle_t task = xTaskGetCurrentTaskHandleForCore(c);
        copyText(crash.task[c], sizeof(crash.task[c]), task ? pcTaskGetName(task) : "?");
        recordBacktrace(g_exc_frames[c], crash.pcs[c]);
    }
    __real_esp_panic_handler(info);
}

// The text goes into the status page: an assert text may contain "<" or
// "&", and a damaged record any byte.
static std::string escaped(const char *text)
{
    std::string out;
    for (; *text; text++) {
        const unsigned char c = (unsigned char)*text;
        switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            case '\n': out += ' '; break;
            default: out += (c < 0x20 || c > 0x7e) ? '?' : (char)c;
        }
    }
    return out;
}

static std::string crashText(CrashInfo &crash)
{
    crash.reason[sizeof(crash.reason) - 1] = 0;
    crash.description[sizeof(crash.description) - 1] = 0;
    char buf[64];
    std::string text = escaped(crash.reason);
    if (crash.description[0]) text += " (" + escaped(crash.description) + ")";
    snprintf(buf, sizeof(buf), " on core %d, addr 0x%08lx", (int)crash.core, (unsigned long)crash.addr);
    text += buf;
    for (int c = 0; c < SOC_CPU_CORES_NUM; c++) {
        crash.task[c][sizeof(crash.task[c]) - 1] = 0;
        snprintf(buf, sizeof(buf), "; core %d task ", c);
        text += buf + escaped(crash.task[c]);
        if (crash.pcs[c][0] == 0) continue;
        text += " backtrace";
        for (int i = 0; i < kDepth && crash.pcs[c][i]; i++) {
            snprintf(buf, sizeof(buf), " 0x%08lx", (unsigned long)crash.pcs[c][i]);
            text += buf;
        }
    }
    return text;
}

std::string crashInfoText()
{
    if (s_log.magic != kMagic || s_log.next >= kSlots) return std::string();
    std::string text;
    char buf[48];
    snprintf(buf, sizeof(buf), "%lu since power-on", (unsigned long)s_log.count);
    text = buf;
    const int shown = s_log.count < (uint32_t)kSlots ? (int)s_log.count : kSlots;
    for (int i = 1; i <= shown; i++) {
        const int slot = ((int)s_log.next - i + kSlots) % kSlots;
        snprintf(buf, sizeof(buf), "<br>#%lu: ", (unsigned long)(s_log.count - i + 1));
        text += buf + crashText(s_log.slots[slot]);
    }
    return text;
}

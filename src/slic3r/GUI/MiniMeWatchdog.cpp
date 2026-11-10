#include "MiniMeWatchdog.hpp"

#include "GUI_App.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/timer.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include <atomic>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace Slic3r::GUI {

#ifdef _WIN32
namespace {

constexpr long long STALL_MS      = 6000;  // first report after the window has not responded for this long
constexpr long long SECOND_MS     = 20000; // a second sample: shows whether it is looping in the same place
constexpr int       MAX_FRAMES    = 48;

std::atomic<long long>  s_beat{0};
std::atomic<bool>       s_running{false};
HANDLE                  s_gui_thread = nullptr;
std::thread             s_thread;
std::mutex              s_mutex;
std::condition_variable s_cv;
wxTimer                *s_timer = nullptr;

long long now_ms() { return (long long) ::GetTickCount64(); }

// The GUI thread's return addresses. While it is suspended nothing here allocates or takes a lock it could hold (it may be
// stuck holding the heap lock): only the unwind tables of the loaded modules are read.
int capture(DWORD64 *pcs)
{
    if (::SuspendThread(s_gui_thread) == DWORD(-1))
        return 0;
    int     n = 0;
    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_FULL;
    if (::GetThreadContext(s_gui_thread, &ctx)) {
        while (n < MAX_FRAMES && ctx.Rip != 0) {
            pcs[n++]        = ctx.Rip;
            DWORD64 image   = 0;
            auto   *entry   = ::RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
            if (entry == nullptr) {
                // a leaf function: the return address is on top of the stack
                if (ctx.Rsp == 0)
                    break;
                ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
                ctx.Rsp += 8;
                continue;
            }
            void   *handler_data = nullptr;
            DWORD64 frame        = 0;
            ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, entry, &ctx, &handler_data, &frame, nullptr);
        }
    }
    ::ResumeThread(s_gui_thread);
    return n;
}

std::string describe(const DWORD64 *pcs, int n)
{
    static bool sym_ready = false;
    HANDLE      process   = ::GetCurrentProcess();
    if (!sym_ready) {
        ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
        sym_ready = ::SymInitialize(process, nullptr, TRUE) != FALSE;
    }
    std::ostringstream out;
    for (int i = 0; i < n; ++i) {
        const DWORD64 pc = pcs[i];
        // module and offset, useful even without symbols
        HMODULE module = nullptr;
        char    module_name[MAX_PATH] = "?";
        if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCSTR>(pc), &module)) {
            char path[MAX_PATH];
            if (::GetModuleFileNameA(module, path, MAX_PATH)) {
                const char *slash = std::strrchr(path, '\\');
                std::snprintf(module_name, sizeof(module_name), "%s", slash ? slash + 1 : path);
            }
        }
        out << "  #" << i << " " << module_name << "+0x" << std::hex << (pc - DWORD64(module)) << std::dec;
        if (sym_ready) {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
            auto *symbol         = reinterpret_cast<SYMBOL_INFO *>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen   = 511;
            DWORD64 displacement = 0;
            if (::SymFromAddr(process, pc, &displacement, symbol))
                out << "  " << symbol->Name;
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD line_disp   = 0;
            if (::SymGetLineFromAddr64(process, pc, &line_disp, &line) && line.FileName != nullptr) {
                const char *file  = line.FileName;
                const char *slash = std::strrchr(file, '\\');
                out << "  (" << (slash ? slash + 1 : file) << ":" << line.LineNumber << ")";
            }
        }
        out << "\n";
    }
    return out.str();
}

void report(long long stalled_ms)
{
    DWORD64   pcs[MAX_FRAMES];
    const int n     = capture(pcs);
    std::string txt = describe(pcs, n);

    SYSTEMTIME t;
    ::GetLocalTime(&t);
    char name[96];
    std::snprintf(name, sizeof(name), "hang_%04d%02d%02d_%02d%02d%02d.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::string path = Slic3r::data_dir() + "\\log\\" + name;
    boost::nowide::ofstream file(path);
    if (file) {
        file << "MiniMe's window had not responded for " << stalled_ms / 1000 << " s. Where the GUI thread is:\n" << txt;
        file.close();
    }
    BOOST_LOG_TRIVIAL(error) << "MiniMe watchdog: the window has not responded for " << stalled_ms / 1000 << " s; stack written to "
                             << path << "\n" << txt;
}

void watch()
{
    int reports = 0;
    while (s_running) {
        {
            std::unique_lock<std::mutex> lock(s_mutex);
            s_cv.wait_for(lock, std::chrono::seconds(2), [] { return !s_running.load(); });
        }
        if (!s_running)
            break;
        const long long stalled = now_ms() - s_beat.load();
        if (stalled < STALL_MS) {
            reports = 0; // responding again
            continue;
        }
        if ((reports == 0 && stalled >= STALL_MS) || (reports == 1 && stalled >= SECOND_MS)) {
            ++reports;
            try {
                report(stalled);
            } catch (...) {
            }
        }
    }
}

} // namespace

void HangWatchdog::start()
{
    if (s_running)
        return;
    if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &s_gui_thread,
                           THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    s_beat = now_ms();
    // the heartbeat: timers also run while a modal window (a dialog) is open, only a stuck GUI thread stops them
    s_timer = new wxTimer();
    s_timer->Bind(wxEVT_TIMER, [](wxTimerEvent &) { s_beat = now_ms(); });
    s_timer->Start(500);
    s_running = true;
    s_thread  = std::thread(watch);
    BOOST_LOG_TRIVIAL(info) << "MiniMe watchdog: watching the window for freezes";
}

void HangWatchdog::stop()
{
    if (!s_running)
        return;
    s_running = false;
    s_cv.notify_all();
    if (s_thread.joinable())
        s_thread.join();
    if (s_timer != nullptr) {
        s_timer->Stop();
        delete s_timer;
        s_timer = nullptr;
    }
    if (s_gui_thread != nullptr) {
        ::CloseHandle(s_gui_thread);
        s_gui_thread = nullptr;
    }
}

#else

void HangWatchdog::start() {}
void HangWatchdog::stop() {}

#endif

} // namespace Slic3r::GUI

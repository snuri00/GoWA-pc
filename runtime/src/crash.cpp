#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/thread.h"

#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

namespace crash {

namespace {

std::string symbol_of(DWORD64 addr) {
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* s = reinterpret_cast<SYMBOL_INFO*>(buf);
    s->SizeOfStruct = sizeof(SYMBOL_INFO);
    s->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), addr, &disp, s)) {
        char out[320];
        std::snprintf(out, sizeof out, "%s+0x%llx", s->Name, static_cast<unsigned long long>(disp));
        return out;
    }
    return "?";
}

LONG WINAPI handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
    auto* rec = ep->ExceptionRecord;
    uint64_t fault = rec->NumberParameters >= 2 ? rec->ExceptionInformation[1] : 0;
    uint64_t base = reinterpret_cast<uint64_t>(vm::g_base);
    guest::Thread* t = guest::current();
    LOG_ERROR("crash", "exception 0x%08lx at %p (%s) fault 0x%llx (guest 0x%llx) thread %s 0x%x", code, rec->ExceptionAddress,
              symbol_of(reinterpret_cast<DWORD64>(rec->ExceptionAddress)).c_str(), static_cast<unsigned long long>(fault),
              static_cast<unsigned long long>(fault - base), t ? t->name.c_str() : "host", t ? t->id : 0);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        LOG_ERROR("crash", "  #%d %s", i, symbol_of(frame.AddrPC.Offset).c_str());
    }
    if (t) {
        auto& c = t->ctx;
        LOG_ERROR("crash", "  guest lr 0x%llx r1 0x%llx r2 0x%llx r3 0x%llx r4 0x%llx r5 0x%llx", static_cast<unsigned long long>(c.lr),
                  static_cast<unsigned long long>(c.gpr[1]), static_cast<unsigned long long>(c.gpr[2]), static_cast<unsigned long long>(c.gpr[3]),
                  static_cast<unsigned long long>(c.gpr[4]), static_cast<unsigned long long>(c.gpr[5]));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}

void install() {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    AddVectoredExceptionHandler(1, handler);
}

}

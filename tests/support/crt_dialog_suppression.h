#pragma once

// A failing test process must never open a modal dialog: on Windows the Debug
// CRT reports abort() and assertion failures through a message box, which stops
// the run dead on a developer desktop and blocks CI until it times out. Calling
// suppressCrtModalDialogs() as the first line of a test main() routes those
// reports to stderr instead, so a failure stays visible but never interactive.

#if defined(_WIN32)
// Test translation units that never pulled in Windows.h must not inherit its
// min/max function macros, which would break std::min/std::max call sites.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace effetune::vst::testing {

inline void suppressCrtModalDialogs() noexcept {
#if defined(_WIN32)
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  // The _Crt* reporting hooks compile away in a release CRT, so the loop
  // variable has to survive an expansion that does not use it.
  for (const int report : {_CRT_ASSERT, _CRT_ERROR, _CRT_WARN}) {
    (void)report;
    (void)_CrtSetReportMode(report, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
  }
  (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                     SEM_NOOPENFILEERRORBOX);
#endif
}

} // namespace effetune::vst::testing

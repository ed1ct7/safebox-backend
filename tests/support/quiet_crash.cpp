// Упавший тест не должен открывать модальные окна CRT ("abort() has been called",
// "Debug Assertion Failed"): прогон зависает, пока окно не закроют руками.
// Сообщения уходят в stderr, процесс завершается с кодом ошибки.
#ifdef _WIN32

#include <crtdbg.h>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

[[maybe_unused]] const bool kQuietCrash = [] {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // в сборке без _DEBUG эти макросы пустые
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    return true;
}();

} // namespace

#endif

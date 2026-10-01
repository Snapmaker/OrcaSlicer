#ifndef CATCH_MAIN
#define CATCH_MAIN

#define CATCH_CONFIG_EXTERNAL_INTERFACES
#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_DEFAULT_REPORTER "verboseconsole"
#include <catch2/catch.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace Catch {

// Per-test-case watchdog. A test case that runs longer than EDGE_TEST_TIMEOUT seconds (default
// 600; 0 disables) is reported by name and section on stderr, a minidump of the process is written
// to %TEMP% (Windows), and the process exits with code 124. Without it a hung test blocks the
// whole run silently: the no-sparse Bambu tower case once sat for three hours in its "wall rib"
// section with no output and no way to tell where it was stuck. The slowest test case takes under
// a minute, so the default leaves an order of magnitude of headroom for a loaded machine.
class TestWatchdog
{
public:
    static TestWatchdog &instance()
    {
        // Never destroyed: the watchdog thread is detached and may still run during static destruction.
        static TestWatchdog *watchdog = new TestWatchdog();
        return *watchdog;
    }

    void test_started(const std::string &name)
    {
        if (m_limit_s <= 0)
            return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_test    = name;
        m_section.clear();
        m_start   = std::chrono::steady_clock::now();
        m_running = true;
        if (!m_thread_started) {
            m_thread_started = true;
            std::thread([this]() { this->run(); }).detach();
        }
    }

    void section_started(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_section = name;
    }

    void test_ended()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
    }

private:
    TestWatchdog()
    {
        if (const char *env = std::getenv("EDGE_TEST_TIMEOUT"))
            m_limit_s = std::atoi(env);
    }

    void run()
    {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::string test, section;
            long long   elapsed_s = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_running)
                    continue;
                elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_start).count();
                if (elapsed_s < m_limit_s)
                    continue;
                test    = m_test;
                section = m_section;
            }
            std::cout.flush();
            std::fprintf(stderr, "\nTEST WATCHDOG: test case \"%s\" (last section \"%s\") still running after %lld s, limit %d s (EDGE_TEST_TIMEOUT).\n",
                         test.c_str(), section.c_str(), elapsed_s, m_limit_s);
            const std::string dump = write_minidump();
            if (!dump.empty())
                std::fprintf(stderr, "TEST WATCHDOG: minidump written to %s\n", dump.c_str());
            std::fflush(stderr);
            std::_Exit(124);
        }
    }

    static std::string write_minidump()
    {
#ifdef _WIN32
        char dir[MAX_PATH + 1] = {};
        if (GetTempPathA(MAX_PATH, dir) == 0)
            return {};
        const std::string path = std::string(dir) + "edgeslicer_test_hang_" + std::to_string(GetCurrentProcessId()) + ".dmp";
        HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return {};
        const auto type = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);
        const BOOL ok   = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, nullptr, nullptr, nullptr);
        CloseHandle(file);
        return ok ? path : std::string();
#else
        return {};
#endif
    }

    int                                   m_limit_s = 600;
    std::mutex                            m_mutex;
    std::string                           m_test;
    std::string                           m_section;
    std::chrono::steady_clock::time_point m_start;
    bool                                  m_running        = false;
    bool                                  m_thread_started = false;
};

struct VerboseConsoleReporter : public ConsoleReporter {
    double duration = 0.;
    using ConsoleReporter::ConsoleReporter;
    
    void testCaseStarting(TestCaseInfo const& _testInfo) override
    {
        Colour::use(Colour::Cyan);
        stream << "Testing ";
        Colour::use(Colour::None);
        stream << _testInfo.name << std::endl;
        TestWatchdog::instance().test_started(_testInfo.name);
        ConsoleReporter::testCaseStarting(_testInfo);
    }
    
    void sectionStarting(const SectionInfo &_sectionInfo) override
    {
        if (_sectionInfo.name != currentTestCaseInfo->name) {
            stream << _sectionInfo.name << std::endl;
            TestWatchdog::instance().section_started(_sectionInfo.name);
        }

        ConsoleReporter::sectionStarting(_sectionInfo);
    }
    
    void sectionEnded(const SectionStats &_sectionStats) override {
        duration += _sectionStats.durationInSeconds;
        ConsoleReporter::sectionEnded(_sectionStats);
    } 
    
    void testCaseEnded(TestCaseStats const& stats) override
    {
        if (stats.totals.assertions.allOk()) {
            Colour::use(Colour::BrightGreen);
            stream << "Passed";
            Colour::use(Colour::None);
            stream << " in " << duration << " [seconds]\n" << std::endl;
        }
        
        duration = 0.;
        TestWatchdog::instance().test_ended();
        ConsoleReporter::testCaseEnded(stats);
    }
};

CATCH_REGISTER_REPORTER( "verboseconsole", VerboseConsoleReporter )

} // namespace Catch

#endif // CATCH_MAIN

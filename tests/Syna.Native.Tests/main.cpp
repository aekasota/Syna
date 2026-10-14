#include "test.h"

#include "syna_api.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace syna_test {

std::wstring fixture(const char* name) {
    std::filesystem::path p = std::filesystem::path(SYNA_SOURCE_DIR) / "tests" / "fixtures" / "generated" / name;
    if (!std::filesystem::exists(p)) throw Skip(std::string("missing fixture ") + name + " (run scripts/make-fixtures.ps1)");
    return p.wstring();
}

std::wstring real_clip() {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(L"SYNA_REAL_CLIP", buf, 1024);
    if (n == 0 || n >= 1024) throw Skip("SYNA_REAL_CLIP not set");
    return std::wstring(buf, n);
}

std::wstring temp_path(const char* name) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "syna-native-tests";
    std::filesystem::create_directories(dir);
    return (dir / name).wstring();
}

}  // namespace syna_test

// Arguments are name filters: a test runs when its name contains any of them, or when none is given.
int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const std::vector<std::string> filters(argv + 1, argv + argc);
    std::filesystem::path logs = std::filesystem::temp_directory_path() / "syna-native-tests" / "logs";
    syna_initialize(logs.wstring().c_str(), 0);

    int passed = 0, failed = 0, skipped = 0;
    for (const auto& c : syna_test::registry()) {
        const std::string name = c.name;
        if (!filters.empty() && std::none_of(filters.begin(), filters.end(),
                                             [&](const std::string& f) { return name.find(f) != std::string::npos; }))
            continue;
        const auto t0 = std::chrono::steady_clock::now();
        try {
            c.fn();
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("PASS  %-48s %8.1f ms\n", c.name, ms);
            ++passed;
        } catch (const syna_test::Skip& s) {
            std::printf("SKIP  %-48s %s\n", c.name, s.what());
            ++skipped;
        } catch (const std::exception& e) {
            std::printf("FAIL  %-48s\n      %s\n", c.name, e.what());
            ++failed;
        }
        std::fflush(stdout);
    }
    std::printf("\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
    syna_shutdown();
    return failed == 0 ? 0 : 1;
}

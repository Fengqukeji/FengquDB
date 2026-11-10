#pragma once
#include <cstdlib>
#include <iostream>
inline void store_test_trace(const char *phase) {
    static const bool enabled = [] {
#ifdef _MSC_VER
        char *value = nullptr;
        size_t length = 0;
        const auto result = _dupenv_s(&value, &length, "FQDB_TEST_TRACE");
        const bool present = result == 0 && value != nullptr;
        std::free(value);
        return present;
#else
        return std::getenv("FQDB_TEST_TRACE") != nullptr;
#endif
    }();
    if (enabled)
        std::cerr << "STORE_TEST_PHASE " << phase << '\n';
}

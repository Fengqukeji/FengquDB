#pragma once
#include <cstdint>
#include <string>
namespace example {
// fqdb:entity prefix=student/ key=id
struct Student {
    int64_t id = 0;
    std::string name;
    int32_t score = 0;
};
}

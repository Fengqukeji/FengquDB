// PTpma's bounded asynchronous local logs, adapted for FengquDB diagnostics.
#pragma once
#include <json.hpp>
#include "fengqudb_log.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <memory>
#include <initializer_list>
#include <utility>
#include <chrono>
namespace fengqudb::diagnostics::ptpma {
using Json = nlohmann::json;
using Fields = std::initializer_list<std::pair<std::string_view, std::string>>;
class LocalLog {
  public:
    explicit LocalLog(std::filesystem::path directory, std::string component, bool reader_only = false);
    ~LocalLog();
    LocalLog(const LocalLog &) = delete;
    LocalLog &operator=(const LocalLog &) = delete;
    void write(std::string_view level, std::string_view event, const Fields &fields = {}) noexcept;
    void write(const fqdb_log_event &event, bool throttle = true) noexcept;
    Json status() const;
    Json query(const Json &args = Json::object()) const;
    Json configure(const Json &args);
    Json clear();
    // Settle the submission watermark captured at entry; later producers cannot prolong it.
    // This is an OS-cache write barrier, not a power-loss durability guarantee. Loss is in status().
    bool flush(std::chrono::milliseconds timeout = std::chrono::seconds(2));
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace fengqudb::diagnostics::ptpma

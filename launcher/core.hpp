#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>

namespace setup {
namespace fs = std::filesystem;
struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("Operation cancelled.") {} };
struct Progress {
    std::string phase, detail;
    std::uint64_t completed{}, total{};
};
using Report = std::function<void(const Progress&)>;
struct Options {
    std::atomic<bool>* cancel{};
    Report report;
    // Deterministic cancellation point for the integration test runner.
    std::uint64_t cancel_after_bytes{};
};
std::wstring wide(const std::string&);
std::string utf8(const std::wstring&);
std::span<const unsigned char> resource(unsigned id);
std::uint64_t verify(const fs::path& iso, const Options& options, bool full);
fs::path prepare(const fs::path& iso, const fs::path& parent, const Options& options);
}

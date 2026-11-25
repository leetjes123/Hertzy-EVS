// Tiny always-on profiler: named accumulators of call count and time, read by `hertzyevs --bench`.
// Usage: { hz::ProfScope p("plot.paint"); ... }   Cost: two clock reads and a mutex per scope.
#pragma once
#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace hz {

struct ProfStat { long long n = 0; double ms = 0, maxMs = 0; };

inline std::mutex& prof_mx() { static std::mutex m; return m; }
inline std::map<std::string, ProfStat>& prof_map() { static std::map<std::string, ProfStat> m; return m; }
inline void prof_add(const char* name, double ms) {
    std::lock_guard<std::mutex> lk(prof_mx());
    ProfStat& s = prof_map()[name];
    ++s.n;
    s.ms += ms;
    if (ms > s.maxMs) s.maxMs = ms;
}
inline std::map<std::string, ProfStat> prof_take() {
    std::lock_guard<std::mutex> lk(prof_mx());
    std::map<std::string, ProfStat> out;
    out.swap(prof_map());
    return out;
}

class ProfScope {
public:
    explicit ProfScope(const char* name) : name_(name), t0_(std::chrono::steady_clock::now()) {}
    ~ProfScope() { prof_add(name_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_).count()); }
    ProfScope(const ProfScope&) = delete;
    ProfScope& operator=(const ProfScope&) = delete;
private:
    const char* name_;
    std::chrono::steady_clock::time_point t0_;
};

}  // namespace hz

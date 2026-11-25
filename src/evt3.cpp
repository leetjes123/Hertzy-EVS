#include "evt3.hpp"
#include <ctime>
#include <fstream>
#include <sstream>

namespace evs {

void Evt3Decoder::decode(const uint16_t* w, size_t n, std::vector<Ev>& out) {
    for (size_t i = 0; i < n; ++i) {
        const uint16_t v = w[i];
        switch (v >> 12) {
        case 0x0:  // ADDR_Y
            y_ = v & 0x7FF;
            break;
        case 0x2: {  // ADDR_X: single event
            if (!have_th_) break;
            const uint16_t x = v & 0x7FF;
            if (x >= width || y_ >= height) { ++badWords; break; }
            out.push_back(Ev{x, y_, uint8_t((v >> 11) & 1), time()});
            break;
        }
        case 0x3:  // VECT_BASE_X
            base_x_ = v & 0x7FF;
            pol_ = (v >> 11) & 1;
            break;
        case 0x4:    // VECT_12
        case 0x5: {  // VECT_8
            const int bits = (v >> 12) == 0x4 ? 12 : 8;
            if (have_th_) {
                const uint16_t mask = v & (bits == 12 ? 0xFFF : 0xFF);
                const int64_t t = time();
                for (int b = 0; b < bits; ++b)
                    if (mask & (1u << b)) {
                        const uint16_t x = uint16_t(base_x_ + b);
                        if (x >= width || y_ >= height) { ++badWords; continue; }
                        out.push_back(Ev{x, y_, pol_, t});
                    }
            }
            base_x_ = uint16_t(base_x_ + bits);
            break;
        }
        case 0x6:  // TIME_LOW
            tl_ = v & 0xFFF;
            break;
        case 0x8: {  // TIME_HIGH
            const uint32_t th = v & 0xFFF;
            // a large step back is the 24-bit wrap (16.78 s); small steps back are ignored as reordering
            if (have_th_ && th < th_ && th_ - th > 2048) epoch_ += int64_t(1) << 24;
            th_ = th;
            have_th_ = true;
            break;
        }
        case 0xA:  // EXT_TRIGGER
            ++triggers;
            if (trigOut && have_th_) trigOut->push_back(TrigEv{time(), uint8_t(v & 1), uint8_t((v >> 8) & 0xF)});
            break;
        default:   // OTHERS, CONTINUED, padding
            break;
        }
    }
}

void Evt3Decoder::decodeBytes(const uint8_t* b, size_t nbytes, std::vector<Ev>& out) {
    if (nbytes == 0) return;
    if (have_odd_) {
        const uint16_t w = uint16_t(odd_) | uint16_t(b[0]) << 8;
        decode(&w, 1, out);
        ++b;
        --nbytes;
        have_odd_ = false;
    }
    const size_t nw = nbytes / 2;
    if (reinterpret_cast<uintptr_t>(b) % 2 == 0) {
        decode(reinterpret_cast<const uint16_t*>(b), nw, out);
    } else {
        std::vector<uint16_t> tmp(nw);
        for (size_t i = 0; i < nw; ++i) tmp[i] = uint16_t(b[2 * i]) | uint16_t(b[2 * i + 1]) << 8;
        decode(tmp.data(), nw, out);
    }
    if (nbytes % 2) { have_odd_ = true; odd_ = b[nbytes - 1]; }
}

void Evt3Encoder::encode(const Ev* e, size_t n, std::vector<uint16_t>& out) {
    if (n == 0) return;
    last_high_ = -1;   // restate the time base in every chunk
    last_low_ = -1;
    last_y_ = -1;
    out.reserve(out.size() + n + n / 4 + 8);
    for (size_t i = 0; i < n; ++i) {
        const Ev& ev = e[i];
        const int64_t t = ev.t < 0 ? 0 : ev.t;
        const int64_t hi = t >> 12, lo = t & 0xFFF;
        if (hi != last_high_) {
            out.push_back(uint16_t(0x8000 | (hi & 0xFFF)));
            last_high_ = hi;
            last_low_ = -1;
        }
        if (lo != last_low_) {
            out.push_back(uint16_t(0x6000 | lo));
            last_low_ = lo;
        }
        if (ev.y != last_y_) {
            out.push_back(uint16_t(0x0000 | (ev.y & 0x7FF)));
            last_y_ = ev.y;
        }
        out.push_back(uint16_t(0x2000 | (ev.p ? 0x800 : 0) | (ev.x & 0x7FF)));
    }
}

std::string raw_header(int width, int height, const std::map<std::string, std::string>& extra) {
    char date[64];
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    std::strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &tmv);
    std::ostringstream h;
    h << "% date " << date << "\n";
    h << "% evt 3.0\n";
    h << "% format EVT3;height=" << height << ";width=" << width << "\n";
    h << "% geometry " << width << "x" << height << "\n";
    h << "% sensor_generation 4.1\n";
    for (const auto& kv : extra) h << "% " << kv.first << " " << kv.second << "\n";
    h << "% end\n";
    return h.str();
}

long long read_raw_header(const std::string& path, std::map<std::string, std::string>& keys) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return -1;
    long long pos = 0;
    std::string line;
    bool any = false;
    while (f.peek() == '%') {
        if (!std::getline(f, line)) break;
        pos += (long long)line.size() + 1;
        any = true;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string body = line.size() > 2 ? line.substr(2) : std::string();
        if (body == "end") return pos;
        const size_t sp = body.find(' ');
        if (sp != std::string::npos) keys[body.substr(0, sp)] = body.substr(sp + 1);
    }
    return any ? pos : -1;
}

}  // namespace evs

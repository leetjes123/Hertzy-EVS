// Prophesee EVT 3.0 event stream: streaming decoder, encoder and the Metavision RAW file header.
// The Triton2 EVS (IMX636) sends EVT 3.0; recordings are written as standard Metavision .raw files (header + the
// camera's own EVT3 words), so OpenEB / Metavision tools and pipelines/evs_raw.py can read them.
//
// EVT3 = little-endian 16-bit words, type in the top 4 bits:
//   0x0 ADDR_Y      y (11 bit)                         0x2 ADDR_X     x (11 bit), polarity bit 11 -> one event
//   0x3 VECT_BASE_X x base (11 bit), polarity bit 11   0x4 VECT_12    12-bit mask of events at base+i, base += 12
//   0x5 VECT_8      8-bit mask, base += 8              0x6 TIME_LOW   time bits 0..11 (us)
//   0x8 TIME_HIGH   time bits 12..23 (wraps every 16.78 s)      0xA EXT_TRIGGER   0xE OTHERS   0x7/0xF CONTINUED
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace evs {

struct Ev {
    uint16_t x = 0, y = 0;
    uint8_t p = 0;          // 1 = ON (brighter), 0 = OFF
    int64_t t = 0;          // microseconds, unwrapped
};

// external trigger edge (EVT3 EXT_TRIGGER word: value bit 0, channel bits 8..11)
struct TrigEv {
    int64_t t = 0;
    uint8_t value = 0, id = 0;
};

class Evt3Decoder {
public:
    void reset() { *this = Evt3Decoder(); }
    // decodes n words, appends events; events before the first TIME_HIGH are dropped (their time is unknown)
    void decode(const uint16_t* w, size_t n, std::vector<Ev>& out);
    void decodeBytes(const uint8_t* b, size_t nbytes, std::vector<Ev>& out);   // keeps an odd trailing byte
    int64_t time() const { return epoch_ + (int64_t(th_) << 12) + tl_; }
    bool hasTime() const { return have_th_; }
    uint64_t triggers = 0;
    std::vector<TrigEv>* trigOut = nullptr;   // when set, every EXT_TRIGGER edge with a known time is appended
    uint64_t badWords = 0;     // words with an x/y outside the sensor (sanity check of the stream format)
    int width = 1280, height = 720;

private:
    int64_t epoch_ = 0;
    uint32_t th_ = 0, tl_ = 0;
    bool have_th_ = false;
    uint16_t y_ = 0, base_x_ = 0;
    uint8_t pol_ = 0;
    bool have_odd_ = false;
    uint8_t odd_ = 0;
};

class Evt3Encoder {
public:
    void reset() { *this = Evt3Encoder(); }
    // events must be time-ordered; appends words. A TIME_HIGH is written first in every call so a chunk can be
    // decoded on its own (pre-trigger buffers start at any chunk).
    void encode(const Ev* e, size_t n, std::vector<uint16_t>& out);

private:
    int64_t last_high_ = -1;
    int64_t last_low_ = -1;
    int last_y_ = -1;
};

// "% key value" header lines of a Metavision RAW file, ending with "% end"
std::string raw_header(int width, int height, const std::map<std::string, std::string>& extra = {});
// reads the header; returns the byte offset of the event data, or -1 when the file is not a RAW file
long long read_raw_header(const std::string& path, std::map<std::string, std::string>& keys);

}  // namespace evs

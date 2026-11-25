// Automatic ROIs from an event-count map:
//   lines: noise-relative activity mask, orientation-guided Hough (every
//          coherent mask pixel votes only near its own edge orientation, from the structure tensor), NMS, weighted TLS
//          refine, band half-width from the perpendicular profile, support length, duplicates merged.
//   blobs: connected regions of the activity mask (8-connected), largest activity first.
#pragma once
#include <vector>
#include "roi.hpp"

namespace evs {

struct LineOptions {
    double rel = 0.08;        // Hough peaks above rel x the strongest
    double minLen = 150;      // support length (px along the line that hold mask pixels)
    int maxLines = 30;
    double maskK = 4.0;       // mask = log activity > median + k MAD
};
std::vector<Roi> detect_lines(const std::vector<float>& count, int W, int H, const LineOptions& o = {});

struct BlobOptions {
    double maskK = 4.0;
    int minArea = 40;         // px
    int maxBlobs = 20;
    int grow = 2;             // dilate each blob by this many px
};
std::vector<Roi> detect_blobs(const std::vector<float>& count, int W, int H, const BlobOptions& o = {});

// the activity mask itself (for display / brush seeding)
std::vector<uint8_t> activity_mask(const std::vector<float>& count, int W, int H, double k, std::vector<float>* logA = nullptr);

}  // namespace evs

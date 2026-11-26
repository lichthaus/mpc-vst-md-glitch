// wavio.h - minimal 16-bit PCM WAV reading and writing for the tools.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace wavio {

inline bool read(const char* path, std::vector<float>& l, std::vector<float>& r, unsigned& rate) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    auto le32 = [&](size_t p) { return uint32_t(d[p] | d[p + 1] << 8 | d[p + 2] << 16 | uint32_t(d[p + 3]) << 24); };
    auto le16 = [&](size_t p) { return uint16_t(d[p] | d[p + 1] << 8); };
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return false;
    unsigned ch = 0, bits = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        const uint32_t sz = le32(p + 4);
        if (!std::memcmp(&d[p], "fmt ", 4)) {
            ch = le16(p + 10);
            rate = le32(p + 12);
            bits = le16(p + 22);
        } else if (!std::memcmp(&d[p], "data", 4)) {
            if (bits != 16 || ch == 0) return false;
            const size_t frames = std::min<size_t>(sz, d.size() - p - 8) / (2 * ch);
            for (size_t i = 0; i < frames; i++) {
                const size_t q = p + 8 + i * 2 * ch;
                l.push_back(int16_t(le16(q)) / 32768.0f);
                r.push_back(int16_t(le16(ch > 1 ? q + 2 : q)) / 32768.0f);
            }
            return true;
        }
        p += 8 + sz + (sz & 1);
    }
    return false;
}

inline void write(const char* path, const std::vector<float>& l, const std::vector<float>& r, unsigned rate) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const uint32_t bytes = uint32_t(l.size() * 4);
    std::fwrite("RIFF", 1, 4, f);
    w32(36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16);
    w16(1);
    w16(2);
    w32(rate);
    w32(rate * 4);
    w16(4);
    w16(16);
    std::fwrite("data", 1, 4, f);
    w32(bytes);
    for (size_t i = 0; i < l.size(); i++)
        for (float v : {l[i], r[i]}) {
            const long s = std::lround(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f);
            w16(uint16_t(int16_t(s)));
        }
    std::fclose(f);
}

}  // namespace wavio

// shim_internal.hpp - shared state and small helpers for the shim TUs.
//
// Not part of the public binding surface (postgres_shim.h). Each shim
// translation unit includes this; everything is `inline` so the definitions
// may appear in every TU.

#pragma once

#include "postgres_shim.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <libpq-fe.h>

namespace cp3pg {

// Handle registry --------------------------------------------------------
//
// libpq pointers live on the C++ side; cp3 sees integer handles only.
// Lookup is mutex-protected. Note the documented rule: one PGconn must not
// be used from two threads at once (libpq requirement); the cp3 client
// serializes calls per connection and never closes a handle with an
// operation in flight.

inline std::mutex g_mutex;
inline std::unordered_map<long long, PGconn*> g_connections;
inline std::unordered_map<long long, PGresult*> g_results;
inline std::unordered_map<long long, PGcancelConn*> g_cancels;
inline long long g_next_handle = 1;

// Thread-local in one TU only (implementing this inline in the header
// emits duplicate thread-local guard symbols per object file).
std::string& lastErrorRef();

inline long long nextHandle() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_next_handle++;
}

inline PGconn* findConnection(long long handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_connections.find(handle);
    return it == g_connections.end() ? nullptr : it->second;
}

inline PGresult* findResult(long long handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_results.find(handle);
    return it == g_results.end() ? nullptr : it->second;
}

inline void rememberError(const std::string& message) {
    lastErrorRef() = message;
}

inline std::string textOrEmpty(const char* text) {
    return text != nullptr ? std::string(text) : std::string();
}

inline long long storeConnection(PGconn* conn) {
    const long long handle = nextHandle();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_connections.emplace(handle, conn);
    return handle;
}

inline long long storeResult(PGresult* result) {
    const long long handle = nextHandle();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_results.emplace(handle, result);
    return handle;
}

// Packed parameters ------------------------------------------------------
//
// `v:<base64-of-value-bytes>` or `n` for NULL, segments separated by single
// spaces. Base64 carries raw bytes, so values may contain spaces and NULs.

struct PackedParam {
    bool isNull = false;
    std::string text;
};

inline std::string base64Decode(const std::string& input) {
    static const signed char table[256] = {
        // 0x00-0x0F
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  62, -1, -1, -1,  63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1,  0, -1, -1,
        // 'A'-'Z' = 0..25, 'a'-'z' = 26..51
        -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    };
    std::string out;
    unsigned acc = 0;
    int bits = 0;
    for (char ch : input) {
        const signed char v = table[static_cast<unsigned char>(ch)];
        if (v < 0) {
            if (ch == '=') break;
            continue;  // ignore anything else; base64 from our own encoder
        }
        acc = (acc << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return out;
}

inline bool parsePacked(const std::string& packed, std::vector<PackedParam>& out,
                        std::string& error) {
    size_t i = 0;
    const size_t n = packed.size();
    while (i < n) {
        if (packed[i] == ' ') {
            ++i;
            continue;
        }
        if (packed[i] == 'n') {
            PackedParam p;
            p.isNull = true;
            out.push_back(p);
            ++i;
            continue;
        }
        if (packed[i] == 'v' && i + 1 < n && packed[i + 1] == ':') {
            size_t pos = i + 2;
            size_t end = pos;
            while (end < n && packed[end] != ' ') ++end;
            PackedParam p;
            p.text = base64Decode(packed.substr(pos, end - pos));
            out.push_back(p);
            i = end;
            continue;
        }
        error = "malformed packed parameters (unknown segment)";
        return false;
    }
    return true;
}

inline std::string base64Encode(const unsigned char* data, size_t size) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        const unsigned b0 = data[i];
        const unsigned b1 = i + 1 < size ? data[i + 1] : 0;
        const unsigned b2 = i + 2 < size ? data[i + 2] : 0;
        out += table[(b0 >> 2) & 0x3F];
        out += table[((b0 << 4) | (b1 >> 4)) & 0x3F];
        out += (i + 1 < size) ? table[((b1 << 2) | (b2 >> 6)) & 0x3F] : '=';
        out += (i + 2 < size) ? table[b2 & 0x3F] : '=';
    }
    return out;
}

// Decode packed parameters into the C arrays libpq takes. The strings back
// the pointers, so `out` must outlive the libpq call.
struct DecodedParams {
    std::vector<PackedParam> values;
    std::vector<int> lengths;
    std::vector<const char*> pointers;
};

inline bool decodePacked(const std::string& packed, DecodedParams& out,
                         std::string& error) {
    if (!parsePacked(packed, out.values, error)) return false;
    const size_t n = out.values.size();
    out.pointers.assign(n, nullptr);
    out.lengths.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (out.values[i].isNull) {
            out.pointers[i] = nullptr;
            out.lengths[i] = 0;
        } else {
            out.pointers[i] = out.values[i].text.c_str();
            out.lengths[i] = static_cast<int>(out.values[i].text.size());
        }
    }
    return true;
}

// Big-endian decoders for binary result values.
inline int16_t readBe16(const unsigned char* p) {
    return static_cast<int16_t>((static_cast<uint16_t>(p[0]) << 8) |
                                static_cast<uint16_t>(p[1]));
}

inline int32_t readBe32(const unsigned char* p) {
    return static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 24) |
                                (static_cast<uint32_t>(p[1]) << 16) |
                                (static_cast<uint32_t>(p[2]) << 8) |
                                static_cast<uint32_t>(p[3]));
}

inline uint32_t readBeU32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline uint64_t readBeU64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

inline float readBeF32(const unsigned char* p) {
    const uint32_t bits = readBeU32(p);
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

inline double readBeF64(const unsigned char* p) {
    const uint64_t bits = readBeU64(p);
    double out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

inline std::string formatFloat(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf);
}

}  // namespace cp3pg

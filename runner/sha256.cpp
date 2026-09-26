#include "sha256.hpp"

// The launcher's streaming SHA-256 (retcomm-launcher src/core/hash.cpp),
// carried here so the runtime depends on nothing of the launcher's.

#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace retro::runner {
namespace {

// Compact public-domain SHA-256 (FIPS 180-4), adapted for file streaming.
struct Sha256Ctx {
    uint32_t state[8]{};
    uint64_t count = 0;
    uint8_t buffer[64]{};
};

uint32_t ror32(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    static constexpr uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
        0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
        0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
        0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + S1 + ch + K[i] + w[i];
        const uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void sha256_init(Sha256Ctx& ctx) {
    ctx.state[0] = 0x6a09e667u;
    ctx.state[1] = 0xbb67ae85u;
    ctx.state[2] = 0x3c6ef372u;
    ctx.state[3] = 0xa54ff53au;
    ctx.state[4] = 0x510e527fu;
    ctx.state[5] = 0x9b05688cu;
    ctx.state[6] = 0x1f83d9abu;
    ctx.state[7] = 0x5be0cd19u;
    ctx.count = 0;
}

void sha256_update(Sha256Ctx& ctx, const uint8_t* data, size_t len) {
    size_t i = 0;
    const size_t idx = size_t(ctx.count & 63ull);
    ctx.count += len;
    if (idx) {
        const size_t fill = 64 - idx;
        if (len < fill) {
            std::memcpy(ctx.buffer + idx, data, len);
            return;
        }
        std::memcpy(ctx.buffer + idx, data, fill);
        sha256_transform(ctx.state, ctx.buffer);
        i = fill;
    }
    for (; i + 64 <= len; i += 64) sha256_transform(ctx.state, data + i);
    if (i < len) std::memcpy(ctx.buffer, data + i, len - i);
}

void sha256_final(Sha256Ctx& ctx, uint8_t digest[32]) {
    uint8_t finalcount[8];
    for (int i = 0; i < 8; ++i)
        finalcount[i] = uint8_t((ctx.count * 8) >> ((7 - i) * 8));

    const uint8_t pad = 0x80;
    sha256_update(ctx, &pad, 1);
    const uint8_t zero = 0;
    while ((ctx.count & 63ull) != 56ull) sha256_update(ctx, &zero, 1);
    sha256_update(ctx, finalcount, 8);
    for (int i = 0; i < 32; ++i)
        digest[i] = uint8_t((ctx.state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xff);
}

} // namespace

std::string file_sha256_hex(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    Sha256Ctx ctx;
    sha256_init(ctx);
    std::array<uint8_t, 1 << 16> buf{};
    while (in) {
        in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
        const auto n = size_t(in.gcount());
        if (n) sha256_update(ctx, buf.data(), n);
    }
    uint8_t digest[32];
    sha256_final(ctx, digest);
    std::ostringstream oss;
    for (uint8_t b : digest)
        oss << std::hex << std::nouppercase << std::setw(2) << std::setfill('0') << int(b);
    return oss.str();
}

} // namespace retro::runner

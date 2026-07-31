#include "md5.h"

#include <string.h>

static const uint32_t K[64] = {
    0xD76AA478u, 0xE8C7B756u, 0x242070DBu, 0xC1BDCEEEu,
    0xF57C0FAFu, 0x4787C62Au, 0xA8304613u, 0xFD469501u,
    0x698098D8u, 0x8B44F7AFu, 0xFFFF5BB1u, 0x895CD7BEu,
    0x6B901122u, 0xFD987193u, 0xA679438Eu, 0x49B40821u,
    0xF61E2562u, 0xC040B340u, 0x265E5A51u, 0xE9B6C7AAu,
    0xD62F105Du, 0x02441453u, 0xD8A1E681u, 0xE7D3FBC8u,
    0x21E1CDE6u, 0xC33707D6u, 0xF4D50D87u, 0x455A14EDu,
    0xA9E3E905u, 0xFCEFA3F8u, 0x676F02D9u, 0x8D2A4C8Au,
    0xFFFA3942u, 0x8771F681u, 0x6D9D6122u, 0xFDE5380Cu,
    0xA4BEEA44u, 0x4BDECFA9u, 0xF6BB4B60u, 0xBEBFBC70u,
    0x289B7EC6u, 0xEAA127FAu, 0xD4EF3085u, 0x04881D05u,
    0xD9D4D039u, 0xE6DB99E5u, 0x1FA27CF8u, 0xC4AC5665u,
    0xF4292244u, 0x432AFF97u, 0xAB9423A7u, 0xFC93A039u,
    0x655B59C3u, 0x8F0CCC92u, 0xFFEFF47Du, 0x85845DD1u,
    0x6FA87E4Fu, 0xFE2CE6E0u, 0xA3014314u, 0x4E0811A1u,
    0xF7537E82u, 0xBD3AF235u, 0x2AD7D2BBu, 0xEB86D391u,
};

static const uint8_t S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static uint32_t rol(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static void block(uint32_t h[4], const uint8_t *p)
{
    uint32_t m[16], a = h[0], b = h[1], c = h[2], d = h[3];
    int i;

    for (i = 0; i < 16; i++)
        m[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) |
               ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);

    for (i = 0; i < 64; i++) {
        uint32_t f;
        int g;

        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        f += a + K[i] + m[g];
        a = d;
        d = c;
        c = b;
        b += rol(f, S[i]);
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
}

void ds10_md5(const void *data, size_t len, uint8_t out[16])
{
    uint32_t h[4] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u};
    const uint8_t *p = (const uint8_t *)data;
    uint8_t tail[128];
    size_t n = len & ~(size_t)63, rest = len - n, i;
    uint64_t bits = (uint64_t)len * 8;

    for (i = 0; i < n; i += 64)
        block(h, p + i);

    memcpy(tail, p + n, rest);
    tail[rest] = 0x80;
    n = (rest < 56) ? 64 : 128;
    memset(tail + rest + 1, 0, n - rest - 1);
    for (i = 0; i < 8; i++)
        tail[n - 8 + i] = (uint8_t)(bits >> (8 * i));
    for (i = 0; i < n; i += 64)
        block(h, tail + i);

    for (i = 0; i < 4; i++) {
        out[4 * i] = (uint8_t)h[i];
        out[4 * i + 1] = (uint8_t)(h[i] >> 8);
        out[4 * i + 2] = (uint8_t)(h[i] >> 16);
        out[4 * i + 3] = (uint8_t)(h[i] >> 24);
    }
}

// Pembuat kode QR mini. Mengikuti ISO/IEC 18004 untuk mode byte, level koreksi M, versi 1-10.
#include "qr.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace qr {
namespace {

// Per versi (1..10), level M: kode koreksi per blok, jumlah blok grup 1 / grup 2
const int kEccPerBlock[11] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
const int kTotalCodewords[11] = {0, 26, 44, 70, 100, 134, 172, 196, 242, 292, 346};
const int kTotalBlocks[11]    = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};

int totalBlocks(int v) { return kTotalBlocks[v]; }

// ---- GF(256), polinom 0x11D
struct GF {
    uint8_t exp[512], log[256];
    GF() {
        int x = 1;
        for (int i = 0; i < 255; i++) { exp[i] = (uint8_t)x; log[x] = (uint8_t)i; x <<= 1; if (x & 0x100) x ^= 0x11D; }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp[log[a] + log[b]] : 0; }
};
const GF& gf() { static GF g; return g; }

std::vector<uint8_t> rsGenerator(int deg) {
    std::vector<uint8_t> g(1, 1);
    for (int i = 0; i < deg; i++) {
        std::vector<uint8_t> n(g.size() + 1, 0);
        for (size_t j = 0; j < g.size(); j++) {
            n[j] ^= g[j];
            n[j + 1] ^= gf().mul(g[j], gf().exp[i]);
        }
        g = n;
    }
    return g;  // g[0] = 1 (koefisien suku tertinggi)
}

std::vector<uint8_t> rsRemainder(const std::vector<uint8_t>& data, int ecc) {
    auto g = rsGenerator(ecc);
    std::vector<uint8_t> r(ecc, 0);
    for (uint8_t b : data) {
        uint8_t f = b ^ r[0];
        r.erase(r.begin());
        r.push_back(0);
        for (int i = 0; i < ecc; i++) r[i] ^= gf().mul(g[i + 1], f);
    }
    return r;
}

int dataCodewords(int v) { return kTotalCodewords[v] - kEccPerBlock[v] * totalBlocks(v); }

struct Bits {
    std::vector<bool> b;
    void put(unsigned val, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((val >> i) & 1); }
};

const int kAlign[11][4] = {  // posisi pola penyelaras (di luar 6 dan sisi akhir), 0 = tak dipakai
    {0,0,0,0}, {0,0,0,0}, {6,18,0,0}, {6,22,0,0}, {6,26,0,0}, {6,30,0,0}, {6,34,0,0},
    {6,22,38,0}, {6,24,42,0}, {6,26,46,0}, {6,28,50,0}};

}  // namespace

std::vector<std::vector<bool>> encode(const std::string& text) {
    // pilih versi terkecil yang muat
    int ver = 0;
    for (int v = 1; v <= 10; v++) {
        int cc = v < 10 ? 8 : 16;  // panjang indikator jumlah karakter mode byte
        int need = 4 + cc + 8 * (int)text.size();
        if (need <= dataCodewords(v) * 8) { ver = v; break; }
    }
    if (!ver) return {};

    const int dataCw = dataCodewords(ver);
    Bits bits;
    bits.put(0x4, 4);
    bits.put((unsigned)text.size(), ver < 10 ? 8 : 16);
    for (unsigned char c : text) bits.put(c, 8);
    int cap = dataCw * 8;
    for (int i = 0; i < 4 && (int)bits.b.size() < cap; i++) bits.b.push_back(false);
    while (bits.b.size() % 8) bits.b.push_back(false);
    std::vector<uint8_t> data;
    for (size_t i = 0; i < bits.b.size(); i += 8) {
        uint8_t x = 0;
        for (int j = 0; j < 8; j++) x = (uint8_t)((x << 1) | bits.b[i + j]);
        data.push_back(x);
    }
    for (uint8_t pad = 0xEC; (int)data.size() < dataCw; pad ^= 0xEC ^ 0x11) data.push_back(pad);

    // bagi blok + hitung ECC + interleave
    const int nb = totalBlocks(ver), ecc = kEccPerBlock[ver];
    const int shortBlocks = nb - kTotalCodewords[ver] % nb;
    const int shortLen = kTotalCodewords[ver] / nb - ecc;
    std::vector<std::vector<uint8_t>> blocks, eccs;
    for (int i = 0, k = 0; i < nb; i++) {
        int len = shortLen + (i < shortBlocks ? 0 : 1);
        std::vector<uint8_t> d(data.begin() + k, data.begin() + k + len);
        k += len;
        eccs.push_back(rsRemainder(d, ecc));
        blocks.push_back(std::move(d));
    }
    std::vector<uint8_t> all;
    for (int i = 0; i < shortLen + 1; i++)
        for (int j = 0; j < nb; j++)
            if (i < (int)blocks[j].size()) all.push_back(blocks[j][i]);
    for (int i = 0; i < ecc; i++)
        for (int j = 0; j < nb; j++) all.push_back(eccs[j][i]);

    // matriks
    const int n = ver * 4 + 17;
    std::vector<std::vector<bool>> m(n, std::vector<bool>(n, false));
    std::vector<std::vector<bool>> fn(n, std::vector<bool>(n, false));  // modul fungsi (tak boleh ditimpa data)
    auto setf = [&](int x, int y, bool dark) { if (x >= 0 && y >= 0 && x < n && y < n) { m[y][x] = dark; fn[y][x] = true; } };

    auto finder = [&](int cx, int cy) {
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++) {
                int d = std::max(std::abs(dx), std::abs(dy));
                setf(cx + dx, cy + dy, d != 4 && d != 2);
            }
    };
    finder(3, 3); finder(n - 4, 3); finder(3, n - 4);
    for (int i = 0; i < n; i++) { if (!fn[6][i]) setf(i, 6, i % 2 == 0); if (!fn[i][6]) setf(6, i, i % 2 == 0); }
    if (ver >= 2) {
        int list[6], nl = 0;
        list[nl++] = 6;
        for (int i = 1; i < 4 && kAlign[ver][i]; i++) list[nl++] = kAlign[ver][i];
        for (int a = 0; a < nl; a++)
            for (int b = 0; b < nl; b++) {
                if ((a == 0 && b == 0) || (a == 0 && b == nl - 1) || (a == nl - 1 && b == 0)) continue;
                int cx = list[a], cy = list[b];
                for (int dy = -2; dy <= 2; dy++)
                    for (int dx = -2; dx <= 2; dx++)
                        setf(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
            }
    }
    // cadangkan area format
    for (int i = 0; i < 9; i++) { setf(8, i, false); setf(i, 8, false); }
    for (int i = 0; i < 8; i++) { setf(n - 1 - i, 8, false); setf(8, n - 1 - i, false); }
    setf(8, n - 8, true);  // modul gelap
    if (ver >= 7) {  // info versi
        int rem = ver;
        for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        int bitsV = (ver << 12) | rem;
        for (int i = 0; i < 18; i++) {
            bool bit = (bitsV >> i) & 1;
            int a = n - 11 + i % 3, b = i / 3;
            setf(a, b, bit); setf(b, a, bit);
        }
    }

    // tempatkan data zig-zag
    {
        size_t bi = 0; const size_t total = all.size() * 8;
        for (int right = n - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;
            for (int vert = 0; vert < n; vert++)
                for (int j = 0; j < 2; j++) {
                    int x = right - j;
                    bool up = ((right + 1) & 2) == 0;
                    int y = up ? n - 1 - vert : vert;
                    if (fn[y][x]) continue;
                    if (bi < total) { m[y][x] = (all[bi >> 3] >> (7 - (bi & 7))) & 1; bi++; }
                }
        }
    }

    auto maskBit = [](int mk, int x, int y) {
        switch (mk) {
            case 0: return (x + y) % 2 == 0;
            case 1: return y % 2 == 0;
            case 2: return x % 3 == 0;
            case 3: return (x + y) % 3 == 0;
            case 4: return (x / 3 + y / 2) % 2 == 0;
            case 5: return x * y % 2 + x * y % 3 == 0;
            case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
            default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
        }
    };

    auto drawFormat = [&](std::vector<std::vector<bool>>& g, int mk) {
        int d = (0 << 3) | mk;  // level M = 00
        int rem = d;
        for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        int fb = ((d << 10) | rem) ^ 0x5412;
        auto bit = [&](int i) { return ((fb >> i) & 1) != 0; };
        for (int i = 0; i <= 5; i++) g[i][8] = bit(i);
        g[7][8] = bit(6); g[8][8] = bit(7); g[8][7] = bit(8);
        for (int i = 9; i < 15; i++) g[8][14 - i] = bit(i);
        for (int i = 0; i < 8; i++) g[8][n - 1 - i] = bit(i);
        for (int i = 8; i < 15; i++) g[n - 15 + i][8] = bit(i);
        g[n - 8][8] = true;
    };

    auto penalty = [&](const std::vector<std::vector<bool>>& g) {
        int p = 0;
        for (int pass = 0; pass < 2; pass++) {
            for (int a = 0; a < n; a++) {
                int run = 1;
                for (int b = 1; b < n; b++) {
                    bool cur = pass ? g[b][a] : g[a][b], prev = pass ? g[b - 1][a] : g[a][b - 1];
                    if (cur == prev) { run++; if (run == 5) p += 3; else if (run > 5) p++; } else run = 1;
                }
            }
        }
        for (int y = 0; y + 1 < n; y++)
            for (int x = 0; x + 1 < n; x++)
                if (g[y][x] == g[y][x + 1] && g[y][x] == g[y + 1][x] && g[y][x] == g[y + 1][x + 1]) p += 3;
        static const int pat[11] = {1,0,1,1,1,0,1,0,0,0,0};
        for (int a = 0; a < n; a++)
            for (int b = 0; b + 11 <= n; b++) {
                bool h1 = true, h2 = true, v1 = true, v2 = true;
                for (int k = 0; k < 11; k++) {
                    if (g[a][b + k] != (bool)pat[k]) h1 = false;
                    if (g[a][b + k] != (bool)pat[10 - k]) h2 = false;
                    if (g[b + k][a] != (bool)pat[k]) v1 = false;
                    if (g[b + k][a] != (bool)pat[10 - k]) v2 = false;
                }
                p += 40 * (h1 + h2 + v1 + v2);
            }
        int dark = 0;
        for (auto& r : g) for (bool b : r) dark += b;
        int k = (std::abs(dark * 20 - n * n * 10) + n * n - 1) / (n * n) - 1;
        return p + k * 10;
    };

    int bestP = 1 << 30;
    std::vector<std::vector<bool>> bestG;
    for (int mk = 0; mk < 8; mk++) {
        auto g = m;
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                if (!fn[y][x] && maskBit(mk, x, y)) g[y][x] = !g[y][x];
        drawFormat(g, mk);
        int p = penalty(g);
        if (p < bestP) { bestP = p; bestG = g; }
    }
    return bestG;
}

}  // namespace qr

// Pembuat kode QR mini (mode byte, koreksi galat M, versi 1-10). Tanpa dependensi.
#pragma once
#include <string>
#include <vector>

namespace qr {
// Mengembalikan matriks modul (true = gelap), ukuran n x n; kosong jika data terlalu panjang.
std::vector<std::vector<bool>> encode(const std::string& text);
}

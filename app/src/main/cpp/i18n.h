#pragma once
#include <string>

// Terjemahan teks antarmuka. Teks sumber di kode berbahasa Indonesia; tiap bahasa lain
// punya satu tabel di i18n.cpp (lihat "Menambah bahasa" di README).
namespace i18n {

struct LangInfo { const char* code; const char* name; };

// Daftar bahasa yang tersedia (kode ISO 639-1 + nama aslinya).
const LangInfo* languages(int& count);

// pref: "auto" atau kode bahasa. sys: bahasa sistem (mis. "id", "en", "es").
// auto -> pakai bahasa sistem bila tabelnya ada, selain itu Inggris.
void setLanguage(const std::string& pref, const std::string& sys);

// Kode bahasa yang sedang aktif.
std::string current();

// Menerjemahkan teks sumber. Pola dengan '*' di tabel dipakai untuk pesan yang memuat nilai dinamis.
std::string tr(const std::string& s);

// Untuk literal di UI: menangani sufiks ImGui "##id" / "###id" dan mengembalikan pointer yang
// berlaku sampai ~32 panggilan berikutnya. Panggil hanya dari thread render.
const char* T(const char* s);

}  // namespace i18n

#define T(s) i18n::T(s)

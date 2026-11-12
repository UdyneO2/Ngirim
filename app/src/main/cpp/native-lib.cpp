#include <jni.h>
#include <GLES3/gl3.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "qr.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"
#include "i18n.h"  // harus terakhir: mendefinisikan makro T(...)

namespace {

constexpr const char* kVersion = "v2.9.2";

std::string urlEncode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char ch : in) {
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') o += (char)ch;
        else { o += '%'; o += hex[ch >> 4]; o += hex[ch & 15]; }
    }
    return o;
}

struct Item { std::string name, sub; long long size = 0; bool sel = false; };
struct Entry { std::string name; long long size = 0; long long mtime = 0; bool dir = false; bool sel = false; };

struct Pane {
    std::string path, err;
    bool remote = false;
    std::vector<Entry> e;
};

struct State {
    std::mutex mu;
    // ---- umum
    bool shizuku = false;
    bool shizukuFailed = false;   // bind gagal: tampilkan tombol Shizuku Legacy
    std::string shizukuMsg = "Menunggu Shizuku...";
    std::deque<std::string> actions;
    std::vector<std::string> logs;
    bool logDirty = false;
    int curTab = 0;   // tab yang sedang tampil (untuk tombol Kembali)
    int tabReq = -1;  // paksa pindah tab (indeks), -1 = tidak
    // ---- Setting
    bool dark = false, showHidden = false, useTrash = true;
    std::string langPref = "auto", sysLang = "en";
    bool instShizuku = false, autoUpdate = true;   // mode pasang APK, cek update saat dibuka
    std::string updMsg;
    std::string xapkDir, devName;
    // ---- file manager (tab Lokal)
    std::string fmPath, fmErr, fmFilter;
    std::vector<Entry> fm;
    int fmSort = 0;  // 0 nama, 1 ukuran, 2 tanggal, 3 jenis
    bool fmAsc = true;
    int clipCount = 0;
    bool clipCut = false;
    bool mBusy = false;
    long long mDone = 0, mTotal = 0;
    double mSpeed = 0;
    std::string mCur;
    // ---- APK
    std::vector<Item> apps;
    std::string apkFilter;
    int apkFmt = 0;  // 0 XAPK, 1 APK + split (folder)
    bool apkObb = false, apkData = false, apkSystem = false;
    bool apkOpen = false;  // layar pemilih aplikasi (dibuka dari tab Lokal)
    bool aBusy = false;
    long long aDone = 0, aTotal = 0;
    double aSpeed = 0;
    std::string aCur;
    // ---- jaringan
    bool permOk = false;
    std::string permMsg = "Memeriksa...";
    std::vector<std::string> peers;
    std::string pairedName, addr, pin, recvDir;
    bool listening = false;
    bool allowBrowse = false;
    std::vector<Item> sendList;
    bool netBusy = false;
    long long netDone = 0, netTotal = 0;
    double netSpeed = 0;
    std::string netCur;
    // ---- FTP (dua panel)
    Pane pane[2];
    int active = 0;
    std::vector<std::string> bookmarks;
    bool fBusy = false;
    long long fDone = 0, fTotal = 0;
    double fSpeed = 0;
    std::string fCur;
    int fMove = 0;
    // ---- file browser (pilih file/folder)
    bool bOpen = false;
    int bPurpose = 0;  // 1 kirim, 2 folder terima, 5 folder hasil XAPK
    std::string bPath;
    std::vector<Entry> bEntries;
    // ---- pratinjau gambar (piksel RGBA dari Kotlin, tekstur dibuat di thread render)
    bool pvOpen = false;
    std::string pvName, pvMsg;
    int pvIdx = 0, pvCnt = 0, pvW = 0, pvH = 0;
    std::vector<unsigned char> pvPix;
    bool pvTexDirty = false;
    unsigned pvTex = 0;
    float pvZoom = 1.f, pvPanX = 0.f, pvPanY = 0.f;
} S;

// Antrean sentuh: ImGui butuh minimal 1 frame hover sebelum tombol ditekan, kalau tidak widget
// ber-PressedOnClick (tab) tidak menangkap klik. Jadi ACTION_DOWN dipecah: hover dulu, tekan di frame berikutnya.
struct TouchEv { int act; float x, y; };  // 0 tekan, 1 gerak, 2 lepas, 3 hover
std::deque<TouchEv> g_touchQ;
std::mutex g_touchMu;

void applyTouchEvents() {
    ImGuiIO& io = ImGui::GetIO();
    std::lock_guard<std::mutex> lk(g_touchMu);
    while (!g_touchQ.empty()) {
        TouchEv e = g_touchQ.front();
        g_touchQ.pop_front();
        if (e.act == 2) {
            io.AddMouseButtonEvent(0, false);
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);  // hilangkan hover setelah jari diangkat
            continue;
        }
        io.AddMousePosEvent(e.x, e.y);
        if (e.act == 0) io.AddMouseButtonEvent(0, true);
        if (e.act == 3) break;  // sisakan sisanya untuk frame berikutnya
    }
}

ImGuiContext* g_ctx = nullptr;
ImFont* g_mono = nullptr;
float g_density = 1.f;

// Palet tema (mirip Windows 10: aksen biru #0078D7, kontrol datar bersudut tajam)
struct Pal { ImVec4 ok, warn, err, accent, dim, onAccent, panel, hoverRow, surface, bg, title; ImU32 text, border, boxBg, surfaceU, bgU; } g_pal;
ImFont* g_bold = nullptr;                  // Source Sans 3 SemiBold (tombol, judul)
std::vector<unsigned char> g_fontReg, g_fontBold;

ImVec4 rgb(int r, int g, int b, float a = 1.f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }

// Warna aksen tetap: biru Windows 10
constexpr int kAccentR = 0, kAccentG = 120, kAccentB = 215;

ImVec4 mixc(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x * t + b.x * (1.f - t), a.y * t + b.y * (1.f - t), a.z * t + b.z * (1.f - t), 1.f);
}

void applyTheme(bool dark) {
    // Token warna mengikuti desain baru: latar #f3f3f3, permukaan putih, garis #d0d0d0, aksen #0078d4
    ImVec4* c = ImGui::GetStyle().Colors;
    const ImVec4 accent = rgb(kAccentR, kAccentG, kAccentB);
    ImVec4 bg, surface, text, muted, border, title;
    if (!dark) {
        bg = rgb(243, 243, 243); surface = rgb(255, 255, 255); text = rgb(27, 27, 27); muted = rgb(77, 77, 77);
        border = rgb(208, 208, 208); title = accent;
        g_pal.ok = rgb(16, 124, 16); g_pal.warn = rgb(196, 89, 17); g_pal.err = rgb(164, 38, 44);
    } else {
        bg = rgb(31, 31, 31); surface = rgb(43, 43, 43); text = rgb(243, 243, 243); muted = rgb(200, 200, 200);
        border = rgb(69, 69, 69); title = rgb(20, 20, 20);
        g_pal.ok = rgb(108, 203, 95); g_pal.warn = rgb(252, 185, 90); g_pal.err = rgb(255, 180, 171);
    }
    const ImVec4 hover = bg;                                   // hover tombol outline = warna latar
    const ImVec4 press = mixc(border, bg, 0.6f);
    const ImVec4 sel = mixc(accent, bg, 0.12f), selH = mixc(accent, bg, 0.06f);
    g_pal.accent = accent; g_pal.dim = muted; g_pal.onAccent = rgb(255, 255, 255); g_pal.panel = surface; g_pal.hoverRow = selH;
    g_pal.surface = surface; g_pal.bg = bg; g_pal.title = title;
    g_pal.text = ImGui::ColorConvertFloat4ToU32(text); g_pal.border = ImGui::ColorConvertFloat4ToU32(border);
    g_pal.boxBg = ImGui::ColorConvertFloat4ToU32(surface);
    g_pal.surfaceU = ImGui::ColorConvertFloat4ToU32(surface); g_pal.bgU = ImGui::ColorConvertFloat4ToU32(bg);

    c[ImGuiCol_Text] = text; c[ImGuiCol_TextDisabled] = muted;
    c[ImGuiCol_WindowBg] = bg; c[ImGuiCol_ChildBg] = bg; c[ImGuiCol_PopupBg] = surface;
    c[ImGuiCol_Border] = border; c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = surface; c[ImGuiCol_FrameBgHovered] = hover; c[ImGuiCol_FrameBgActive] = press;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_Button] = surface; c[ImGuiCol_ButtonHovered] = hover; c[ImGuiCol_ButtonActive] = press;
    c[ImGuiCol_Header] = sel; c[ImGuiCol_HeaderHovered] = selH; c[ImGuiCol_HeaderActive] = sel;
    c[ImGuiCol_Separator] = border; c[ImGuiCol_SeparatorHovered] = accent; c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_Tab] = surface; c[ImGuiCol_TabHovered] = selH; c[ImGuiCol_TabSelected] = surface;
    c[ImGuiCol_TabSelectedOverline] = accent; c[ImGuiCol_TabDimmed] = surface; c[ImGuiCol_TabDimmedSelected] = surface;
    c[ImGuiCol_TabDimmedSelectedOverline] = accent;
    c[ImGuiCol_PlotHistogram] = accent; c[ImGuiCol_PlotLines] = accent;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_ScrollbarGrab] = border;
    c[ImGuiCol_TextSelectedBg] = sel;
}

struct Rilis {
    const char* ver;
    const char* judul;
    const char* info;
    std::vector<const char*> butir;
};

// Urut dari versi terlama ke terbaru.
const std::vector<Rilis> kRilis = {
    {"v0.1 beta", "Kirim lewat Wi-Fi", "8 Nov 2026",
     {"Transfer antar-HP di Wi-Fi yang sama, dipasangkan dengan PIN 6 digit",
      "Kirim file dan folder lengkap dengan isinya; penerima memilih folder simpan",
      "Backup dan restore APK (termasuk split), foto, serta data game OBB dan DATA",
      "Progress langsung: persen, kecepatan, sisa waktu; bisa dibatalkan kapan saja",
      "Log gaya terminal dan akses file lewat Shizuku, tanpa root"}},
    {"v0.2 beta", "FTP dan saklar Terima", "8 Nov 2026",
     {"Tab FTP: dua folder berdampingan dengan pintasan folder dan favorit sendiri",
      "Panel kanan bisa menjelajah HP lain yang sudah dipasangkan, lalu salin ke dua arah",
      "Saklar Terima ON/OFF; perangkat aktif muncul otomatis di daftar",
      "Perbaikan: tab Wi-Fi, FTP, dan lainnya tidak bisa diketuk"}},
    {"v1.1", "Rilis stabil pertama", "8 Nov 2026 - gabungan v0.1 beta dan v0.2 beta",
     {"Koneksi Shizuku diperbaiki: tersambung sendiri setelah izin diberikan, tak lagi menggantung di 'Menunggu Shizuku'",
      "Ada tombol langsung ke app Shizuku dan pesan status yang lebih jelas",
      "Semua fitur beta di bawah ikut di rilis ini"}},
    {"v2.0", "Tampilan baru dan file manager", "9 Nov 2026",
     {"Antarmuka ala Windows 10 dengan tema Terang dan Gelap",
      "Tab Lokal jadi file manager: salin, potong, tempel, hapus ke sampah, ubah nama, properti, cari",
      "Akses cepat ke folder penting, bisa menyematkan folder sendiri",
      "Backup APK langsung ke .xapk, OBB dan DATA game opsional",
      "Tab Setting menggantikan Changelog; Terminal berganti nama jadi Log"}},
    {"v2.1", "Update langsung dan Legacy", "9 Nov 2026",
     {"Cek pembaruan dari GitHub Release, tampil catatan rilis lalu pasang langsung",
      "File manager: Buka, Bagikan, Pasang APK, dan Ekstrak ZIP",
      "Pilihan pemasangan: installer sistem atau Shizuku (senyap)",
      "Tombol Shizuku Legacy bila bind gagal; coba ulang otomatis dihapus",
      "Nama paket jadi com.udyneos.ngirim (terpasang sebagai aplikasi baru)",
      "FTP: kotak centang lebih kecil, nama panjang kini terbaca"}},
    {"v2.2", "Warna aksen dan bahasa", "10 Nov 2026",
     {"Pilihan warna aksen di Setting",
      "Bahasa Indonesia dan Inggris, otomatis mengikuti bahasa sistem",
      "Tabel bahasa terpisah, jadi bahasa baru mudah ditambahkan",
      "Riwayat perubahan di Setting diringkas dalam satu bagian lipat"}},
    {"v2.3", "Arsip ZIP dan QR", "10 Nov 2026",
     {"Tombol Arsipkan di file manager: buat .zip dari file/folder terpilih",
      "Pairing lewat QR: tampilkan QR di penerima, pindai dengan kamera pengirim"}},
    {"v2.4", "Pemindai QR dan ikon baru", "11 Nov 2026",
     {"Tombol Pindai QR di tab Wi-Fi: kamera langsung di dalam Ngirim",
      "Ikon aplikasi baru"}},
    {"v2.5", "Lanjutkan transfer", "11 Nov 2026",
     {"Transfer yang putus dilanjutkan dari byte terakhir, bukan mulai dari nol",
      "Otomatis mencoba menyambung lagi sampai 4 kali saat koneksi terputus",
      "Berlaku untuk kirim, terima, dan FTP dua panel"}},
    {"v2.6", "Pratinjau, XAPK, geser pilih", "11 Nov 2026",
     {"Pratinjau gambar di file manager: zoom, geser, sebelumnya/berikutnya",
      "Pasang XAPK, APKS, dan APKM langsung dari file manager (OBB ikut disalin)",
      "Multi-pilih dengan menggeser di kolom kotak centang, di FTP dan Lokal",
      "Pilihan warna aksen dihapus; warna kembali biru"}},
    {"v2.7", "Menu klik kanan dan Kembali", "11 Nov 2026",
     {"Tekan lama file, folder, atau APK di file manager untuk membuka menu seperti klik kanan",
      "Kembali: naik folder, lalu pindah ke Lokal, lalu tekan 2x untuk keluar",
      "File manager jadi halaman utama"}},
    {"v2.8", "Menu ringkas dan lisensi", "11 Nov 2026",
     {"Tombol aksi di atas Akses cepat dihapus; semua ada di menu tekan-lama",
      "Menu lebih rapat, dan tekan lama di area kosong membuka Tempel / Folder baru",
      "Lisensi MIT ditambahkan, lengkap dengan lisensi pustaka pihak ketiga"}},
    {"v2.9", "Tampilan baru", "12 Nov 2026",
     {"Seluruh tampilan didesain ulang: bilah judul biru, tab berikon, bilah status di bawah, panel berbingkai",
      "Font Source Sans 3 (lisensi OFL), toggle bentuk pil, kotak centang dan progress bar membulat",
      "Tema terang dan gelap mengikuti palet baru"}}
};

const std::vector<const char*> kBatasan = {
    "Data Wi-Fi tidak dienkripsi, hanya dijaga PIN",
    "Nama non-Latin bisa tampil sebagai '?'",
    "Router dengan client isolation memblokir koneksi antar-perangkat",
    "XAPK dengan isi di atas 4 GB belum didukung (batas ZIP)",
};

std::string fmtSize(double b) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (b >= 1024 && i < 4) { b /= 1024; i++; }
    char buf[48];
    snprintf(buf, sizeof buf, i ? "%.2f %s" : "%.0f %s", b, u[i]);
    return buf;
}

std::string fmtEta(double s) {
    if (s < 0 || s > 172800) return "--";
    int t = (int)s;
    char buf[32];
    if (t >= 3600) snprintf(buf, sizeof buf, "%dj %02dm", t / 3600, (t % 3600) / 60);
    else snprintf(buf, sizeof buf, "%dm %02ds", t / 60, t % 60);
    return buf;
}

std::string fmtDate(long long ms) {
    if (ms <= 0) return "";
    time_t t = (time_t)(ms / 1000);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tmv);
    return b;
}

std::string js(JNIEnv* e, jstring s) {
    if (!s) return "";
    const char* c = e->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) e->ReleaseStringUTFChars(s, c);
    return r;
}

void act(const std::string& a) { S.actions.push_back(a); }

void dimWrapped(const char* t) {
    ImGui::PushStyleColor(ImGuiCol_Text, g_pal.dim);
    ImGui::TextWrapped("%s", t);
    ImGui::PopStyleColor();
}

// Drag sentuh = scroll. Dipanggil di dalam child window.
void touchScroll() {
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        ImGui::IsMouseDragging(0, 12.f * g_density)) {
        ImGui::ClearActiveID();  // batalkan klik saat drag
        ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
    }
}

// Multi-pilih dengan menggeser jari vertikal di kolom kotak centang (seperti Explorer).
// Pakai: begin() di awal child list, row() untuk tiap baris yang digambar, end() sebelum touchScroll.
struct DragSelect {
    bool cand = false, active = false, started = false, val = false;
    const void* owner = nullptr;
    float prevY = 0;

    void begin(const void* id, float boxW) {
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseClicked(0)) {
            const ImVec2 wp = ImGui::GetWindowPos();
            const bool inBox = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && io.MousePos.x < wp.x + boxW;
            if (inBox) { cand = true; active = false; started = false; owner = id; prevY = io.MousePos.y; }
            else if (owner == id) { cand = active = started = false; }
        }
        if (owner != id) return;
        if (!io.MouseDown[0]) { cand = active = started = false; return; }
        if (cand && !active && fabsf(io.MousePos.y - io.MouseClickedPos[0].y) > 8.f * g_density) active = true;
        if (active) {
            ImGui::ClearActiveID();  // jangan dihitung sebagai ketukan saat jari diangkat
            const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
            const float edge = 36.f * g_density, v = 520.f * g_density * io.DeltaTime;
            if (io.MousePos.y < wp.y + edge) ImGui::SetScrollY(ImGui::GetScrollY() - v);
            else if (io.MousePos.y > wp.y + ws.y - edge) ImGui::SetScrollY(ImGui::GetScrollY() + v);
        }
    }
    // y0..y1 = rentang vertikal baris di layar
    void row(const void* id, bool& sel, float y0, float y1) {
        if (owner != id || !active) return;
        const float cy = ImGui::GetIO().MouseClickedPos[0].y;
        if (!started && cy >= y0 && cy < y1) { val = !sel; started = true; }
        if (!started) return;
        const float cur = ImGui::GetIO().MousePos.y;
        const float lo = std::min(prevY, cur), hi = std::max(prevY, cur);
        if (y1 > lo && y0 < hi) sel = val;
    }
    void end(const void* id) {
        if (owner == id && active) prevY = ImGui::GetIO().MousePos.y;
    }
    bool blocksScroll(const void* id) const { return owner == id && (cand || active); }
} g_ds;

void progressBlock(bool busy, long long done, long long total, double speed, const std::string& cur) {
    const float d = g_density;
    if (!(busy || total > 0)) return;
    float pct = total > 0 ? (float)((double)done / (double)total) : 0.f;
    {
        const float bh = 8.f * d;
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + bh), g_pal.border, bh * 0.5f);
        if (pct > 0.f) dl->AddRectFilled(p, ImVec2(p.x + std::max(bh, w * std::min(1.f, pct)), p.y + bh),
                                         ImGui::ColorConvertFloat4ToU32(g_pal.accent), bh * 0.5f);
        ImGui::Dummy(ImVec2(w, bh + 6.f * d));
    }
    ImGui::TextWrapped("%s", i18n::tr(cur).c_str());
    double eta = speed > 1 ? (double)(total - done) / speed : -1;
    ImGui::Text("%s / %s | %s/s | ETA %s", fmtSize((double)done).c_str(), fmtSize((double)total).c_str(),
                fmtSize(speed).c_str(), busy ? fmtEta(eta).c_str() : T("selesai"));
}

// Log gaya terminal (monospace, latar gelap, warna per jenis baris)
void drawTerminal(const char* id, float h) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.05f, 0.04f, 1.f));
    ImGui::BeginChild(id, ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    if (g_mono) ImGui::PushFont(g_mono);
    for (auto& l : S.logs) {
        const char* body = l.size() > 11 ? l.c_str() + 11 : l.c_str();
        ImVec4 c(0.72f, 0.88f, 0.72f, 1.f);
        if (!strncmp(body, "GAGAL", 5) || !strncmp(body, "ERR", 3)) c = ImVec4(1.f, 0.45f, 0.4f, 1.f);
        else if (!strncmp(body, "OK", 2)) c = ImVec4(0.4f, 1.f, 0.5f, 1.f);
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        ImGui::TextWrapped("%.11s%s", l.c_str(), i18n::tr(body).c_str());
        ImGui::PopStyleColor();
    }
    if (g_mono) ImGui::PopFont();
    if (S.logDirty) { ImGui::SetScrollHereY(1.f); S.logDirty = false; }
    touchScroll();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------------ widget kecil
// Saklar ON/OFF; true saat diketuk.
bool toggleSwitch(const char* id, bool on) {
    const float d = g_density;
    const float h = 24.f * d, w = 44.f * d;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, std::max(h, 32.f * d)));
    p.y += (std::max(h, 32.f * d) - h) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (on) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::ColorConvertFloat4ToU32(g_pal.accent), h * 0.5f);
    else {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), g_pal.boxBg, h * 0.5f);
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), g_pal.border, h * 0.5f, 0, 1.5f * d);
    }
    const float kr = h * 0.5f - 4.f * d;
    dl->AddCircleFilled(ImVec2(on ? p.x + w - h * 0.5f : p.x + h * 0.5f, p.y + h * 0.5f), kr,
                        on ? IM_COL32(255, 255, 255, 255) : g_pal.text);
    return clicked;
}

// Judul bagian: semibold, garis tipis di bawahnya
void Heading(const char* text) {
    const float d = g_density;
    ImGui::Dummy(ImVec2(0, 4.f * d));
    if (g_bold) ImGui::PushFont(g_bold);
    ImGui::TextUnformatted(text, ImGui::FindRenderedTextEnd(text));
    if (g_bold) ImGui::PopFont();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 1.f), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + 1.f), g_pal.border);
    ImGui::Dummy(ImVec2(0, 5.f * d));
}

// Tombol berhuruf semibold (seperti desain baru)
bool Btn(const char* label, ImVec2 size = ImVec2(0, 0)) {
    if (g_bold) ImGui::PushFont(g_bold);
    const bool r = ImGui::Button(label, size);
    if (g_bold) ImGui::PopFont();
    return r;
}

// Tombol yang pindah baris bila tidak muat. `first` = tombol pertama di baris.
bool wrapButton(const std::string& label, bool& first, bool highlight = false) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImGuiStyle& st = ImGui::GetStyle();
    if (g_bold) ImGui::PushFont(g_bold);
    float bw = ImGui::CalcTextSize(label.c_str(), nullptr, true).x + st.FramePadding.x * 2.f;
    if (g_bold) ImGui::PopFont();
    if (!first) {
        float nextX2 = ImGui::GetItemRectMax().x + st.ItemSpacing.x + bw;
        if (nextX2 < w->WorkRect.Max.x) ImGui::SameLine();
    }
    first = false;
    if (highlight) {
        ImGui::PushStyleColor(ImGuiCol_Button, g_pal.accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mixc(g_pal.accent, ImVec4(0, 0, 0, 1), 0.88f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, mixc(g_pal.accent, ImVec4(0, 0, 0, 1), 0.76f));
        ImGui::PushStyleColor(ImGuiCol_Border, g_pal.accent);
        ImGui::PushStyleColor(ImGuiCol_Text, g_pal.onAccent);
    }
    bool r = Btn(label.c_str());
    if (highlight) ImGui::PopStyleColor(5);
    return r;
}

// Potong dari depan dengan "..." (aman untuk UTF-8).
std::string tailEllipsis(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t k = s.size() - (maxBytes - 3);
    while (k < s.size() && (s[k] & 0xC0) == 0x80) k++;
    return "..." + s.substr(k);
}

std::string baseName(const std::string& p) {
    size_t i = p.find_last_of('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}

std::string lowerExt(const std::string& n) {
    size_t i = n.find_last_of('.');
    if (i == std::string::npos || i + 1 >= n.size()) return "";
    std::string e = n.substr(i + 1);
    for (auto& ch : e) ch = (char)tolower((unsigned char)ch);
    return e;
}

// Ikon folder (kuning) dan file (kertas dengan pita warna menurut jenis)
void drawEntryIcon(ImDrawList* dl, ImVec2 p, float s, bool dir, const std::string& name) {
    const float r = s * 0.12f;
    if (dir) {
        const ImU32 body = IM_COL32(255, 200, 70, 255), back = IM_COL32(232, 160, 20, 255);
        dl->AddRectFilled(ImVec2(p.x, p.y + s * 0.1f), ImVec2(p.x + s * 0.48f, p.y + s * 0.4f), back, r);
        dl->AddRectFilled(ImVec2(p.x, p.y + s * 0.26f), ImVec2(p.x + s, p.y + s * 0.88f), body, r);
        return;
    }
    const std::string e = lowerExt(name);
    ImU32 band = IM_COL32(120, 144, 156, 255);
    if (e == "apk" || e == "xapk" || e == "apks") band = IM_COL32(61, 190, 110, 255);
    else if (e == "jpg" || e == "jpeg" || e == "png" || e == "webp" || e == "gif" || e == "heic") band = IM_COL32(0, 150, 136, 255);
    else if (e == "mp4" || e == "mkv" || e == "mov" || e == "avi" || e == "webm") band = IM_COL32(156, 39, 176, 255);
    else if (e == "mp3" || e == "m4a" || e == "flac" || e == "wav" || e == "ogg") band = IM_COL32(233, 30, 99, 255);
    else if (e == "zip" || e == "rar" || e == "7z" || e == "tar" || e == "gz" || e == "obb") band = IM_COL32(255, 152, 0, 255);
    else if (e == "pdf") band = IM_COL32(229, 57, 53, 255);
    const float x0 = p.x + s * 0.16f, x1 = p.x + s * 0.84f, y0 = p.y, y1 = p.y + s, f = s * 0.24f;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(250, 250, 250, 255), r);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(150, 150, 150, 255), r);
    dl->AddTriangleFilled(ImVec2(x1 - f, y0 + 1), ImVec2(x1 - 1, y0 + f), ImVec2(x1 - f, y0 + f), IM_COL32(205, 205, 205, 255));
    dl->AddRectFilled(ImVec2(x0 + s * 0.1f, y1 - s * 0.3f), ImVec2(x1 - s * 0.1f, y1 - s * 0.12f), band, s * 0.06f);
}

// Kotak centang digambar manual agar bisa rata tengah pada baris tinggi.
void drawCheck(ImDrawList* dl, ImVec2 c, float s, bool on) {
    ImVec2 a(c.x - s * 0.5f, c.y - s * 0.5f), b(c.x + s * 0.5f, c.y + s * 0.5f);
    if (on) {
        dl->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32(g_pal.accent), 4.f * g_density);
        const float t = s * 0.14f;
        ImVec2 pts[3] = {ImVec2(c.x - s * 0.26f, c.y), ImVec2(c.x - s * 0.06f, c.y + s * 0.22f), ImVec2(c.x + s * 0.28f, c.y - s * 0.2f)};
        dl->AddPolyline(pts, 3, IM_COL32(255, 255, 255, 255), 0, t);
    } else {
        dl->AddRectFilled(a, b, g_pal.boxBg, 4.f * g_density);
        dl->AddRect(a, b, g_pal.border, 4.f * g_density, 0, 1.5f * g_density);
    }
}

// Baris gaya Explorer. Return: 0 tidak ada, 1 nama diketuk, 2 kotak centang diketuk.
int rowFrame(const char* id, bool sel, float rowH, bool showBox, ImVec2& origin, float& width, bool compact = false) {
    const float d = g_density;
    origin = ImGui::GetCursorScreenPos();
    width = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(id);
    ImGui::SetNextItemAllowOverlap();
    bool nameClicked = ImGui::Selectable("##row", sel, ImGuiSelectableFlags_AllowOverlap, ImVec2(width, rowH));
    int r = nameClicked ? 1 : 0;
    const float cbW = showBox ? (compact ? 32.f : 52.f) * d : 0.f;
    if (showBox) {
        ImGui::SetCursorScreenPos(origin);
        if (ImGui::InvisibleButton("##box", ImVec2(cbW, rowH))) r = 2;
        drawCheck(ImGui::GetWindowDrawList(), ImVec2(origin.x + cbW * 0.5f, origin.y + rowH * 0.5f), (compact ? 15.f : 22.f) * d, sel);
    }
    ImGui::PopID();
    origin.x += cbW;
    width -= cbW;
    return r;
}

int fileRow(int idx, Entry& e, float rowH, bool showBox = true, bool showDate = true, bool compact = false) {
    const float d = g_density;
    ImVec2 q; float w;
    int r = rowFrame(std::to_string(idx).c_str(), e.sel, rowH, showBox, q, w, compact);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float is = rowH * (compact ? 0.44f : 0.56f);
    drawEntryIcon(dl, ImVec2(q.x + (compact ? 0.f : 4.f * d), q.y + (rowH - is) * 0.5f), is, e.dir, e.name);
    const ImVec4 clip(q.x, q.y, q.x + w, q.y + rowH);
    ImFont* f = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * (compact ? 0.9f : 1.f);
    const float tx = q.x + is + (compact ? 6.f : 14.f) * d;
    std::string info = e.dir ? "" : fmtSize((double)e.size);
    if (showDate && e.mtime > 0) info += (info.empty() ? "" : "   ") + fmtDate(e.mtime);
    if (compact) {
        // Panel sempit: nama panjang dibungkus (2 baris, lalu 3 baris berfont lebih kecil); sisa dipotong "...".
        const float aw = clip.z - tx - 2.f * d;
        const float one = f->CalcTextSizeA(fs, FLT_MAX, 0.f, e.name.c_str()).x;
        if (one > aw && aw > 20.f * d) {
            const char* end = e.name.c_str() + e.name.size();
            auto wrapLines = [&](float sz, int maxLines, std::vector<std::string>& ls) {
                ls.clear();
                const char* b = e.name.c_str();
                while (b < end && (int)ls.size() < maxLines) {
                    const char* br = end;
                    if (f->CalcTextSizeA(sz, FLT_MAX, 0.f, b, end).x > aw) {
                        br = f->CalcWordWrapPositionA(sz / f->FontSize, b, end, aw);
                        if (br <= b) br = b + 1;
                    }
                    ls.emplace_back(b, br);
                    b = br;
                    while (b < end && *b == ' ') b++;
                }
                return b >= end;
            };
            std::vector<std::string> ls;
            float sz = fs;
            if (!wrapLines(sz, 2, ls)) { sz = fs * 0.8f; wrapLines(sz, 3, ls); }
            // tandai potongan bila teks belum habis pada baris terakhir
            size_t used = 0; { const char* b = e.name.c_str(); for (auto& l : ls) { used = (size_t)((b - e.name.c_str())); b += l.size(); while (b < end && *b == ' ') b++; used = (size_t)(b - e.name.c_str()); } }
            if (used < e.name.size() && !ls.empty()) {
                std::string& L = ls.back();
                while (!L.empty() && f->CalcTextSizeA(sz, FLT_MAX, 0.f, (L + "...").c_str()).x > aw) L.pop_back();
                L += "...";
            }
            const float lh = sz + 1.f * d;
            float y0 = q.y + rowH * 0.5f - lh * ls.size() * 0.5f;
            for (auto& l : ls) { dl->AddText(f, sz, ImVec2(tx, y0), g_pal.text, l.c_str(), nullptr, 0.f, &clip); y0 += lh; }
            return r;
        }
    }
    const float ty = info.empty() ? q.y + (rowH - fs) * 0.5f : q.y + rowH * 0.5f - fs - 1.f * d;
    dl->AddText(f, fs, ImVec2(tx, ty), g_pal.text, e.name.c_str(), nullptr, 0.f, &clip);
    if (!info.empty())
        dl->AddText(f, fs * 0.8f, ImVec2(tx, q.y + rowH * 0.5f + 2.f * d), ImGui::ColorConvertFloat4ToU32(g_pal.dim),
                    info.c_str(), nullptr, 0.f, &clip);
    return r;
}

// Baris aplikasi: avatar huruf + nama + info
int appRow(int idx, Item& it, float rowH) {
    const float d = g_density;
    ImVec2 q; float w;
    int r = rowFrame(std::to_string(idx).c_str(), it.sel, rowH, true, q, w);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float is = rowH * 0.6f;
    ImVec2 c(q.x + 4.f * d + is * 0.5f, q.y + rowH * 0.5f);
    dl->AddCircleFilled(c, is * 0.5f, ImGui::ColorConvertFloat4ToU32(g_pal.accent));
    char ch[2] = {it.name.empty() ? '?' : (char)toupper((unsigned char)it.name[0]), 0};
    if ((unsigned char)ch[0] >= 0x80) ch[0] = '?';
    ImVec2 ts = ImGui::CalcTextSize(ch);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), ch);
    const ImVec4 clip(q.x, q.y, q.x + w, q.y + rowH);
    ImFont* f = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const float tx = q.x + is + 14.f * d;
    dl->AddText(f, fs, ImVec2(tx, q.y + rowH * 0.5f - fs - 1.f * d), g_pal.text, it.name.c_str(), nullptr, 0.f, &clip);
    dl->AddText(f, fs * 0.8f, ImVec2(tx, q.y + rowH * 0.5f + 2.f * d), ImGui::ColorConvertFloat4ToU32(g_pal.dim),
                it.sub.c_str(), nullptr, 0.f, &clip);
    return r;
}

template <class T> int countSel(const std::vector<T>& v) { int n = 0; for (auto& x : v) if (x.sel) n++; return n; }
template <class T> long long sizeSel(const std::vector<T>& v) { long long n = 0; for (auto& x : v) if (x.sel) n += x.size; return n; }
template <class T> std::string idxSel(const std::vector<T>& v) {
    std::string idx;
    for (int i = 0; i < (int)v.size(); i++)
        if (v[i].sel) idx += (idx.empty() ? "" : ",") + std::to_string(i);
    return idx;
}

// Panel berlatar abu-abu (area "ribbon" ala Explorer) dengan tinggi mengikuti isi.
void beginPanel(const char* id) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, g_pal.panel);
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar);
}
void endPanel() { ImGui::EndChild(); ImGui::PopStyleColor(); }

struct Preset { const char* label; const char* path; };
const Preset kPresets[] = {
    {"Internal", "/sdcard"},
    {"Download", "/sdcard/Download"},
    {"DCIM", "/sdcard/DCIM"},
    {"Pictures", "/sdcard/Pictures"},
    {"Data", "/sdcard/Android/data"},
    {"OBB", "/sdcard/Android/obb"},
    {"Ngirim", "/sdcard/Ngirim"},
};

// Baris "Akses cepat": pintasan bawaan + favorit; tombol terakhir menyematkan/melepas folder saat ini.
void quickAccess(const std::string& prefix, const std::string& curPath, bool allowPin) {
    bool first = true;
    for (auto& p : kPresets)
        if (wrapButton(std::string(p.label) + "##q" + prefix, first, curPath == p.path)) act(prefix + "|GO|" + p.path);
    for (int i = 0; i < (int)S.bookmarks.size(); i++) {
        std::string l = tailEllipsis(baseName(S.bookmarks[i]), 14) + "##bm" + prefix + std::to_string(i);
        if (wrapButton(l, first, curPath == S.bookmarks[i])) act(prefix + "|GO|" + S.bookmarks[i]);
    }
    if (allowPin) {
        bool isBm = false;
        for (auto& b : S.bookmarks) if (b == curPath) isBm = true;
        if (wrapButton(isBm ? T("Lepas sematan##pin") : T("Sematkan folder ini##pin"), first)) act("BM|" + curPath);
    }
}

// ------------------------------------------------------------------ Lokal: file manager
void breadcrumb() {
    // "/sdcard/Download/x" -> [Internal] [> Download] [> x]; path lain dipecah per "/"
    std::vector<std::pair<std::string, std::string>> segs;  // label, target
    std::string p = S.fmPath, cum;
    size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && p[i] == '/') i++;
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        if (j > i) {
            std::string part = p.substr(i, j - i);
            cum += "/" + part;
            segs.push_back({cum == "/sdcard" ? "Internal" : part, cum});
        }
        i = j;
    }
    bool first = true;
    if (wrapButton(T("Naik##fmup"), first)) act("FM|UP");
    for (size_t k = 0; k < segs.size(); k++) {
        std::string l = (k ? "> " : "") + tailEllipsis(segs[k].first, 18) + "##seg" + std::to_string(k);
        if (wrapButton(l, first, k + 1 == segs.size())) act("FM|GO|" + segs[k].second);
    }
}

// ---- Menu konteks (tekan lama pada baris), seperti klik kanan di Windows
struct CtxReq { bool want = false, blank = false; ImVec2 pos{0, 0}; } g_ctxReq;
std::atomic<bool> g_ctxOpen{false}, g_ctxClose{false};

void fmContextMenu(bool busy) {
    const float d = g_density;
    static bool blankMenu = false;
    if (g_ctxReq.want) { g_ctxReq.want = false; blankMenu = g_ctxReq.blank; ImGui::OpenPopup("fm_ctx"); }
    struct CI { std::string label, action; bool sep; };
    std::vector<CI> it;
    const Entry* one = nullptr;
    int nSel = 0, nFile = 0;
    for (auto& e : S.fm) if (e.sel) { nSel++; if (!e.dir) nFile++; one = &e; }
    const std::string ex = (nSel == 1 && one) ? lowerExt(one->name) : "";
    const std::string sel = idxSel(S.fm);
    auto add = [&](const char* label, const std::string& a) { it.push_back({label, a, false}); };
    auto sepr = [&]() { if (!it.empty() && !it.back().sep) it.push_back({"", "", true}); };
    if (blankMenu) {
        if (S.clipCount > 0) { char b[64]; snprintf(b, sizeof b, T("Tempel (%d)"), S.clipCount); add(b, "FM|PASTE"); }
        add(T("Folder baru"), "FM|NEW");
        add(T("Semua"), "FM|ALL");
        add(T("Segarkan"), "FM|REF");
    } else if (nSel == 1 && one) {
        if (one->dir) add(T("Buka folder"), "FM|OPEN|" + sel);
        else {
            add(T("Buka"), "FM|VIEW|" + sel);
            if (ex == "jpg" || ex == "jpeg" || ex == "png" || ex == "webp" || ex == "gif" || ex == "bmp" || ex == "heic" || ex == "heif")
                add(T("Pratinjau"), "FM|PREVIEW|" + sel);
            if (ex == "apk" || ((ex == "xapk" || ex == "apks" || ex == "apkm") && S.shizuku)) add(T("Pasang"), "FM|INSTALL|" + sel);
            if (ex == "zip" && S.shizuku) add(T("Ekstrak"), "FM|UNZIP|" + sel);
        }
        sepr();
    }
    if (!blankMenu) {
    if (nSel > 0) { add(T("Salin"), "FM|COPY|" + sel); add(T("Potong"), "FM|CUT|" + sel); }
    if (S.clipCount > 0) {
        char b[64]; snprintf(b, sizeof b, T("Tempel (%d)"), S.clipCount);
        add(b, "FM|PASTE");
    }
    sepr();
    if (nSel == 1) add(T("Ubah nama"), "FM|REN|" + sel);
    if (nSel > 0) add(T("Hapus"), "FM|DEL|" + sel);
    sepr();
    if (nSel > 0 && S.shizuku) add(T("Arsipkan"), "FM|ZIP|" + sel);
    if (nFile > 0 && nFile == nSel) add(T("Bagikan"), "FM|SHARE|" + sel);
    if (nSel > 0) { add(T("Kirim"), "FM|SEND|" + sel); add(T("Properti"), "FM|PROP|" + sel); }
    }
    while (!it.empty() && it.back().sep) it.pop_back();

    const float rowH = 34.f * d, w = 190.f * d, pad = 4.f * d;
    float h = pad * 2 + (blankMenu ? 0.f : 24.f * d);
    for (auto& c : it) h += c.sep ? 5.f * d : rowH;
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImVec2 pos = g_ctxReq.pos;
    pos.x = std::max(6.f * d, std::min(pos.x, ds.x - w - 6.f * d));
    pos.y = std::max(6.f * d, std::min(pos.y, ds.y - h - 6.f * d));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.f);
    const bool open = ImGui::BeginPopup("fm_ctx");
    ImGui::PopStyleVar(4);
    g_ctxOpen = open;
    if (!open) return;
    if (g_ctxClose.exchange(false) || busy || it.empty()) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::SetWindowFontScale(0.92f);
    if (!blankMenu) {
        std::string head;
        if (nSel == 1 && one) head = tailEllipsis(one->name, 22);
        else { char b[48]; snprintf(b, sizeof b, T("%d dipilih"), nSel); head = b; }
        ImGui::PushStyleColor(ImGuiCol_Text, g_pal.dim);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.f * d);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", head.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
    }
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.f, 0.5f));
    int k = 0;
    for (auto& c : it) {
        if (c.sep) { ImGui::Dummy(ImVec2(0, 1.f * d)); ImGui::Separator(); ImGui::Dummy(ImVec2(0, 1.f * d)); continue; }
        std::string id = "  " + c.label + "##ctx" + std::to_string(k++);
        if (ImGui::Selectable(id.c_str(), false, 0, ImVec2(0, rowH))) {
            if (c.action == "FM|ALL") { for (auto& e : S.fm) e.sel = true; }   // pilihan hanya ada di sisi native
            else act(c.action);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::PopStyleVar(2);
    ImGui::SetWindowFontScale(1.f);
    ImGui::EndPopup();
}

void drawFiles() {
    const float d = g_density;
    const int nSel = countSel(S.fm);
    const bool busy = S.mBusy;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.f * d, 4.f * d));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f * d, 6.f * d));
    beginPanel("fm_head");
    breadcrumb();

    // ribbon: aksi per item ada di menu tekan-lama; di sini hanya yang berlaku untuk folder/daftar
    bool first = true;
    ImGui::BeginDisabled(busy);
    if (wrapButton(T("Folder baru##fm"), first)) act("FM|NEW");
    ImGui::EndDisabled();
    if (wrapButton(nSel == (int)S.fm.size() && nSel > 0 ? T("Batal##fm") : T("Semua##fm"), first))
        for (auto& e : S.fm) e.sel = !(nSel == (int)S.fm.size() && nSel > 0);
    if (wrapButton(T("Segarkan##fm"), first)) act("FM|REF");
    ImGui::BeginDisabled(!S.shizuku);
    if (wrapButton("Backup APK##fm", first, S.aBusy)) { S.apkOpen = true; act("APK|OPEN"); }
    ImGui::EndDisabled();

    // akses cepat
    ImGui::TextColored(g_pal.dim, "%s", T("Akses cepat"));
    quickAccess("FM", S.fmPath, true);

    // urutkan + cari
    ImGui::TextColored(g_pal.dim, "%s", T("Urutkan"));
    first = true;
    const char* sk[] = {T("Nama"), T("Ukuran"), T("Tanggal"), T("Jenis")};
    for (int k = 0; k < 4; k++) {
        std::string l = std::string(sk[k]) + (S.fmSort == k ? (S.fmAsc ? " ^" : " v") : "") + "##sort" + std::to_string(k);
        if (wrapButton(l, first, S.fmSort == k)) act("FM|SORT|" + std::to_string(k));
    }
    if (wrapButton(S.fmFilter.empty() ? T("Cari...##fm") : ([&] { char b[96]; snprintf(b, sizeof b, T("Cari: %s (hapus)"), tailEllipsis(S.fmFilter, 12).c_str()); return std::string(b) + "##fm"; })(), first, !S.fmFilter.empty()))
        act(S.fmFilter.empty() ? "FM|FIND" : "FM|FINDX");
    endPanel();
    ImGui::PopStyleVar(2);

    // daftar
    const bool prog = S.mBusy || S.mTotal > 0;
    float footer = (prog ? 160.f : 12.f) * d;
    float h = ImGui::GetContentRegionAvail().y - footer;
    if (h < 150.f * d) h = 150.f * d;
    ImGui::BeginChild("fm_list", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    if (!S.fmErr.empty()) ImGui::TextColored(g_pal.err, "%s", i18n::tr(S.fmErr).c_str());
    else if (S.fm.empty()) ImGui::TextColored(g_pal.dim, "%s", S.fmFilter.empty() ? T("Folder ini kosong.") : T("Tidak ada yang cocok dengan pencarian."));
    ImGuiListClipper clip;
    clip.Begin((int)S.fm.size());
    g_ds.begin(&S.fm, 52.f * d);
    static int lpIdx = -1; static bool lpFired = false; static ImVec2 lpPos;
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            Entry& e = S.fm[i];
            const float ry = ImGui::GetCursorScreenPos().y;
            if (ImGui::IsMouseClicked(0) && !g_ctxOpen && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                const ImVec2 mp = ImGui::GetIO().MousePos;
                if (mp.y >= ry && mp.y < ry + 56.f * d + ImGui::GetStyle().ItemSpacing.y && mp.x > ImGui::GetWindowPos().x + 52.f * d) {
                    lpIdx = i; lpFired = false; lpPos = mp;
                }
            }
            g_ds.row(&S.fm, e.sel, ry, ry + 56.f * d + ImGui::GetStyle().ItemSpacing.y);
            int r = fileRow(i, e, 56.f * d);
            if (r == 2) e.sel = !e.sel;
            else if (r == 1) {
                if (e.dir) act("FM|OPEN|" + std::to_string(i));
                else e.sel = !e.sel;
            }
        }
    }
    g_ds.end(&S.fm);
    {
        // tekan lama di area kosong daftar: menu Tempel / Folder baru / Semua / Segarkan
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseClicked(0) && !g_ctxOpen && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            io.MousePos.y >= ImGui::GetCursorScreenPos().y) {
            lpIdx = -2; lpFired = false; lpPos = io.MousePos;
        }
    }
    if (lpIdx != -1) {
        ImGuiIO& io = ImGui::GetIO();
        const float dx = io.MousePos.x - lpPos.x, dy = io.MousePos.y - lpPos.y;
        if (!io.MouseDown[0] || dx * dx + dy * dy > 100.f * d * d) lpIdx = -1;
        else if (!lpFired && !busy && io.MouseDownDuration[0] > 0.5f && lpIdx < (int)S.fm.size()) {
            lpFired = true;
            ImGui::ClearActiveID();  // lepas jari nanti tidak dihitung sebagai ketukan
            if (lpIdx >= 0 && !S.fm[lpIdx].sel) { for (auto& e : S.fm) e.sel = false; S.fm[lpIdx].sel = true; }
            g_ctxReq.want = true; g_ctxReq.pos = lpPos; g_ctxReq.blank = lpIdx < 0;
            act("HAPTIC");
        }
    }
    if (!g_ds.blocksScroll(&S.fm) && !(lpFired && lpIdx != -1)) touchScroll();
    ImGui::EndChild();
    fmContextMenu(busy);

    if (S.mBusy) {
        if (Btn(T("BATALKAN##fm"), ImVec2(-FLT_MIN, 44.f * d))) act("CANCEL");
    }
    progressBlock(S.mBusy, S.mDone, S.mTotal, S.mSpeed, S.mCur);
}

// ------------------------------------------------------------------ APK -> XAPK
void drawApk() {
    const float d = g_density;
    const int n = countSel(S.apps);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.f * d, 4.f * d));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f * d, 6.f * d));
    beginPanel("apk_head");
    bool first = true;
    if (wrapButton(T("< Kembali ke file manager##apkback"), first)) S.apkOpen = false;
    ImGui::TextColored(g_pal.accent, "%s", T("Backup aplikasi"));
    ImGui::BeginDisabled(S.aBusy);
    ImGui::TextColored(g_pal.dim, "%s", T("Format backup"));
    first = true;
    if (wrapButton(T("XAPK (satu file)##f"), first, S.apkFmt == 0)) { S.apkFmt = 0; }
    if (wrapButton(T("APK + split (folder)##f"), first, S.apkFmt == 1)) { S.apkFmt = 1; }
    ImGui::BeginDisabled(S.apkFmt != 0);
    if (wrapButton(S.apkObb ? T("[x] Sertakan OBB game##obb") : T("[ ] Sertakan OBB game##obb"), first, S.apkObb)) S.apkObb = !S.apkObb;
    if (wrapButton(S.apkData ? T("[x] Sertakan DATA game##dat") : T("[ ] Sertakan DATA game##dat"), first, S.apkData)) S.apkData = !S.apkData;
    ImGui::EndDisabled();
    if (S.apkFmt == 0 && S.apkData) dimWrapped(T("DATA bisa sangat besar dan bukan bagian standar XAPK; hanya terbaca installer yang mendukungnya."));
    ImGui::TextColored(g_pal.dim, T("Tujuan: %s"), tailEllipsis(S.xapkDir, 48).c_str());
    first = true;
    if (wrapButton(T("Cari...##apk"), first, !S.apkFilter.empty())) act(S.apkFilter.empty() ? "APK|FIND" : "APK|FINDX");
    if (wrapButton(S.apkSystem ? T("Sembunyikan app sistem##sys") : T("Tampilkan app sistem##sys"), first)) {
        S.apkSystem = !S.apkSystem;
        act(std::string("APK|SYS|") + (S.apkSystem ? "1" : "0"));
    }
    if (wrapButton(n == (int)S.apps.size() && n > 0 ? T("Batal pilih##apk") : T("Pilih semua##apk"), first))
        for (auto& a : S.apps) a.sel = !(n == (int)S.apps.size() && n > 0);
    if (wrapButton(T("Segarkan##apk"), first)) act("APK|REF");
    ImGui::EndDisabled();
    endPanel();
    ImGui::PopStyleVar(2);

    const bool prog = S.aBusy || S.aTotal > 0;
    float footer = (prog ? 235.f : 120.f) * d;
    float h = ImGui::GetContentRegionAvail().y - footer;
    if (h < 150.f * d) h = 150.f * d;
    ImGui::BeginChild("apk_list", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    if (S.apps.empty()) ImGui::TextColored(g_pal.dim, "%s", T("Tidak ada aplikasi."));
    ImGuiListClipper clip;
    clip.Begin((int)S.apps.size());
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            Item& it = S.apps[i];
            if (appRow(i, it, 58.f * d)) it.sel = !it.sel;
        }
    }
    touchScroll();
    ImGui::EndChild();

    ImGui::TextColored(g_pal.dim, T("%d aplikasi  |  %d dipilih (%s)"), (int)S.apps.size(), n, fmtSize((double)sizeSel(S.apps)).c_str());
    if (!S.aBusy) {
        ImGui::BeginDisabled(!S.shizuku || n == 0);
        char lb[96];
        snprintf(lb, sizeof lb, S.apkFmt == 0 ? T("BACKUP %d APLIKASI KE XAPK") : T("BACKUP %d APLIKASI"), n);
        std::string l = lb;
        ImGui::PushStyleColor(ImGuiCol_Button, g_pal.accent);
        ImGui::PushStyleColor(ImGuiCol_Text, g_pal.onAccent);
        bool go = Btn(l.c_str(), ImVec2(-FLT_MIN, 54.f * d));
        ImGui::PopStyleColor(2);
        if (go) {
            act("APK_GO|" + std::to_string(S.apkFmt) + "|" + std::to_string((S.apkObb ? 1 : 0) | (S.apkData ? 2 : 0)) + "|" + idxSel(S.apps));
            S.aBusy = true; S.aDone = 0; S.aTotal = 0; S.aSpeed = 0; S.aCur = "Menyiapkan...";
        }
        ImGui::EndDisabled();
    } else {
        if (Btn(T("BATALKAN##apk"), ImVec2(-FLT_MIN, 54.f * d))) act("CANCEL");
    }
    progressBlock(S.aBusy, S.aDone, S.aTotal, S.aSpeed, S.aCur);
}

// ------------------------------------------------------------------ Wi-Fi
void drawNet() {
    const float d = g_density;
    const ImVec4 ok = g_pal.ok, warn = g_pal.warn, acc = g_pal.accent;

    {
        std::string shown, line;
        for (size_t i = 0; i <= S.permMsg.size(); i++) {
            if (i == S.permMsg.size() || S.permMsg[i] == '\n') { if (!shown.empty()) shown += '\n'; shown += i18n::tr(line); line.clear(); }
            else line += S.permMsg[i];
        }
        ImGui::TextColored(S.permOk ? ok : warn, "%s", shown.c_str());
    }
    if (!S.permOk) {
        if (Btn(T("Berikan izin / periksa ulang"), ImVec2(-FLT_MIN, 52.f * d))) act("NET_PERM");
    }
    if (!S.shizuku) ImGui::TextDisabled("%s", T("Shizuku harus terhubung dulu."));
    ImGui::BeginDisabled(!(S.permOk && S.shizuku));

    // ---- Terima (receiver)
    Heading(T("Terima"));
    if (toggleSwitch("##rx", S.listening)) act(S.listening ? "RX_OFF" : "RX_ON");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(S.listening ? ok : g_pal.dim, "%s", S.listening ? T("Terima AKTIF - terlihat oleh perangkat lain") : T("Terima MATI"));
    ImGui::TextWrapped(T("Folder simpan: %s"), S.recvDir.empty() ? T("(belum dipilih)") : S.recvDir.c_str());
    ImGui::BeginDisabled(S.listening);
    if (Btn(T("Ubah folder simpan"))) act("BPURPOSE|2");
    ImGui::EndDisabled();
    if (toggleSwitch("##bro", S.allowBrowse)) {
        S.allowBrowse = !S.allowBrowse;
        act(std::string("RX_BROWSE|") + (S.allowBrowse ? "1" : "0"));
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped("%s", T("Izinkan jelajah FTP ke /sdcard"));
    if (S.listening) {
        ImGui::Text(T("Alamat: %s"), S.addr.c_str());
        ImGui::Text("PIN:");
        ImGui::SameLine();
        ImGui::SetWindowFontScale(2.2f);
        ImGui::TextColored(warn, "%s", S.pin.c_str());
        ImGui::SetWindowFontScale(1.f);
        // QR pairing: dipindai aplikasi kamera/pemindai apa pun, lalu membuka Ngirim di perangkat pengirim
        static bool showQr = false;
        if (Btn(showQr ? T("Sembunyikan QR##qr") : T("Tampilkan QR##qr"))) showQr = !showQr;
        if (showQr) {
            const size_t c = S.addr.rfind(':');
            if (c != std::string::npos) {
                std::string url = "ngirim://pair?h=" + S.addr.substr(0, c) + "&p=" + S.addr.substr(c + 1) +
                                  "&pin=" + S.pin + "&n=" + urlEncode(S.devName);
                static std::string cachedUrl;
                static std::vector<std::vector<bool>> cachedQr;
                if (url != cachedUrl) { cachedUrl = url; cachedQr = qr::encode(url); }
                const int n = (int)cachedQr.size();
                if (n > 0) {
                    const int quiet = 3;
                    const float avail = ImGui::GetContentRegionAvail().x;
                    float cell = std::floor(std::min(avail, 260.f * d) / (float)(n + 2 * quiet));
                    if (cell < 2.f) cell = 2.f;
                    const float side = cell * (float)(n + 2 * quiet);
                    ImVec2 q0 = ImGui::GetCursorScreenPos();
                    ImGui::Dummy(ImVec2(side, side));
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(q0, ImVec2(q0.x + side, q0.y + side), IM_COL32(255, 255, 255, 255));
                    for (int y = 0; y < n; y++)
                        for (int x = 0; x < n; x++)
                            if (cachedQr[y][x]) {
                                ImVec2 p0(q0.x + (x + quiet) * cell, q0.y + (y + quiet) * cell);
                                dl->AddRectFilled(p0, ImVec2(p0.x + cell, p0.y + cell), IM_COL32(0, 0, 0, 255));
                            }
                } else {
                    ImGui::TextDisabled("%s", T("QR tidak bisa dibuat."));
                }
                ImGui::PushStyleColor(ImGuiCol_Text, g_pal.dim);
                ImGui::TextWrapped("%s", T("Pindai dengan kamera di perangkat pengirim; Ngirim terbuka dan memasangkan otomatis."));
                ImGui::PopStyleColor();
            }
        }
    }

    // ---- Perangkat aktif
    Heading(T("Perangkat aktif"));
    if (Btn(T("Segarkan"))) act("NET_SCAN");
    ImGui::SameLine();
    if (Btn(T("IP manual"))) act("NET_MANUAL");
    ImGui::SameLine();
    if (Btn(T("Pindai QR"))) act("SCAN_QR");
    if (S.peers.empty()) ImGui::TextDisabled("%s", T("Belum ada. Nyalakan Terima di perangkat lain (satu Wi-Fi)."));
    else { ImGui::PushStyleColor(ImGuiCol_Text, g_pal.dim); ImGui::TextWrapped("%s", T("Ketuk perangkat, lalu masukkan PIN dari layarnya.")); ImGui::PopStyleColor(); }
    for (int i = 0; i < (int)S.peers.size(); i++) {
        ImVec2 q = ImGui::GetCursorScreenPos();
        std::string l = "##p" + std::to_string(i);
        bool clicked = ImGui::Selectable(l.c_str(), false, 0, ImVec2(0, 52.f * d));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(q.x + 14.f * d, q.y + 26.f * d), 7.f * d, ImGui::ColorConvertFloat4ToU32(ok));
        dl->AddText(ImVec2(q.x + 34.f * d, q.y + (52.f * d - ImGui::GetFontSize()) * 0.5f), g_pal.text, S.peers[i].c_str());
        if (clicked) act("NET_PAIR|" + std::to_string(i));
    }

    // ---- Terhubung
    if (!S.pairedName.empty()) {
        Heading(T("Terhubung"));
        ImGui::TextColored(ok, "%s", S.pairedName.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(S.netBusy);
        if (Btn(T("Putus"))) act("NET_DISCONNECT");
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", T("Jelajah dan salin dua arah ada di tab FTP."));
        ImGui::BeginDisabled(S.netBusy);
        if (Btn("+ File / folder")) act("BPURPOSE|1");
        ImGui::SameLine();
        if (Btn(T("Kosongkan"))) { S.sendList.clear(); act("NET_CLEAR"); }
        ImGui::EndDisabled();
        if (S.sendList.empty()) ImGui::TextDisabled("%s", T("Belum ada file/folder dipilih."));
        const int kShow = 6;
        for (int i = 0; i < (int)S.sendList.size() && i < kShow; i++) ImGui::TextWrapped("%s", S.sendList[i].name.c_str());
        if ((int)S.sendList.size() > kShow) ImGui::TextDisabled(T("... dan %d lainnya"), (int)S.sendList.size() - kShow);
        if (!S.netBusy) {
            ImGui::BeginDisabled(S.sendList.empty());
            ImGui::PushStyleColor(ImGuiCol_Button, acc);
            ImGui::PushStyleColor(ImGuiCol_Text, g_pal.onAccent);
            if (Btn(T("KIRIM SEKARANG"), ImVec2(-FLT_MIN, 56.f * d))) {
                act("NET_SEND");
                S.netBusy = true; S.netDone = 0; S.netTotal = 0; S.netSpeed = 0; S.netCur = "Menyiapkan...";
            }
            ImGui::PopStyleColor(2);
            ImGui::EndDisabled();
        } else {
            if (Btn(T("BATALKAN"), ImVec2(-FLT_MIN, 56.f * d))) act("NET_CANCEL");
        }
    }
    ImGui::EndDisabled();

    progressBlock(S.netBusy, S.netDone, S.netTotal, S.netSpeed, S.netCur);

    // log terakhir (selengkapnya di tab Log)
    Heading(T("Log terakhir"));
    if (g_mono) ImGui::PushFont(g_mono);
    size_t from = S.logs.size() > 5 ? S.logs.size() - 5 : 0;
    for (size_t i = from; i < S.logs.size(); i++) ImGui::TextWrapped("%.11s%s", S.logs[i].c_str(), i18n::tr(S.logs[i].size() > 11 ? S.logs[i].c_str() + 11 : "").c_str());
    if (g_mono) ImGui::PopFont();
}

// ------------------------------------------------------------------ FTP (dua panel)
void drawPane(int i, float w, float h) {
    const float d = g_density;
    Pane& P = S.pane[i];
    const bool isActive = S.active == i;
    ImGui::PushStyleColor(ImGuiCol_Border, isActive ? g_pal.accent : ImGui::ColorConvertU32ToFloat4(g_pal.border));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, isActive ? 2.f * d : 1.f);
    std::string id = "pane" + std::to_string(i);
    ImGui::BeginChild(id.c_str(), ImVec2(w, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(0)) S.active = i;

    if (i == 0) {
        ImGui::TextColored(g_pal.accent, "%s", T("HP ini"));
    } else {
        bool first = true;
        if (wrapButton(T("HP ini##src0"), first, !P.remote)) act("FP|1|SRC|0");
        ImGui::BeginDisabled(S.pairedName.empty());
        std::string rl = S.pairedName.empty() ? T("Perangkat lain") : tailEllipsis(S.pairedName, 14);
        if (wrapButton(rl + "##src1", first, P.remote)) act("FP|1|SRC|1");
        ImGui::EndDisabled();
    }
    ImGui::TextColored(g_pal.dim, "%s", tailEllipsis(P.path, 30).c_str());
    if (i == 1 && S.pairedName.empty() && !P.remote) ImGui::TextColored(g_pal.dim, "%s", T("Pairing dulu di tab Wi-Fi."));
    if (!P.err.empty()) ImGui::TextColored(g_pal.err, "%s", P.err.c_str());

    float lh = ImGui::GetContentRegionAvail().y - 4.f * d;
    if (lh < 60.f * d) lh = 60.f * d;
    ImGui::BeginChild((id + "_list").c_str(), ImVec2(0, lh), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    if (P.e.empty() && P.err.empty()) ImGui::TextColored(g_pal.dim, "%s", T("Kosong"));
    ImGuiListClipper clip;
    clip.Begin((int)P.e.size());
    g_ds.begin(&P, 32.f * d);
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            Entry& e = P.e[r];
            const float ry = ImGui::GetCursorScreenPos().y;
            g_ds.row(&P, e.sel, ry, ry + 50.f * d + ImGui::GetStyle().ItemSpacing.y);
            int c = fileRow(r, e, 50.f * d, true, false, true);
            if (c == 2) e.sel = !e.sel;
            else if (c == 1) {
                if (e.dir) act("FP|" + std::to_string(i) + "|OPEN|" + std::to_string(r));
                else e.sel = !e.sel;
            }
        }
    }
    g_ds.end(&P);
    if (!g_ds.blocksScroll(&P)) touchScroll();
    ImGui::EndChild();
    ImGui::EndChild();
}

void drawFtp() {
    const float d = g_density;
    Pane& A = S.pane[S.active];
    const std::string ai = std::to_string(S.active);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.f * d, 4.f * d));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f * d, 6.f * d));
    beginPanel("ftp_head");
    ImGui::TextColored(g_pal.accent, T("Panel aktif: %s"), S.active == 0 ? T("kiri") : T("kanan"));
    quickAccess("FP|" + ai, A.path, false);
    bool isBm = false;
    for (auto& b : S.bookmarks) if (b == A.path) isBm = true;
    bool first = true;
    if (wrapButton(T("Naik"), first)) act("FP|" + ai + "|UP");
    if (wrapButton(T("Segarkan"), first)) act("FP|" + ai + "|REF");
    ImGui::BeginDisabled(A.remote);
    if (wrapButton(T("Folder baru"), first)) act("FP_MKDIR|" + ai);
    ImGui::EndDisabled();
    if (wrapButton(isBm ? T("Lepas sematan") : T("Sematkan folder"), first)) act("BM|" + A.path);
    endPanel();
    ImGui::PopStyleVar(2);

    const bool remote = S.pane[1].remote;
    if (remote) S.fMove = 0;  // jelajah jarak jauh: salin saja

    const float footer = (S.fBusy || S.fTotal > 0 ? 235.f : 150.f) * d;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float h = avail.y - footer;
    if (h < 150.f * d) h = 150.f * d;
    float w = (avail.x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    drawPane(0, w, h);
    ImGui::SameLine();
    drawPane(1, w, h);

    const int nL = countSel(S.pane[0].e), nR = countSel(S.pane[1].e);
    ImGui::BeginDisabled(S.fBusy);
    ImGui::RadioButton(T("Salin"), &S.fMove, 0);
    ImGui::SameLine();
    ImGui::BeginDisabled(remote);
    ImGui::RadioButton(T("Pindah"), &S.fMove, 1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(g_pal.dim, T("Dipilih: kiri %d, kanan %d"), nL, nR);
    ImGui::EndDisabled();

    if (!S.fBusy) {
        const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::BeginDisabled(!S.shizuku || nL == 0);
        if (Btn(T("Kiri  >>  Kanan"), ImVec2(bw, 50.f * d))) {
            act("FP_XFER|0|" + std::to_string(S.fMove) + "|" + idxSel(S.pane[0].e));
            S.fBusy = true; S.fDone = 0; S.fTotal = 0; S.fSpeed = 0; S.fCur = "Menyiapkan...";
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!S.shizuku || nR == 0);
        if (Btn(T("Kiri  <<  Kanan"), ImVec2(-FLT_MIN, 50.f * d))) {
            act("FP_XFER|1|" + std::to_string(S.fMove) + "|" + idxSel(S.pane[1].e));
            S.fBusy = true; S.fDone = 0; S.fTotal = 0; S.fSpeed = 0; S.fCur = "Menyiapkan...";
        }
        ImGui::EndDisabled();
    } else {
        if (Btn(T("BATALKAN##ftp"), ImVec2(-FLT_MIN, 54.f * d))) act("CANCEL");
    }
    progressBlock(S.fBusy, S.fDone, S.fTotal, S.fSpeed, S.fCur);
}

// ------------------------------------------------------------------ Setting
void settingToggle(const char* id, const char* label, const char* sub, bool on, const std::string& key) {
    const float d = g_density;
    ImVec2 q = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::BeginGroup();
    bool clicked = toggleSwitch(id, on);
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", label);
    if (sub && *sub) dimWrapped(sub);
    ImGui::EndGroup();
    (void)q; (void)w; (void)d;
    if (clicked) act("SET|" + key + "|" + (on ? "0" : "1"));
}

void drawSettings() {
    const float d = g_density;

    Heading(T("Tampilan"));
    {
        bool first = true;
        if (wrapButton(T("Terang##th"), first, !S.dark)) { S.dark = false; applyTheme(false); act("SET|theme|0"); }
        if (wrapButton(T("Gelap##th"), first, S.dark)) { S.dark = true; applyTheme(true); act("SET|theme|1"); }
    }

    Heading(T("Bahasa"));
    {
        bool first = true;
        if (wrapButton(std::string(T("Otomatis")) + "##lg", first, S.langPref == "auto")) { S.langPref = "auto"; i18n::setLanguage("auto", S.sysLang); act("LANG|auto"); }
        int n = 0;
        const i18n::LangInfo* L = i18n::languages(n);
        for (int i = 0; i < n; i++) {
            const std::string code = L[i].code;
            if (wrapButton(std::string(L[i].name) + "##lg" + code, first, S.langPref == code)) { S.langPref = code; i18n::setLanguage(code, S.sysLang); act("LANG|" + code); }
        }
        if (S.langPref == "auto") dimWrapped(T("Otomatis: ikuti bahasa sistem, selain Indonesia memakai Inggris."));
    }

    Heading("File manager");
    settingToggle("##hid", T("Tampilkan file tersembunyi"), T("Nama berawalan titik (.)"), S.showHidden, "hidden");
    ImGui::Spacing();
    settingToggle("##trash", T("Gunakan tempat sampah"), T("Hapus memindahkan ke /sdcard/Ngirim/.Sampah (Android/data dan OBB dihapus permanen)"), S.useTrash, "trash");
    if (Btn(T("Buka tempat sampah"))) act("TRASH_OPEN");
    ImGui::SameLine();
    if (Btn(T("Kosongkan sampah"))) act("TRASH_EMPTY");

    Heading(T("Pasang APK"));
    {
        bool first = true;
        if (wrapButton(T("Installer sistem##inst"), first, !S.instShizuku)) { S.instShizuku = false; act("SET|inst|0"); }
        if (wrapButton(T("Shizuku (senyap)##inst"), first, S.instShizuku)) { S.instShizuku = true; act("SET|inst|1"); }
        dimWrapped(S.instShizuku ? T("Dipasang tanpa dialog lewat Shizuku (perlu Shizuku terhubung).")
                                 : T("Memakai installer Android; perlu izin 'pasang aplikasi tak dikenal'."));
    }

    Heading(T("Pembaruan"));
    ImGui::Text(T("Versi terpasang: %s"), kVersion);
    settingToggle("##autoupd", T("Cek pembaruan saat dibuka"), T("Mengambil rilis terbaru dari GitHub"), S.autoUpdate, "autoupd");
    if (Btn(T("Cek pembaruan"))) act("UPD_CHECK");
    if (!S.updMsg.empty()) dimWrapped(i18n::tr(S.updMsg).c_str());
    dimWrapped(T("Pembaruan dipasang lewat Shizuku bila terhubung, kalau tidak lewat installer sistem."));

    Heading("Backup APK");
    ImGui::TextWrapped(T("Folder hasil: %s"), S.xapkDir.c_str());
    if (Btn(T("Ubah folder hasil"))) act("BPURPOSE|5");

    Heading(T("Jaringan"));
    ImGui::TextWrapped(T("Nama perangkat: %s"), S.devName.c_str());
    if (Btn(T("Ubah nama perangkat"))) act("SET_NAME");
    dimWrapped(T("Nama baru dipakai saat Terima dinyalakan ulang."));

    Heading("Shizuku");
    ImGui::TextColored(S.shizuku ? g_pal.ok : g_pal.err, "%s", i18n::tr(S.shizukuMsg).c_str());
    {
        bool first = true;
        if (S.shizukuFailed && wrapButton("Shizuku Legacy", first, true)) act("LEGACY");
        if (wrapButton(T("Hubungkan Shizuku"), first)) act("CONNECT");
        if (wrapButton(T("Buka app Shizuku"), first)) act("OPEN_SHIZUKU");
        if (S.shizukuFailed) dimWrapped(T("Legacy: tanpa UserService, perintah dijalankan lewat Shizuku.Process."));
    }

    Heading(T("Tentang"));
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextColored(g_pal.accent, "%s", T("Ngirim: Berbagi aplikasi dengan mudah"));
    ImGui::SetWindowFontScale(1.f);
    ImGui::TextWrapped("Sender, FTP & File manager");
    dimWrapped(T("Kirim lewat Wi-Fi, kelola dua folder sekaligus, dan backup aplikasi ke XAPK, semuanya dalam satu app."));
    ImGui::Spacing();
    ImGui::Text(T("Versi %s"), kVersion);
    dimWrapped(T("Dibuat oleh mnrdnn. Dibangun dengan Dear ImGui, Shizuku API, dan Kotlin."));
    dimWrapped("github.com/UdyneO2/Ngirim");
    
    if (ImGui::TreeNode(T("Lisensi"))) {
        ImGui::SetWindowFontScale(0.92f);
        ImGui::TextWrapped("Ngirim - MIT License");
        dimWrapped("Copyright (c) 2026 mnrdnn");
        dimWrapped(T("Boleh dipakai, diubah, dan dibagikan selama pemberitahuan hak cipta ini disertakan. Tanpa jaminan."));
        ImGui::Spacing();
        ImGui::TextWrapped("Dear ImGui - MIT License (Omar Cornut)");
        ImGui::TextWrapped("Shizuku, Shizuku-API - Apache License 2.0 (RikkaApps)");
        ImGui::TextWrapped("ZXing, zxing-android-embedded - Apache License 2.0");
        ImGui::TextWrapped("Source Sans 3 (Adobe) - SIL Open Font License 1.1");
        dimWrapped(T("Teks lengkap ada di berkas LICENSE di repositori."));
        ImGui::SetWindowFontScale(1.f);
        ImGui::TreePop();
    }

    ImGui::Spacing();
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f * d, 10.f * d));
        // Menggunakan TreeNode
        const bool open = ImGui::TreeNode(
            (std::string(T("Riwayat perubahan")) + "  (v0.1 beta - " + kRilis.back().ver + ")###rl").c_str());
        ImGui::PopStyleVar();
        
        if (open) {
            ImGui::SetWindowFontScale(0.92f);
            for (const Rilis& r : kRilis) {
                ImGui::TextColored(g_pal.accent, "%s  -  %s", r.ver, T(r.judul));
                if (&r == &kRilis.back()) {
                    ImGui::SameLine();
                    const char* bt = T("Baru");
                    const ImVec2 ts = ImGui::CalcTextSize(bt), p = ImGui::GetCursorScreenPos();
                    const float px = 6.f * d;
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + ts.x + px * 2, p.y + ts.y), ImGui::ColorConvertFloat4ToU32(g_pal.accent), ts.y * 0.5f);
                    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + px, p.y), IM_COL32(255, 255, 255, 255), bt);
                    ImGui::Dummy(ImVec2(ts.x + px * 2, ts.y));
                }
                ImGui::TextColored(g_pal.dim, "%s", T(r.info));
                for (const char* it : r.butir) ImGui::TextWrapped("+ %s", T(it));
                ImGui::Spacing();
            }
            ImGui::TextColored(g_pal.warn, "%s", T("Batasan yang diketahui"));
            for (const char* it : kBatasan) ImGui::TextWrapped("- %s", T(it));
            
            ImGui::SetWindowFontScale(1.f); // Skala dikembalikan ke normal
            ImGui::TreePop(); // Pop untuk TreeNode Riwayat Perubahan
        }
    }
    ImGui::Dummy(ImVec2(0, 24.f * d));
}


// ------------------------------------------------------------------ file browser (pilih file/folder)
void drawBrowser() {
    const float d = g_density;
    const int P = S.bPurpose;
    const char* title = P == 1 ? T("Pilih file / folder untuk dikirim")
                      : P == 2 ? T("Pilih folder penyimpanan terima")
                      : T("Pilih folder hasil backup XAPK");
    ImGui::TextColored(g_pal.accent, "%s", title);
    ImGui::TextWrapped("%s", S.bPath.c_str());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.f * d, 4.f * d));
    bool first = true;
    if (wrapButton(T("Naik"), first)) act("BUP");
    if (wrapButton(T("Folder baru"), first)) act("BNEWDIR");
    if (wrapButton(T("Tutup"), first)) act("BCLOSE");
    if (wrapButton("Internal", first)) act("BGO|/sdcard");
    if (wrapButton("Download", first)) act("BGO|/sdcard/Download");
    if (wrapButton("Data", first)) act("BGO|/sdcard/Android/data");
    if (wrapButton("OBB", first)) act("BGO|/sdcard/Android/obb");
    ImGui::PopStyleVar();

    int n = countSel(S.bEntries);
    float h = ImGui::GetContentRegionAvail().y - 75.f * d;
    if (h < 100.f * d) h = 100.f * d;
    ImGui::BeginChild("blist", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    if (S.bEntries.empty()) ImGui::TextColored(g_pal.dim, "%s", T("Folder kosong / tidak bisa dibaca."));
    ImGuiListClipper clip;
    clip.Begin((int)S.bEntries.size());
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            Entry& e = S.bEntries[i];
            int r = fileRow(i, e, 50.f * d, P == 1, false);
            if (r == 2) e.sel = !e.sel;
            else if (r == 1) {
                if (e.dir) act("BOPEN|" + std::to_string(i));
                else if (P == 1) e.sel = !e.sel;
            }
        }
    }
    touchScroll();
    ImGui::EndChild();

    if (P == 1) {
        char lb[64];
        snprintf(lb, sizeof lb, T("Tambahkan %d item"), n);
        std::string l = lb;
        ImGui::BeginDisabled(n == 0);
        if (Btn(l.c_str(), ImVec2(-FLT_MIN, 56.f * d))) act("BFILES|" + idxSel(S.bEntries));
        ImGui::EndDisabled();
    } else {
        if (Btn(T("PILIH FOLDER INI"), ImVec2(-FLT_MIN, 56.f * d))) act("BDIR");
    }
}

// ------------------------------------------------------------------ root
// ------------------------------------------------------------------ pratinjau gambar
void drawPreview() {
    const float d = g_density;
    ImGuiIO& io = ImGui::GetIO();
    if (S.pvTexDirty && !S.pvPix.empty()) {
        if (S.pvTex) { GLuint o = S.pvTex; glDeleteTextures(1, &o); }
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, S.pvW, S.pvH, 0, GL_RGBA, GL_UNSIGNED_BYTE, S.pvPix.data());
        S.pvTex = t;
        S.pvTexDirty = false;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.f * d, 4.f * d));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f * d, 6.f * d));
    bool first = true;
    if (wrapButton(T("Tutup##pv"), first, true)) { S.pvOpen = false; act("PV|CLOSE"); ImGui::PopStyleVar(2); return; }
    ImGui::BeginDisabled(S.pvCnt < 2);
    if (wrapButton("<##pv", first)) act("PV|PREV");
    if (wrapButton(">##pv", first)) act("PV|NEXT");
    ImGui::EndDisabled();
    if (wrapButton("+##pv", first)) S.pvZoom = std::min(8.f, S.pvZoom * 1.5f);
    if (wrapButton("-##pv", first)) { S.pvZoom = std::max(1.f, S.pvZoom / 1.5f); if (S.pvZoom < 1.01f) { S.pvPanX = S.pvPanY = 0.f; } }
    if (wrapButton(T("Pas##pv"), first)) { S.pvZoom = 1.f; S.pvPanX = S.pvPanY = 0.f; }
    ImGui::PopStyleVar(2);

    char info[256];
    snprintf(info, sizeof info, "%d / %d   %s%s", S.pvIdx + 1, S.pvCnt, S.pvName.c_str(), S.pvW > 0 ? ("   " + std::to_string(S.pvW) + "x" + std::to_string(S.pvH)).c_str() : "");
    ImGui::TextColored(g_pal.dim, "%s", info);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("pv_area", ImVec2(0, avail.y - 4.f * d), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 sz = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##pvhit", sz);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), IM_COL32(18, 18, 18, 255));
    static float lastX = 0.f, lastY = 0.f;
    if (ImGui::IsItemActive()) {
        lastX = io.MousePos.x; lastY = io.MousePos.y;
        if (S.pvZoom > 1.01f && ImGui::IsMouseDragging(0, 4.f * d)) { S.pvPanX += io.MouseDelta.x; S.pvPanY += io.MouseDelta.y; }
    }
    if (S.pvTex && S.pvW > 0 && S.pvH > 0) {
        const float fit = std::min(sz.x / (float)S.pvW, sz.y / (float)S.pvH);
        const float iw = (float)S.pvW * fit * S.pvZoom, ih = (float)S.pvH * fit * S.pvZoom;
        const float mx = std::max(0.f, (iw - sz.x) * 0.5f), my = std::max(0.f, (ih - sz.y) * 0.5f);
        S.pvPanX = std::max(-mx, std::min(mx, S.pvPanX));
        S.pvPanY = std::max(-my, std::min(my, S.pvPanY));
        const ImVec2 c(p0.x + sz.x * 0.5f + S.pvPanX, p0.y + sz.y * 0.5f + S.pvPanY);
        dl->PushClipRect(p0, ImVec2(p0.x + sz.x, p0.y + sz.y), true);
        dl->AddImage((ImTextureID)(intptr_t)S.pvTex, ImVec2(c.x - iw * 0.5f, c.y - ih * 0.5f), ImVec2(c.x + iw * 0.5f, c.y + ih * 0.5f));
        dl->PopClipRect();
    }
    if (!S.pvMsg.empty()) {
        const std::string m = i18n::tr(S.pvMsg);
        const ImVec2 ts = ImGui::CalcTextSize(m.c_str());
        dl->AddText(ImVec2(p0.x + (sz.x - ts.x) * 0.5f, p0.y + (sz.y - ts.y) * 0.5f), IM_COL32(220, 220, 220, 255), m.c_str());
    }
    // geser horizontal saat tidak di-zoom = gambar sebelumnya/berikutnya
    if (ImGui::IsItemDeactivated() && S.pvZoom <= 1.01f && S.pvCnt > 1) {
        const float dx = lastX - io.MouseClickedPos[0].x, dy = lastY - io.MouseClickedPos[0].y;
        if (fabsf(dx) > 90.f * d && fabsf(dx) > fabsf(dy) * 1.5f) act(dx < 0 ? "PV|NEXT" : "PV|PREV");
    }
    ImGui::EndChild();
}

// ------------------------------------------------------------------ ikon garis (gaya Lucide, grid 24)
enum Icon { IC_FOLDER, IC_WIFI, IC_PHONE, IC_GEAR, IC_LOG };

void drawIcon(ImDrawList* dl, Icon ic, ImVec2 c, float size, ImU32 col) {
    const float u = size / 24.f, th = std::max(1.4f, 1.7f * u);
    const ImVec2 o(c.x - size * 0.5f, c.y - size * 0.5f);
    auto P = [&](float x, float y) { return ImVec2(o.x + x * u, o.y + y * u); };
    switch (ic) {
        case IC_FOLDER: {
            dl->AddRect(P(3, 7.5f), P(21, 19.5f), col, 2.f * u, 0, th);
            ImVec2 t[5] = {P(3, 7.5f), P(3, 5.5f), P(4.5f, 4.5f), P(9, 4.5f), P(11, 7.5f)};
            dl->AddPolyline(t, 5, col, 0, th);
            break;
        }
        case IC_WIFI: {
            const ImVec2 ctr = P(12, 19.f);
            for (float r : {4.5f, 9.f, 13.5f}) {
                dl->PathArcTo(ctr, r * u, -2.356f, -0.785f, 16);
                dl->PathStroke(col, 0, th);
            }
            dl->AddCircleFilled(ctr, 1.4f * u, col);
            break;
        }
        case IC_PHONE:
            dl->AddRect(P(7, 2.5f), P(17, 21.5f), col, 2.5f * u, 0, th);
            dl->AddCircleFilled(P(12, 18.3f), 1.1f * u, col);
            break;
        case IC_GEAR: {
            dl->AddCircle(P(12, 12), 3.2f * u, col, 20, th);
            dl->AddCircle(P(12, 12), 7.f * u, col, 24, th);
            for (int i = 0; i < 8; i++) {
                const float ang = i * 0.7854f;
                dl->AddLine(P(12 + 7.f * cosf(ang), 12 + 7.f * sinf(ang)), P(12 + 10.2f * cosf(ang), 12 + 10.2f * sinf(ang)), col, th * 1.2f);
            }
            break;
        }
        case IC_LOG:
            dl->AddRect(P(5, 3), P(19, 21), col, 2.f * u, 0, th);
            dl->AddLine(P(8.5f, 8), P(15.5f, 8), col, th);
            dl->AddLine(P(8.5f, 12), P(15.5f, 12), col, th);
            dl->AddLine(P(8.5f, 16), P(13, 16), col, th);
            break;
    }
}

// Bilah tab datar: ikon + label, garis bawah aksen pada tab aktif. Bisa digeser mendatar bila tak muat.
void drawTabBar(float y, float h) {
    const float d = g_density;
    const ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x;
    struct Tab { const char* label; Icon icon; };
    const Tab tabs[] = {{T("Lokal"), IC_FOLDER}, {"Wi-Fi", IC_WIFI}, {"FTP", IC_PHONE}, {T("Setting"), IC_GEAR}, {"Log", IC_LOG}};
    const int n = 5;
    const float padX = 15.f * d, icon = 18.f * d, gap = 8.f * d;
    float widths[5], total = 0;
    if (g_bold) ImGui::PushFont(g_bold);
    for (int i = 0; i < n; i++) { widths[i] = padX * 2 + icon + gap + ImGui::CalcTextSize(tabs[i].label).x; total += widths[i]; }
    if (g_bold) ImGui::PopFont();

    static float scroll = 0.f;
    static bool dragStart = false;
    const float maxScroll = std::max(0.f, total - W);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 base(0, y);
    dl->AddRectFilled(base, ImVec2(W, y + h), g_pal.surfaceU);
    dl->AddLine(ImVec2(0, y + h - 0.5f), ImVec2(W, y + h - 0.5f), g_pal.border);

    const bool inStrip = io.MousePos.y >= y && io.MousePos.y < y + h;
    if (ImGui::IsMouseClicked(0)) dragStart = inStrip;
    if (!io.MouseDown[0]) dragStart = false;
    if (dragStart && ImGui::IsMouseDragging(0, 8.f * d)) {
        scroll -= io.MouseDelta.x;
        ImGui::ClearActiveID();
    }
    scroll = std::max(0.f, std::min(scroll, maxScroll));

    dl->PushClipRect(base, ImVec2(W, y + h), true);
    float x = (total < W) ? (W - total) * 0.5f;
    for (int i = 0; i < n; i++) {
        const bool on = S.curTab == i;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(widths[i], h)) && !(dragStart && ImGui::IsMouseDragging(0, 8.f * d)) &&
            fabsf(io.MouseClickedPos[0].x - io.MousePos.x) < 8.f * d) {
            S.tabReq = i;
        }
        ImGui::PopID();
        const ImU32 col = on ? g_pal.text : ImGui::ColorConvertFloat4ToU32(g_pal.dim);
        drawIcon(dl, tabs[i].icon, ImVec2(x + padX + icon * 0.5f, y + h * 0.5f), icon, col);
        const float fs = ImGui::GetFontSize();
        ImFont* f = g_bold ? g_bold : ImGui::GetFont();
        dl->AddText(f, fs, ImVec2(x + padX + icon + gap, y + (h - fs) * 0.5f), col, tabs[i].label);
        if (on) dl->AddRectFilled(ImVec2(x, y + h - 2.5f * d), ImVec2(x + widths[i], y + h), ImGui::ColorConvertFloat4ToU32(g_pal.accent));
        x += widths[i];
    }
    dl->PopClipRect();
}

void drawFooter(float y, float h) {
    const float d = g_density;
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, y), ImVec2(io.DisplaySize.x, y + h), g_pal.surfaceU);
    dl->AddLine(ImVec2(0, y + 0.5f), ImVec2(io.DisplaySize.x, y + 0.5f), g_pal.border);
    std::string label;
    if (S.curTab == 0) {
        const int n = countSel(S.fm);
        if (n > 0) {
            char b[96];
            snprintf(b, sizeof b, T("%d dipilih (%s)"), n, fmtSize((double)sizeSel(S.fm)).c_str());
            label = b;
        } else label = S.fmPath;
        if (S.clipCount) label += std::string("  |  ") + (S.clipCut ? T("Clipboard: potong") : T("Clipboard: salin"));
    } else if (S.curTab == 1) {
        label = std::string(T("Terima")) + " " + (S.listening ? T("nyala") : T("mati"));
    } else label = S.shizuku ? T("Shizuku terhubung") : T("Shizuku terputus");
    const float fs = ImGui::GetFontSize() * 0.92f;
    const ImVec4 clip(12.f * d, y, io.DisplaySize.x - 12.f * d, y + h);
    dl->AddText(ImGui::GetFont(), fs, ImVec2(12.f * d, y + (h - fs) * 0.5f), ImGui::ColorConvertFloat4ToU32(g_pal.dim), label.c_str(),
                nullptr, 0.f, &clip);
}

void drawUI() {
    const float d = g_density;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f * d, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);

    // bilah judul: biru (gelap pada tema gelap), nama + versi di kiri, status Shizuku di kanan
    const float barH = 48.f * d;
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(io.DisplaySize.x, barH), ImGui::ColorConvertFloat4ToU32(g_pal.title));
        const float fs = ImGui::GetFontSize();
        const ImU32 white = IM_COL32(255, 255, 255, 255);
        ImFont* bf = g_bold ? g_bold : ImGui::GetFont();
        const float tsz = fs * 1.1f;
        dl->AddText(bf, tsz, ImVec2(12.f * d, (barH - tsz) * 0.5f), white, "Ngirim");
        const float nx = 12.f * d + bf->CalcTextSizeA(tsz, FLT_MAX, 0.f, "Ngirim").x + 12.f * d;
        char ver[32];
        snprintf(ver, sizeof ver, T("Versi %s"), kVersion[0] == 'v' ? kVersion + 1 : kVersion);
        dl->AddText(ImGui::GetFont(), fs * 0.92f, ImVec2(nx, (barH - fs * 0.92f) * 0.5f), IM_COL32(255, 255, 255, 205), ver);
        const char* st = S.shizuku ? T("Shizuku terhubung") : T("Shizuku terputus");
        const float sw = ImGui::GetFont()->CalcTextSizeA(fs * 0.92f, FLT_MAX, 0.f, st).x;
        dl->AddText(ImGui::GetFont(), fs * 0.92f, ImVec2(io.DisplaySize.x - sw - 12.f * d, (barH - fs * 0.92f) * 0.5f), white, st);
    }
    ImGui::SetCursorPosY(barH + 8.f * d);

    if (!S.shizuku) {
        ImGui::PushStyleColor(ImGuiCol_Text, g_pal.err);
        ImGui::TextWrapped("%s", i18n::tr(S.shizukuMsg).c_str());
        ImGui::PopStyleColor();
        bool first = true;
        if (S.shizukuFailed && wrapButton("Shizuku Legacy##top", first, true)) act("LEGACY");
        if (wrapButton(T("Hubungkan Shizuku##top"), first)) act("CONNECT");
        if (wrapButton(T("Buka app Shizuku##top"), first)) act("OPEN_SHIZUKU");
    }

    if (S.tabReq >= 0) { S.curTab = S.tabReq; S.apkOpen = false; S.tabReq = -1; }
    if (S.pvOpen) {
        drawPreview();
    } else if (S.bOpen) {
        drawBrowser();
    } else if (S.apkOpen) {
        drawApk();
    } else {
        const float tabH = 46.f * d, footH = 32.f * d;
        const float y0 = ImGui::GetCursorScreenPos().y;
        drawTabBar(y0, tabH);
        const float bodyY = y0 + tabH;
        const float bodyH = io.DisplaySize.y - bodyY - footH;
        ImGui::SetCursorScreenPos(ImVec2(0, bodyY));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f * d, 8.f * d));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.f);
        ImGui::BeginChild("##body", ImVec2(io.DisplaySize.x, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(2);
        switch (S.curTab) {
            case 0: drawFiles(); break;
            case 1:
                ImGui::BeginChild("t_net", ImVec2(0, 0), 0, ImGuiWindowFlags_NoScrollbar);
                drawNet();
                touchScroll();
                ImGui::EndChild();
                break;
            case 2: drawFtp(); break;
            case 3:
                ImGui::BeginChild("t_set", ImVec2(0, 0), 0, ImGuiWindowFlags_NoScrollbar);
                drawSettings();
                touchScroll();
                ImGui::EndChild();
                break;
            default:
                if (Btn(T("Bersihkan log"))) S.logs.clear();
                drawTerminal("term_main", ImGui::GetContentRegionAvail().y);
                break;
        }
        ImGui::EndChild();
        drawFooter(io.DisplaySize.y - footH, footH);
    }
    ImGui::End();
}

}  // namespace

namespace {
bool g_themeDirty = false;

std::vector<std::string> jstrings(JNIEnv* e, jobjectArray arr) {
    std::vector<std::string> v;
    jsize n = arr ? e->GetArrayLength(arr) : 0;
    for (jsize i = 0; i < n; i++) {
        jstring s = (jstring)e->GetObjectArrayElement(arr, i);
        v.push_back(js(e, s));
        e->DeleteLocalRef(s);
    }
    return v;
}

std::vector<Entry> jentries(JNIEnv* e, jobjectArray names, jlongArray sizes, jbooleanArray dirs, jlongArray mtimes) {
    std::vector<Entry> v;
    auto n = jstrings(e, names);
    jlong* sz = e->GetLongArrayElements(sizes, nullptr);
    jboolean* dr = e->GetBooleanArrayElements(dirs, nullptr);
    jlong* mt = mtimes ? e->GetLongArrayElements(mtimes, nullptr) : nullptr;
    for (size_t i = 0; i < n.size(); i++) {
        Entry en; en.name = n[i]; en.size = sz[i]; en.dir = dr[i]; en.mtime = mt ? mt[i] : 0;
        v.push_back(std::move(en));
    }
    e->ReleaseLongArrayElements(sizes, sz, JNI_ABORT);
    e->ReleaseBooleanArrayElements(dirs, dr, JNI_ABORT);
    if (mt) e->ReleaseLongArrayElements(mtimes, mt, JNI_ABORT);
    return v;
}

std::vector<Item> jitems(JNIEnv* e, jobjectArray names, jobjectArray subs, jlongArray sizes) {
    std::vector<Item> v;
    auto n = jstrings(e, names);
    auto s2 = subs ? jstrings(e, subs) : std::vector<std::string>();
    jlong* sz = e->GetLongArrayElements(sizes, nullptr);
    for (size_t i = 0; i < n.size(); i++) {
        Item it; it.name = n[i]; it.size = sz[i]; if (i < s2.size()) it.sub = s2[i];
        v.push_back(std::move(it));
    }
    e->ReleaseLongArrayElements(sizes, sz, JNI_ABORT);
    return v;
}
}  // namespace

extern "C" {

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_initGl(JNIEnv*, jclass, jfloat density) {
    g_density = density;
    S.pvTex = 0;                        // konteks GL baru: tekstur lama tak berlaku
    S.pvTexDirty = !S.pvPix.empty();
    if (!g_ctx) {
        IMGUI_CHECKVERSION();
        g_ctx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        ImGui::StyleColorsLight();
        ImGuiStyle& st = ImGui::GetStyle();
        // desain baru: datar, sudut membulat 4dp, tombol berbingkai tipis
        st.WindowRounding = 4; st.FrameRounding = 4; st.ChildRounding = 4;
        st.GrabRounding = 4; st.TabRounding = 4; st.ScrollbarRounding = 4; st.PopupRounding = 4;
        st.FrameBorderSize = 1.f; st.ChildBorderSize = 1.f; st.WindowBorderSize = 0.f; st.TabBorderSize = 0.f;
        st.PopupBorderSize = 1.f;
        st.TabBarOverlineSize = 2.f;
        st.FramePadding = ImVec2(12, 9);
        st.ItemSpacing = ImVec2(8, 8);
        st.ScrollbarSize = 6;
        st.SeparatorTextBorderSize = 1.f;
        st.ScaleAllSizes(density);
        {
            std::lock_guard<std::mutex> lk(S.mu);
            applyTheme(S.dark);
        }

        // Source Sans 3 dari assets (dikirim Kotlin lewat setFonts); cadangan: Roboto sistem
        const float px = 16.f * density;
        ImFontConfig fc;
        fc.FontDataOwnedByAtlas = false;
        fc.OversampleH = 2; fc.OversampleV = 1;
        if (!g_fontReg.empty()) io.Fonts->AddFontFromMemoryTTF(g_fontReg.data(), (int)g_fontReg.size(), px, &fc);
        else {
            const char* font = "/system/fonts/Roboto-Regular.ttf";
            if (access(font, R_OK) == 0) io.Fonts->AddFontFromFileTTF(font, px);
            else { ImFontConfig c; c.SizePixels = px; io.Fonts->AddFontDefault(&c); }
        }
        if (!g_fontBold.empty()) g_bold = io.Fonts->AddFontFromMemoryTTF(g_fontBold.data(), (int)g_fontBold.size(), px, &fc);

        const char* mono = "/system/fonts/DroidSansMono.ttf";
        if (access(mono, R_OK) == 0) g_mono = io.Fonts->AddFontFromFileTTF(mono, 13.f * density);
    } else {
        ImGui_ImplOpenGL3_Shutdown();  // konteks GL dibuat ulang
    }
    ImGui_ImplOpenGL3_Init("#version 300 es");
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_render(JNIEnv*, jclass, jint w, jint h) {
    if (!g_ctx) return;
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime = dt > 0 ? dt : 1.f / 60.f;

    applyTouchEvents();
    {
        std::lock_guard<std::mutex> lk(S.mu);
        if (g_themeDirty) { applyTheme(S.dark); g_themeDirty = false; }
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    {
        std::lock_guard<std::mutex> lk(S.mu);
        drawUI();
    }
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_touch(JNIEnv*, jclass, jint action, jfloat x, jfloat y) {
    if (!g_ctx) return;
    std::lock_guard<std::mutex> lk(g_touchMu);
    if (action == 0) {
        g_touchQ.push_back({3, x, y});  // hover dulu
        g_touchQ.push_back({0, x, y});  // tekan di frame berikutnya
    } else if (action == 1) {
        g_touchQ.push_back({1, x, y});
    } else if (action == 2) {
        g_touchQ.push_back({1, x, y});
        g_touchQ.push_back({2, x, y});
    }
}

// Font dikirim dari Kotlin (assets) sebelum initGl.
JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setFonts(JNIEnv* e, jclass, jbyteArray reg, jbyteArray bold) {
    auto cp = [&](jbyteArray a, std::vector<unsigned char>& out) {
        if (!a) return;
        const jsize n = e->GetArrayLength(a);
        out.resize((size_t)n);
        e->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(out.data()));
    };
    cp(reg, g_fontReg);
    cp(bold, g_fontBold);
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setShizuku(JNIEnv* e, jclass, jboolean ok, jstring msg) {
    std::string m = js(e, msg);
    std::lock_guard<std::mutex> lk(S.mu);
    S.shizuku = ok;
    S.shizukuMsg = m;
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setShizukuFailed(JNIEnv*, jclass, jboolean f) {
    std::lock_guard<std::mutex> lk(S.mu);
    S.shizukuFailed = f;
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_log(JNIEnv* e, jclass, jstring msg) {
    std::string m = js(e, msg);
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char ts[16];
    strftime(ts, sizeof ts, "[%H:%M:%S] ", &tmv);  // 11 karakter
    std::lock_guard<std::mutex> lk(S.mu);
    S.logs.push_back(std::string(ts) + m);
    if (S.logs.size() > 600) S.logs.erase(S.logs.begin(), S.logs.begin() + 100);
    S.logDirty = true;
}

JNIEXPORT jstring JNICALL Java_com_udyneos_ngirim_Native_pollAction(JNIEnv* e, jclass) {
    std::lock_guard<std::mutex> lk(S.mu);
    if (S.actions.empty()) return e->NewStringUTF("");
    std::string a = S.actions.front();
    S.actions.pop_front();
    return e->NewStringUTF(a.c_str());
}

// Tombol Kembali: true bila sudah ditangani di sini (menu konteks, layar tambahan, atau pindah ke tab Lokal).
JNIEXPORT jboolean JNICALL Java_com_udyneos_ngirim_Native_back(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(S.mu);
    if (g_ctxOpen) { g_ctxClose = true; return (jboolean)1; }
    if (S.bOpen) { S.actions.push_back("BCLOSE"); return (jboolean)1; }
    if (S.apkOpen) { S.apkOpen = false; return (jboolean)1; }
    if (S.curTab != 0) { S.tabReq = 0; return (jboolean)1; }
    return (jboolean)0;
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_selectTab(JNIEnv*, jclass, jint idx) {
    std::lock_guard<std::mutex> lk(S.mu);
    S.tabReq = idx;
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setSettings(JNIEnv* e, jclass, jboolean dark, jboolean hidden,
                                                              jboolean trash, jstring xapkDir, jstring devName,
                                                              jboolean instShizuku, jboolean autoUpdate,
                                                              jstring lang, jstring sysLang) {
    std::string x = js(e, xapkDir), n = js(e, devName), lg = js(e, lang), sl = js(e, sysLang);
    std::lock_guard<std::mutex> lk(S.mu);
    if (S.dark != (bool)dark) g_themeDirty = true;
    S.dark = dark; S.showHidden = hidden; S.useTrash = trash; S.xapkDir = x; S.devName = n;
    S.instShizuku = instShizuku; S.autoUpdate = autoUpdate;
    S.langPref = lg; S.sysLang = sl;
    i18n::setLanguage(lg, sl);
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setPreview(JNIEnv* e, jclass, jint w, jint h, jobject buf,
                                                                 jstring name, jint idx, jint cnt, jstring msg) {
    const std::string n = js(e, name), m = js(e, msg);
    unsigned char* src = buf ? (unsigned char*)e->GetDirectBufferAddress(buf) : nullptr;
    const jlong cap = buf ? e->GetDirectBufferCapacity(buf) : 0;
    std::lock_guard<std::mutex> lk(S.mu);
    S.pvOpen = true;
    S.pvName = n; S.pvMsg = m; S.pvIdx = idx; S.pvCnt = cnt;
    if (src && w > 0 && h > 0 && cap >= (jlong)w * h * 4) {
        S.pvPix.assign(src, src + (size_t)w * h * 4);
        S.pvW = w; S.pvH = h; S.pvTexDirty = true;
        S.pvZoom = 1.f; S.pvPanX = S.pvPanY = 0.f;
    } else if (!m.empty()) {
        S.pvPix.clear(); S.pvW = S.pvH = 0;
        if (S.pvTex) { /* tekstur lama dibuang di render berikutnya */ S.pvTexDirty = false; }
    }
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_closePreview(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(S.mu);
    S.pvOpen = false;
    S.pvPix.clear(); S.pvPix.shrink_to_fit(); S.pvW = S.pvH = 0;
}

JNIEXPORT jstring JNICALL Java_com_udyneos_ngirim_Native_tr(JNIEnv* e, jclass, jstring s) {
    return e->NewStringUTF(i18n::tr(js(e, s)).c_str());
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setUpdateMsg(JNIEnv* e, jclass, jstring msg) {
    std::string m = js(e, msg);
    std::lock_guard<std::mutex> lk(S.mu);
    S.updMsg = m;
}

// ---- file manager
JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setFm(JNIEnv* e, jclass, jstring path, jstring err, jstring filter,
                                                        jint sort, jboolean asc, jobjectArray names, jlongArray sizes,
                                                        jbooleanArray dirs, jlongArray mtimes) {
    std::string p = js(e, path), er = js(e, err), f = js(e, filter);
    auto v = jentries(e, names, sizes, dirs, mtimes);
    std::lock_guard<std::mutex> lk(S.mu);
    S.fmPath = p; S.fmErr = er; S.fmFilter = f; S.fmSort = sort; S.fmAsc = asc; S.fm = std::move(v);
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setClip(JNIEnv*, jclass, jint count, jboolean cut) {
    std::lock_guard<std::mutex> lk(S.mu);
    S.clipCount = count; S.clipCut = cut;
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setFmProgress(JNIEnv* e, jclass, jboolean busy, jlong done,
                                                                jlong total, jdouble speed, jstring cur) {
    std::string c = js(e, cur);
    std::lock_guard<std::mutex> lk(S.mu);
    S.mBusy = busy; S.mDone = done; S.mTotal = total; S.mSpeed = speed; S.mCur = c;
}

// ---- APK
JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setApps(JNIEnv* e, jclass, jstring filter, jobjectArray names,
                                                          jobjectArray subs, jlongArray sizes) {
    std::string f = js(e, filter);
    auto v = jitems(e, names, subs, sizes);
    std::lock_guard<std::mutex> lk(S.mu);
    S.apkFilter = f; S.apps = std::move(v);
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setApkProgress(JNIEnv* e, jclass, jboolean busy, jlong done,
                                                                 jlong total, jdouble speed, jstring cur) {
    std::string c = js(e, cur);
    std::lock_guard<std::mutex> lk(S.mu);
    S.aBusy = busy; S.aDone = done; S.aTotal = total; S.aSpeed = speed; S.aCur = c;
}

// ---- jaringan
JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setNetPerm(JNIEnv* e, jclass, jboolean ok, jstring msg) {
    std::string m = js(e, msg);
    std::lock_guard<std::mutex> lk(S.mu);
    S.permOk = ok; S.permMsg = m;
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setPeers(JNIEnv* e, jclass, jobjectArray names) {
    auto v = jstrings(e, names);
    std::lock_guard<std::mutex> lk(S.mu);
    S.peers = std::move(v);
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setNetInfo(JNIEnv* e, jclass, jstring addr, jstring pin,
                                                             jstring recvDir, jboolean listening, jstring paired) {
    std::string a = js(e, addr), p = js(e, pin), r = js(e, recvDir), pr = js(e, paired);
    std::lock_guard<std::mutex> lk(S.mu);
    S.addr = a; S.pin = p; S.recvDir = r; S.listening = listening; S.pairedName = pr;
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setSendList(JNIEnv* e, jclass, jobjectArray names, jlongArray sizes) {
    auto v = jitems(e, names, nullptr, sizes);
    std::lock_guard<std::mutex> lk(S.mu);
    S.sendList = std::move(v);
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setNetProgress(JNIEnv* e, jclass, jboolean busy, jlong done,
                                                                 jlong total, jdouble speed, jstring cur) {
    std::string c = js(e, cur);
    std::lock_guard<std::mutex> lk(S.mu);
    S.netBusy = busy; S.netDone = done; S.netTotal = total; S.netSpeed = speed; S.netCur = c;
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setBrowser(JNIEnv* e, jclass, jboolean open, jint purpose,
                                                             jstring path, jobjectArray names, jlongArray sizes,
                                                             jbooleanArray dirs) {
    std::string p = js(e, path);
    auto v = jentries(e, names, sizes, dirs, nullptr);
    std::lock_guard<std::mutex> lk(S.mu);
    S.bOpen = open; S.bPurpose = purpose; S.bPath = p; S.bEntries = std::move(v);
}

// ---- FTP
JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setPane(JNIEnv* e, jclass, jint idx, jstring path, jboolean remote,
                                                          jstring err, jobjectArray names, jlongArray sizes,
                                                          jbooleanArray dirs) {
    if (idx < 0 || idx > 1) return;
    std::string p = js(e, path), er = js(e, err);
    auto v = jentries(e, names, sizes, dirs, nullptr);
    std::lock_guard<std::mutex> lk(S.mu);
    Pane& P = S.pane[idx];
    P.path = p; P.err = er; P.remote = remote; P.e = std::move(v);
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setBookmarks(JNIEnv* e, jclass, jobjectArray paths) {
    auto v = jstrings(e, paths);
    std::lock_guard<std::mutex> lk(S.mu);
    S.bookmarks = std::move(v);
}

JNIEXPORT void JNICALL Java_com_udyneos_ngirim_Native_setFtpProgress(JNIEnv* e, jclass, jboolean busy, jlong done,
                                                                 jlong total, jdouble speed, jstring cur) {
    std::string c = js(e, cur);
    std::lock_guard<std::mutex> lk(S.mu);
    S.fBusy = busy; S.fDone = done; S.fTotal = total; S.fSpeed = speed; S.fCur = c;
}

}  // extern "C"

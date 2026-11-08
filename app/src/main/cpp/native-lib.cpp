#include <jni.h>
#include <GLES3/gl3.h>
#include <unistd.h>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"

namespace {

constexpr int kCustomRoute = 6;  // harus sama dengan indeks rute "Kustom" di MainActivity

struct Item { std::string name; long long size = 0; bool sel = false; };
struct Entry { std::string name; long long size = 0; bool dir = false; bool sel = false; };

struct Pane {
    std::string path, err;
    bool remote = false;
    std::vector<Entry> e;
};

struct State {
    std::mutex mu;
    // ---- transfer lokal
    std::vector<std::string> routes;
    int route = 0;
    std::vector<Item> items;
    bool shizuku = false;
    std::string shizukuMsg = "Menunggu Shizuku...";
    bool busy = false;
    long long done = 0, total = 0;
    double speed = 0;
    std::string cur;
    int move = 0;  // 0 salin, 1 pindah
    std::string customSrc, customDst;
    // ---- jaringan
    bool permOk = false;
    std::string permMsg = "Memeriksa...";
    std::vector<std::string> peers;
    std::string pairedName, addr, pin, recvDir;
    bool listening = false;
    bool allowBrowse = false;  // penerima: izinkan perangkat terpasang menjelajah /sdcard
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
    int fMove = 0;  // 0 salin, 1 pindah
    // ---- file browser
    bool bOpen = false;
    int bPurpose = 0;  // 1 kirim, 2 folder terima, 3 sumber kustom, 4 tujuan kustom
    std::string bPath;
    std::vector<Entry> bEntries;
    // ---- umum
    std::deque<std::string> actions;
    std::vector<std::string> logs;
    bool logDirty = false;
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

const char* kChangelog[] = {
    "v1.0 (final)",
    "",
    "Baru",
    "- Tab FTP: dua panel berdampingan (HP ini | perangkat lain) dengan pintasan folder",
    "- Panel kanan bisa menjelajah folder perangkat yang sudah dipairing, lalu salin dua arah",
    "- Pintasan folder bawaan + simpan folder favorit sendiri",
    "- Saklar Terima ON/OFF di tab Wi-Fi; folder simpan default /sdcard/Ngirim/Terima",
    "- Perangkat aktif tampil otomatis (tanpa menekan Cari), yang offline hilang sendiri",
    "- Izin jelajah FTP bisa dimatikan di perangkat penerima (dibatasi ke /sdcard)",
    "",
    "Perbaikan",
    "- Tab Wi-Fi, FTP, Terminal, dan Changelog sekarang bisa di-tap",
    "- Koneksi Shizuku otomatis dicoba ulang (cek tiap 2 dtk, bind ulang bila macet),",
    "  tetap tersambung walau izin diberikan lewat app Shizuku; ada tombol Buka app Shizuku",
    "",
    "Riwayat: v1.1 Beta, v1.2 Beta",
    "- Transfer lokal, Wi-Fi (PIN), file browser, terminal, live progress",
    "",
    "Catatan",
    "- Data Wi-Fi tidak dienkripsi (otentikasi PIN saja)",
    "- Nama non-Latin bisa tampil sebagai '?'",
    "- Router dengan client isolation memblokir koneksi antar-perangkat",
    "- Jelajah jarak jauh: hanya salin (tanpa pindah/hapus)",
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

std::string js(JNIEnv* e, jstring s) {
    if (!s) return "";
    const char* c = e->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) e->ReleaseStringUTFChars(s, c);
    return r;
}

void act(const std::string& a) { S.actions.push_back(a); }

// Drag sentuh = scroll. Dipanggil di dalam child window.
void touchScroll() {
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        ImGui::IsMouseDragging(0, 12.f * g_density)) {
        ImGui::ClearActiveID();  // batalkan klik saat drag
        ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
    }
}

void progressBlock(bool busy, long long done, long long total, double speed, const std::string& cur) {
    const float d = g_density;
    if (!(busy || total > 0)) return;
    float pct = total > 0 ? (float)((double)done / (double)total) : 0.f;
    ImGui::ProgressBar(pct, ImVec2(-FLT_MIN, 30.f * d));
    ImGui::TextWrapped("%s", cur.c_str());
    double eta = speed > 1 ? (double)(total - done) / speed : -1;
    ImGui::Text("%s / %s | %s/s | ETA %s", fmtSize((double)done).c_str(), fmtSize((double)total).c_str(),
                fmtSize(speed).c_str(), busy ? fmtEta(eta).c_str() : "selesai");
}

// Log gaya terminal (monospace, latar gelap, warna per jenis baris)
void drawTerminal(const char* id, float h) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.02f, 0.03f, 0.02f, 1.f));
    ImGui::BeginChild(id, ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    if (g_mono) ImGui::PushFont(g_mono);
    for (auto& l : S.logs) {
        const char* body = l.size() > 11 ? l.c_str() + 11 : l.c_str();
        ImVec4 c(0.72f, 0.88f, 0.72f, 1.f);
        if (!strncmp(body, "GAGAL", 5) || !strncmp(body, "ERR", 3)) c = ImVec4(1.f, 0.45f, 0.4f, 1.f);
        else if (!strncmp(body, "OK", 2)) c = ImVec4(0.4f, 1.f, 0.5f, 1.f);
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        ImGui::TextWrapped("%s", l.c_str());
        ImGui::PopStyleColor();
    }
    if (g_mono) ImGui::PopFont();
    if (S.logDirty) { ImGui::SetScrollHereY(1.f); S.logDirty = false; }
    touchScroll();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------------ transfer lokal
void drawTransfer() {
    const float d = g_density;

    ImGui::SetNextItemWidth(-FLT_MIN);
    const char* prev = S.routes.empty() ? "-" : S.routes[S.route].c_str();
    ImGui::BeginDisabled(S.busy);
    if (ImGui::BeginCombo("##route", prev)) {
        for (int i = 0; i < (int)S.routes.size(); i++) {
            if (ImGui::Selectable(S.routes[i].c_str(), i == S.route) && i != S.route) {
                S.route = i;
                S.items.clear();
                act("ROUTE|" + std::to_string(i));
            }
        }
        ImGui::EndCombo();
    }
    if (S.route == kCustomRoute) {
        ImGui::TextWrapped("Sumber: %s", S.customSrc.empty() ? "(belum dipilih)" : S.customSrc.c_str());
        ImGui::TextWrapped("Tujuan: %s", S.customDst.empty() ? "(belum dipilih)" : S.customDst.c_str());
        if (ImGui::Button("Pilih sumber")) act("BPURPOSE|3");
        ImGui::SameLine();
        if (ImGui::Button("Pilih tujuan")) act("BPURPOSE|4");
    }
    if (ImGui::Button("Segarkan")) act("REFRESH");
    ImGui::SameLine();
    if (ImGui::Button("Pilih semua")) for (auto& it : S.items) it.sel = true;
    ImGui::SameLine();
    if (ImGui::Button("Kosongkan")) for (auto& it : S.items) it.sel = false;
    ImGui::RadioButton("Salin", &S.move, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Pindah", &S.move, 1);
    ImGui::EndDisabled();

    int n = 0; long long tot = 0;
    for (auto& it : S.items) if (it.sel) { n++; tot += it.size; }

    const float footer = 235.f * d;
    float h = ImGui::GetContentRegionAvail().y - footer;
    if (h < 120.f * d) h = 120.f * d;
    ImGui::BeginChild("list", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    if (S.items.empty()) {
        ImGui::TextDisabled(S.route == kCustomRoute ? "Pilih folder sumber dan tujuan." :
                            (S.shizuku || S.route == 0) ? "Daftar kosong." : "Hubungkan Shizuku dulu.");
    }
    ImGuiListClipper clip;
    clip.Begin((int)S.items.size());
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            Item& it = S.items[i];
            std::string label = it.name + "  [" + fmtSize((double)it.size) + "]##" + std::to_string(i);
            ImGui::Checkbox(label.c_str(), &it.sel);
        }
    }
    touchScroll();
    ImGui::EndChild();

    ImGui::Text("Dipilih: %d item (%s)", n, fmtSize((double)tot).c_str());

    if (!S.busy) {
        bool dstOk = S.route != kCustomRoute || (!S.customSrc.empty() && !S.customDst.empty());
        ImGui::BeginDisabled(!(S.shizuku && n > 0 && dstOk));
        if (ImGui::Button("MULAI TRANSFER", ImVec2(-FLT_MIN, 58.f * d))) {
            std::string idx;
            for (int i = 0; i < (int)S.items.size(); i++)
                if (S.items[i].sel) idx += (idx.empty() ? "" : ",") + std::to_string(i);
            act("START|" + std::to_string(S.route) + "|" + std::to_string(S.move) + "|" + idx);
            S.busy = true; S.done = 0; S.total = 0; S.speed = 0; S.cur = "Menyiapkan...";
        }
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("BATALKAN", ImVec2(-FLT_MIN, 58.f * d))) act("CANCEL");
    }
    progressBlock(S.busy, S.done, S.total, S.speed, S.cur);
}

// ------------------------------------------------------------------ widget kecil
// Saklar ON/OFF; true saat diketuk.
bool toggleSwitch(const char* id, bool on) {
    const float d = g_density;
    const float h = 30.f * d, w = 56.f * d;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), on ? IM_COL32(60, 190, 100, 255) : IM_COL32(90, 90, 105, 255), h * 0.5f);
    dl->AddCircleFilled(ImVec2(on ? p.x + w - h * 0.5f : p.x + h * 0.5f, p.y + h * 0.5f), h * 0.5f - 4.f * d,
                        IM_COL32(255, 255, 255, 255));
    return clicked;
}

// Tombol yang pindah baris bila tidak muat. `first` = tombol pertama di baris.
bool wrapButton(const std::string& label, bool& first, bool highlight = false) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImGuiStyle& st = ImGui::GetStyle();
    float bw = ImGui::CalcTextSize(label.c_str(), nullptr, true).x + st.FramePadding.x * 2.f;
    if (!first) {
        float nextX2 = ImGui::GetItemRectMax().x + st.ItemSpacing.x + bw;
        if (nextX2 < w->WorkRect.Max.x) ImGui::SameLine();
    }
    first = false;
    if (highlight) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.45f, 0.75f, 1.f));
    bool r = ImGui::Button(label.c_str());
    if (highlight) ImGui::PopStyleColor();
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

// ------------------------------------------------------------------ jaringan
void drawNet() {
    const float d = g_density;
    const ImVec4 blue(0.45f, 0.75f, 1.f, 1.f), green(0.4f, 0.9f, 0.5f, 1.f), orange(1.f, 0.7f, 0.3f, 1.f);

    ImGui::TextColored(S.permOk ? green : orange, "%s", S.permMsg.c_str());
    if (!S.permOk) {
        if (ImGui::Button("Berikan izin / periksa ulang", ImVec2(-FLT_MIN, 52.f * d))) act("NET_PERM");
    }
    if (!S.shizuku) ImGui::TextDisabled("Shizuku harus terhubung dulu.");
    ImGui::BeginDisabled(!(S.permOk && S.shizuku));

    // ---- Terima (receiver)
    ImGui::SeparatorText("Terima");
    if (toggleSwitch("##rx", S.listening)) act(S.listening ? "RX_OFF" : "RX_ON");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(S.listening ? green : ImVec4(0.7f, 0.7f, 0.75f, 1.f), S.listening ? "Terima AKTIF - terlihat oleh perangkat lain" : "Terima MATI");
    ImGui::TextWrapped("Folder simpan: %s", S.recvDir.empty() ? "(belum dipilih)" : S.recvDir.c_str());
    ImGui::BeginDisabled(S.listening);
    if (ImGui::Button("Ubah folder simpan")) act("BPURPOSE|2");
    ImGui::EndDisabled();
    if (toggleSwitch("##bro", S.allowBrowse)) {
        S.allowBrowse = !S.allowBrowse;
        act(std::string("RX_BROWSE|") + (S.allowBrowse ? "1" : "0"));
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped("Izinkan jelajah FTP ke /sdcard");
    if (S.listening) {
        ImGui::Text("Alamat: %s", S.addr.c_str());
        ImGui::Text("PIN:");
        ImGui::SameLine();
        ImGui::SetWindowFontScale(2.2f);
        ImGui::TextColored(orange, "%s", S.pin.c_str());
        ImGui::SetWindowFontScale(1.f);
    }

    // ---- Perangkat aktif
    ImGui::SeparatorText("Perangkat aktif");
    if (ImGui::Button("Segarkan")) act("NET_SCAN");
    ImGui::SameLine();
    if (ImGui::Button("IP manual")) act("NET_MANUAL");
    if (S.peers.empty()) ImGui::TextDisabled("Belum ada. Nyalakan Terima di perangkat lain (satu Wi-Fi).");
    else ImGui::TextDisabled("Ketuk perangkat, lalu masukkan PIN dari layarnya.");
    for (int i = 0; i < (int)S.peers.size(); i++) {
        std::string l = "o  " + S.peers[i] + "##p" + std::to_string(i);
        if (ImGui::Selectable(l.c_str(), false, 0, ImVec2(0, 52.f * d))) act("NET_PAIR|" + std::to_string(i));
    }

    // ---- Terhubung
    if (!S.pairedName.empty()) {
        ImGui::SeparatorText("Terhubung");
        ImGui::TextColored(green, "%s", S.pairedName.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(S.netBusy);
        if (ImGui::Button("Putus")) act("NET_DISCONNECT");
        ImGui::EndDisabled();
        ImGui::TextDisabled("Jelajah dan salin dua arah ada di tab FTP.");
        ImGui::BeginDisabled(S.netBusy);
        if (ImGui::Button("+ File / folder")) act("BPURPOSE|1");
        ImGui::SameLine();
        if (ImGui::Button("Kosongkan")) { S.sendList.clear(); act("NET_CLEAR"); }
        ImGui::EndDisabled();
        if (S.sendList.empty()) ImGui::TextDisabled("Belum ada file/folder dipilih.");
        const int kShow = 6;
        for (int i = 0; i < (int)S.sendList.size() && i < kShow; i++) ImGui::TextWrapped("%s", S.sendList[i].name.c_str());
        if ((int)S.sendList.size() > kShow) ImGui::TextDisabled("... dan %d lainnya", (int)S.sendList.size() - kShow);
        if (!S.netBusy) {
            ImGui::BeginDisabled(S.sendList.empty());
            if (ImGui::Button("KIRIM SEKARANG", ImVec2(-FLT_MIN, 58.f * d))) {
                act("NET_SEND");
                S.netBusy = true; S.netDone = 0; S.netTotal = 0; S.netSpeed = 0; S.netCur = "Menyiapkan...";
            }
            ImGui::EndDisabled();
        } else {
            if (ImGui::Button("BATALKAN", ImVec2(-FLT_MIN, 58.f * d))) act("NET_CANCEL");
        }
    }
    ImGui::EndDisabled();

    progressBlock(S.netBusy, S.netDone, S.netTotal, S.netSpeed, S.netCur);

    // log terakhir (selengkapnya di tab Terminal)
    ImGui::SeparatorText("Log terakhir");
    if (g_mono) ImGui::PushFont(g_mono);
    size_t from = S.logs.size() > 5 ? S.logs.size() - 5 : 0;
    for (size_t i = from; i < S.logs.size(); i++) ImGui::TextWrapped("%s", S.logs[i].c_str());
    if (g_mono) ImGui::PopFont();
}

// ------------------------------------------------------------------ FTP (dua panel)
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

int selCount(const Pane& p) {
    int n = 0;
    for (auto& e : p.e) if (e.sel) n++;
    return n;
}

std::string selIdx(const Pane& p) {
    std::string idx;
    for (int i = 0; i < (int)p.e.size(); i++)
        if (p.e[i].sel) idx += (idx.empty() ? "" : ",") + std::to_string(i);
    return idx;
}

void drawPane(int i, float w, float h) {
    const float d = g_density;
    const ImVec4 blue(0.45f, 0.75f, 1.f, 1.f);
    Pane& P = S.pane[i];
    const bool isActive = S.active == i;
    ImGui::PushStyleColor(ImGuiCol_Border, isActive ? ImVec4(0.45f, 0.75f, 1.f, 1.f) : ImVec4(0.28f, 0.28f, 0.33f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, isActive ? 2.f * d : 1.f);
    std::string id = "pane" + std::to_string(i);
    ImGui::BeginChild(id.c_str(), ImVec2(w, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(0)) S.active = i;

    if (i == 0) {
        ImGui::TextColored(blue, "HP ini");
    } else {
        // sumber panel kanan: HP ini atau perangkat yang dipairing
        bool first = true;
        if (wrapButton("HP ini##src0", first, !P.remote)) act("FP|1|SRC|0");
        ImGui::BeginDisabled(S.pairedName.empty());
        std::string rl = S.pairedName.empty() ? "Perangkat lain" : tailEllipsis(S.pairedName, 14);
        if (wrapButton(rl + "##src1", first, P.remote)) act("FP|1|SRC|1");
        ImGui::EndDisabled();
    }
    ImGui::TextDisabled("%s", tailEllipsis(P.path, 30).c_str());
    if (i == 1 && S.pairedName.empty() && !P.remote) ImGui::TextDisabled("Pairing dulu di tab Wi-Fi.");
    if (!P.err.empty()) ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f), "%s", P.err.c_str());

    const int n = selCount(P);
    float lh = ImGui::GetContentRegionAvail().y - 4.f * d;
    if (lh < 60.f * d) lh = 60.f * d;
    ImGui::BeginChild((id + "_list").c_str(), ImVec2(0, lh), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    if (P.e.empty() && P.err.empty()) ImGui::TextDisabled("Kosong");
    const ImVec2 rowSize(0, 34.f * d);
    ImGuiListClipper clip;
    clip.Begin((int)P.e.size());
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            Entry& e = P.e[r];
            ImGui::PushID(r);
            ImGui::Checkbox("##s", &e.sel);
            ImGui::SameLine();
            std::string l = (e.dir ? "[DIR] " : "") + e.name;
            if (ImGui::Selectable(l.c_str(), false, 0, rowSize)) {
                if (e.dir) act("FP|" + std::to_string(i) + "|OPEN|" + std::to_string(r));
                else e.sel = !e.sel;
            }
            ImGui::PopID();
        }
    }
    touchScroll();
    ImGui::EndChild();
    (void)n;
    ImGui::EndChild();
}

void drawFtp() {
    const float d = g_density;
    const ImVec4 blue(0.45f, 0.75f, 1.f, 1.f);
    Pane& A = S.pane[S.active];
    const std::string ai = std::to_string(S.active);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.f * d, 4.f * d));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f * d, 6.f * d));
    ImGui::TextColored(blue, "Panel aktif: %s", S.active == 0 ? "kiri" : "kanan");
    bool first = true;
    for (auto& p : kPresets)
        if (wrapButton(std::string(p.label) + "##pr", first)) act("FP|" + ai + "|GO|" + p.path);
    for (int i = 0; i < (int)S.bookmarks.size(); i++) {
        std::string l = "* " + tailEllipsis(baseName(S.bookmarks[i]), 14) + "##bm" + std::to_string(i);
        if (wrapButton(l, first)) act("FP|" + ai + "|GO|" + S.bookmarks[i]);
    }

    bool isBm = false;
    for (auto& b : S.bookmarks) if (b == A.path) isBm = true;
    first = true;
    if (wrapButton("Naik", first)) act("FP|" + ai + "|UP");
    if (wrapButton("Segarkan", first)) act("FP|" + ai + "|REF");
    ImGui::BeginDisabled(A.remote);
    if (wrapButton("Folder baru", first)) act("FP_MKDIR|" + ai);
    ImGui::EndDisabled();
    if (wrapButton(isBm ? "Hapus pintasan" : "Simpan pintasan", first)) act("BM|" + A.path);
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

    const int nL = selCount(S.pane[0]), nR = selCount(S.pane[1]);
    ImGui::BeginDisabled(S.fBusy);
    ImGui::RadioButton("Salin", &S.fMove, 0);
    ImGui::SameLine();
    ImGui::BeginDisabled(remote);
    ImGui::RadioButton("Pindah", &S.fMove, 1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Dipilih: kiri %d, kanan %d", nL, nR);
    ImGui::EndDisabled();

    if (!S.fBusy) {
        const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::BeginDisabled(!S.shizuku || nL == 0);
        if (ImGui::Button("Kiri  >>  Kanan", ImVec2(bw, 50.f * d))) {
            act("FP_XFER|0|" + std::to_string(S.fMove) + "|" + selIdx(S.pane[0]));
            S.fBusy = true; S.fDone = 0; S.fTotal = 0; S.fSpeed = 0; S.fCur = "Menyiapkan...";
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!S.shizuku || nR == 0);
        if (ImGui::Button("Kiri  <<  Kanan", ImVec2(-FLT_MIN, 54.f * d))) {
            act("FP_XFER|1|" + std::to_string(S.fMove) + "|" + selIdx(S.pane[1]));
            S.fBusy = true; S.fDone = 0; S.fTotal = 0; S.fSpeed = 0; S.fCur = "Menyiapkan...";
        }
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("BATALKAN", ImVec2(-FLT_MIN, 54.f * d))) act("CANCEL");
    }
    progressBlock(S.fBusy, S.fDone, S.fTotal, S.fSpeed, S.fCur);
}

// ------------------------------------------------------------------ file browser
void drawBrowser() {
    const float d = g_density;
    const int P = S.bPurpose;
    const char* title = P == 1 ? "Pilih file / folder untuk dikirim"
                      : P == 2 ? "Pilih folder penyimpanan"
                      : P == 3 ? "Pilih folder sumber" : "Pilih folder tujuan";
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.f, 1.f), "%s", title);
    ImGui::TextWrapped("%s", S.bPath.c_str());
    if (ImGui::Button("Naik")) act("BUP");
    ImGui::SameLine();
    if (ImGui::Button("Folder baru")) act("BNEWDIR");
    ImGui::SameLine();
    if (ImGui::Button("Tutup")) act("BCLOSE");
    if (ImGui::Button("Internal")) act("BGO|/sdcard");
    ImGui::SameLine();
    if (ImGui::Button("Download")) act("BGO|/sdcard/Download");
    ImGui::SameLine();
    if (ImGui::Button("Data")) act("BGO|/sdcard/Android/data");
    ImGui::SameLine();
    if (ImGui::Button("OBB")) act("BGO|/sdcard/Android/obb");

    int n = 0;
    for (auto& e : S.bEntries) if (e.sel) n++;

    float h = ImGui::GetContentRegionAvail().y - 75.f * d;
    if (h < 100.f * d) h = 100.f * d;
    ImGui::BeginChild("blist", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    if (S.bEntries.empty()) ImGui::TextDisabled("Folder kosong / tidak bisa dibaca.");
    const ImVec2 rowSize(0, 44.f * d);
    ImGuiListClipper clip;
    clip.Begin((int)S.bEntries.size());
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            Entry& e = S.bEntries[i];
            ImGui::PushID(i);
            if (P == 1) {
                ImGui::Checkbox("##sel", &e.sel);
                ImGui::SameLine();
            }
            if (e.dir) {
                std::string l = "[DIR] " + e.name;
                if (ImGui::Selectable(l.c_str(), false, 0, rowSize)) act("BOPEN|" + std::to_string(i));
            } else {
                std::string l = e.name + "  [" + fmtSize((double)e.size) + "]";
                if (P == 1) {
                    if (ImGui::Selectable(l.c_str(), false, 0, rowSize)) e.sel = !e.sel;
                } else {
                    ImGui::Selectable(l.c_str(), false, ImGuiSelectableFlags_Disabled, rowSize);
                }
            }
            ImGui::PopID();
        }
    }
    touchScroll();
    ImGui::EndChild();

    if (P == 1) {
        std::string l = "Tambahkan " + std::to_string(n) + " item";
        ImGui::BeginDisabled(n == 0);
        if (ImGui::Button(l.c_str(), ImVec2(-FLT_MIN, 58.f * d))) {
            std::string idx;
            for (int i = 0; i < (int)S.bEntries.size(); i++)
                if (S.bEntries[i].sel) idx += (idx.empty() ? "" : ",") + std::to_string(i);
            act("BFILES|" + idx);
        }
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("PILIH FOLDER INI", ImVec2(-FLT_MIN, 58.f * d))) act("BDIR");
    }
}

void drawUI() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.f, 1.f), "Ngirim");
    ImGui::SameLine();
    ImGui::TextDisabled("v1.0");
    if (S.shizuku) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.f), "%s", S.shizukuMsg.c_str());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.5f, 0.4f, 1.f));
        ImGui::TextWrapped("%s", S.shizukuMsg.c_str());
        ImGui::PopStyleColor();
        if (ImGui::Button("Hubungkan Shizuku")) act("CONNECT");
        ImGui::SameLine();
        if (ImGui::Button("Buka app Shizuku")) act("OPEN_SHIZUKU");
    }

    if (S.bOpen) {
        drawBrowser();
    } else if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Lokal")) {
            ImGui::BeginChild("t_local", ImVec2(0, 0), 0, ImGuiWindowFlags_NoScrollbar);
            drawTransfer();
            touchScroll();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Wi-Fi")) {
            ImGui::BeginChild("t_net", ImVec2(0, 0), 0, ImGuiWindowFlags_NoScrollbar);
            drawNet();
            touchScroll();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("FTP")) {
            drawFtp();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Terminal")) {
            drawTerminal("term_main", ImGui::GetContentRegionAvail().y);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Changelog")) {
            ImGui::BeginChild("chg", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
            for (const char* l : kChangelog) {
                if (!*l) ImGui::Spacing();
                else ImGui::TextWrapped("%s", l);
            }
            touchScroll();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

}  // namespace

extern "C" {

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_initGl(JNIEnv*, jclass, jfloat density) {
    g_density = density;
    if (!g_ctx) {
        IMGUI_CHECKVERSION();
        g_ctx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGuiStyle& st = ImGui::GetStyle();
        st.WindowRounding = 0; st.FrameRounding = 8; st.ChildRounding = 8;
        st.GrabRounding = 8; st.TabRounding = 8;
        st.FramePadding = ImVec2(12, 10);
        st.ItemSpacing = ImVec2(10, 12);
        st.ScrollbarSize = 6;
        st.ScaleAllSizes(density);

        float px = 15.f * density;
        const char* font = "/system/fonts/Roboto-Regular.ttf";
        if (access(font, R_OK) == 0) io.Fonts->AddFontFromFileTTF(font, px);
        else { ImFontConfig c; c.SizePixels = px; io.Fonts->AddFontDefault(&c); }

        const char* mono = "/system/fonts/DroidSansMono.ttf";
        if (access(mono, R_OK) == 0) g_mono = io.Fonts->AddFontFromFileTTF(mono, 13.f * density);
    } else {
        ImGui_ImplOpenGL3_Shutdown();  // konteks GL dibuat ulang
    }
    ImGui_ImplOpenGL3_Init("#version 300 es");
}

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_render(JNIEnv*, jclass, jint w, jint h) {
    if (!g_ctx) return;
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime = dt > 0 ? dt : 1.f / 60.f;

    applyTouchEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    {
        std::lock_guard<std::mutex> lk(S.mu);
        drawUI();
    }
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_touch(JNIEnv*, jclass, jint action, jfloat x, jfloat y) {
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

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_setShizuku(JNIEnv* e, jclass, jboolean ok, jstring msg) {
    std::lock_guard<std::mutex> lk(S.mu);
    S.shizuku = ok;
    S.shizukuMsg = js(e, msg);
}

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_setRoutes(JNIEnv* e, jclass, jobjectArray names) {
    std::lock_guard<std::mutex> lk(S.mu);
    S.routes.clear();
    jsize n = e->GetArrayLength(names);
    for (jsize i = 0; i < n; i++) {
        jstring s = (jstring)e->GetObjectArrayElement(names, i);
        S.routes.push_back(js(e, s));
        e->DeleteLocalRef(s);
    }
}

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_setItems(JNIEnv* e, jclass, jobjectArray names, jlongArray sizes) {
    std::vector<Item> v;
    jsize n = e->GetArrayLength(names);
    jlong* sz = e->GetLongArrayElements(sizes, nullptr);
    for (jsize i = 0; i < n; i++) {
        jstring s = (jstring)e->GetObjectArrayElement(names, i);
        Item it; it.name = js(e, s); it.size = sz[i];
        v.push_back(std::move(it));
        e->DeleteLocalRef(s);
    }
    e->ReleaseLongArrayElements(sizes, sz, JNI_ABORT);
    std::lock_guard<std::mutex> lk(S.mu);
    S.items = std::move(v);
}

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_setProgress(JNIEnv* e, jclass, jboolean busy, jlong done,
                                                              jlong total, jdouble speed, jstring cur) {
    std::string c = js(e, cur);
    std::lock_guard<std::mutex> lk(S.mu);
    S.busy = busy; S.done = done; S.total = total; S.speed = speed; S.cur = c;
}

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_setCustom(JNIEnv* e, jclass, jstring src, jstring dst) {
    std::string a = js(e, src), b = js(e, dst);
    std::lock_guard<std::mutex> lk(S.mu);
    S.customSrc = a; S.customDst = b;
}

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_log(JNIEnv* e, jclass, jstring msg) {
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

JNIEXPORT jstring JNICALL Java_com_ngirim_app_Native_pollAction(JNIEnv* e, jclass) {
    std::lock_guard<std::mutex> lk(S.mu);
    if (S.actions.empty()) return e->NewStringUTF("");
    std::string a = S.actions.front();
    S.actions.pop_front();
    return e->NewStringUTF(a.c_str());
}

// ---- jaringan
JNIEXPORT void JNICALL Java_com_ngirim_app_Native_setNetPerm(JNIEnv* e, jclass, jboolean ok, jstring msg) {
    std::string m = js(e, msg);
    std::lock_guard<std::mutex> lk(S.mu);
    S.permOk = ok; S.permMsg = m;
}

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_setPeers(JNIEnv* e, jclass, jobjectArray names) {
    std::vector<std::string> v;
    jsize n = e->GetArrayLength(names);
    for (jsize i = 0; i < n; i++) {
        jstring s = (jstring)e->GetObjectArrayElement(names, i);
        v.push_back(js(e, s));
        e->DeleteLocalRef(s);
    }
    std::lock_guard<std::mutex> lk(S.mu);
    S.peers = std::move(v);
}

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_setNetInfo(JNIEnv* e, jclass, jstring addr, jstring pin,
                                                             jstring recvDir, jboolean listening, jstring paired) {
    std::string a = js(e, addr), p = js(e, pin), r = js(e, recvDir), pr = js(e, paired);
    std::lock_guard<std::mutex> lk(S.mu);
    S.addr = a; S.pin = p; S.recvDir = r; S.listening = listening; S.pairedName = pr;
}

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_setSendList(JNIEnv* e, jclass, jobjectArray names, jlongArray sizes) {
    std::vector<Item> v;
    jsize n = e->GetArrayLength(names);
    jlong* sz = e->GetLongArrayElements(sizes, nullptr);
    for (jsize i = 0; i < n; i++) {
        jstring s = (jstring)e->GetObjectArrayElement(names, i);
        Item it; it.name = js(e, s); it.size = sz[i];
        v.push_back(std::move(it));
        e->DeleteLocalRef(s);
    }
    e->ReleaseLongArrayElements(sizes, sz, JNI_ABORT);
    std::lock_guard<std::mutex> lk(S.mu);
    S.sendList = std::move(v);
}

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_setNetProgress(JNIEnv* e, jclass, jboolean busy, jlong done,
                                                                 jlong total, jdouble speed, jstring cur) {
    std::string c = js(e, cur);
    std::lock_guard<std::mutex> lk(S.mu);
    S.netBusy = busy; S.netDone = done; S.netTotal = total; S.netSpeed = speed; S.netCur = c;
}

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_setBrowser(JNIEnv* e, jclass, jboolean open, jint purpose,
                                                             jstring path, jobjectArray names, jlongArray sizes,
                                                             jbooleanArray dirs) {
    std::vector<Entry> v;
    std::string p = js(e, path);
    jsize n = e->GetArrayLength(names);
    jlong* sz = e->GetLongArrayElements(sizes, nullptr);
    jboolean* dr = e->GetBooleanArrayElements(dirs, nullptr);
    for (jsize i = 0; i < n; i++) {
        jstring s = (jstring)e->GetObjectArrayElement(names, i);
        Entry en; en.name = js(e, s); en.size = sz[i]; en.dir = dr[i];
        v.push_back(std::move(en));
        e->DeleteLocalRef(s);
    }
    e->ReleaseLongArrayElements(sizes, sz, JNI_ABORT);
    e->ReleaseBooleanArrayElements(dirs, dr, JNI_ABORT);
    std::lock_guard<std::mutex> lk(S.mu);
    S.bOpen = open; S.bPurpose = purpose; S.bPath = p; S.bEntries = std::move(v);
}

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_setPane(JNIEnv* e, jclass, jint idx, jstring path, jboolean remote,
                                                          jstring err, jobjectArray names, jlongArray sizes,
                                                          jbooleanArray dirs) {
    if (idx < 0 || idx > 1) return;
    std::vector<Entry> v;
    std::string p = js(e, path), er = js(e, err);
    jsize n = e->GetArrayLength(names);
    jlong* sz = e->GetLongArrayElements(sizes, nullptr);
    jboolean* dr = e->GetBooleanArrayElements(dirs, nullptr);
    for (jsize i = 0; i < n; i++) {
        jstring s = (jstring)e->GetObjectArrayElement(names, i);
        Entry en; en.name = js(e, s); en.size = sz[i]; en.dir = dr[i];
        v.push_back(std::move(en));
        e->DeleteLocalRef(s);
    }
    e->ReleaseLongArrayElements(sizes, sz, JNI_ABORT);
    e->ReleaseBooleanArrayElements(dirs, dr, JNI_ABORT);
    std::lock_guard<std::mutex> lk(S.mu);
    Pane& P = S.pane[idx];
    P.path = p; P.err = er; P.remote = remote; P.e = std::move(v);
}

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_setBookmarks(JNIEnv* e, jclass, jobjectArray paths) {
    std::vector<std::string> v;
    jsize n = e->GetArrayLength(paths);
    for (jsize i = 0; i < n; i++) {
        jstring s = (jstring)e->GetObjectArrayElement(paths, i);
        v.push_back(js(e, s));
        e->DeleteLocalRef(s);
    }
    std::lock_guard<std::mutex> lk(S.mu);
    S.bookmarks = std::move(v);
}

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_setFtpProgress(JNIEnv* e, jclass, jboolean busy, jlong done,
                                                                 jlong total, jdouble speed, jstring cur) {
    std::string c = js(e, cur);
    std::lock_guard<std::mutex> lk(S.mu);
    S.fBusy = busy; S.fDone = done; S.fTotal = total; S.fSpeed = speed; S.fCur = c;
}

}  // extern "C"

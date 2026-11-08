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
    int step = 0;  // 0 izin, 1 peran, 2 kirim:pairing, 3 kirim:file, 4 terima
    bool permOk = false;
    std::string permMsg = "Memeriksa...";
    std::vector<std::string> peers;
    std::string pairedName, addr, pin, recvDir;
    bool listening = false;
    std::vector<Item> sendList;
    bool netBusy = false;
    long long netDone = 0, netTotal = 0;
    double netSpeed = 0;
    std::string netCur;
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

ImGuiContext* g_ctx = nullptr;
ImFont* g_mono = nullptr;
float g_density = 1.f;

const char* kChangelog[] = {
    "v1.1 Beta",
    "",
    "Baru",
    "- Transfer antar-perangkat lewat Wi-Fi yang sama (Kirim / Terima)",
    "- Alur: izin -> pilih peran -> pairing PIN -> kirim -> progress",
    "- Penerima memilih folder penyimpanan sendiri",
    "- Transfer folder (rekursif) dan lokasi kustom lewat file browser",
    "- Log gaya terminal dengan timestamp dan warna",
    "- Transfer APK (termasuk split APK), foto, data game OBB dan DATA",
    "- Akses lewat Shizuku, tanpa root dan tanpa SAF",
    "- Live progress: persen, ukuran, kecepatan, ETA, file aktif",
    "- Mode Salin / Pindah, backup dan restore data game",
    "- Verifikasi ukuran file, batalkan kapan saja",
    "",
    "Catatan Beta",
    "- Data Wi-Fi tidak dienkripsi (otentikasi PIN saja)",
    "- Nama non-Latin bisa tampil sebagai '?'",
    "- Router dengan client isolation memblokir koneksi antar-perangkat",
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

// ------------------------------------------------------------------ jaringan
void drawNet() {
    const float d = g_density;
    const ImVec4 blue(0.45f, 0.75f, 1.f, 1.f), green(0.4f, 0.9f, 0.5f, 1.f), orange(1.f, 0.7f, 0.3f, 1.f);

    switch (S.step) {
    case 0: {
        ImGui::TextColored(blue, "Langkah 1 - Izin dan koneksi");
        ImGui::Separator();
        ImGui::TextColored(S.permOk ? green : orange, "%s", S.permMsg.c_str());
        if (ImGui::Button("Berikan izin / periksa ulang", ImVec2(-FLT_MIN, 52.f * d))) act("NET_PERM");
        if (!S.shizuku) ImGui::TextDisabled("Shizuku harus terhubung dulu.");
        ImGui::BeginDisabled(!(S.permOk && S.shizuku));
        if (ImGui::Button("LANJUT", ImVec2(-FLT_MIN, 58.f * d))) S.step = 1;
        ImGui::EndDisabled();
        break;
    }
    case 1: {
        ImGui::TextColored(blue, "Langkah 2 - Pilih peran");
        ImGui::Separator();
        ImGui::TextWrapped("Kedua perangkat harus berada di Wi-Fi yang sama.");
        if (ImGui::Button("KIRIM", ImVec2(-FLT_MIN, 80.f * d))) { S.step = 2; act("NET_ROLE|send"); }
        if (ImGui::Button("TERIMA", ImVec2(-FLT_MIN, 80.f * d))) { S.step = 4; act("NET_ROLE|recv"); }
        if (ImGui::Button("Kembali", ImVec2(-FLT_MIN, 48.f * d))) S.step = 0;
        break;
    }
    case 2: {
        ImGui::TextColored(blue, "Kirim - cari dan pairing perangkat");
        ImGui::Separator();
        if (ImGui::Button("Cari perangkat")) act("NET_SCAN");
        ImGui::SameLine();
        if (ImGui::Button("IP manual")) act("NET_MANUAL");
        ImGui::TextDisabled("Ketuk perangkat, lalu masukkan PIN yang tampil di penerima.");
        float h = ImGui::GetContentRegionAvail().y - 70.f * d;
        if (h < 100.f * d) h = 100.f * d;
        ImGui::BeginChild("peers", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        if (S.peers.empty()) ImGui::TextDisabled("Belum ada perangkat. Penerima harus sudah menekan MULAI TERIMA.");
        for (int i = 0; i < (int)S.peers.size(); i++) {
            std::string l = S.peers[i] + "##p" + std::to_string(i);
            if (ImGui::Selectable(l.c_str(), false, 0, ImVec2(0, 52.f * d))) act("NET_PAIR|" + std::to_string(i));
        }
        touchScroll();
        ImGui::EndChild();
        if (ImGui::Button("Kembali", ImVec2(-FLT_MIN, 52.f * d))) { S.step = 1; act("NET_RESET"); }
        break;
    }
    case 3: {
        ImGui::TextColored(green, "Terhubung: %s", S.pairedName.c_str());
        ImGui::BeginDisabled(S.netBusy);
        if (ImGui::Button("+ File / folder")) act("BPURPOSE|1");
        ImGui::SameLine();
        if (ImGui::Button("Kosongkan")) { S.sendList.clear(); act("NET_CLEAR"); }
        ImGui::EndDisabled();
        ImGui::BeginChild("sendlist", ImVec2(0, 150.f * d), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        if (S.sendList.empty()) ImGui::TextDisabled("Belum ada file/folder dipilih.");
        long long tot = 0;
        for (auto& it : S.sendList) { ImGui::TextWrapped("%s", it.name.c_str()); tot += it.size; }
        touchScroll();
        ImGui::EndChild();
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
        progressBlock(S.netBusy, S.netDone, S.netTotal, S.netSpeed, S.netCur);
        float th = ImGui::GetContentRegionAvail().y - 64.f * d;
        if (th < 110.f * d) th = 110.f * d;
        drawTerminal("term_send", th);
        ImGui::BeginDisabled(S.netBusy);
        if (ImGui::Button("Putus", ImVec2(-FLT_MIN, 48.f * d))) { S.step = 1; act("NET_RESET"); }
        ImGui::EndDisabled();
        break;
    }
    case 4: {
        ImGui::TextColored(blue, "Terima");
        ImGui::TextWrapped("Folder tujuan: %s", S.recvDir.empty() ? "(belum dipilih)" : S.recvDir.c_str());
        ImGui::BeginDisabled(S.listening);
        if (ImGui::Button("Pilih folder tujuan")) act("BPURPOSE|2");
        ImGui::EndDisabled();
        if (!S.listening) {
            ImGui::BeginDisabled(S.recvDir.empty());
            if (ImGui::Button("MULAI TERIMA", ImVec2(-FLT_MIN, 58.f * d))) act("NET_LISTEN");
            ImGui::EndDisabled();
        } else {
            ImGui::TextColored(green, "Menunggu pengirim...");
            ImGui::Text("Alamat: %s", S.addr.c_str());
            ImGui::Text("PIN:");
            ImGui::SameLine();
            ImGui::SetWindowFontScale(2.2f);
            ImGui::TextColored(orange, "%s", S.pin.c_str());
            ImGui::SetWindowFontScale(1.f);
            if (ImGui::Button("BERHENTI", ImVec2(-FLT_MIN, 58.f * d))) act("NET_STOP");
        }
        progressBlock(S.netBusy, S.netDone, S.netTotal, S.netSpeed, S.netCur);
        float th = ImGui::GetContentRegionAvail().y - 64.f * d;
        if (th < 110.f * d) th = 110.f * d;
        drawTerminal("term_recv", th);
        ImGui::BeginDisabled(S.listening);
        if (ImGui::Button("Kembali", ImVec2(-FLT_MIN, 48.f * d))) { S.step = 1; act("NET_RESET"); }
        ImGui::EndDisabled();
        break;
    }
    }
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
    ImGui::TextDisabled("v1.1 Beta");
    if (S.shizuku) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.f), "%s", S.shizukuMsg.c_str());
    } else {
        ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f), "%s", S.shizukuMsg.c_str());
        if (ImGui::Button("Hubungkan Shizuku")) act("CONNECT");
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
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    if (action == 0) io.AddMouseButtonEvent(0, true);
    else if (action == 2) {
        io.AddMouseButtonEvent(0, false);
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);  // hilangkan hover setelah jari diangkat
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

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_setNetStep(JNIEnv*, jclass, jint step) {
    std::lock_guard<std::mutex> lk(S.mu);
    S.step = step;
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

}  // extern "C"

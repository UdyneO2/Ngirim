#include <jni.h>
#include <GLES3/gl3.h>
#include <unistd.h>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"

namespace {

struct Item { std::string name; long long size = 0; bool sel = false; };

struct State {
    std::mutex mu;
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
    std::deque<std::string> actions;
    std::vector<std::string> logs;
    bool logDirty = false;
} S;

ImGuiContext* g_ctx = nullptr;
float g_density = 1.f;

const char* kChangelog[] = {
    "v1.1 Beta",
    "",
    "Baru",
    "- Transfer APK (termasuk split APK), foto, data game OBB dan DATA",
    "- Akses lewat Shizuku, tanpa root dan tanpa SAF",
    "- Live progress: persen, ukuran, kecepatan, ETA, file aktif",
    "- Mode Salin / Pindah, backup dan restore data game",
    "- Verifikasi ukuran file setelah disalin",
    "- Batalkan transfer kapan saja (file parsial dihapus)",
    "- UI ImGui (OpenGL ES 3) ramah sentuhan, drag untuk scroll",
    "",
    "Catatan Beta",
    "- Nama non-Latin bisa tampil sebagai '?'",
    "- Menghitung ukuran folder data besar bisa agak lama",
    "- Restore data game: pastikan game belum berjalan",
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
    const char* c = e->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) e->ReleaseStringUTFChars(s, c);
    return r;
}

// Drag sentuh = scroll. Dipanggil di dalam child window.
void touchScroll() {
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        ImGui::IsMouseDragging(0, 12.f * g_density)) {
        ImGui::ClearActiveID();  // batalkan klik checkbox saat drag
        ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
    }
}

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
                S.actions.push_back("ROUTE|" + std::to_string(i));
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("Segarkan")) S.actions.push_back("REFRESH");
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
        ImGui::TextDisabled(S.shizuku || S.route == 0 ? "Daftar kosong." : "Hubungkan Shizuku dulu.");
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
        ImGui::BeginDisabled(!(S.shizuku && n > 0));
        if (ImGui::Button("MULAI TRANSFER", ImVec2(-FLT_MIN, 58.f * d))) {
            std::string idx;
            for (int i = 0; i < (int)S.items.size(); i++)
                if (S.items[i].sel) idx += (idx.empty() ? "" : ",") + std::to_string(i);
            S.actions.push_back("START|" + std::to_string(S.route) + "|" + std::to_string(S.move) + "|" + idx);
            S.busy = true; S.done = 0; S.total = 0; S.speed = 0; S.cur = "Menyiapkan...";
        }
        ImGui::EndDisabled();
    } else {
        if (ImGui::Button("BATALKAN", ImVec2(-FLT_MIN, 58.f * d))) S.actions.push_back("CANCEL");
    }

    if (S.busy || S.total > 0) {
        float pct = S.total > 0 ? (float)((double)S.done / (double)S.total) : 0.f;
        ImGui::ProgressBar(pct, ImVec2(-FLT_MIN, 30.f * d));
        ImGui::TextWrapped("%s", S.cur.c_str());
        double eta = S.speed > 1 ? (double)(S.total - S.done) / S.speed : -1;
        ImGui::Text("%s / %s | %s/s | ETA %s", fmtSize((double)S.done).c_str(),
                    fmtSize((double)S.total).c_str(), fmtSize(S.speed).c_str(),
                    S.busy ? fmtEta(eta).c_str() : "selesai");
    }
}

void drawUI() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.f, 1.f), "Ngirim");
    ImGui::SameLine();
    ImGui::TextDisabled("v1.1 Beta");
    if (S.shizuku) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.f), "%s", S.shizukuMsg.c_str());
    } else {
        ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f), "%s", S.shizukuMsg.c_str());
        if (ImGui::Button("Hubungkan Shizuku")) S.actions.push_back("CONNECT");
    }

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Transfer")) { drawTransfer(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Log")) {
            ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
            for (auto& l : S.logs) ImGui::TextWrapped("%s", l.c_str());
            if (S.logDirty) { ImGui::SetScrollHereY(1.f); S.logDirty = false; }
            touchScroll();
            ImGui::EndChild();
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

        const char* font = "/system/fonts/Roboto-Regular.ttf";
        float px = 15.f * density;
        if (access(font, R_OK) == 0) io.Fonts->AddFontFromFileTTF(font, px);
        else { ImFontConfig c; c.SizePixels = px; io.Fonts->AddFontDefault(&c); }
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
    std::lock_guard<std::mutex> lk(S.mu);
    S.busy = busy; S.done = done; S.total = total; S.speed = speed; S.cur = js(e, cur);
}

JNIEXPORT void JNICALL Java_com_ngirim_app_Native_log(JNIEnv* e, jclass, jstring msg) {
    std::lock_guard<std::mutex> lk(S.mu);
    S.logs.push_back(js(e, msg));
    if (S.logs.size() > 500) S.logs.erase(S.logs.begin(), S.logs.begin() + 100);
    S.logDirty = true;
}

JNIEXPORT jstring JNICALL Java_com_ngirim_app_Native_pollAction(JNIEnv* e, jclass) {
    std::lock_guard<std::mutex> lk(S.mu);
    if (S.actions.empty()) return e->NewStringUTF("");
    std::string a = S.actions.front();
    S.actions.pop_front();
    return e->NewStringUTF(a.c_str());
}

}  // extern "C"

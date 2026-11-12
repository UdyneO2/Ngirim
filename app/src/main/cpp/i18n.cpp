// Tabel terjemahan dan mesin pencocokan. Teks sumber = bahasa Indonesia.
//
// Menambah bahasa:
//   1. Salin tabel kEn di bawah menjadi tabel baru (mis. kEs), terjemahkan sisi kanan.
//      Sisi kiri adalah teks sumber dan tidak boleh diubah. '*' = bagian dinamis, {1}, {2}, ... = isi
//      bagian itu pada terjemahan.
//   2. Daftarkan di kLangs. Aksara Latin langsung jalan; aksara lain butuh font tambahan.
#include "i18n.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#undef T

namespace i18n {
namespace {

struct Pair { const char* id; const char* tr; };

const Pair kEn[] = {
    {"Menunggu Shizuku...", "Waiting for Shizuku..."},
    {"Memeriksa...", "Checking..."},
    {"selesai", "done"},
    {"Naik", "Up"},
    {"Salin", "Copy"},
    {"Potong", "Cut"},
    {"Tempel", "Paste"},
    {"Tempel (%d)", "Paste (%d)"},
    {"Tempel (%d, potong)", "Paste (%d, cut)"},
    {"Hapus", "Delete"},
    {"Ubah nama", "Rename"},
    {"Folder baru", "New folder"},
    {"Buka", "Open"},
    {"Bagikan", "Share"},
    {"Pasang", "Install"},
    {"Ekstrak", "Extract"},
    {"Properti", "Properties"},
    {"Kirim", "Send"},
    {"Batal", "Cancel"},
    {"Semua", "All"},
    {"Segarkan", "Refresh"},
    {"Akses cepat", "Quick access"},
    {"Urutkan", "Sort"},
    {"Nama", "Name"},
    {"Ukuran", "Size"},
    {"Tanggal", "Date"},
    {"Jenis", "Type"},
    {"Cari...", "Search..."},
    {"Cari: %s (hapus)", "Search: %s (clear)"},
    {"Folder ini kosong.", "This folder is empty."},
    {"Tidak ada yang cocok dengan pencarian.", "Nothing matches the search."},
    {"%d item | %d dipilih (%s)%s", "%d items | %d selected (%s)%s"},
    {" | Clipboard: potong", " | Clipboard: cut"},
    {" | Clipboard: salin", " | Clipboard: copy"},
    {"BATALKAN", "CANCEL"},
    {"Lepas sematan", "Unpin"},
    {"Sematkan folder ini", "Pin this folder"},
    {"Sematkan folder", "Pin folder"},
    {"< Kembali ke file manager", "< Back to file manager"},
    {"Backup aplikasi", "App backup"},
    {"Format backup", "Backup format"},
    {"XAPK (satu file)", "XAPK (single file)"},
    {"APK + split (folder)", "APK + splits (folder)"},
    {"[x] Sertakan OBB game", "[x] Include game OBB"},
    {"[ ] Sertakan OBB game", "[ ] Include game OBB"},
    {"[x] Sertakan DATA game", "[x] Include game DATA"},
    {"[ ] Sertakan DATA game", "[ ] Include game DATA"},
    {"DATA bisa sangat besar dan bukan bagian standar XAPK; hanya terbaca installer yang mendukungnya.", "DATA can be very large and is not part of the standard XAPK; only installers that support it can read it."},
    {"Tujuan: %s", "Destination: %s"},
    {"Sembunyikan app sistem", "Hide system apps"},
    {"Tampilkan app sistem", "Show system apps"},
    {"Batal pilih", "Deselect"},
    {"Pilih semua", "Select all"},
    {"Tidak ada aplikasi.", "No apps."},
    {"%d aplikasi  |  %d dipilih (%s)", "%d apps  |  %d selected (%s)"},
    {"BACKUP %d APLIKASI KE XAPK", "BACK UP %d APPS TO XAPK"},
    {"BACKUP %d APLIKASI", "BACK UP %d APPS"},
    {"Menyiapkan...", "Preparing..."},
    {"Menyiapkan file...", "Preparing files..."},
    {"Berikan izin / periksa ulang", "Grant permission / recheck"},
    {"Shizuku harus terhubung dulu.", "Shizuku must be connected first."},
    {"Terima", "Receive"},
    {"Terima AKTIF - terlihat oleh perangkat lain", "Receive ON - visible to other devices"},
    {"Terima MATI", "Receive OFF"},
    {"Folder simpan: %s", "Save folder: %s"},
    {"(belum dipilih)", "(not chosen)"},
    {"Ubah folder simpan", "Change save folder"},
    {"Izinkan jelajah FTP ke /sdcard", "Allow FTP browsing of /sdcard"},
    {"Alamat: %s", "Address: %s"},
    {"Perangkat aktif", "Active devices"},
    {"IP manual", "Manual IP"},
    {"Belum ada. Nyalakan Terima di perangkat lain (satu Wi-Fi).", "None yet. Turn on Receive on another device (same Wi-Fi)."},
    {"Ketuk perangkat, lalu masukkan PIN dari layarnya.", "Tap a device, then enter the PIN shown on its screen."},
    {"Terhubung", "Connected"},
    {"Putus", "Disconnect"},
    {"Jelajah dan salin dua arah ada di tab FTP.", "Browsing and two-way copying are in the FTP tab."},
    {"Kosongkan", "Clear"},
    {"Belum ada file/folder dipilih.", "No files or folders chosen yet."},
    {"... dan %d lainnya", "... and %d more"},
    {"KIRIM SEKARANG", "SEND NOW"},
    {"Log terakhir", "Recent log"},
    {"HP ini", "This phone"},
    {"Perangkat lain", "Other device"},
    {"Pairing dulu di tab Wi-Fi.", "Pair first in the Wi-Fi tab."},
    {"Kosong", "Empty"},
    {"Panel aktif: %s", "Active panel: %s"},
    {"kiri", "left"},
    {"kanan", "right"},
    {"Pindah", "Move"},
    {"Dipilih: kiri %d, kanan %d", "Selected: left %d, right %d"},
    {"Kiri  >>  Kanan", "Left  >>  Right"},
    {"Kiri  <<  Kanan", "Left  <<  Right"},
    {"Tampilan", "Appearance"},
    {"Terang", "Light"},
    {"Gelap", "Dark"},
    {"Warna aksen", "Accent color"},
    {"Bahasa", "Language"},
    {"Otomatis", "Automatic"},
    {"Otomatis: ikuti bahasa sistem, selain Indonesia memakai Inggris.", "Automatic: follows the system language; anything other than Indonesian uses English."},
    {"Aktif: %s", "Active: %s"},
    {"Biru", "Blue"},
    {"Hijau", "Green"},
    {"Ungu", "Purple"},
    {"Oranye", "Orange"},
    {"Merah", "Red"},
    {"Teal", "Teal"},
    {"Merah muda", "Pink"},
    {"File manager", "File manager"},
    {"Tampilkan file tersembunyi", "Show hidden files"},
    {"Nama berawalan titik (.)", "Names starting with a dot (.)"},
    {"Gunakan tempat sampah", "Use trash"},
    {"Hapus memindahkan ke /sdcard/Ngirim/.Sampah (Android/data dan OBB dihapus permanen)", "Delete moves items to /sdcard/Ngirim/.Sampah (Android/data and OBB are deleted permanently)"},
    {"Buka tempat sampah", "Open trash"},
    {"Kosongkan sampah", "Empty trash"},
    {"Pasang APK", "Install APK"},
    {"Installer sistem", "System installer"},
    {"Shizuku (senyap)", "Shizuku (silent)"},
    {"Dipasang tanpa dialog lewat Shizuku (perlu Shizuku terhubung).", "Installed without a dialog through Shizuku (Shizuku must be connected)."},
    {"Memakai installer Android; perlu izin 'pasang aplikasi tak dikenal'.", "Uses the Android installer; needs the 'install unknown apps' permission."},
    {"Pembaruan", "Updates"},
    {"Versi terpasang: %s", "Installed version: %s"},
    {"Cek pembaruan saat dibuka", "Check for updates on launch"},
    {"Mengambil rilis terbaru dari GitHub", "Fetches the latest release from GitHub"},
    {"Cek pembaruan", "Check for updates"},
    {"Pembaruan dipasang lewat Shizuku bila terhubung, kalau tidak lewat installer sistem.", "Updates are installed through Shizuku when connected, otherwise through the system installer."},
    {"Backup APK", "Backup APK"},
    {"Folder hasil: %s", "Output folder: %s"},
    {"Ubah folder hasil", "Change output folder"},
    {"Jaringan", "Network"},
    {"Nama perangkat: %s", "Device name: %s"},
    {"Ubah nama perangkat", "Change device name"},
    {"Nama baru dipakai saat Terima dinyalakan ulang.", "The new name applies when Receive is turned on again."},
    {"Hubungkan Shizuku", "Connect Shizuku"},
    {"Buka app Shizuku", "Open Shizuku app"},
    {"Legacy: tanpa UserService, perintah dijalankan lewat Shizuku.Process.", "Legacy: no UserService; commands run through Shizuku.Process."},
    {"Tentang", "About"},
    {"Ngirim: Berbagi aplikasi dengan mudah", "Ngirim: Share apps with ease"},
    {"Kirim lewat Wi-Fi, kelola dua folder sekaligus, dan backup aplikasi ke XAPK, semuanya dalam satu app.", "Send over Wi-Fi, manage two folders side by side, and back up apps to XAPK, all in one app."},
    {"Versi %s", "Version %s"},
    {"Dibuat oleh UdyneO2. Dibangun dengan Dear ImGui, Shizuku API, dan Kotlin.", "Made by UdyneO2. Built with Dear ImGui, the Shizuku API, and Kotlin."},
    {"Riwayat perubahan", "Changelog"},
    {"Batasan yang diketahui", "Known limitations"},
    {"Pilih file / folder untuk dikirim", "Choose files / folders to send"},
    {"Pilih folder penyimpanan terima", "Choose the receive folder"},
    {"Pilih folder hasil backup XAPK", "Choose the XAPK backup folder"},
    {"Tutup", "Close"},
    {"Folder kosong / tidak bisa dibaca.", "Folder is empty or unreadable."},
    {"Tambahkan %d item", "Add %d items"},
    {"PILIH FOLDER INI", "CHOOSE THIS FOLDER"},
    {"Shizuku terputus", "Shizuku disconnected"},
    {"Lokal", "Local"},
    {"Setting", "Settings"},
    {"Bersihkan log", "Clear log"},
    {"Rilis stabil pertama", "First stable release"},
    {"8 Nov 2026 - gabungan v1.1 dan v1.2 beta", "8 Nov 2026 - combines v1.1 and v1.2 beta"},
    {"Koneksi Shizuku diperbaiki: tersambung sendiri setelah izin diberikan, tak lagi menggantung di 'Menunggu Shizuku'", "Shizuku connection fixed: it connects by itself once permission is granted, no longer stuck on 'Waiting for Shizuku'"},
    {"Ada tombol langsung ke app Shizuku dan pesan status yang lebih jelas", "A shortcut button to the Shizuku app and clearer status messages"},
    {"Semua fitur beta di bawah ikut di rilis ini", "Every beta feature listed below is included in this release"},
    {"Kirim lewat Wi-Fi", "Send over Wi-Fi"},
    {"Transfer antar-HP di Wi-Fi yang sama, dipasangkan dengan PIN 6 digit", "Phone-to-phone transfer on the same Wi-Fi, paired with a 6-digit PIN"},
    {"Kirim file dan folder lengkap dengan isinya; penerima memilih folder simpan", "Send files and whole folders with their contents; the receiver picks the save folder"},
    {"Backup dan restore APK (termasuk split), foto, serta data game OBB dan DATA", "Back up and restore APKs (including splits), photos, and game OBB and DATA"},
    {"Progress langsung: persen, kecepatan, sisa waktu; bisa dibatalkan kapan saja", "Live progress: percent, speed, time left; cancel any time"},
    {"Log gaya terminal dan akses file lewat Shizuku, tanpa root", "Terminal-style log and file access through Shizuku, no root needed"},
    {"FTP dan saklar Terima", "FTP and the Receive switch"},
    {"Tab FTP: dua folder berdampingan dengan pintasan folder dan favorit sendiri", "FTP tab: two folders side by side with folder shortcuts and your own favorites"},
    {"Panel kanan bisa menjelajah HP lain yang sudah dipasangkan, lalu salin ke dua arah", "The right panel can browse a paired phone, then copy in both directions"},
    {"Saklar Terima ON/OFF; perangkat aktif muncul otomatis di daftar", "Receive ON/OFF switch; active devices show up in the list automatically"},
    {"Perbaikan: tab Wi-Fi, FTP, dan lainnya tidak bisa diketuk", "Fix: the Wi-Fi, FTP, and other tabs could not be tapped"},
    {"Tampilan baru dan file manager", "New look and file manager"},
    {"Antarmuka ala Windows 10 dengan tema Terang dan Gelap", "Windows 10 style interface with Light and Dark themes"},
    {"Tab Lokal jadi file manager: salin, potong, tempel, hapus ke sampah, ubah nama, properti, cari", "The Local tab is now a file manager: copy, cut, paste, delete to trash, rename, properties, search"},
    {"Akses cepat ke folder penting, bisa menyematkan folder sendiri", "Quick access to key folders, and you can pin your own"},
    {"Backup APK langsung ke .xapk, OBB dan DATA game opsional", "Back up APKs straight to .xapk, with optional game OBB and DATA"},
    {"Tab Setting menggantikan Changelog; Terminal berganti nama jadi Log", "The Settings tab replaces Changelog; Terminal is now called Log"},
    {"Update langsung dan Legacy", "Live updates and Legacy"},
    {"Cek pembaruan dari GitHub Release, tampil catatan rilis lalu pasang langsung", "Check for updates from GitHub Releases, review the release notes, then install right away"},
    {"File manager: Buka, Bagikan, Pasang APK, dan Ekstrak ZIP", "File manager: Open, Share, Install APK, and Extract ZIP"},
    {"Pilihan pemasangan: installer sistem atau Shizuku (senyap)", "Install option: system installer or Shizuku (silent)"},
    {"Tombol Shizuku Legacy bila bind gagal; coba ulang otomatis dihapus", "A Shizuku Legacy button when binding fails; automatic retry removed"},
    {"Nama paket jadi com.udyneos.ngirim (terpasang sebagai aplikasi baru)", "Package name is now com.udyneos.ngirim (installs as a new app)"},
    {"FTP: kotak centang lebih kecil, nama panjang kini terbaca", "FTP: smaller checkboxes, long names are now readable"},
    {"Warna aksen dan bahasa", "Accent colors and languages"},
    {"Pilihan warna aksen di Setting", "Accent color choice in Settings"},
    {"Bahasa Indonesia dan Inggris, otomatis mengikuti bahasa sistem", "Indonesian and English, picked automatically from the system language"},
    {"Tabel bahasa terpisah, jadi bahasa baru mudah ditambahkan", "Language tables are separate, so new languages are easy to add"},
    {"Riwayat perubahan di Setting diringkas dalam satu bagian lipat", "The changelog in Settings is folded into a single collapsible section"},
    {"Data Wi-Fi tidak dienkripsi, hanya dijaga PIN", "Wi-Fi data is not encrypted, only protected by the PIN"},
    {"Nama non-Latin bisa tampil sebagai '?'", "Non-Latin names may show as '?'"},
    {"Router dengan client isolation memblokir koneksi antar-perangkat", "Routers with client isolation block device-to-device connections"},
    {"XAPK dengan isi di atas 4 GB belum didukung (batas ZIP)", "XAPK larger than 4 GB is not supported yet (ZIP limit)"},
    {"Shizuku terhubung", "Shizuku connected"},
    {"OK     service Shizuku aktif", "OK     Shizuku service active"},
    {"Service terputus - ketuk Shizuku Legacy atau Hubungkan Shizuku", "Service disconnected - tap Shizuku Legacy or Connect Shizuku"},
    {"Shizuku berhenti - start lagi lewat app Shizuku", "Shizuku stopped - start it again in the Shizuku app"},
    {"Shizuku belum berjalan - buka app Shizuku lalu Start", "Shizuku is not running - open the Shizuku app and tap Start"},
    {"Izin Shizuku ditolak - izinkan Ngirim di app Shizuku (otomatis tersambung)", "Shizuku permission denied - allow Ngirim in the Shizuku app (it connects automatically)"},
    {"Menunggu izin Shizuku - ketuk Izinkan pada dialog", "Waiting for Shizuku permission - tap Allow in the dialog"},
    {"Shizuku: bind UserService tidak merespons", "Shizuku: UserService bind did not respond"},
    {"Bind service Shizuku gagal. Coba Shizuku Legacy, atau Hubungkan Shizuku untuk bind manual.", "Shizuku service bind failed. Try Shizuku Legacy, or Connect Shizuku to bind manually."},
    {"Shizuku error: *", "Shizuku error: {1}"},
    {"Menyambung lewat Shizuku Legacy (Shizuku.Process)...", "Connecting through Shizuku Legacy (Shizuku.Process)..."},
    {"Shizuku terhubung (mode Legacy)", "Shizuku connected (Legacy mode)"},
    {"OK     service aktif lewat Shizuku Legacy", "OK     service active through Shizuku Legacy"},
    {"Shizuku Legacy gagal: *", "Shizuku Legacy failed: {1}"},
    {"Menghubungkan service Shizuku...", "Connecting Shizuku service..."},
    {"Gagal bind: *", "Bind failed: {1}"},
    {"ERR app Shizuku tidak ditemukan - pasang dari shizuku.rikka.app", "ERR Shizuku app not found - install it from shizuku.rikka.app"},
    {"Nama perangkat", "Device name"},
    {"Kosongkan tempat sampah?", "Empty the trash?"},
    {"Semua isi dihapus permanen.", "Everything inside is deleted permanently."},
    {"Tempat sampah sudah kosong", "The trash is already empty"},
    {"Mengosongkan sampah...", "Emptying trash..."},
    {"ERR sampah: *", "ERR trash: {1}"},
    {"Alamat penerima", "Receiver address"},
    {"ERR format harus IP:port", "ERR format must be IP:port"},
    {"Nama folder baru", "New folder name"},
    {"nama folder", "folder name"},
    {"ERR Shizuku belum siap", "ERR Shizuku is not ready"},
    {"Folder penerimaan: *", "Receive folder: {1}"},
    {"Folder hasil backup: *", "Backup output folder: {1}"},
    {"Wi-Fi: terhubung (*)\nIzin perangkat sekitar: OK", "Wi-Fi: connected ({1})\nNearby devices permission: OK"},
    {"Wi-Fi: terhubung (*)\nIzin perangkat sekitar: belum diberikan", "Wi-Fi: connected ({1})\nNearby devices permission: not granted"},
    {"Wi-Fi: TIDAK terhubung\nIzin perangkat sekitar: OK", "Wi-Fi: NOT connected\nNearby devices permission: OK"},
    {"Wi-Fi: TIDAK terhubung\nIzin perangkat sekitar: belum diberikan", "Wi-Fi: NOT connected\nNearby devices permission: not granted"},
    {"PIN dari *", "PIN from {1}"},
    {"6 digit", "6 digits"},
    {"OK     pairing dengan * (*:*)", "OK     paired with {1} ({2}:{3})"},
    {"ERR pairing gagal: *", "ERR pairing failed: {1}"},
    {"Mengirim * item ke *", "Sending {1} items to {2}"},
    {"ERR kirim: *", "ERR send: {1}"},
    {"ERR pilih folder tujuan dulu", "ERR choose a destination folder first"},
    {"Menunggu pengirim di *:* - PIN *", "Waiting for sender at {1}:{2} - PIN {3}"},
    {"Belum terhubung ke perangkat lain", "Not connected to another device"},
    {"Gagal memuat", "Failed to load"},
    {"Shizuku belum siap", "Shizuku is not ready"},
    {"Tidak ada item terpilih", "No items selected"},
    {"Tidak bisa menyalin folder ke dalam dirinya sendiri: *", "Cannot copy a folder into itself: {1}"},
    {"Sumber dan tujuan sama", "Source and destination are the same"},
    {"Mulai: * item, pindah ke *", "Start: {1} items, move to {2}"},
    {"Mulai: * item, salin ke *", "Start: {1} items, copy to {2}"},
    {"Kirim * item ke *:*", "Send {1} items to {2}:{3}"},
    {"Ambil * item dari * ke *", "Fetch {1} items from {2} to {3}"},
    {"gagal memulai: *", "failed to start: {1}"},
    {"Clipboard: * item (potong)", "Clipboard: {1} items (cut)"},
    {"Clipboard: * item (salin)", "Clipboard: {1} items (copy)"},
    {"* item masuk daftar kirim - pairing dulu di tab Wi-Fi", "{1} items added to the send list - pair first in the Wi-Fi tab"},
    {"* item masuk daftar kirim", "{1} items added to the send list"},
    {"gagal mengekstrak: *", "failed to extract: {1}"},
    {"Mengekstrak *...", "Extracting {1}..."},
    {"Cari di folder ini", "Search in this folder"},
    {"nama berisi...", "name contains..."},
    {"ERR    menyiapkan file: *", "ERR    preparing files: {1}"},
    {"Shizuku belum siap dan file tidak terbaca langsung: *", "Shizuku is not ready and the file cannot be read directly: {1}"},
    {"gagal menyiapkan file: *", "failed to prepare files: {1}"},
    {"ERR    tidak ada aplikasi untuk membuka *", "ERR    no app to open {1}"},
    {"Bagikan lewat", "Share via"},
    {"ERR    tidak ada aplikasi untuk berbagi", "ERR    no app to share with"},
    {"Memasang * lewat Shizuku...", "Installing {1} through Shizuku..."},
    {"ERR    pasang: *", "ERR    install: {1}"},
    {"Shizuku belum siap - memakai installer sistem", "Shizuku is not ready - using the system installer"},
    {"Izinkan 'pasang aplikasi tak dikenal' untuk Ngirim, lalu pasang lagi", "Allow 'install unknown apps' for Ngirim, then install again"},
    {"ERR    installer sistem tidak ditemukan", "ERR    system installer not found"},
    {"Memeriksa pembaruan...", "Checking for updates..."},
    {"Belum ada rilis di GitHub.", "No releases on GitHub yet."},
    {"Sudah versi terbaru (*); rilis terakhir *.", "Already up to date ({1}); latest release {2}."},
    {"Pembaruan * tersedia.", "Update {1} available."},
    {"Gagal memeriksa pembaruan: *", "Failed to check for updates: {1}"},
    {"ERR    cek pembaruan: *", "ERR    update check: {1}"},
    {"(Tidak ada catatan rilis)", "(No release notes)"},
    {"dipasang lewat Shizuku", "installed through Shizuku"},
    {"dipasang lewat installer sistem", "installed through the system installer"},
    {"Terpasang", "Installed"},
    {"Ukuran unduhan", "Download size"},
    {"Yang baru", "What's new"},
    {"Perbarui", "Update"},
    {"Nanti", "Later"},
    {"Pembaruan *", "Update {1}"},
    {"Memperbarui ke *", "Updating to {1}"},
    {"Mengunduh *...", "Downloading {1}..."},
    {"OK     unduhan selesai: *", "OK     download finished: {1}"},
    {"Pembaruan gagal: *", "Update failed: {1}"},
    {"ERR    pembaruan: *", "ERR    update: {1}"},
    {"Tidak bisa menempel folder ke dalam dirinya sendiri: *", "Cannot paste a folder into itself: {1}"},
    {"Lewati *: sudah di folder ini", "Skip {1}: already in this folder"},
    {"Pindah * item ke *", "Move {1} items to {2}"},
    {"Salin * item ke *", "Copy {1} items to {2}"},
    {"Dihapus permanen dan tidak bisa dikembalikan.", "Deleted permanently and cannot be restored."},
    {"Dipindahkan ke tempat sampah (*).", "Moved to the trash ({1})."},
    {"Menghapus...", "Deleting..."},
    {"gagal menghapus: *", "failed to delete: {1}"},
    {"nama baru", "new name"},
    {"ERR    gagal mengubah nama (nama sudah ada?)", "ERR    rename failed (name already exists?)"},
    {"Lokasi", "Location"},
    {"Diubah", "Modified"},
    {"Total ukuran", "Total size"},
    {"Folder", "Folder"},
    {"Gagal membaca properti: *", "Failed to read properties: {1}"},
    {"Cari aplikasi", "Search apps"},
    {"nama atau paket", "name or package"},
    {"Tidak ada aplikasi terpilih", "No apps selected"},
    {"Backup APK: * aplikasi (* file) ke *", "Backup APK: {1} apps ({2} files) to {3}"},
    {"Backup XAPK: * aplikasi* ke *", "Backup XAPK: {1} apps{2} to {3}"},
    {"Transfer lain masih berjalan", "Another transfer is still running"},
    {"GAGAL  *: *", "FAILED {1}: {2}"},
    {"Dibatalkan: * sukses, * gagal", "Cancelled: {1} succeeded, {2} failed"},
    {"Selesai: * sukses, * gagal", "Done: {1} succeeded, {2} failed"},
    {"Folder tujuan tidak valid", "Invalid destination folder"},
    {"Server terima aktif di port *, simpan ke *", "Receive server active on port {1}, saving to {2}"},
    {"Server terima berhenti", "Receive server stopped"},
    {"PIN salah", "Wrong PIN"},
    {"ERR PIN salah dari * (*)", "ERR wrong PIN from {1} ({2})"},
    {"Server dihentikan: terlalu banyak PIN salah", "Server stopped: too many wrong PINs"},
    {"OK     pairing dengan * (*)", "OK     paired with {1} ({2})"},
    {"Pengirim terhubung: * (*)", "Sender connected: {1} ({2})"},
    {"ERR jelajah dari *: *", "ERR browse from {1}: {2}"},
    {"Akses jelajah dimatikan di perangkat penerima", "Browsing is turned off on the receiving device"},
    {"Path ditolak", "Path rejected"},
    {"Path ditolak: *", "Path rejected: {1}"},
    {"* mengambil * item", "{1} is fetching {2} items"},
    {"* mengirim ke *", "{1} is sending to {2}"},
    {"Perangkat sedang sibuk", "Device is busy"},
    {"ERR sesi dibatalkan", "ERR session cancelled"},
    {"Sesi dibatalkan", "Session cancelled"},
    {"ERR sesi: *", "ERR session: {1}"},
    {"Sesi gagal: *", "Session failed: {1}"},
    {"Daftar tidak valid", "Invalid list"},
    {"Bukan folder", "Not a folder"},
    {"Manifest tidak valid", "Invalid manifest"},
    {"Ruang penyimpanan tidak cukup", "Not enough storage space"},
    {"Ruang tidak cukup (butuh *)", "Not enough space (need {1})"},
    {"Koneksi terputus", "Connection lost"},
    {"Selesai: * file diterima dari * (*)", "Done: {1} files received from {2} ({3})"},
    {"Menyiapkan * entri (* file, *)", "Preparing {1} entries ({2} files, {3})"},
    {"Ditolak penerima: *", "Rejected by receiver: {1}"},
    {"File berubah saat dikirim: *", "File changed while sending: {1}"},
    {"Penerima gagal menyimpan", "Receiver failed to save"},
    {"Selesai: * file terkirim ke * (*)", "Done: {1} files sent to {2} ({3})"},
    {"ERR dibatalkan", "ERR cancelled"},
    {"Dibatalkan", "Cancelled"},
    {"Gagal: *", "Failed: {1}"},
    {"OK     terautentikasi ke *", "OK     authenticated to {1}"},
    {"Ditolak: *", "Rejected: {1}"},
    {"tidak bisa dihapus", "cannot be deleted"},
    {"salinan ke sampah ok, asli tidak terhapus", "copy to trash ok, original not deleted"},
    {"OK     hapus *", "OK     deleted {1}"},
    {"GAGAL  hapus *: *", "FAILED delete {1}: {2}"},
    {"* dihapus, * gagal", "{1} deleted, {2} failed"},
    {"OK     terpasang *", "OK     installed {1}"},
    {"Pasang APK: * berhasil, * gagal", "Install APK: {1} succeeded, {2} failed"},
    {"entri tidak aman: *", "unsafe entry: {1}"},
    {"Diekstrak * file ke *", "Extracted {1} files to {2}"},
    {"melebihi batas ZIP 4 GB - matikan opsi OBB/DATA", "exceeds the 4 GB ZIP limit - turn off the OBB/DATA option"},
    {"gagal menamai hasil", "failed to name the result"},
    {"* XAPK dibuat, * gagal", "{1} XAPK created, {2} failed"},
    {"Bukan server Ngirim", "Not a Ngirim server"},
    {"Versi protokol berbeda - perbarui Ngirim di kedua perangkat", "Protocol version differs - update Ngirim on both devices"},
    {"path harus absolut: *", "path must be absolute: {1}"},
    {"akses di luar /sdcard ditolak: *", "access outside /sdcard denied: {1}"},
    {"path ilegal: *", "illegal path: {1}"},
    {"path keluar folder: *", "path escapes the folder: {1}"},
    {"Ukuran tidak cocok: *", "Size mismatch: {1}"},
    {"ERR NSD gagal (*) - pakai IP manual", "ERR NSD failed ({1}) - use manual IP"},
    {"NSD terdaftar: *", "NSD registered: {1}"},
    {"ERR pencarian gagal (*)", "ERR discovery failed ({1})"},
    {"GitHub membalas HTTP *", "GitHub replied with HTTP {1}"},
    {"Unduhan gagal: HTTP *", "Download failed: HTTP {1}"},
    {"Unduhan tidak lengkap (* dari *)", "Download incomplete ({1} of {2})"},
    {"Proses Shizuku terputus", "Shizuku process disconnected"},
    {"Shizuku.Process gagal: *", "Shizuku.Process failed: {1}"},
    {"proses shell tidak merespons", "shell process did not respond"},
    {"Hapus *?", "Delete {1}?"},
    {"Server dihentikan", "Server stopped"},
    {"TIDAK terhubung", "NOT connected"},
    {"belum diberikan", "not granted"},
    {"terhubung (*)", "connected ({1})"},
    {"Selesai: *", "Done: {1}"},
    {"Izin perangkat sekitar: *", "Nearby devices permission: {1}"},
    {"Arsipkan", "Zip"},
    {"Membuat arsip *...", "Creating archive {1}..."},
    {"gagal membuat arsip: *", "failed to create archive: {1}"},
    {"Arsip dibuat: * (* file)", "Archive created: {1} ({2} files)"},
    {"lewati *: *", "skipped {1}: {2}"},
    {"Tampilkan QR", "Show QR"},
    {"Sembunyikan QR", "Hide QR"},
    {"QR tidak bisa dibuat.", "Could not create the QR code."},
    {"Pindai dengan kamera di perangkat pengirim; Ngirim terbuka dan memasangkan otomatis.", "Scan with the camera on the sending device; Ngirim opens and pairs automatically."},
    {"Pasangkan dengan *?", "Pair with {1}?"},
    {"Alamat *. Pasangkan hanya dengan perangkat yang kamu kenal.", "Address {1}. Only pair with devices you know."},
    {"Pasangkan", "Pair"},
    {"QR tidak valid", "Invalid QR"},
    {"OK     pairing dengan * (*) lewat QR", "OK     paired with {1} ({2}) via QR"},
    {"Wi-Fi: *", "Wi-Fi: {1}"},
    {"Arsip ZIP dan QR", "ZIP archives and QR"},
    {"Tombol Arsipkan di file manager: buat .zip dari file/folder terpilih", "Zip button in the file manager: create a .zip from the selected files/folders"},
    {"Pairing lewat QR: tampilkan QR di penerima, pindai dengan kamera pengirim", "QR pairing: show a QR on the receiver, scan it with the sender's camera"},
    {"Pindai QR", "Scan QR"},
    {"Arahkan kamera ke QR di perangkat penerima", "Point the camera at the QR on the receiving device"},
    {"pemindai QR gagal dibuka: *", "QR scanner failed to open: {1}"},
    {"Pemindai QR dan ikon baru", "QR scanner and new icon"},
    {"Tombol Pindai QR di tab Wi-Fi: kamera langsung di dalam Ngirim", "Scan QR button on the Wi-Fi tab: camera right inside Ngirim"},
    {"Ikon aplikasi baru", "New app icon"},
    {"Melanjutkan: * sudah ada", "Resuming: {1} already there"},
    {"Melanjutkan: * sudah diterima", "Resuming: {1} already received"},
    {"Koneksi terputus (*), mencoba lanjut */*...", "Connection lost ({1}), trying to resume {2}/{3}..."},
    {"Gagal: *. Kirim ulang untuk melanjutkan dari titik terakhir.", "Failed: {1}. Send again to resume from where it stopped."},
    {"Offset lanjutan tidak valid", "Invalid resume offset"},
    {"Gagal menyimpan *", "Failed to save {1}"},
    {"Lanjutkan transfer", "Resume transfers"},
    {"Transfer yang putus dilanjutkan dari byte terakhir, bukan mulai dari nol", "Interrupted transfers resume from the last byte instead of starting over"},
    {"Otomatis mencoba menyambung lagi sampai 4 kali saat koneksi terputus", "Automatically retries up to 4 times when the connection drops"},
    {"Berlaku untuk kirim, terima, dan FTP dua panel", "Works for send, receive and the two-pane FTP"},
    {"Memasang *...", "Installing {1}..."},
    {"Pasang * butuh Shizuku", "Installing {1} needs Shizuku"},
    {"gagal memasang: *", "install failed: {1}"},
    {"Memasang *: * APK", "Installing {1}: {2} APKs"},
    {"Memasang *: * APK, * berkas OBB/data", "Installing {1}: {2} APKs, {3} OBB/data files"},
    {"OK     APK terpasang", "OK     APK installed"},
    {"OK     APK terpasang (*)", "OK     APK installed ({1})"},
    {"Terpasang: * (* APK)", "Installed: {1} ({2} APKs)"},
    {"Terpasang: * (* APK, * OBB/data)", "Installed: {1} ({2} APKs, {3} OBB/data)"},
    {"tidak ada APK di dalam *", "no APK found inside {1}"},
    {"sesi pemasangan gagal: *", "install session failed: {1}"},
    {"Pratinjau", "Preview"},
    {"Pas", "Fit"},
    {"Memuat...", "Loading..."},
    {"Gagal memuat gambar", "Could not load the image"},
    {"Buka folder", "Open folder"},
    {"%d dipilih", "%d selected"},
    {"Tekan sekali lagi untuk keluar", "Press back again to exit"},
    {"Menu klik kanan dan Kembali", "Context menu and Back"},
    {"Tekan lama file, folder, atau APK di file manager untuk membuka menu seperti klik kanan", "Long-press a file, folder or APK in the file manager to open a right-click style menu"},
    {"Kembali: naik folder, lalu pindah ke Lokal, lalu tekan 2x untuk keluar", "Back: go up a folder, then switch to Local, then press twice to exit"},
    {"File manager jadi halaman utama", "The file manager is now the home page"},
    {"Lisensi", "License"},
    {"Boleh dipakai, diubah, dan dibagikan selama pemberitahuan hak cipta ini disertakan. Tanpa jaminan.", "Free to use, modify and share as long as this copyright notice is included. No warranty."},
    {"Teks lengkap ada di berkas LICENSE di repositori.", "The full text is in the LICENSE file in the repository."},
    {"Menu ringkas dan lisensi", "Compact menu and license"},
    {"Tombol aksi di atas Akses cepat dihapus; semua ada di menu tekan-lama", "The action buttons above Quick access are gone; everything is in the long-press menu"},
    {"Menu lebih rapat, dan tekan lama di area kosong membuka Tempel / Folder baru", "Tighter menu, and a long-press on empty space opens Paste / New folder"},
    {"Lisensi MIT ditambahkan, lengkap dengan lisensi pustaka pihak ketiga", "MIT license added, with the licenses of third-party libraries"},
    {"Clipboard: potong", "Clipboard: cut"},
    {"Clipboard: salin", "Clipboard: copy"},
    {"nyala", "on"},
    {"mati", "off"},
    {"Baru", "New"},
    {"Tampilan baru", "New look"},
    {"Seluruh tampilan didesain ulang: bilah judul biru, tab berikon, bilah status di bawah, panel berbingkai", "The whole interface was redesigned: blue title bar, icon tabs, a status bar at the bottom, framed panels"},
    {"Font Source Sans 3 (lisensi OFL), toggle bentuk pil, kotak centang dan progress bar membulat", "Source Sans 3 font (OFL), pill-shaped toggles, rounded checkboxes and progress bars"},
    {"Tema terang dan gelap mengikuti palet baru", "Light and dark themes follow the new palette"},
};

struct Table { const char* code; const char* name; const Pair* pairs; size_t n; };
const Table kLangs[] = {
    {"id", "Bahasa Indonesia", nullptr, 0},
    {"en", "English", kEn, sizeof(kEn) / sizeof(kEn[0])},
};
const size_t kLangCount = sizeof(kLangs) / sizeof(kLangs[0]);

struct Pattern { std::vector<std::string> lit; std::string out; };  // lit = potongan di antara '*'

struct Active {
    std::string code = "id";
    std::unordered_map<std::string, std::string> exact;
    std::vector<Pattern> patterns;
} g_act;
std::mutex g_mu;

const Table* find(const std::string& code) {
    for (const Table& t : kLangs) if (code == t.code) return &t;
    return nullptr;
}

void load(const Table* t) {
    g_act.exact.clear();
    g_act.patterns.clear();
    g_act.code = t->code;
    for (size_t i = 0; i < t->n; i++) {
        const std::string id = t->pairs[i].id;
        if (id.find('*') == std::string::npos) { g_act.exact[id] = t->pairs[i].tr; continue; }
        Pattern p;
        size_t pos = 0;
        while (true) {
            size_t s = id.find('*', pos);
            if (s == std::string::npos) { p.lit.push_back(id.substr(pos)); break; }
            p.lit.push_back(id.substr(pos, s - pos));
            pos = s + 1;
        }
        p.out = t->pairs[i].tr;
        g_act.patterns.push_back(std::move(p));
    }
    // Pola yang lebih spesifik (teks tetap lebih panjang) dicoba lebih dulu.
    auto weight = [](const Pattern& p) { size_t w = 0; for (auto& l : p.lit) w += l.size(); return w; };
    std::stable_sort(g_act.patterns.begin(), g_act.patterns.end(),
                     [&](const Pattern& a, const Pattern& b) { return weight(a) > weight(b); });
}

bool matchPattern(const Pattern& p, const std::string& s, std::vector<std::string>& caps) {
    const std::string& first = p.lit.front();
    if (s.compare(0, first.size(), first) != 0) return false;
    size_t pos = first.size();
    caps.clear();
    const size_t k = p.lit.size();
    for (size_t i = 1; i < k; i++) {
        const std::string& l = p.lit[i];
        if (i == k - 1) {
            if (s.size() < pos + l.size()) return false;
            if (!l.empty() && s.compare(s.size() - l.size(), l.size(), l) != 0) return false;
            caps.push_back(s.substr(pos, s.size() - l.size() - pos));
        } else {
            if (l.empty()) return false;
            size_t f = s.find(l, pos);
            if (f == std::string::npos) return false;
            caps.push_back(s.substr(pos, f - pos));
            pos = f + l.size();
        }
    }
    return true;
}

std::string trLocked(const std::string& s, int depth) {
    auto it = g_act.exact.find(s);
    if (it != g_act.exact.end()) return it->second;
    std::vector<std::string> caps;
    for (const Pattern& p : g_act.patterns) {
        if (!matchPattern(p, s, caps)) continue;
        std::string out;
        for (size_t i = 0; i < p.out.size(); i++) {
            if (p.out[i] == '{' && i + 2 < p.out.size() + 0 && p.out[i + 2] == '}' && p.out[i + 1] >= '1' && p.out[i + 1] <= '9') {
                size_t idx = (size_t)(p.out[i + 1] - '1');
                if (idx < caps.size()) {
                    // isi dinamis sendiri bisa berupa teks sumber (mis. pesan galat)
                    out += depth < 2 ? trLocked(caps[idx], depth + 1) : caps[idx];
                }
                i += 2;
            } else out += p.out[i];
        }
        return out;
    }
    return s;
}

}  // namespace

const LangInfo* languages(int& count) {
    static LangInfo infos[sizeof(kLangs) / sizeof(kLangs[0])];
    for (size_t i = 0; i < kLangCount; i++) infos[i] = {kLangs[i].code, kLangs[i].name};
    count = (int)kLangCount;
    return infos;
}

void setLanguage(const std::string& pref, const std::string& sys) {
    std::string code = pref;
    if (code == "auto" || !find(code)) code = find(sys) ? sys : "en";
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_act.code == code && (!g_act.exact.empty() || code == "id")) return;
    load(find(code));
}

std::string current() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_act.code;
}

std::string tr(const std::string& s) {
    if (s.empty()) return s;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_act.code == "id") return s;
    return trLocked(s, 0);
}

const char* T(const char* s) {
    static std::string ring[32];
    static unsigned idx = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_act.code == "id") return s;
    }
    std::string in = s;
    size_t h = in.find("##");
    std::string base = h == std::string::npos ? in : in.substr(0, h);
    std::string suffix = h == std::string::npos ? "" : in.substr(h);
    std::string out = tr(base);
    if (out == base && suffix.empty()) return s;
    std::string& slot = ring[idx++ % 32];
    slot = out + suffix;
    return slot.c_str();
}

}  // namespace i18n

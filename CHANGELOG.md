# Changelog Ngirim

## v1.1-beta - 2026-11-08

### Baru
- Transfer antar-perangkat lewat Wi-Fi yang sama: izin, pilih Kirim/Terima, pairing PIN, kirim, progress
- Penerima memilih folder penyimpanan sendiri
- Transfer folder (rekursif) dan lokasi kustom lewat file browser bawaan
- Rute Kustom di transfer lokal (pilih folder sumber dan tujuan sendiri)
- Log gaya terminal dengan timestamp dan warna
- Transfer APK (termasuk split APK), foto, serta data game OBB dan DATA
- Akses lewat Shizuku, tanpa root dan tanpa SAF
- Live progress: persen, ukuran, kecepatan, ETA, dan file yang sedang diproses
- Mode Salin / Pindah, serta backup dan restore data game
- UI ImGui (OpenGL ES 3) ramah sentuhan, drag untuk scroll
- Build rilis bertanda tangan otomatis lewat GitHub Actions
- README

### Catatan Beta
- Data Wi-Fi tidak dienkripsi (otentikasi PIN saja)
- Nama non-Latin bisa tampil sebagai "?"
- Router dengan client isolation memblokir koneksi antar-perangkat
- Restore data game: pastikan game tidak sedang berjalan

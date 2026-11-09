# Changelog Ngirim

## v2.0 - 2026-11-09

### Perbaikan
- Bind UserService Shizuku gagal 3x: otomatis pindah ke jalur cadangan via Shizuku.Process (tombol Hubungkan Shizuku mencoba bind lagi)
- FTP: kotak centang dan ikon diperkecil, nama panjang dibungkus hingga 3 baris

### Baru
- **UI bergaya Windows 10**: kontrol datar, aksen biru #0078D7, ikon file/folder, tema Terang (default) dan Gelap
- **Tab Lokal menjadi file manager**: salin, potong, tempel, hapus (tempat sampah), ganti nama, folder baru,
  properti, urutkan, cari, file tersembunyi, dan akses cepat ke folder (bisa disematkan)
- **Backup APK ke XAPK** lewat tombol di tab Lokal: pemilih aplikasi, manifest XAPK, split APK, OBB opsional,
  DATA game opsional; atau APK + split dalam folder
- **Tab Pengaturan** menggantikan Changelog; riwayat perubahan dipindah ke bagian bawah Pengaturan
- Terminal menjadi **tab Log**, diletakkan setelah Pengaturan (urutan tab: Lokal, Wi-Fi, FTP, Pengaturan, Log)

### Berubah
- Rute lama (Foto DCIM, backup/restore OBB dan DATA, Kustom) diganti file manager dan akses cepat
- Folder hasil XAPK dapat diubah di Pengaturan (default `/sdcard/Ngirim/XAPK`)

## v1.0 - 2026-11-08 (rilis final)

Menggabungkan semua fitur v1.1-beta dan v1.2-beta, ditambah:

### Perbaikan
- Status "Menunggu Shizuku" tidak lagi menggantung: koneksi dicek ulang tiap 2 detik dan otomatis tersambung
  setelah izin diberikan (termasuk lewat daftar aplikasi di app Shizuku); service yang macet di-bind ulang
  tiap 6 detik dengan nama proses bergantian
- Pesan status Shizuku lebih jelas, ada tombol *Buka app Shizuku*

## v1.2-beta - 2026-11-08

### Baru
- **Tab FTP**: dua panel berdampingan (kiri HP ini, kanan HP ini atau perangkat yang sudah dipairing), pintasan folder bawaan (Internal, Download, DCIM, Pictures, Data, OBB, Ngirim) dan pintasan favorit sendiri
- Salin/pindah antar panel; ke/dari perangkat lain lewat Wi-Fi (jelajah folder jarak jauh, unduh, unggah ke folder pilihan)
- **Saklar Terima ON/OFF** di tab Wi-Fi (folder simpan default `/sdcard/Ngirim/Terima`)
- **Perangkat aktif** tampil otomatis tanpa menekan Cari; perangkat yang mati hilang sendiri
- Saklar "Izinkan jelajah FTP ke /sdcard" di penerima (default mati)
- Protokol jaringan v2 (mode LIST, PULL, PUSH); v1.1 dan v1.2 tidak saling kompatibel

### Perbaikan
- Tab Wi-Fi, FTP, Terminal, dan Changelog tidak bisa di-tap (ImGui butuh satu frame hover sebelum tekan)
- Server penerima melayani koneksi paralel; pengecekan perangkat aktif tidak lagi memunculkan error

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

### Catatan
- Data Wi-Fi tidak dienkripsi (otentikasi PIN saja)
- Nama non-Latin bisa tampil sebagai "?"
- Router dengan client isolation memblokir koneksi antar-perangkat
- Restore data game: pastikan game tidak sedang berjalan

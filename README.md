# Ngirim

Berbagi aplikasi dengan mudah. Ngirim menggabungkan pengirim file lewat Wi-Fi, FTP dua panel, dan
file manager dalam satu aplikasi Android. Akses ke `Android/data` dan `Android/obb` lewat Shizuku, jadi
tanpa root dan tanpa SAF.

Versi sekarang v2.1. Riwayat lengkap ada di [CHANGELOG.md](CHANGELOG.md), rilis di
[GitHub Releases](https://github.com/UdyneO2/Ngirim/releases).

## Yang sudah ada

Kirim dan terima
- Transfer antar-HP di Wi-Fi yang sama, dipasangkan dengan PIN 6 digit
- Kirim file maupun folder lengkap dengan isinya
- Saklar Terima ON/OFF; HP lain yang aktif muncul sendiri di daftar
- Progress langsung (persen, kecepatan, sisa waktu) dan bisa dibatalkan

FTP dua panel
- Dua folder berdampingan dengan pintasan folder dan favorit sendiri
- Panel kanan bisa diarahkan ke HP lain yang sudah dipasangkan, salin ke dua arah

File manager (tab Lokal)
- Salin, potong, tempel, hapus (ke tempat sampah), ubah nama, folder baru, properti, cari, urutkan
- Buka file, bagikan, pasang APK, ekstrak ZIP
- Akses cepat ke Download, DCIM, Pictures, Data, OBB, dan folder yang kamu sematkan

Backup aplikasi
- Pilih beberapa aplikasi lalu simpan langsung jadi `.xapk` (manifest, ikon, base + split APK)
- OBB dan DATA game bisa ikut disertakan

Lainnya
- Tema Terang dan Gelap, tampilan terinspirasi Windows 10
- Cek pembaruan dari GitHub Release dengan catatan rilis, lalu pasang langsung
- Mode pasang APK: installer sistem atau Shizuku (senyap)
- Shizuku Legacy: jalur cadangan lewat Shizuku.Process bila bind service gagal
- Log dengan timestamp dan warna

## Rencana

Belum janji, ini baru daftar ide:
- Enkripsi transfer (TLS) supaya aman di jaringan umum
- Lanjutkan transfer yang terputus
- Pairing lewat QR agar tidak perlu mengetik PIN
- Kirim ke beberapa perangkat sekaligus
- Pasang XAPK/APKS langsung dari file manager
- Buat arsip ZIP, pratinjau gambar, dan multi-pilih lewat geser
- Pilihan warna aksen dan bahasa Inggris

Punya usul? Buka issue di repo.

## Persyaratan

- Android 11 atau lebih baru (minSdk 30), target Android 15
- [Shizuku](https://shizuku.rikka.app/) terpasang dan sedang berjalan (wireless debugging atau root)
- Untuk Wi-Fi: dua perangkat di jaringan/hotspot yang sama, dan izin Perangkat di sekitar (Android 13+)

## Kredit dan sumber

- [Dear ImGui](https://github.com/ocornut/imgui) oleh Omar Cornut, lisensi MIT. Dipakai untuk seluruh antarmuka.
- [Shizuku](https://github.com/RikkaApps/Shizuku) dan Shizuku-API oleh RikkaApps, lisensi Apache 2.0. Dasar akses file tanpa root.
- Font Roboto bawaan sistem Android; font monospace sistem untuk tab Log.
- Android NSD (mDNS) untuk menemukan perangkat, GitHub Releases API untuk cek pembaruan.
- Gaya visual terinspirasi antarmuka Windows 10.
- Dibuat dan dirawat oleh [UdyneO2](https://github.com/UdyneO2).

## Cara pakai

### File manager (tab Lokal)
1. Buka Ngirim, pastikan status Shizuku hijau (ketuk **Hubungkan Shizuku** lalu izinkan jika perlu).
2. Ketuk folder untuk masuk, atau pakai *Akses cepat* dan *Naik*. Ketuk kotak centang untuk memilih.
3. **Salin** atau **Potong**, pindah ke folder tujuan, lalu **Tempel**. **Hapus** memindahkan ke
   `/sdcard/Ngirim/.Sampah` (bisa dimatikan di Pengaturan; isi `Android/data` dan `Android/obb` dihapus permanen).
4. **Sematkan folder ini** menambahkan pintasan favorit. **Kirim** memasukkan item terpilih ke daftar kirim Wi-Fi.

### Backup APK ke XAPK
1. Di tab Lokal ketuk **Backup APK**, pilih format (XAPK atau APK + split dalam folder).
2. Centang aplikasi (*Tampilkan app sistem* bila perlu). Opsional: **Sertakan OBB** dan **Sertakan DATA game**.
3. Ketuk **BACKUP ... KE XAPK**. Hasil di `/sdcard/Ngirim/XAPK` (ubah di Pengaturan).

DATA bukan bagian standar XAPK; berkas disimpan di `Android/data/<paket>/` dalam arsip dan dicantumkan di
`expansions` pada manifest. Batas ZIP 4 GB berlaku.

### Transfer lewat Wi-Fi (tab Wi-Fi)
1. **Izin**: bila status belum hijau, ketuk *Berikan izin / periksa ulang*.
2. **Perangkat penerima**: nyalakan saklar **Terima**. Perangkat langsung terlihat aktif oleh perangkat lain;
   layar menampilkan `IP:port` dan **PIN 6 digit**. Folder simpan default `/sdcard/Ngirim/Terima` (bisa diubah).
3. **Perangkat pengirim**: bagian *Perangkat aktif* terisi otomatis (ketuk *Segarkan* atau *IP manual* bila
   perlu). Ketuk perangkat, masukkan PIN (pairing).
4. Tambahkan file/folder lewat *+ File / folder*, lalu **KIRIM SEKARANG**.

Folder dikirim lengkap beserta isi dan subfolder. Jika nama file sudah ada di penerima, file baru
disimpan dengan akhiran ` (1)`, ` (2)`, dan seterusnya (tidak menimpa).

### FTP dua panel (tab FTP)
1. Pilih panel (ketuk di dalamnya), lalu ketuk pintasan folder, *Naik*, atau ketuk folder untuk masuk.
   *Simpan pintasan* menambah folder saat ini ke daftar pintasan favorit.
2. Panel kanan: pilih *HP ini* atau nama perangkat yang sudah dipairing.
3. Centang item di satu panel, lalu **Kiri >> Kanan** atau **Kiri << Kanan**. Salin/Pindah berlaku untuk
   dua panel lokal; ke/dari perangkat lain hanya salin.
4. Agar perangkat lain bisa menjelajah, di perangkat itu nyalakan Terima **dan** *Izinkan jelajah FTP*.

## Keamanan

- Pairing memakai PIN 6 digit acak per sesi; PIN tidak pernah dikirim, hanya bukti HMAC-SHA256 atas
  nonce acak dari penerima.
- Penerima berhenti otomatis setelah 5 kali PIN salah.
- Path dari pengirim divalidasi (tidak boleh `..`, absolut, atau keluar dari folder tujuan).
- Jelajah FTP dari perangkat lain hanya aktif bila saklar *Izinkan jelajah FTP* menyala, dibatasi ke `/sdcard`
  (termasuk `Android/data` dan `Android/obb`), dan tetap butuh PIN. Nyalakan hanya untuk perangkat yang dipercaya.
- **Data tidak dienkripsi** selama transfer; gunakan hanya di jaringan yang dipercaya.

## Protokol singkat

TCP, port acak (diumumkan lewat mDNS `_ngirim._tcp` dan ditampilkan di layar).
Server mengirim `MAGIC "NGR1" + versi (2) + nonce + nama`; klien membalas `mode + nama + HMAC`.
Mode: 1 cek PIN, 2 kirim ke folder terima, 3 daftar folder, 4 ambil (pull), 5 kirim ke folder pilihan (push).
Untuk mode 2/5 (dan 4 dengan arah terbalik), manifest (`jumlah, total, [path, isDir, ukuran]*`), konfirmasi penerima, aliran isi file
berurutan, lalu ACK akhir. Seluruh jaringan dan file I/O berjalan di proses Shizuku (uid shell).

## Troubleshooting

| Masalah | Solusi |
|---|---|
| Perangkat tidak muncul di *Perangkat aktif* | Pastikan saklar Terima di penerima menyala, satu Wi-Fi, dan pakai *IP manual* |
| Tetap tidak terhubung | Matikan *AP/Client isolation* di router, atau pakai hotspot salah satu HP |
| `Shizuku belum berjalan` | Start Shizuku lewat app Shizuku (tombol *Buka app Shizuku*); Ngirim tersambung otomatis |
| Bind service Shizuku gagal | Ketuk **Shizuku Legacy** (jalur Shizuku.Process, tanpa UserService) atau *Hubungkan Shizuku* untuk bind manual; tidak ada coba ulang otomatis |
| Shizuku tidak merespons | Stop lalu Start Shizuku, ketuk *Hubungkan Shizuku* |
| Daftar `Android/data` kosong | Pastikan izin Shizuku diberikan ke Ngirim |
| Transfer terputus saat layar mati | Biarkan layar menyala (Ngirim menahan layar saat dibuka) |

## Build

Tidak ada Gradle wrapper di repo; gunakan Gradle 8.10+ (`gradle wrapper` untuk membuatnya).

```
gradle :app:assembleDebug
```

Dependensi: JDK 17, Android SDK 35, NDK + CMake 3.22.1. Dear ImGui (v1.91.5) diunduh otomatis
oleh CMake (`FetchContent`) saat build pertama, jadi butuh koneksi internet.

## Rilis otomatis (GitHub Actions)

Workflow `.github/workflows/release.yml` membangun APK **bertanda tangan** dan membuat GitHub Release.

1. Buat keystore:
   ```
   keytool -genkeypair -v -keystore release.jks -alias ngirim -keyalg RSA -keysize 2048 -validity 10000
   base64 -w0 release.jks
   ```
2. Tambahkan secrets repo: `KEYSTORE_BASE64`, `KEYSTORE_PASSWORD`, `KEY_ALIAS`, `KEY_PASSWORD`.
3. Rilis: `git tag v2.1 && git push origin v2.1` (atau jalankan manual dari tab Actions).

`versionName` diambil dari nama tag, `versionCode` dari nomor run. Tag berisi `alpha`, `beta`, atau `rc`
ditandai pre-release. Catatan rilis diambil dari bagian yang cocok di `CHANGELOG.md`.
Jangan commit file `.jks` (sudah ada di `.gitignore`).

# Ngirim

Aplikasi Android untuk transfer file, APK, foto, dan data game (`/sdcard/Android/obb` dan
`/sdcard/Android/data`), lokal maupun ke perangkat lain lewat Wi-Fi yang sama.
Akses file memakai **Shizuku** (tanpa root, tanpa SAF), UI memakai **Dear ImGui** (OpenGL ES 3).

Versi saat ini: **v1.0** (final) (lihat [CHANGELOG.md](CHANGELOG.md)).

## Fitur

- **Lokal**: backup/restore APK (termasuk split APK), foto, OBB dan DATA game, plus rute **Kustom**
  (pilih folder sumber dan tujuan sendiri). Mode Salin atau Pindah.
- **Wi-Fi (antar perangkat)**: kirim/terima file dan **folder** lewat jaringan yang sama.
- **FTP (dua panel)**: tab FTP menampilkan dua folder berdampingan dengan pintasan folder; panel kanan bisa
  diarahkan ke perangkat lain yang sudah dipairing untuk menjelajah, mengambil, dan mengirim file dua arah.
- **File browser bawaan**: pilih file atau folder di mana saja, termasuk `Android/data` dan `Android/obb`,
  buat folder baru, dan tentukan folder penyimpanan penerima.
- **Live progress**: persen, ukuran, kecepatan, ETA, dan file yang sedang diproses.
- **Terminal**: log bergaya terminal dengan timestamp dan warna (OK hijau, ERR merah).
- Verifikasi ukuran file (lokal), batalkan kapan saja, file parsial otomatis dihapus.

## Persyaratan

- Android 11+ (minSdk 30), diuji target Android 15.
- [Shizuku](https://shizuku.rikka.app/) terpasang dan **sedang berjalan** (wireless debugging atau root).
- Untuk transfer Wi-Fi: kedua perangkat di Wi-Fi/hotspot yang sama dan izin *Perangkat di sekitar*
  (Android 13+).

## Cara pakai

### Transfer lokal (tab Lokal)
1. Buka Ngirim, pastikan status Shizuku hijau (ketuk **Hubungkan Shizuku** lalu izinkan jika perlu).
2. Pilih rute, centang item, pilih **Salin** atau **Pindah**, lalu **MULAI TRANSFER**.
3. Rute **Kustom**: ketuk **Pilih sumber** dan **Pilih tujuan**, lalu centang folder/file di sumber.

Hasil backup tersimpan di `/sdcard/Ngirim/{APK,Foto,obb,data}`.

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
3. Rilis: `git tag v1.0 && git push origin v1.0` (atau jalankan manual dari tab Actions).

`versionName` diambil dari nama tag, `versionCode` dari nomor run. Tag berisi `alpha`, `beta`, atau `rc`
ditandai pre-release. Catatan rilis diambil dari bagian yang cocok di `CHANGELOG.md`.
Jangan commit file `.jks` (sudah ada di `.gitignore`).

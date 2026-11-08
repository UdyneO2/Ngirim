# Ngirim

Aplikasi Android untuk transfer file, APK, foto, dan data game (`/sdcard/Android/obb` dan
`/sdcard/Android/data`), lokal maupun ke perangkat lain lewat Wi-Fi yang sama.
Akses file memakai **Shizuku** (tanpa root, tanpa SAF), UI memakai **Dear ImGui** (OpenGL ES 3).

Versi saat ini: **v1.1 Beta** (lihat [CHANGELOG.md](CHANGELOG.md)).

## Fitur

- **Lokal**: backup/restore APK (termasuk split APK), foto, OBB dan DATA game, plus rute **Kustom**
  (pilih folder sumber dan tujuan sendiri). Mode Salin atau Pindah.
- **Wi-Fi (antar perangkat)**: kirim/terima file dan **folder** lewat jaringan yang sama.
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
1. **Izin**: ketuk *Berikan izin / periksa ulang* (izin perangkat sekitar, dan membuka pengaturan Wi-Fi
   bila belum terhubung). Lanjut setelah semua OK.
2. **Pilih peran**: *Kirim* atau *Terima*.
3. **Perangkat penerima**: ketuk *Pilih folder tujuan*, lalu **MULAI TERIMA**. Layar menampilkan
   alamat `IP:port` dan **PIN 6 digit**.
4. **Perangkat pengirim**: ketuk *Cari perangkat* (atau *IP manual* dan isi `IP:port`), pilih penerima,
   masukkan PIN (pairing).
5. Tambahkan file/folder lewat *+ File / folder* (bisa dari banyak lokasi), lalu **KIRIM SEKARANG**.
   Progress dan log terminal tampil di kedua perangkat.

Folder dikirim lengkap beserta isi dan subfolder. Jika nama file sudah ada di penerima, file baru
disimpan dengan akhiran ` (1)`, ` (2)`, dan seterusnya (tidak menimpa).

## Keamanan

- Pairing memakai PIN 6 digit acak per sesi; PIN tidak pernah dikirim, hanya bukti HMAC-SHA256 atas
  nonce acak dari penerima.
- Penerima berhenti otomatis setelah 5 kali PIN salah.
- Path dari pengirim divalidasi (tidak boleh `..`, absolut, atau keluar dari folder tujuan).
- **Data tidak dienkripsi** selama transfer; gunakan hanya di jaringan yang dipercaya.

## Protokol singkat

TCP, port acak (diumumkan lewat mDNS `_ngirim._tcp` dan ditampilkan di layar).
Server mengirim `MAGIC "NGR1" + versi + nonce + nama`; klien membalas `mode + nama + HMAC`;
setelah itu manifest (`jumlah, total, [path, isDir, ukuran]*`), konfirmasi penerima, aliran isi file
berurutan, lalu ACK akhir. Seluruh jaringan dan file I/O berjalan di proses Shizuku (uid shell).

## Troubleshooting

| Masalah | Solusi |
|---|---|
| Perangkat tidak muncul saat *Cari* | Pastikan penerima sudah *MULAI TERIMA*, satu Wi-Fi, dan pakai *IP manual* |
| Tetap tidak terhubung | Matikan *AP/Client isolation* di router, atau pakai hotspot salah satu HP |
| `Shizuku belum berjalan` | Start Shizuku lewat app Shizuku, lalu ketuk *Hubungkan Shizuku* |
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
3. Rilis: `git tag v1.1-beta && git push origin v1.1-beta` (atau jalankan manual dari tab Actions).

`versionName` diambil dari nama tag, `versionCode` dari nomor run. Tag berisi `alpha`, `beta`, atau `rc`
ditandai pre-release. Catatan rilis diambil dari bagian yang cocok di `CHANGELOG.md`.
Jangan commit file `.jks` (sudah ada di `.gitignore`).

# Changelog

Versi terbaru di atas. Di dalam aplikasi, riwayat ini ada di Pengaturan > Riwayat perubahan.

## v2.6 (11 Nov 2026)

- **Pratinjau gambar** di tab Lokal: pilih satu gambar lalu ketuk Pratinjau. Ada zoom (+ / - / Pas), geser
  untuk menggerakkan gambar saat di-zoom, dan geser kiri-kanan atau tombol < > untuk gambar lain di folder
  yang sama. Format: JPG, PNG, WebP, GIF (frame pertama), BMP, HEIC. Gambar besar diperkecil ke maks. 2048 px
  dan orientasi EXIF diikuti.
- **Pasang XAPK / APKS / APKM** langsung dari file manager (butuh Shizuku). Semua split APK dipasang dalam
  satu sesi; untuk bundle yang berisi banyak ABI dan kepadatan layar, hanya yang cocok dengan HP yang dipakai.
  Berkas OBB/data di dalam XAPK disalin ke `Android/obb` atau `Android/data` setelah pemasangan berhasil.
- **Multi-pilih lewat geser**: tekan di kolom kotak centang lalu geser ke atas/bawah untuk memilih (atau
  melepas) banyak baris sekaligus; daftar menggulir sendiri di tepi. Berlaku di FTP dan juga di tab Lokal.
  Menggeser di bagian nama tetap menggulir daftar seperti biasa.
- Pilihan warna aksen dihapus; tampilan kembali memakai biru Windows 10.

## v2.5 (11 Nov 2026)

- Transfer yang putus (Wi-Fi hilang, aplikasi ditutup, baterai habis) sekarang bisa dilanjutkan. Penerima
  menyimpan bagian yang sudah masuk sebagai file `.ngpart`; saat kamu mengirim ulang file yang sama,
  hanya sisanya yang dikirim. File dianggap sama bila nama, ukuran, dan waktu ubahnya sama.
- Saat koneksi putus di tengah jalan, Ngirim mencoba menyambung lagi sendiri (sampai 4 kali) sebelum menyerah.
- Sisa `.ngpart` yang tidak pernah dilanjutkan dihapus otomatis setelah 7 hari, saat Terima diaktifkan.
- Protokol berubah: kedua perangkat harus sama-sama v2.5. Versi lama menampilkan pesan "Versi protokol berbeda".

## v2.4 (11 Nov 2026)

- Tombol **Pindai QR** di tab Wi-Fi membuka kamera di dalam Ngirim, jadi tidak perlu aplikasi pemindai
  lain. Izin kamera hanya diminta saat tombol ini dipakai. Memakai pustaka zxing-android-embedded.
- Ikon aplikasi baru (adaptive icon, latar hitam).

## v2.3 (10 Nov 2026)

- File manager: tombol **Arsipkan** membuat `.zip` dari file atau folder yang dipilih, lengkap dengan
  progress. Nama otomatis diberi akhiran (1), (2) bila sudah ada.
- Pairing lewat QR: di tab Wi-Fi, saat Terima aktif, ketuk **Tampilkan QR**. Pindai dengan kamera atau
  pemindai QR apa pun di perangkat pengirim; Ngirim terbuka, menanyakan konfirmasi, lalu memasangkan
  tanpa mengetik PIN. QR berisi alamat dan PIN sesi itu, jadi tunjukkan hanya ke orang yang kamu percaya.

## v2.2 (10 Nov 2026)

- Warna aksen: tujuh pilihan (biru, hijau, ungu, oranye, merah, teal, merah muda) di Pengaturan > Tampilan.
  Tombol, pilihan, dan header ikut berubah, baik di tema Terang maupun Gelap.
- Bahasa: Indonesia dan Inggris. Default "Otomatis": kalau bahasa sistem Indonesia, aplikasi pakai Indonesia,
  selain itu Inggris. Bisa dipaksa manual di Pengaturan > Bahasa.
- Semua teks ikut diterjemahkan: antarmuka, dialog, toast, dan pesan di Log.
- Bahasa lain tinggal menambah satu tabel di `i18n.cpp` (lihat README). Aksara non-Latin belum, karena
  butuh font tambahan.
- Riwayat perubahan di Pengaturan dipadatkan jadi satu bagian yang bisa dibuka-tutup untuk semua versi.
- README sekarang berisi daftar centang fitur yang sudah ada dan yang mungkin dibuat.

## v2.1 (9 Nov 2026)

- Cek pembaruan dari GitHub Release. Ada dialog konfirmasi berisi catatan rilis, lalu APK diunduh dan
  dipasang (lewat Shizuku kalau terhubung, kalau tidak lewat installer sistem). Bisa otomatis saat aplikasi
  dibuka atau manual dari Pengaturan.
- File manager sekarang bisa Buka file, Bagikan, Pasang APK, dan Ekstrak ZIP.
- Pengaturan > Pasang APK: pilih installer sistem atau Shizuku (senyap).
- Tombol Shizuku Legacy muncul bila bind service gagal. Jalurnya tanpa UserService: FileService dijalankan
  lewat Shizuku.Process.
- Bind Shizuku dicoba sekali saja, coba ulang otomatis dihapus. Ada tombol Hubungkan Shizuku untuk bind manual.
- Nama paket jadi `com.udyneos.ngirim`. Android menganggapnya aplikasi baru, bukan pembaruan dari `com.ngirim.app`.
- FTP: kotak centang dan ikon dikecilkan, nama panjang dibungkus sampai 3 baris.
- Pengaturan: bagian Tentang baru dan riwayat perubahan yang bisa dibuka-tutup.

## v2.0 (9 Nov 2026)

- Tampilan baru ala Windows 10, tema Terang (default) dan Gelap.
- Tab Lokal menjadi file manager: salin, potong, tempel, hapus ke tempat sampah, ubah nama, folder baru,
  properti, urutkan, cari, file tersembunyi.
- Akses cepat ke folder penting; folder sendiri bisa disematkan.
- Backup APK langsung ke `.xapk` dari tombol di tab Lokal, OBB dan DATA game opsional.
- Tab Pengaturan menggantikan Changelog, dan Terminal berganti nama jadi Log (urutan tab: Lokal, Wi-Fi, FTP,
  Pengaturan, Log).
- Rute lama (Foto, OBB, DATA, Kustom) diganti file manager dan akses cepat.

## v1.0 (8 Nov 2026)

Rilis stabil pertama, menggabungkan v1.1 dan v1.2 beta.

- Perbaikan "Menunggu Shizuku" yang menggantung. Koneksi dicek tiap 2 detik dan tersambung sendiri setelah
  izin diberikan.
- Tombol langsung ke app Shizuku dan pesan status yang lebih jelas.

## v1.2 beta (8 Nov 2026)

- Tab FTP dengan dua panel, pintasan folder, dan favorit sendiri.
- Panel kanan bisa menjelajah HP lain yang sudah dipasangkan; salin ke dua arah.
- Saklar Terima ON/OFF, daftar perangkat aktif otomatis, saklar izin jelajah FTP (default mati).
- Protokol jaringan v2 (mode LIST, PULL, PUSH), tidak kompatibel dengan v1.1.
- Perbaikan: tab Wi-Fi, FTP, Terminal, dan Changelog tidak bisa diketuk.

## v1.1 beta (8 Nov 2026)

- Transfer antar-HP lewat Wi-Fi dengan pairing PIN, termasuk folder.
- Penerima memilih folder simpan; ada file browser bawaan.
- Backup dan restore APK (termasuk split), foto, dan data game OBB/DATA.
- Progress langsung, mode salin atau pindah, log bergaya terminal.
- Akses lewat Shizuku, UI Dear ImGui, build rilis otomatis lewat GitHub Actions.

## Batasan yang diketahui

- Data Wi-Fi tidak dienkripsi, hanya dijaga PIN.
- Nama non-Latin bisa tampil sebagai `?`.
- Router dengan client isolation memblokir koneksi antar-perangkat.
- XAPK di atas 4 GB belum didukung (batas ZIP).

package com.udyneos.ngirim;

import com.udyneos.ngirim.IProgressCallback;

interface IFileService {
    void destroy() = 16777114;
    // Lokal
    String[] list(String dir) = 1;                       // dengan ukuran folder rekursif
    void start(in String[] srcs, in String[] dsts, boolean move, IProgressCallback cb) = 2;
    void cancel() = 3;
    String[] browse(String dir) = 9;                     // listing dangkal untuk file browser
    boolean mkdir(String path) = 8;
    // Jaringan (Wi-Fi/LAN)
    int listen(String destDir, String pin, String deviceName, IProgressCallback cb) = 4;  // return port / -1
    void stopListen() = 5;
    void send(String host, int port, String pin, String deviceName, in String[] srcs, IProgressCallback cb) = 6;
    String checkPair(String host, int port, String pin, String deviceName) = 7;           // "" = sukses
    // FTP (jelajah perangkat lain)
    String[] remoteList(String host, int port, String pin, String deviceName, String path) = 10; // [0]="OK" lalu "nama\tukuran\tdir", selain itu pesan error
    void push(String host, int port, String pin, String deviceName, in String[] srcs, String remoteDir, IProgressCallback cb) = 11;
    void pull(String host, int port, String pin, String deviceName, in String[] remotePaths, String destDir, IProgressCallback cb) = 12;
    void setAllowBrowse(boolean allow) = 13;             // penerima: izinkan perangkat terpasang menjelajah /sdcard
    // File manager
    String[] listDetailed(String dir) = 14;              // "nama\tukuran\tdir\tmtime(ms)"; ukuran folder = 0
    void deletePaths(in String[] paths, String trashDir, IProgressCallback cb) = 15; // trashDir kosong = hapus permanen
    boolean rename(String from, String to) = 16;
    long pathSize(String path) = 17;                     // rekursif
    // Backup XAPK: tiap elemen = satu job JSON (lihat FileService.doXapk)
    void backupXapk(in String[] jobs, IProgressCallback cb) = 18;
    // Pasang APK senyap lewat `pm install` (uid shell) dan ekstrak ZIP
    void installApk(in String[] paths, IProgressCallback cb) = 19;
    void unzip(String zip, String destDir, IProgressCallback cb) = 20;
    void zip(in String[] srcs, String outZip, IProgressCallback cb) = 21;   // buat arsip ZIP dari file/folder
}

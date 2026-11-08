package com.ngirim.app;

import com.ngirim.app.IProgressCallback;

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
}

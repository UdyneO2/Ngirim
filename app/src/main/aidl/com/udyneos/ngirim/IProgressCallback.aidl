package com.udyneos.ngirim;

oneway interface IProgressCallback {
    void onProgress(String current, long done, long total, double bytesPerSec);
    void onLog(String msg);
    void onFinish(boolean ok, String message, long done, long total);
}

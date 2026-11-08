package com.ngirim.app;

oneway interface IProgressCallback {
    void onProgress(String current, long done, long total, double bytesPerSec);
    void onLog(String msg);
    void onFinish(boolean ok, String message, long done, long total);
}

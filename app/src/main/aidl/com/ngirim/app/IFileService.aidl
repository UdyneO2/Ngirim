package com.ngirim.app;

import com.ngirim.app.IProgressCallback;

interface IFileService {
    void destroy() = 16777114;
    String[] list(String dir) = 1;
    void start(in String[] srcs, in String[] dsts, boolean move, IProgressCallback cb) = 2;
    void cancel() = 3;
}

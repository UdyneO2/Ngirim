package com.ngirim.app

/** Jembatan JNI ke UI ImGui (C++). State UI disimpan di native, aksi di-poll tiap frame. */
object Native {
    init { System.loadLibrary("ngirim") }

    @JvmStatic external fun initGl(density: Float)
    @JvmStatic external fun render(w: Int, h: Int)
    @JvmStatic external fun touch(action: Int, x: Float, y: Float)
    @JvmStatic external fun pollAction(): String
    @JvmStatic external fun log(msg: String)

    // lokal
    @JvmStatic external fun setShizuku(ok: Boolean, msg: String)
    @JvmStatic external fun setRoutes(names: Array<String>)
    @JvmStatic external fun setItems(names: Array<String>, sizes: LongArray)
    @JvmStatic external fun setProgress(busy: Boolean, done: Long, total: Long, speed: Double, cur: String)
    @JvmStatic external fun setCustom(src: String, dst: String)

    // jaringan
    @JvmStatic external fun setNetPerm(ok: Boolean, msg: String)
    @JvmStatic external fun setPeers(names: Array<String>)
    @JvmStatic external fun setNetInfo(addr: String, pin: String, recvDir: String, listening: Boolean, paired: String)
    @JvmStatic external fun setSendList(names: Array<String>, sizes: LongArray)
    @JvmStatic external fun setNetProgress(busy: Boolean, done: Long, total: Long, speed: Double, cur: String)

    // FTP (dua panel)
    @JvmStatic external fun setPane(
        idx: Int, path: String, remote: Boolean, err: String,
        names: Array<String>, sizes: LongArray, dirs: BooleanArray
    )
    @JvmStatic external fun setBookmarks(paths: Array<String>)
    @JvmStatic external fun setFtpProgress(busy: Boolean, done: Long, total: Long, speed: Double, cur: String)

    // file browser
    @JvmStatic external fun setBrowser(
        open: Boolean, purpose: Int, path: String,
        names: Array<String>, sizes: LongArray, dirs: BooleanArray
    )
}

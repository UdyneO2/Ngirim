package com.udyneos.ngirim

/** Jembatan JNI ke UI ImGui (C++). State UI disimpan di native, aksi di-poll tiap frame. */
object Native {
    init { System.loadLibrary("ngirim") }

    @JvmStatic external fun initGl(density: Float)
    @JvmStatic external fun render(w: Int, h: Int)
    @JvmStatic external fun touch(action: Int, x: Float, y: Float)
    @JvmStatic external fun pollAction(): String
    @JvmStatic external fun log(msg: String)

    // umum / pengaturan
    @JvmStatic external fun setShizuku(ok: Boolean, msg: String)
    @JvmStatic external fun setShizukuFailed(failed: Boolean)
    @JvmStatic external fun setUpdateMsg(msg: String)
    @JvmStatic external fun selectTab(idx: Int)
    @JvmStatic external fun setSettings(dark: Boolean, hidden: Boolean, trash: Boolean, xapkDir: String, devName: String, instShizuku: Boolean, autoUpdate: Boolean)

    // file manager (tab Lokal)
    @JvmStatic external fun setFm(
        path: String, err: String, filter: String, sort: Int, asc: Boolean,
        names: Array<String>, sizes: LongArray, dirs: BooleanArray, mtimes: LongArray
    )
    @JvmStatic external fun setClip(count: Int, cut: Boolean)
    @JvmStatic external fun setFmProgress(busy: Boolean, done: Long, total: Long, speed: Double, cur: String)

    // APK / XAPK
    @JvmStatic external fun setApps(filter: String, names: Array<String>, subs: Array<String>, sizes: LongArray)
    @JvmStatic external fun setApkProgress(busy: Boolean, done: Long, total: Long, speed: Double, cur: String)

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

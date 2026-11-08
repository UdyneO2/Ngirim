package com.ngirim.app

/** Jembatan JNI ke UI ImGui (C++). State UI disimpan di native, aksi di-poll tiap frame. */
object Native {
    init { System.loadLibrary("ngirim") }

    @JvmStatic external fun initGl(density: Float)
    @JvmStatic external fun render(w: Int, h: Int)
    @JvmStatic external fun touch(action: Int, x: Float, y: Float)
    @JvmStatic external fun setShizuku(ok: Boolean, msg: String)
    @JvmStatic external fun setRoutes(names: Array<String>)
    @JvmStatic external fun setItems(names: Array<String>, sizes: LongArray)
    @JvmStatic external fun setProgress(busy: Boolean, done: Long, total: Long, speed: Double, cur: String)
    @JvmStatic external fun log(msg: String)
    @JvmStatic external fun pollAction(): String
}

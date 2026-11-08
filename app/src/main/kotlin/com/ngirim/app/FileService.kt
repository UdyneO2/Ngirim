package com.ngirim.app

import java.io.File
import java.io.IOException
import kotlin.system.exitProcess

/** Berjalan di proses Shizuku (uid shell) -> bisa akses Android/data & Android/obb. */
class FileService : IFileService.Stub() {

    @Volatile private var cancelled = false
    @Volatile private var running = false

    override fun destroy() { exitProcess(0) }

    override fun list(dir: String): Array<String> {
        val kids = File(dir).listFiles() ?: return emptyArray()
        return kids.sortedWith(
            compareBy<File>(
                { !it.isDirectory },
                { if (it.isDirectory) it.name.lowercase() else "" },
                { -it.lastModified() }
            )
        ).map { "${it.name}\t${sizeOf(it)}\t${if (it.isDirectory) 1 else 0}" }.toTypedArray()
    }

    override fun cancel() { cancelled = true }

    override fun start(srcs: Array<String>, dsts: Array<String>, move: Boolean, cb: IProgressCallback) {
        if (running) { cb.onFinish(false, "Transfer lain masih berjalan", 0, 0); return }
        cancelled = false
        running = true
        Thread {
            try { runJob(srcs, dsts, move, cb) }
            catch (t: Throwable) { cb.onFinish(false, "Error: ${t.message}", 0, 0) }
            finally { running = false }
        }.start()
    }

    private fun sizeOf(f: File): Long =
        if (f.isFile) f.length() else f.walkTopDown().filter { it.isFile }.sumOf { it.length() }

    private fun runJob(srcs: Array<String>, dsts: Array<String>, move: Boolean, cb: IProgressCallback) {
        var total = 0L
        for (s in srcs) total += sizeOf(File(s))
        val job = Job(cb, total)
        var ok = 0
        var failed = 0

        for (i in srcs.indices) {
            if (cancelled) break
            val s = File(srcs[i])
            val d = File(dsts[i])
            try {
                var renamed = false
                if (move && !d.exists()) {
                    d.parentFile?.mkdirs()
                    if (s.renameTo(d)) { job.done += sizeOf(d); renamed = true }
                }
                if (!renamed) {
                    job.copy(s, d)
                    if (move) s.deleteRecursively()
                }
                ok++
                cb.onLog("OK     ${s.name}")
            } catch (e: InterruptedException) {
                break
            } catch (e: Throwable) {
                failed++
                cb.onLog("GAGAL  ${s.name}: ${e.message}")
            }
        }

        job.emit("", force = true)
        val msg = if (cancelled) "Dibatalkan: $ok sukses, $failed gagal"
        else "Selesai: $ok sukses, $failed gagal"
        cb.onFinish(!cancelled && failed == 0, msg, job.done, total)
    }

    private inner class Job(val cb: IProgressCallback, val total: Long) {
        var done = 0L
        private var lastEmit = 0L
        private var lastDone = 0L
        private var speed = 0.0
        private val buf = ByteArray(1 shl 20)

        fun emit(cur: String, force: Boolean = false) {
            val now = System.nanoTime()
            if (!force && now - lastEmit < 150_000_000L) return
            if (lastEmit != 0L) {
                val dt = (now - lastEmit) / 1e9
                if (dt > 0) {
                    val inst = (done - lastDone) / dt
                    speed = if (speed == 0.0) inst else speed * 0.7 + inst * 0.3
                }
            }
            lastEmit = now
            lastDone = done
            cb.onProgress(cur, done, total, speed)
        }

        fun copy(src: File, dst: File) {
            if (cancelled) throw InterruptedException("cancel")
            if (src.isDirectory) {
                dst.mkdirs()
                src.listFiles()?.forEach { copy(it, File(dst, it.name)) }
                return
            }
            dst.parentFile?.mkdirs()
            try {
                src.inputStream().use { input ->
                    dst.outputStream().use { out ->
                        while (true) {
                            val n = input.read(buf)
                            if (n < 0) break
                            out.write(buf, 0, n)
                            done += n
                            emit(src.name)
                            if (cancelled) throw InterruptedException("cancel")
                        }
                    }
                }
            } catch (e: InterruptedException) {
                dst.delete()
                throw e
            }
            if (dst.length() != src.length()) throw IOException("Ukuran tidak cocok: ${src.name}")
            dst.setLastModified(src.lastModified())
        }
    }
}

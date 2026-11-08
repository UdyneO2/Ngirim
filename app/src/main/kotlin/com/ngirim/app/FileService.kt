package com.ngirim.app

import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.EOFException
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Collections
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec
import kotlin.system.exitProcess

/**
 * Berjalan di proses Shizuku (uid shell): akses Android/data, Android/obb, dan seluruh storage,
 * termasuk server/klien TCP untuk transfer antar-perangkat lewat Wi-Fi yang sama.
 */
class FileService : IFileService.Stub() {

    companion object {
        private const val MAGIC = 0x4E475231 // "NGR1"
        private const val MAX_PIN_FAILS = 5
    }

    @Volatile private var cancelled = false
    @Volatile private var running = false
    @Volatile private var server: ServerSocket? = null
    @Volatile private var pinFails = 0
    private val active = Collections.synchronizedSet(HashSet<Socket>())

    override fun destroy() { stopListen(); exitProcess(0) }

    // ---------------------------------------------------------------- listing
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

    override fun browse(dir: String): Array<String> {
        val kids = File(dir).listFiles() ?: return emptyArray()
        return kids.sortedWith(compareBy<File>({ !it.isDirectory }, { it.name.lowercase() }))
            .map { "${it.name}\t${if (it.isFile) it.length() else 0}\t${if (it.isDirectory) 1 else 0}" }
            .toTypedArray()
    }

    override fun mkdir(path: String): Boolean = try { File(path).mkdirs() } catch (_: Throwable) { false }

    override fun cancel() { cancelled = true; closeActive() }

    private fun closeActive() {
        val list = synchronized(active) { active.toList() }
        list.forEach { try { it.close() } catch (_: Exception) {} }
    }

    // ---------------------------------------------------------------- transfer lokal
    override fun start(srcs: Array<String>, dsts: Array<String>, move: Boolean, cb: IProgressCallback) {
        if (running) { cb.onFinish(false, "Transfer lain masih berjalan", 0, 0); return }
        cancelled = false
        running = true
        Thread {
            try { runJob(srcs, dsts, move, cb) }
            catch (t: Throwable) { finish(cb, false, "Error: ${t.message}", 0, 0) }
            finally { running = false }
        }.start()
    }

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
                log(cb, "OK     ${s.name}")
            } catch (e: InterruptedException) {
                break
            } catch (e: Throwable) {
                failed++
                log(cb, "GAGAL  ${s.name}: ${e.message}")
            }
        }

        job.emit("", force = true)
        val msg = if (cancelled) "Dibatalkan: $ok sukses, $failed gagal"
        else "Selesai: $ok sukses, $failed gagal"
        finish(cb, !cancelled && failed == 0, msg, job.done, total)
    }

    // ---------------------------------------------------------------- jaringan: terima
    override fun listen(destDir: String, pin: String, deviceName: String, cb: IProgressCallback): Int {
        stopListen()
        return try {
            val root = File(destDir)
            root.mkdirs()
            if (!root.isDirectory) throw IOException("Folder tujuan tidak valid")
            val ss = ServerSocket(0)
            server = ss
            pinFails = 0
            Thread { acceptLoop(ss, root, pin, deviceName, cb) }.start()
            ss.localPort
        } catch (t: Throwable) {
            log(cb, "ERR listen: ${t.message}")
            -1
        }
    }

    override fun stopListen() {
        try { server?.close() } catch (_: Exception) {}
        server = null
        closeActive()
    }

    private fun acceptLoop(ss: ServerSocket, root: File, pin: String, name: String, cb: IProgressCallback) {
        log(cb, "Server terima aktif di port ${ss.localPort}, simpan ke ${root.path}")
        while (!ss.isClosed) {
            val s = try { ss.accept() } catch (_: Exception) { break }
            active.add(s)
            try {
                incoming(s, root, pin, name, cb)
            } catch (e: Throwable) {
                if (cancelled) { log(cb, "ERR sesi dibatalkan"); finish(cb, false, "Sesi dibatalkan", 0, 0) }
                else { log(cb, "ERR sesi: ${e.message}"); finish(cb, false, "Sesi gagal: ${e.message}", 0, 0) }
            } finally {
                active.remove(s)
                try { s.close() } catch (_: Exception) {}
            }
        }
        log(cb, "Server terima berhenti")
    }

    private class Ent(val rel: String, val dir: Boolean, val size: Long)

    private fun incoming(s: Socket, root: File, pin: String, name: String, cb: IProgressCallback) {
        cancelled = false
        s.soTimeout = 60_000
        val inp = DataInputStream(BufferedInputStream(s.getInputStream(), 1 shl 16))
        val out = DataOutputStream(BufferedOutputStream(s.getOutputStream(), 1 shl 16))

        val nonce = ByteArray(16).also { SecureRandom().nextBytes(it) }
        out.writeInt(MAGIC); out.writeByte(1); out.write(nonce); out.writeUTF(name); out.flush()

        val mode = inp.readByte().toInt()
        val peer = inp.readUTF()
        val mac = ByteArray(32)
        inp.readFully(mac)
        val shown = peer.take(64)
        val from = s.inetAddress?.hostAddress ?: "?"

        if (!MessageDigest.isEqual(mac, hmac(pin, nonce, mode, peer))) {
            out.writeByte(0); out.writeUTF("PIN salah"); out.flush()
            log(cb, "ERR PIN salah dari $shown ($from)")
            if (++pinFails >= MAX_PIN_FAILS) {
                stopListen()
                finish(cb, false, "Server dihentikan: terlalu banyak PIN salah", 0, 0)
            }
            return
        }
        pinFails = 0
        out.writeByte(1); out.writeUTF("OK"); out.flush()
        if (mode == 1) { log(cb, "OK     pairing dengan $shown ($from)"); return }
        if (mode != 2) return

        log(cb, "Pengirim terhubung: $shown ($from)")
        val count = inp.readInt()
        val total = inp.readLong()
        if (count < 0 || count > 2_000_000 || total < 0) throw IOException("Manifest tidak valid")
        val list = ArrayList<Ent>(count)
        repeat(count) { list.add(Ent(inp.readUTF(), inp.readByte().toInt() == 1, inp.readLong())) }

        val targets = try {
            list.map { safeResolve(root, it.rel) }
        } catch (e: SecurityException) {
            out.writeByte(0); out.writeUTF("Path ditolak"); out.flush()
            log(cb, "ERR ${e.message}")
            return
        }
        if (root.usableSpace < total) {
            out.writeByte(0); out.writeUTF("Ruang penyimpanan tidak cukup"); out.flush()
            log(cb, "ERR ruang tidak cukup (butuh ${fmt(total)})")
            return
        }
        out.writeByte(1); out.writeUTF("SIAP"); out.flush()
        log(cb, "Menerima ${list.size} entri (${fmt(total)})")

        val job = Job(cb, total)
        var files = 0
        for ((i, e) in list.withIndex()) {
            val target = targets[i]
            if (e.dir) { target.mkdirs(); continue }
            target.parentFile?.mkdirs()
            val dst = uniqueFile(target)
            try {
                FileOutputStream(dst).use { o ->
                    var left = e.size
                    while (left > 0) {
                        val n = inp.read(job.buf, 0, minOf(left, job.buf.size.toLong()).toInt())
                        if (n < 0) throw EOFException("Koneksi terputus")
                        o.write(job.buf, 0, n)
                        left -= n
                        job.done += n
                        job.emit(e.rel)
                        if (cancelled) throw InterruptedException("cancel")
                    }
                }
            } catch (ex: Throwable) {
                dst.delete()
                throw ex
            }
            files++
            log(cb, "OK     ${e.rel}")
        }
        job.emit("", force = true)
        out.writeByte(1); out.flush()
        finish(cb, true, "Selesai: $files file diterima dari $shown (${fmt(job.done)})", job.done, total)
    }

    // ---------------------------------------------------------------- jaringan: kirim
    override fun checkPair(host: String, port: Int, pin: String, deviceName: String): String = try {
        Socket().use { s ->
            s.connect(InetSocketAddress(host, port), 5000)
            s.soTimeout = 8000
            handshake(DataInputStream(s.getInputStream()), DataOutputStream(s.getOutputStream()), 1, pin, deviceName)
        }
        ""
    } catch (t: Throwable) {
        t.message ?: t.javaClass.simpleName
    }

    override fun send(host: String, port: Int, pin: String, deviceName: String, srcs: Array<String>, cb: IProgressCallback) {
        if (running) { cb.onFinish(false, "Transfer lain masih berjalan", 0, 0); return }
        cancelled = false
        running = true
        Thread {
            try { doSend(host, port, pin, deviceName, srcs, cb) }
            catch (t: Throwable) {
                if (cancelled) { log(cb, "ERR dibatalkan"); finish(cb, false, "Dibatalkan", 0, 0) }
                else { log(cb, "ERR ${t.message}"); finish(cb, false, "Gagal: ${t.message}", 0, 0) }
            } finally { running = false }
        }.start()
    }

    private class SendEnt(val rel: String, val f: File, val dir: Boolean, val size: Long)

    private fun doSend(host: String, port: Int, pin: String, name: String, srcs: Array<String>, cb: IProgressCallback) {
        val list = ArrayList<SendEnt>()
        fun walk(rel: String, f: File) {
            if (f.isDirectory) {
                list.add(SendEnt(rel, f, true, 0))
                f.listFiles()?.sortedBy { it.name }?.forEach { walk("$rel/${it.name}", it) }
            } else if (f.isFile) {
                list.add(SendEnt(rel, f, false, f.length()))
            }
        }
        for (p in srcs) { val f = File(p); walk(f.name, f) }
        val total = list.sumOf { it.size }
        val fileCount = list.count { !it.dir }
        log(cb, "Menyiapkan ${list.size} entri ($fileCount file, ${fmt(total)})")

        val s = Socket()
        active.add(s)
        try {
            s.connect(InetSocketAddress(host, port), 8000)
            s.soTimeout = 60_000
            val inp = DataInputStream(BufferedInputStream(s.getInputStream(), 1 shl 16))
            val out = DataOutputStream(BufferedOutputStream(s.getOutputStream(), 1 shl 16))

            val serverName = handshake(inp, out, 2, pin, name)
            log(cb, "OK     terautentikasi ke $serverName")

            out.writeInt(list.size); out.writeLong(total)
            for (e in list) { out.writeUTF(e.rel); out.writeByte(if (e.dir) 1 else 0); out.writeLong(e.size) }
            out.flush()
            if (inp.readByte().toInt() != 1) throw IOException("Ditolak penerima: ${inp.readUTF()}")
            inp.readUTF()

            val job = Job(cb, total)
            for (e in list) {
                if (e.dir) continue
                FileInputStream(e.f).use { i ->
                    var left = e.size
                    while (left > 0) {
                        val n = i.read(job.buf, 0, minOf(left, job.buf.size.toLong()).toInt())
                        if (n < 0) throw EOFException("File berubah saat dikirim: ${e.rel}")
                        out.write(job.buf, 0, n)
                        left -= n
                        job.done += n
                        job.emit(e.rel)
                        if (cancelled) throw InterruptedException("cancel")
                    }
                }
                log(cb, "OK     ${e.rel}")
            }
            out.flush()
            if (inp.readByte().toInt() != 1) throw IOException("Penerima gagal menyimpan")
            job.emit("", force = true)
            finish(cb, true, "Selesai: $fileCount file terkirim ke $serverName (${fmt(job.done)})", job.done, total)
        } finally {
            active.remove(s)
            try { s.close() } catch (_: Exception) {}
        }
    }

    // ---------------------------------------------------------------- util
    private fun handshake(inp: DataInputStream, out: DataOutputStream, mode: Int, pin: String, name: String): String {
        if (inp.readInt() != MAGIC) throw IOException("Bukan server Ngirim")
        if (inp.readByte().toInt() != 1) throw IOException("Versi protokol berbeda")
        val nonce = ByteArray(16)
        inp.readFully(nonce)
        val serverName = inp.readUTF()
        out.writeByte(mode); out.writeUTF(name); out.write(hmac(pin, nonce, mode, name)); out.flush()
        if (inp.readByte().toInt() != 1) throw IOException(inp.readUTF())
        inp.readUTF()
        return serverName
    }

    private fun hmac(pin: String, nonce: ByteArray, mode: Int, peer: String): ByteArray {
        val m = Mac.getInstance("HmacSHA256")
        m.init(SecretKeySpec(pin.toByteArray(), "HmacSHA256"))
        m.update(nonce)
        m.update(mode.toByte())
        m.update(peer.toByteArray())
        return m.doFinal()
    }

    private fun safeResolve(root: File, rel: String): File {
        if (rel.isEmpty() || rel.startsWith("/") || rel.contains("\\")) throw SecurityException("path ilegal: $rel")
        if (rel.split("/").any { it.isEmpty() || it == "." || it == ".." }) throw SecurityException("path ilegal: $rel")
        val f = File(root, rel)
        if (!f.canonicalPath.startsWith(root.canonicalPath + "/")) throw SecurityException("path keluar folder: $rel")
        return f
    }

    private fun uniqueFile(f: File): File {
        if (!f.exists()) return f
        val base = f.nameWithoutExtension
        val ext = f.extension.let { if (it.isEmpty()) "" else ".$it" }
        var n = 1
        while (true) {
            val c = File(f.parentFile, "$base ($n)$ext")
            if (!c.exists()) return c
            n++
        }
    }

    private fun sizeOf(f: File): Long =
        if (f.isFile) f.length() else f.walkTopDown().filter { it.isFile }.sumOf { it.length() }

    private fun fmt(b: Long): String {
        var v = b.toDouble()
        val u = arrayOf("B", "KB", "MB", "GB", "TB")
        var i = 0
        while (v >= 1024 && i < 4) { v /= 1024; i++ }
        return if (i == 0) "${v.toLong()} B" else String.format("%.2f %s", v, u[i])
    }

    private fun log(cb: IProgressCallback, msg: String) { try { cb.onLog(msg) } catch (_: Throwable) {} }
    private fun finish(cb: IProgressCallback, ok: Boolean, msg: String, done: Long, total: Long) {
        try { cb.onFinish(ok, msg, done, total) } catch (_: Throwable) {}
    }

    private inner class Job(val cb: IProgressCallback, val total: Long) {
        var done = 0L
        private var lastEmit = 0L
        private var lastDone = 0L
        private var speed = 0.0
        val buf = ByteArray(1 shl 20)

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
            try { cb.onProgress(cur, done, total, speed) } catch (_: Throwable) {}
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

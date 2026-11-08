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
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.locks.ReentrantLock
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
        private const val VERSION = 2
        private const val MAX_PIN_FAILS = 5
    }

    @Volatile private var cancelled = false
    @Volatile private var running = false
    @Volatile private var server: ServerSocket? = null
    private val pinFails = AtomicInteger(0)
    @Volatile private var allowBrowse = false
    private val transferLock = ReentrantLock()
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

    // ---------------------------------------------------------------- jaringan: server
    override fun listen(destDir: String, pin: String, deviceName: String, cb: IProgressCallback): Int {
        stopListen()
        return try {
            val root = File(destDir)
            root.mkdirs()
            if (!root.isDirectory) throw IOException("Folder tujuan tidak valid")
            val ss = ServerSocket(0)
            server = ss
            pinFails.set(0)
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

    override fun setAllowBrowse(allow: Boolean) { allowBrowse = allow }

    private fun acceptLoop(ss: ServerSocket, root: File, pin: String, name: String, cb: IProgressCallback) {
        log(cb, "Server terima aktif di port ${ss.localPort}, simpan ke ${root.path}")
        while (!ss.isClosed) {
            val s = try { ss.accept() } catch (_: Exception) { break }
            active.add(s)
            // satu thread per koneksi: jelajah folder tidak menunggu transfer yang sedang berjalan
            Thread {
                try { incoming(s, root, pin, name, cb) }
                catch (_: Throwable) {}
                finally {
                    active.remove(s)
                    try { s.close() } catch (_: Exception) {}
                }
            }.start()
        }
        log(cb, "Server terima berhenti")
    }

    private class Ent(val rel: String, val dir: Boolean, val size: Long)
    private class Hello(val mode: Int, val peer: String, val mac: ByteArray)
    private class Outcome(val ok: Boolean, val msg: String, val done: Long, val total: Long)

    private fun reply(out: DataOutputStream, ok: Boolean, msg: String) {
        out.writeByte(if (ok) 1 else 0); out.writeUTF(msg); out.flush()
    }

    private fun incoming(s: Socket, root: File, pin: String, name: String, cb: IProgressCallback) {
        s.soTimeout = 60_000
        val inp = DataInputStream(BufferedInputStream(s.getInputStream(), 1 shl 16))
        val out = DataOutputStream(BufferedOutputStream(s.getOutputStream(), 1 shl 16))

        val nonce = ByteArray(16).also { SecureRandom().nextBytes(it) }
        try {
            out.writeInt(MAGIC); out.writeByte(VERSION); out.write(nonce); out.writeUTF(name); out.flush()
        } catch (_: IOException) { return }

        // Koneksi yang diputus sebelum autentikasi (mis. pengecekan "perangkat aktif") diabaikan diam-diam
        val hello = try {
            val m = inp.readByte().toInt()
            val p = inp.readUTF()
            val mac = ByteArray(32)
            inp.readFully(mac)
            Hello(m, p, mac)
        } catch (_: IOException) { return }

        val mode = hello.mode
        val shown = hello.peer.take(64)
        val from = s.inetAddress?.hostAddress ?: "?"

        if (!MessageDigest.isEqual(hello.mac, hmac(pin, nonce, mode, hello.peer))) {
            reply(out, false, "PIN salah")
            log(cb, "ERR PIN salah dari $shown ($from)")
            if (pinFails.incrementAndGet() >= MAX_PIN_FAILS) {
                stopListen()
                finish(cb, false, "Server dihentikan: terlalu banyak PIN salah", 0, 0)
            }
            return
        }
        pinFails.set(0)
        reply(out, true, "OK")

        when (mode) {
            1 -> log(cb, "OK     pairing dengan $shown ($from)")
            2 -> {
                log(cb, "Pengirim terhubung: $shown ($from)")
                serverTransfer(out, cb) { receiveSession(inp, out, root, shown, cb) }
            }
            3 -> {
                try { serveList(inp, out) }
                catch (e: Throwable) { log(cb, "ERR jelajah dari $shown: ${e.message}") }
            }
            4 -> {
                val paths = try { readPaths(inp) } catch (e: Throwable) { return }
                if (!allowBrowse) { reply(out, false, "Akses jelajah dimatikan di perangkat penerima"); return }
                val files = try { paths.map { resolveRemote(it) } }
                catch (e: SecurityException) { reply(out, false, "Path ditolak"); log(cb, "ERR ${e.message}"); return }
                reply(out, true, "OK")
                log(cb, "$shown mengambil ${files.size} item")
                serverTransfer(out, cb) { sendSession(inp, out, files, cb, shown) }
            }
            5 -> {
                val destStr = try { inp.readUTF() } catch (e: Throwable) { return }
                if (!allowBrowse) { reply(out, false, "Akses jelajah dimatikan di perangkat penerima"); return }
                val dest = try { resolveRemote(destStr) }
                catch (e: SecurityException) { reply(out, false, "Path ditolak"); log(cb, "ERR ${e.message}"); return }
                dest.mkdirs()
                if (!dest.isDirectory) { reply(out, false, "Folder tujuan tidak valid"); return }
                reply(out, true, "OK")
                log(cb, "$shown mengirim ke ${dest.path}")
                serverTransfer(out, cb) { receiveSession(inp, out, dest, shown, cb) }
            }
        }
    }

    /** Hanya satu transfer pada satu waktu di sisi server; hasilnya dilaporkan ke UI. */
    private fun serverTransfer(out: DataOutputStream, cb: IProgressCallback, block: () -> Outcome) {
        if (!transferLock.tryLock()) { try { reply(out, false, "Perangkat sedang sibuk") } catch (_: Throwable) {}; return }
        try {
            cancelled = false
            val r = block()
            finish(cb, r.ok, r.msg, r.done, r.total)
        } catch (e: Throwable) {
            if (cancelled) { log(cb, "ERR sesi dibatalkan"); finish(cb, false, "Sesi dibatalkan", 0, 0) }
            else { log(cb, "ERR sesi: ${e.message}"); finish(cb, false, "Sesi gagal: ${e.message}", 0, 0) }
        } finally {
            transferLock.unlock()
        }
    }

    private fun readPaths(inp: DataInputStream): List<String> {
        val n = inp.readInt()
        if (n < 0 || n > 10_000) throw IOException("Daftar tidak valid")
        return List(n) { inp.readUTF() }
    }

    private fun serveList(inp: DataInputStream, out: DataOutputStream) {
        val path = inp.readUTF()
        if (!allowBrowse) { reply(out, false, "Akses jelajah dimatikan di perangkat penerima"); return }
        val dir = try { resolveRemote(path) } catch (e: SecurityException) { reply(out, false, "Path ditolak"); return }
        if (!dir.isDirectory) { reply(out, false, "Bukan folder"); return }
        val kids = (dir.listFiles() ?: emptyArray())
            .sortedWith(compareBy<File>({ !it.isDirectory }, { it.name.lowercase() }))
        reply(out, true, "OK")
        out.writeInt(kids.size)
        for (k in kids) {
            out.writeUTF(k.name)
            out.writeLong(if (k.isFile) k.length() else 0L)
            out.writeByte(if (k.isDirectory) 1 else 0)
        }
        out.flush()
    }

    // Sisi penerima isi: baca manifest, simpan ke root, balas ACK.
    private fun receiveSession(inp: DataInputStream, out: DataOutputStream, root: File, label: String, cb: IProgressCallback): Outcome {
        val count = inp.readInt()
        val total = inp.readLong()
        if (count < 0 || count > 2_000_000 || total < 0) throw IOException("Manifest tidak valid")
        val list = ArrayList<Ent>(count)
        repeat(count) { list.add(Ent(inp.readUTF(), inp.readByte().toInt() == 1, inp.readLong())) }

        val targets = try {
            list.map { safeResolve(root, it.rel) }
        } catch (e: SecurityException) {
            reply(out, false, "Path ditolak")
            return Outcome(false, "Path ditolak: ${e.message}", 0, total)
        }
        if (root.usableSpace < total) {
            reply(out, false, "Ruang penyimpanan tidak cukup")
            return Outcome(false, "Ruang tidak cukup (butuh ${fmt(total)})", 0, total)
        }
        reply(out, true, "SIAP")
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
        return Outcome(true, "Selesai: $files file diterima dari $label (${fmt(job.done)})", job.done, total)
    }

    private class SendEnt(val rel: String, val f: File, val dir: Boolean, val size: Long)

    private fun buildManifest(srcs: List<File>): List<SendEnt> {
        val list = ArrayList<SendEnt>()
        fun walk(rel: String, f: File) {
            if (f.isDirectory) {
                list.add(SendEnt(rel, f, true, 0))
                f.listFiles()?.sortedBy { it.name }?.forEach { walk("$rel/${it.name}", it) }
            } else if (f.isFile) {
                list.add(SendEnt(rel, f, false, f.length()))
            }
        }
        for (f in srcs) walk(f.name, f)
        return list
    }

    // Sisi pengirim isi: tulis manifest, tunggu SIAP, alirkan file, tunggu ACK.
    private fun sendSession(inp: DataInputStream, out: DataOutputStream, srcs: List<File>, cb: IProgressCallback, label: String): Outcome {
        val list = buildManifest(srcs)
        val total = list.sumOf { it.size }
        val fileCount = list.count { !it.dir }
        log(cb, "Menyiapkan ${list.size} entri ($fileCount file, ${fmt(total)})")

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
        return Outcome(true, "Selesai: $fileCount file terkirim ke $label (${fmt(job.done)})", job.done, total)
    }

    // ---------------------------------------------------------------- jaringan: klien
    private class Conn(val s: Socket, val inp: DataInputStream, val out: DataOutputStream, val peerName: String)

    private fun <T> withConn(host: String, port: Int, mode: Int, pin: String, name: String,
                             connectMs: Int, readMs: Int, block: (Conn) -> T): T {
        val s = Socket()
        active.add(s)
        try {
            s.connect(InetSocketAddress(host, port), connectMs)
            s.soTimeout = readMs
            val inp = DataInputStream(BufferedInputStream(s.getInputStream(), 1 shl 16))
            val out = DataOutputStream(BufferedOutputStream(s.getOutputStream(), 1 shl 16))
            val peerName = handshake(inp, out, mode, pin, name)
            return block(Conn(s, inp, out, peerName))
        } finally {
            active.remove(s)
            try { s.close() } catch (_: Exception) {}
        }
    }

    override fun checkPair(host: String, port: Int, pin: String, deviceName: String): String = try {
        withConn(host, port, 1, pin, deviceName, 5000, 8000) { }
        ""
    } catch (t: Throwable) {
        t.message ?: t.javaClass.simpleName
    }

    override fun remoteList(host: String, port: Int, pin: String, deviceName: String, path: String): Array<String> {
        return try {
            withConn(host, port, 3, pin, deviceName, 5000, 15_000) { c ->
                c.out.writeUTF(path); c.out.flush()
                if (c.inp.readByte().toInt() != 1) {
                    arrayOf(c.inp.readUTF())
                } else {
                    c.inp.readUTF()
                    val n = c.inp.readInt()
                    if (n < 0 || n > 200_000) throw IOException("Daftar tidak valid")
                    val res = ArrayList<String>(n + 1)
                    res.add("OK")
                    repeat(n) {
                        val nm = c.inp.readUTF()
                        val sz = c.inp.readLong()
                        val d = c.inp.readByte().toInt()
                        res.add("$nm\t$sz\t$d")
                    }
                    res.toTypedArray()
                }
            }
        } catch (t: Throwable) {
            arrayOf(t.message ?: t.javaClass.simpleName)
        }
    }

    private fun startClient(cb: IProgressCallback, block: () -> Unit) {
        if (running) { cb.onFinish(false, "Transfer lain masih berjalan", 0, 0); return }
        cancelled = false
        running = true
        Thread {
            try { block() }
            catch (t: Throwable) {
                if (cancelled) { log(cb, "ERR dibatalkan"); finish(cb, false, "Dibatalkan", 0, 0) }
                else { log(cb, "ERR ${t.message}"); finish(cb, false, "Gagal: ${t.message}", 0, 0) }
            } finally { running = false }
        }.start()
    }

    override fun send(host: String, port: Int, pin: String, deviceName: String, srcs: Array<String>, cb: IProgressCallback) =
        startClient(cb) { doSend(host, port, pin, deviceName, srcs, null, cb) }

    override fun push(host: String, port: Int, pin: String, deviceName: String, srcs: Array<String>,
                      remoteDir: String, cb: IProgressCallback) =
        startClient(cb) { doSend(host, port, pin, deviceName, srcs, remoteDir, cb) }

    /** remoteDir == null: kirim ke folder terima (mode 2); selain itu ke folder pilihan di perangkat lain (mode 5). */
    private fun doSend(host: String, port: Int, pin: String, name: String, srcs: Array<String>, remoteDir: String?, cb: IProgressCallback) {
        withConn(host, port, if (remoteDir == null) 2 else 5, pin, name, 8000, 60_000) { c ->
            log(cb, "OK     terautentikasi ke ${c.peerName}")
            if (remoteDir != null) {
                c.out.writeUTF(remoteDir); c.out.flush()
                if (c.inp.readByte().toInt() != 1) throw IOException("Ditolak penerima: ${c.inp.readUTF()}")
                c.inp.readUTF()
            }
            val r = sendSession(c.inp, c.out, srcs.map { File(it) }, cb, c.peerName)
            finish(cb, r.ok, r.msg, r.done, r.total)
        }
    }

    override fun pull(host: String, port: Int, pin: String, deviceName: String, remotePaths: Array<String>,
                      destDir: String, cb: IProgressCallback) = startClient(cb) {
        withConn(host, port, 4, pin, deviceName, 8000, 60_000) { c ->
            log(cb, "OK     terautentikasi ke ${c.peerName}")
            c.out.writeInt(remotePaths.size)
            remotePaths.forEach { c.out.writeUTF(it) }
            c.out.flush()
            if (c.inp.readByte().toInt() != 1) throw IOException("Ditolak: ${c.inp.readUTF()}")
            c.inp.readUTF()
            val dest = File(destDir)
            dest.mkdirs()
            if (!dest.isDirectory) throw IOException("Folder tujuan tidak valid")
            val r = receiveSession(c.inp, c.out, dest, c.peerName, cb)
            finish(cb, r.ok, r.msg, r.done, r.total)
        }
    }

    // ---------------------------------------------------------------- util
    private fun handshake(inp: DataInputStream, out: DataOutputStream, mode: Int, pin: String, name: String): String {
        if (inp.readInt() != MAGIC) throw IOException("Bukan server Ngirim")
        if (inp.readByte().toInt() != VERSION) throw IOException("Versi protokol berbeda - perbarui Ngirim di kedua perangkat")
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

    /** Akses jarak jauh dibatasi ke /sdcard (path kanonik, jadi ".." dan symlink tidak bisa keluar). */
    private fun resolveRemote(p: String): File {
        if (!p.startsWith("/")) throw SecurityException("path harus absolut: $p")
        val root = File("/sdcard").canonicalFile.path
        val f = File(p).canonicalFile
        if (f.path != root && !f.path.startsWith("$root/")) throw SecurityException("akses di luar /sdcard ditolak: $p")
        return f
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

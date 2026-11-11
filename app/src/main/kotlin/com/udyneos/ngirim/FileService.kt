package com.udyneos.ngirim

import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
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
import java.text.SimpleDateFormat
import java.util.Collections
import java.util.Date
import java.util.Locale
import java.util.zip.Deflater
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
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
        private const val VERSION = 3   // 3: manifest membawa waktu ubah + penerima mengirim offset lanjutan
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
            try { cleanStaleParts(root) } catch (_: Throwable) {}
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

    private class Ent(val rel: String, val dir: Boolean, val size: Long, val mtime: Long)

    /** Penolakan dari lawan bicara (bukan gangguan jaringan). */
    private class Rejected(msg: String) : IOException(msg)

    /** File sementara penerima: nama + ukuran + waktu ubah sumber, jadi hanya cocok dengan file yang sama persis. */
    private fun partFile(target: File, size: Long, mtime: Long) =
        File(target.parentFile, "${target.name}.$size-$mtime.ngpart")

    /** Hapus sisa transfer yang tak pernah dilanjutkan (> 7 hari). */
    private fun cleanStaleParts(dir: File, depth: Int = 0) {
        if (depth > 8) return
        val cutoff = System.currentTimeMillis() - 7L * 24 * 3600 * 1000
        dir.listFiles()?.forEach {
            if (it.isDirectory) cleanStaleParts(it, depth + 1)
            else if (it.name.endsWith(".ngpart") && it.lastModified() < cutoff) it.delete()
        }
    }
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
        repeat(count) { list.add(Ent(inp.readUTF(), inp.readByte().toInt() == 1, inp.readLong(), inp.readLong())) }

        val targets = try {
            list.map { safeResolve(root, it.rel) }
        } catch (e: SecurityException) {
            reply(out, false, "Path ditolak")
            return Outcome(false, "Path ditolak: ${e.message}", 0, total)
        }
        // sisa transfer sebelumnya: berapa byte tiap file sudah ada
        val offsets = LongArray(list.size)
        for ((i, e) in list.withIndex()) {
            if (e.dir) continue
            val p = partFile(targets[i], e.size, e.mtime)
            if (p.isFile) {
                val len = p.length()
                if (len in 1..e.size) offsets[i] = len else p.delete()
            }
        }
        val resumed = offsets.sum()
        if (root.usableSpace < total - resumed) {
            reply(out, false, "Ruang penyimpanan tidak cukup")
            return Outcome(false, "Ruang tidak cukup (butuh ${fmt(total - resumed)})", 0, total)
        }
        reply(out, true, "SIAP")
        for (o in offsets) out.writeLong(o)
        out.flush()
        log(cb, "Menerima ${list.size} entri (${fmt(total)})")
        if (resumed > 0) log(cb, "Melanjutkan: ${fmt(resumed)} sudah ada")

        val job = Job(cb, total)
        job.skip(resumed)
        var files = 0
        for ((i, e) in list.withIndex()) {
            val target = targets[i]
            if (e.dir) { target.mkdirs(); continue }
            target.parentFile?.mkdirs()
            val part = partFile(target, e.size, e.mtime)
            // Gangguan di tengah jalan meninggalkan file .ngpart supaya bisa dilanjutkan; dihapus otomatis setelah 7 hari
            run {
                FileOutputStream(part, offsets[i] > 0).use { o ->
                    var left = e.size - offsets[i]
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
            }
            val dst = uniqueFile(target)
            if (!part.renameTo(dst)) throw IOException("Gagal menyimpan ${e.rel}")
            if (e.mtime > 0) dst.setLastModified(e.mtime)
            files++
            log(cb, "OK     ${e.rel}")
        }
        job.emit("", force = true)
        out.writeByte(1); out.flush()
        return Outcome(true, "Selesai: $files file diterima dari $label (${fmt(job.done)})", job.done, total)
    }

    private class SendEnt(val rel: String, val f: File, val dir: Boolean, val size: Long, val mtime: Long)

    private fun buildManifest(srcs: List<File>): List<SendEnt> {
        val list = ArrayList<SendEnt>()
        fun walk(rel: String, f: File) {
            if (f.isDirectory) {
                list.add(SendEnt(rel, f, true, 0, 0))
                f.listFiles()?.sortedBy { it.name }?.forEach { walk("$rel/${it.name}", it) }
            } else if (f.isFile) {
                list.add(SendEnt(rel, f, false, f.length(), f.lastModified()))
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
        for (e in list) { out.writeUTF(e.rel); out.writeByte(if (e.dir) 1 else 0); out.writeLong(e.size); out.writeLong(e.mtime) }
        out.flush()
        if (inp.readByte().toInt() != 1) throw Rejected("Ditolak penerima: ${inp.readUTF()}")
        inp.readUTF()
        val offsets = LongArray(list.size) { inp.readLong() }

        val job = Job(cb, total)
        job.skip(offsets.sum())
        if (offsets.any { it > 0 }) log(cb, "Melanjutkan: ${fmt(offsets.sum())} sudah diterima")
        for ((idx, e) in list.withIndex()) {
            if (e.dir) continue
            val off = offsets[idx]
            if (off < 0 || off > e.size) throw IOException("Offset lanjutan tidak valid")
            FileInputStream(e.f).use { i ->
                if (off > 0) i.channel.position(off)
                var left = e.size - off
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

    /**
     * Jalankan satu sesi transfer; bila koneksi putus di tengah jalan, sambung lagi dan lanjutkan dari
     * byte terakhir (penerima menyimpan sisa file .ngpart). Penolakan (PIN salah dsb.) tidak diulang.
     */
    private fun withResume(cb: IProgressCallback, session: () -> Unit) {
        val maxTry = 5
        var attempt = 1
        while (true) {
            try { session(); return }
            catch (t: Throwable) {
                val busy = t is Rejected && (t.message ?: "").contains("sibuk")
                val net = t is java.net.SocketException || t is EOFException || t is java.net.SocketTimeoutException
                if (cancelled || attempt >= maxTry || !(busy || net)) throw t
                log(cb, "Koneksi terputus (${t.message ?: t.javaClass.simpleName}), mencoba lanjut $attempt/${maxTry - 1}...")
                var waited = 0L
                while (waited < 3000L * attempt && !cancelled) { Thread.sleep(250); waited += 250 }
                if (cancelled) throw t
                attempt++
            }
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
                else {
                    log(cb, "ERR ${t.message}")
                    val resumable = t is java.net.SocketException || t is EOFException || t is java.net.SocketTimeoutException
                    finish(cb, false, "Gagal: ${t.message}" + if (resumable) ". Kirim ulang untuk melanjutkan dari titik terakhir." else "", 0, 0)
                }
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
        withResume(cb) {
          withConn(host, port, if (remoteDir == null) 2 else 5, pin, name, 8000, 60_000) { c ->
            log(cb, "OK     terautentikasi ke ${c.peerName}")
            if (remoteDir != null) {
                c.out.writeUTF(remoteDir); c.out.flush()
                if (c.inp.readByte().toInt() != 1) throw Rejected("Ditolak penerima: ${c.inp.readUTF()}")
                c.inp.readUTF()
            }
            val r = sendSession(c.inp, c.out, srcs.map { File(it) }, cb, c.peerName)
            finish(cb, r.ok, r.msg, r.done, r.total)
          }
        }
    }

    override fun pull(host: String, port: Int, pin: String, deviceName: String, remotePaths: Array<String>,
                      destDir: String, cb: IProgressCallback) = startClient(cb) {
        withResume(cb) {
         withConn(host, port, 4, pin, deviceName, 8000, 60_000) { c ->
            log(cb, "OK     terautentikasi ke ${c.peerName}")
            c.out.writeInt(remotePaths.size)
            remotePaths.forEach { c.out.writeUTF(it) }
            c.out.flush()
            if (c.inp.readByte().toInt() != 1) throw Rejected("Ditolak: ${c.inp.readUTF()}")
            c.inp.readUTF()
            val dest = File(destDir)
            dest.mkdirs()
            if (!dest.isDirectory) throw IOException("Folder tujuan tidak valid")
            val r = receiveSession(c.inp, c.out, dest, c.peerName, cb)
            finish(cb, r.ok, r.msg, r.done, r.total)
         }
        }
    }


    // ---------------------------------------------------------------- file manager
    override fun listDetailed(dir: String): Array<String> {
        val kids = File(dir).listFiles() ?: return emptyArray()
        return kids.map {
            "${it.name}\t${if (it.isFile) it.length() else 0L}\t${if (it.isDirectory) 1 else 0}\t${it.lastModified()}"
        }.toTypedArray()
    }

    override fun rename(from: String, to: String): Boolean = try {
        val t = File(to)
        !t.exists() && File(from).renameTo(t)
    } catch (_: Throwable) { false }

    override fun pathSize(path: String): Long = try { sizeOf(File(path)) } catch (_: Throwable) { 0L }

    /** Hapus; bila trashDir tidak kosong, pindahkan ke <trashDir>/<stempel>/ (kecuali Android/data, Android/obb, dan isi sampah). */
    override fun deletePaths(paths: Array<String>, trashDir: String, cb: IProgressCallback) =
        startClient(cb) { doDelete(paths, trashDir, cb) }

    private fun doDelete(paths: Array<String>, trashDir: String, cb: IProgressCallback) {
        var ok = 0
        var failed = 0
        val total = paths.size.toLong()
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val bin = if (trashDir.isEmpty()) null else File(trashDir, stamp)
        val origin = StringBuilder()
        for ((i, p) in paths.withIndex()) {
            if (cancelled) break
            val f = File(p)
            try {
                cb.onProgress(f.name, i.toLong(), total, 0.0)
                val permanent = bin == null || p.startsWith("/sdcard/Android/data") || p.startsWith("/sdcard/Android/obb") ||
                        p.startsWith(trashDir)
                if (permanent) {
                    if (!f.deleteRecursively()) throw IOException("tidak bisa dihapus")
                } else {
                    bin!!.mkdirs()
                    val target = uniqueFile(File(bin, f.name))
                    if (!f.renameTo(target)) {
                        Job(cb, 0).copy(f, target)
                        if (!f.deleteRecursively()) throw IOException("salinan ke sampah ok, asli tidak terhapus")
                    }
                    origin.append(target.name).append('\t').append(p).append('\n')
                }
                ok++
                log(cb, "OK     hapus ${f.name}")
            } catch (e: Throwable) {
                failed++
                log(cb, "GAGAL  hapus ${f.name}: ${e.message}")
            }
        }
        if (bin != null && origin.isNotEmpty()) try { File(bin, ".asal.txt").writeText(origin.toString()) } catch (_: Throwable) {}
        cb.onProgress("", ok.toLong() + failed, total, 0.0)
        finish(cb, !cancelled && failed == 0,
            (if (cancelled) "Dibatalkan: " else "Selesai: ") + "$ok dihapus, $failed gagal", ok.toLong() + failed, total)
    }

    // ---------------------------------------------------------------- backup XAPK
    override fun backupXapk(jobs: Array<String>, cb: IProgressCallback) = startClient(cb) { doXapk(jobs, cb) }

    // ---------------------------------------------------------------- pasang APK + ekstrak ZIP
    override fun installApk(paths: Array<String>, cb: IProgressCallback) {
        Thread {
            var ok = 0
            var failed = 0
            for (p in paths) {
                try {
                    // Isi APK dialirkan lewat stdin (-S) agar `pm` tidak perlu akses ke path-nya
                    val f = File(p)
                    val pr = ProcessBuilder("pm", "install", "-r", "-S", f.length().toString()).redirectErrorStream(true).start()
                    Thread {
                        try { pr.outputStream.use { os -> FileInputStream(f).use { it.copyTo(os, 1 shl 16) } } } catch (_: Throwable) {}
                    }.start()
                    val out = pr.inputStream.bufferedReader().readText().trim()
                    val code = pr.waitFor()
                    if (code == 0 && out.contains("Success", ignoreCase = true)) {
                        ok++; log(cb, "OK     terpasang ${File(p).name}")
                    } else {
                        failed++; log(cb, "GAGAL  ${File(p).name}: ${out.lines().lastOrNull { it.isNotBlank() } ?: "kode $code"}")
                    }
                } catch (t: Throwable) {
                    failed++; log(cb, "GAGAL  ${File(p).name}: ${t.message}")
                }
            }
            finish(cb, failed == 0, "Pasang APK: $ok berhasil, $failed gagal", ok.toLong(), paths.size.toLong())
        }.start()
    }

    override fun unzip(zip: String, destDir: String, cb: IProgressCallback) = startClient(cb) { doUnzip(zip, destDir, cb) }
    override fun installBundle(path: String, cb: IProgressCallback) = startClient(cb) { doInstallBundle(path, cb) }
    override fun zip(srcs: Array<String>, outZip: String, cb: IProgressCallback) = startClient(cb) { doZip(srcs, outZip, cb) }

    // ---- Pasang bundle (XAPK / APKS / APKM): split APK lewat satu sesi `pm`, OBB/data disalin setelahnya
    private val abiNames = listOf("arm64_v8a", "armeabi_v7a", "armeabi", "x86_64", "x86", "mips64", "mips")
    private val densityDpi = mapOf("ldpi" to 120, "mdpi" to 160, "tvdpi" to 213, "hdpi" to 240,
        "xhdpi" to 320, "xxhdpi" to 480, "xxxhdpi" to 640)

    private fun token(name: String, set: Collection<String>): String? {
        val n = name.substringAfterLast('/').lowercase().removeSuffix(".apk").replace('-', '.')
        return n.split('.').firstOrNull { it in set }
    }

    /** Pilih split yang cocok dengan perangkat: satu ABI terbaik dan satu kepadatan layar terdekat. */
    private fun pickSplits(apks: List<java.util.zip.ZipEntry>): List<java.util.zip.ZipEntry> {
        var list = apks
        try {
            val supported = android.os.Build.SUPPORTED_ABIS.map { it.lowercase().replace('-', '_') }
            val present = list.mapNotNull { token(it.name, abiNames) }.toSet()
            val best = supported.firstOrNull { it in present }
            if (best != null) list = list.filter { val t = token(it.name, abiNames); t == null || t == best }
        } catch (_: Throwable) {}
        try {
            val dpi = android.content.res.Resources.getSystem().displayMetrics.densityDpi
            val present = list.mapNotNull { token(it.name, densityDpi.keys) }.toSet()
            val best = present.minByOrNull { Math.abs((densityDpi[it] ?: 0) - dpi) }
            if (best != null) list = list.filter { val t = token(it.name, densityDpi.keys); t == null || t == best }
        } catch (_: Throwable) {}
        return list
    }

    /** Jalankan `pm` dengan stdin opsional; kembalikan (kode, keluaran). */
    private fun runPm(args: List<String>, feed: ((java.io.OutputStream) -> Unit)? = null): Pair<Int, String> {
        val pr = ProcessBuilder(listOf("pm") + args).redirectErrorStream(true).start()
        var feedErr: Throwable? = null
        val t = Thread {
            try { pr.outputStream.use { feed?.invoke(it) } } catch (e: Throwable) { feedErr = e }
        }
        t.start()
        val out = pr.inputStream.bufferedReader().readText().trim()
        val code = pr.waitFor()
        t.join(2000)
        feedErr?.let { if (code == 0) throw it }
        return code to out
    }

    private fun doInstallBundle(path: String, cb: IProgressCallback) {
        java.util.zip.ZipFile(path).use { zf ->
            val entries = zf.entries().toList().filter { !it.isDirectory }
            val man = entries.firstOrNull { it.name == "manifest.json" }?.let {
                try { JSONObject(zf.getInputStream(it).bufferedReader().readText()) } catch (_: Throwable) { null }
            }
            val pkg = man?.optString("package_name", "") ?: ""

            // daftar APK: urutan di manifest XAPK bila ada, kalau tidak semua .apk (kecuali universal/standalone bila ada split)
            var apks = entries.filter { it.name.endsWith(".apk", ignoreCase = true) }
            val named = man?.optJSONArray("split_apks")?.let { arr ->
                (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.optString("file") }.toSet()
            }.orEmpty()
            if (named.isNotEmpty() && apks.any { it.name in named }) {
                apks = apks.filter { it.name in named }
            } else {
                val splits = apks.filter { it.name.startsWith("splits/") }
                if (splits.isNotEmpty()) apks = splits
                else apks = apks.filter { !it.name.startsWith("standalones/") && !it.name.startsWith("instant/") && !it.name.startsWith("asset-slices/") }
                    .let { l -> if (l.size > 1) l.filter { it.name != "universal.apk" } else l }
            }
            apks = pickSplits(apks)
            if (apks.isEmpty()) throw IOException("tidak ada APK di dalam ${File(path).name}")

            // OBB / data: entri yang berawalan Android/
            val extras = entries.filter {
                it.name.startsWith("Android/obb/") || it.name.startsWith("Android/data/")
            }
            val total = apks.sumOf { it.size } + extras.sumOf { it.size }
            val job = Job(cb, total)
            log(cb, "Memasang ${File(path).name}: ${apks.size} APK" + if (extras.isNotEmpty()) ", ${extras.size} berkas OBB/data" else "")

            val (c1, o1) = runPm(listOf("install-create", "-r"))
            val sid = Regex("\\[(\\d+)]").find(o1)?.groupValues?.get(1)
            if (c1 != 0 || sid == null) throw IOException("sesi pemasangan gagal: ${o1.lines().lastOrNull() ?: "kode $c1"}")
            try {
                for ((i, e) in apks.withIndex()) {
                    if (cancelled) throw InterruptedException("cancel")
                    val label = "${i}_" + e.name.substringAfterLast('/').replace(Regex("[^A-Za-z0-9._-]"), "_")
                    val (c, o) = runPm(listOf("install-write", "-S", e.size.toString(), sid, label, "-")) { os ->
                        zf.getInputStream(e).use { ins ->
                            while (true) {
                                val n = ins.read(job.buf)
                                if (n < 0) break
                                os.write(job.buf, 0, n)
                                job.done += n
                                job.emit(e.name.substringAfterLast('/'))
                                if (cancelled) throw InterruptedException("cancel")
                            }
                        }
                    }
                    if (c != 0) throw IOException("${e.name}: ${o.lines().lastOrNull() ?: "kode $c"}")
                }
                val (c2, o2) = runPm(listOf("install-commit", sid))
                if (c2 != 0 || !o2.contains("Success", ignoreCase = true))
                    throw IOException(o2.lines().lastOrNull { it.isNotBlank() } ?: "kode $c2")
            } catch (t: Throwable) {
                try { runPm(listOf("install-abandon", sid)) } catch (_: Throwable) {}
                throw t
            }
            log(cb, "OK     APK terpasang" + if (pkg.isNotEmpty()) " ($pkg)" else "")

            // OBB/data
            val androidRoot = File("/sdcard/Android").canonicalFile
            var copied = 0
            for (e in extras) {
                if (cancelled) throw InterruptedException("cancel")
                val dst = File("/sdcard", e.name).canonicalFile
                if (!dst.path.startsWith(androidRoot.path + "/")) { log(cb, "ERR lewati ${e.name}: path tidak aman"); continue }
                dst.parentFile?.mkdirs()
                zf.getInputStream(e).use { ins ->
                    FileOutputStream(dst).use { o ->
                        while (true) {
                            val n = ins.read(job.buf)
                            if (n < 0) break
                            o.write(job.buf, 0, n)
                            job.done += n
                            job.emit(dst.name)
                            if (cancelled) throw InterruptedException("cancel")
                        }
                    }
                }
                copied++
            }
            job.emit("", force = true)
            finish(cb, true, "Terpasang: ${File(path).name} (${apks.size} APK" + (if (copied > 0) ", $copied OBB/data" else "") + ")", job.done, total)
        }
    }

    private fun doZip(srcs: Array<String>, outZip: String, cb: IProgressCallback) {
        val out = uniqueFile(File(outZip))
        out.parentFile?.mkdirs()
        // hitung total dulu supaya progress bermakna
        fun sizeOfTree(f: File): Long =
            if (f.isDirectory) (f.listFiles() ?: emptyArray()).sumOf { sizeOfTree(it) } else f.length()
        val files = srcs.map { File(it) }.filter { it.exists() }
        val total = files.sumOf { sizeOfTree(it) }
        var done = 0L
        var n = 0
        var last = 0L
        val buf = ByteArray(1 shl 16)
        val outCanon = out.canonicalPath
        try {
            java.util.zip.ZipOutputStream(java.io.BufferedOutputStream(FileOutputStream(out))).use { zo ->
                fun add(f: File, name: String) {
                    if (cancelled) throw InterruptedException("cancel")
                    if (f.canonicalPath == outCanon) return          // jangan masukkan arsip ke dirinya sendiri
                    if (f.isDirectory) {
                        zo.putNextEntry(java.util.zip.ZipEntry("$name/"))
                        zo.closeEntry()
                        for (c in (f.listFiles() ?: emptyArray()).sortedBy { it.name }) add(c, "$name/${c.name}")
                        return
                    }
                    val ze = java.util.zip.ZipEntry(name)
                    ze.time = f.lastModified()
                    zo.putNextEntry(ze)
                    try {
                        java.io.FileInputStream(f).use { ins ->
                            while (true) {
                                val r = ins.read(buf)
                                if (r < 0) break
                                zo.write(buf, 0, r)
                                done += r
                                val now = System.nanoTime()
                                if (now - last > 150_000_000L) { last = now; try { cb.onProgress(name, done, total, 0.0) } catch (_: Throwable) {} }
                            }
                        }
                    } catch (e: IOException) {
                        // file tak terbaca (mis. izin): lewati, jangan gagalkan seluruh arsip
                        try { cb.onLog("ERR lewati ${f.name}: ${e.message}") } catch (_: Throwable) {}
                    }
                    zo.closeEntry()
                    n++
                }
                for (f in files) add(f, f.name)
            }
        } catch (t: Throwable) {
            out.delete()
            throw t
        }
        finish(cb, true, "Arsip dibuat: ${out.name} ($n file)", done, total)
    }

    private fun doUnzip(zip: String, destDir: String, cb: IProgressCallback) {
        val src = File(zip)
        val target = uniqueFile(File(destDir, src.nameWithoutExtension.ifEmpty { "arsip" }))
        target.mkdirs()
        val root = target.canonicalPath
        java.util.zip.ZipFile(src).use { zf ->
            val entries = zf.entries().toList()
            val total = entries.sumOf { it.size.coerceAtLeast(0L) }
            var done = 0L
            var n = 0
            var last = 0L
            val buf = ByteArray(1 shl 16)
            for (e in entries) {
                if (cancelled) throw InterruptedException("cancel")
                val out = File(target, e.name)
                if (!out.canonicalPath.startsWith(root + File.separator) && out.canonicalPath != root)
                    throw IOException("entri tidak aman: ${e.name}")
                if (e.isDirectory) { out.mkdirs(); continue }
                out.parentFile?.mkdirs()
                zf.getInputStream(e).use { ins ->
                    FileOutputStream(out).use { os ->
                        while (true) {
                            val r = ins.read(buf)
                            if (r < 0) break
                            os.write(buf, 0, r)
                            done += r
                            val now = System.nanoTime()
                            if (now - last > 150_000_000L) { last = now; try { cb.onProgress(e.name, done, total, 0.0) } catch (_: Throwable) {} }
                        }
                    }
                }
                n++
            }
            finish(cb, true, "Diekstrak $n file ke ${target.name}", done, total)
        }
    }

    private class XJob(val j: JSONObject) {
        val out: String = j.getString("out")
        val obb: Boolean = j.optBoolean("obb", false)
        val data: Boolean = j.optBoolean("data", false)
        val apks: List<Pair<String, String>> = j.getJSONArray("apks").let { a ->
            List(a.length()) { val o = a.getJSONObject(it); o.getString("path") to o.getString("entry") }
        }
    }

    /**
     * Satu job = {out, obb, icon_b64, manifest{...}, apks:[{path,entry}]}.
     * Hasil: ZIP (.xapk) berisi manifest.json, icon.png, APK (base + split), dan opsional isi OBB/DATA (Android/obb/<pkg> dan Android/data/<pkg>).
     */
    private fun doXapk(jobs: Array<String>, cb: IProgressCallback) {
        val parsed = jobs.map { XJob(JSONObject(it)) }
        var total = 0L
        for (x in parsed) {
            total += x.apks.sumOf { File(it.first).length() }
            val pk = x.j.getJSONObject("manifest").getString("package_name")
            if (x.obb) total += extraFiles("obb", pk).sumOf { it.length() }
            if (x.data) total += extraFiles("data", pk).sumOf { it.length() }
        }
        val job = Job(cb, total)
        var ok = 0
        var failed = 0
        for (x in parsed) {
            if (cancelled) break
            val pkg = x.j.getJSONObject("manifest").getString("package_name")
            var tmp: File? = null
            try {
                // pasangan (folder dasar, file): OBB lalu DATA, masing-masing di bawah Android/<jenis>/<pkg>/
                val extras = ArrayList<Pair<String, File>>()
                if (x.obb) extraFiles("obb", pkg).forEach { extras += "obb" to it }
                if (x.data) extraFiles("data", pkg).forEach { extras += "data" to it }
                val size = x.apks.sumOf { File(it.first).length() } + extras.sumOf { it.second.length() }
                if (size >= 0xFFFFFFFFL - 1_000_000L)
                    throw IOException("melebihi batas ZIP 4 GB - matikan opsi OBB/DATA")
                val outFile = uniqueFile(File(x.out))
                outFile.parentFile?.mkdirs()
                tmp = File(outFile.path + ".part")

                val m = x.j.getJSONObject("manifest")
                m.put("total_size", size)
                if (extras.isNotEmpty()) {
                    val ex = JSONArray()
                    for ((kind, f) in extras) {
                        val rel = "Android/$kind/$pkg/" + f.relativeTo(File("/sdcard/Android/$kind/$pkg")).path
                        ex.put(JSONObject().put("file", rel).put("install_location", "EXTERNAL_STORAGE").put("install_path", rel))
                    }
                    m.put("expansions", ex)
                }

                ZipOutputStream(BufferedOutputStream(FileOutputStream(tmp), 1 shl 16)).use { z ->
                    z.setLevel(Deflater.DEFAULT_COMPRESSION)
                    z.putNextEntry(ZipEntry("manifest.json"))
                    z.write(m.toString(2).toByteArray()); z.closeEntry()
                    val icon = x.j.optString("icon_b64", "")
                    if (icon.isNotEmpty()) {
                        z.putNextEntry(ZipEntry("icon.png"))
                        z.write(Base64.decode(icon, Base64.DEFAULT)); z.closeEntry()
                    }
                    z.setLevel(Deflater.NO_COMPRESSION) // APK/OBB sudah terkompresi
                    for ((path, entry) in x.apks) zipFile(z, File(path), entry, job)
                    for ((kind, f) in extras)
                        zipFile(z, f, "Android/$kind/$pkg/" + f.relativeTo(File("/sdcard/Android/$kind/$pkg")).path, job)
                }
                if (!tmp.renameTo(outFile)) throw IOException("gagal menamai hasil")
                ok++
                log(cb, "OK     ${outFile.name} (${fmt(outFile.length())})")
            } catch (e: InterruptedException) {
                tmp?.delete(); break
            } catch (e: Throwable) {
                tmp?.delete()
                failed++
                log(cb, "GAGAL  $pkg: ${e.message}")
            }
        }
        job.emit("", force = true)
        finish(cb, !cancelled && failed == 0,
            (if (cancelled) "Dibatalkan: " else "Selesai: ") + "$ok XAPK dibuat, $failed gagal", job.done, total)
    }

    /** kind = "obb" atau "data": semua file di /sdcard/Android/<kind>/<pkg>. */
    private fun extraFiles(kind: String, pkg: String): List<File> =
        File("/sdcard/Android/$kind/$pkg").takeIf { it.isDirectory }?.walkTopDown()?.filter { it.isFile }?.toList() ?: emptyList()

    private fun zipFile(z: ZipOutputStream, f: File, entry: String, job: Job) {
        z.putNextEntry(ZipEntry(entry))
        FileInputStream(f).use { i ->
            while (true) {
                val n = i.read(job.buf)
                if (n < 0) break
                z.write(job.buf, 0, n)
                job.done += n
                job.emit(f.name)
                if (cancelled) throw InterruptedException("cancel")
            }
        }
        z.closeEntry()
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

        /** Byte yang sudah ada dihitung selesai tanpa merusak perhitungan kecepatan. */
        fun skip(n: Long) { done += n; lastDone += n }

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

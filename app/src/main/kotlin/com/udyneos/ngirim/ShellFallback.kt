package com.udyneos.ngirim

import android.content.Context
import android.os.RemoteException
import rikka.shizuku.Shizuku
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.security.SecureRandom
import java.util.IdentityHashMap
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ExecutionException
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/**
 * Jalur cadangan tanpa UserService. Proses [ShellHost] dijalankan lewat Shizuku.Process
 * (uid shell), lalu kelas ini menjadi IFileService lokal yang meneruskan tiap panggilan
 * ke proses itu lewat socket localhost bertoken. Bagi kode lain ia sama saja dengan
 * binder UserService.
 */
class ShellFallback private constructor(
    private val proc: Process,
    private val sock: Socket,
) : IFileService.Stub() {

    @Volatile private var alive = true
    private val inp = DataInputStream(BufferedInputStream(sock.getInputStream()))
    private val out = DataOutputStream(BufferedOutputStream(sock.getOutputStream()))
    private val wlock = Any()
    private val nextId = AtomicInteger(1)
    private val pending = ConcurrentHashMap<Int, CompletableFuture<Any?>>()
    private val cbIds = IdentityHashMap<IProgressCallback, Int>()
    private val cbById = ConcurrentHashMap<Int, IProgressCallback>()
    private val nextCb = AtomicInteger(1)
    private val cbExec = java.util.concurrent.Executors.newSingleThreadExecutor { r -> Thread(r, "ngirim-fallback-cb").apply { isDaemon = true } }

    init {
        Thread({ readLoop() }, "ngirim-fallback-rx").apply { isDaemon = true }.start()
    }

    // Pengganti ping binder: hidup selama soket ke proses shell masih terbuka.
    override fun pingBinder(): Boolean = alive
    override fun isBinderAlive(): Boolean = alive

    private fun cbId(cb: IProgressCallback): Int = synchronized(cbIds) {
        cbIds.getOrPut(cb) { nextCb.getAndIncrement().also { cbById[it] = cb } }
    }

    private fun readLoop() {
        try {
            while (true) {
                when (inp.readUnsignedByte()) {
                    0 -> {
                        val id = inp.readInt()
                        val ok = inp.readUnsignedByte() == 0
                        val f = pending.remove(id)
                        if (ok) f?.complete(Rpc.rVal(inp))
                        else f?.completeExceptionally(RuntimeException(Rpc.rStr(inp)))
                    }
                    1 -> {
                        // Callback dijalankan di thread terpisah agar callback yang memanggil svc
                        // (mis. memuat ulang daftar) tidak memblokir pembaca balasan.
                        val cb = cbById[inp.readInt()]
                        val kind = inp.readUnsignedByte()
                        var ev: () -> Unit = {}
                        when (kind) {
                            0 -> {
                                val c = Rpc.rStr(inp); val d = inp.readLong(); val t = inp.readLong(); val bps = inp.readDouble()
                                ev = { cb?.onProgress(c, d, t, bps) }
                            }
                            1 -> {
                                val m = Rpc.rStr(inp)
                                ev = { cb?.onLog(m) }
                            }
                            else -> {
                                val ok = inp.readBoolean(); val m = Rpc.rStr(inp); val d = inp.readLong(); val t = inp.readLong()
                                ev = { cb?.onFinish(ok, m, d, t) }
                            }
                        }
                        cbExec.execute { try { ev() } catch (_: Throwable) {} }
                    }
                    else -> throw java.io.IOException("frame tidak dikenal")
                }
            }
        } catch (_: Throwable) {
            die()
        }
    }

    fun die() {
        if (!alive) return
        alive = false
        for (f in pending.values) f.completeExceptionally(RemoteException("Proses Shizuku terputus"))
        pending.clear()
        cbExec.shutdown()
        try { sock.close() } catch (_: Throwable) {}
        try { proc.destroy() } catch (_: Throwable) {}
    }

    private fun call(code: Int, vararg a: Any?): Any? {
        if (!alive) throw RemoteException("Proses Shizuku terputus")
        val id = nextId.getAndIncrement()
        val f = CompletableFuture<Any?>()
        pending[id] = f
        try {
            synchronized(wlock) {
                out.writeInt(id); out.writeInt(code); out.writeInt(a.size)
                for (v in a) Rpc.wVal(out, if (v is IProgressCallback) CbRef(cbId(v)) else v)
                out.flush()
            }
        } catch (t: Throwable) {
            pending.remove(id)
            die()
            throw RemoteException("Proses Shizuku terputus")
        }
        try {
            return f.get()
        } catch (e: ExecutionException) {
            val c = e.cause
            if (c is RemoteException) throw c
            throw RuntimeException(c?.message, c)
        } finally {
            pending.remove(id)
        }
    }

    @Suppress("UNCHECKED_CAST")
    override fun list(dir: String): Array<String> = call(1, dir) as Array<String>
    override fun start(srcs: Array<String>, dsts: Array<String>, move: Boolean, cb: IProgressCallback) { call(2, srcs, dsts, move, cb) }
    override fun cancel() { call(3) }
    override fun listen(destDir: String, pin: String, deviceName: String, cb: IProgressCallback): Int = call(4, destDir, pin, deviceName, cb) as Int
    override fun stopListen() { call(5) }
    override fun send(host: String, port: Int, pin: String, deviceName: String, srcs: Array<String>, cb: IProgressCallback) { call(6, host, port, pin, deviceName, srcs, cb) }
    override fun checkPair(host: String, port: Int, pin: String, deviceName: String): String = call(7, host, port, pin, deviceName) as String
    override fun mkdir(path: String): Boolean = call(8, path) as Boolean
    @Suppress("UNCHECKED_CAST")
    override fun browse(dir: String): Array<String> = call(9, dir) as Array<String>
    @Suppress("UNCHECKED_CAST")
    override fun remoteList(host: String, port: Int, pin: String, deviceName: String, path: String): Array<String> =
        call(10, host, port, pin, deviceName, path) as Array<String>
    override fun push(host: String, port: Int, pin: String, deviceName: String, srcs: Array<String>, remoteDir: String, cb: IProgressCallback) {
        call(11, host, port, pin, deviceName, srcs, remoteDir, cb)
    }
    override fun pull(host: String, port: Int, pin: String, deviceName: String, remotePaths: Array<String>, destDir: String, cb: IProgressCallback) {
        call(12, host, port, pin, deviceName, remotePaths, destDir, cb)
    }
    override fun setAllowBrowse(allow: Boolean) { call(13, allow) }
    @Suppress("UNCHECKED_CAST")
    override fun listDetailed(dir: String): Array<String> = call(14, dir) as Array<String>
    override fun deletePaths(paths: Array<String>, trashDir: String, cb: IProgressCallback) { call(15, paths, trashDir, cb) }
    override fun rename(from: String, to: String): Boolean = call(16, from, to) as Boolean
    override fun pathSize(path: String): Long = call(17, path) as Long
    override fun backupXapk(jobs: Array<String>, cb: IProgressCallback) { call(18, jobs, cb) }
    override fun installApk(paths: Array<String>, cb: IProgressCallback) { call(19, paths, cb) }
    override fun unzip(zip: String, destDir: String, cb: IProgressCallback) { call(20, zip, destDir, cb) }

    override fun destroy() {
        try { call(0) } catch (_: Throwable) {}
        die()
    }

    companion object {
        /**
         * Menjalankan ShellHost lewat Shizuku.Process dan menyambung ke sana.
         * Melempar exception berisi alasan bila gagal. Panggil dari thread latar belakang.
         */
        fun connect(ctx: Context, log: (String) -> Unit): ShellFallback {
            val apk = ctx.applicationInfo.sourceDir
            val token = ByteArray(24).also { SecureRandom().nextBytes(it) }
                .joinToString("") { "%02x".format(it) }
            val cmd = arrayOf(
                "sh", "-c",
                "export CLASSPATH='$apk'; exec /system/bin/app_process /system/bin " +
                    "--nice-name=ngirim_shell com.udyneos.ngirim.ShellHost 2>&1"
            )
            val proc: Process = try {
                // newProcess bersifat private sejak Shizuku API 13, jadi dipanggil lewat refleksi.
                val m = Shizuku::class.java.getDeclaredMethod(
                    "newProcess", Array<String>::class.java, Array<String>::class.java, String::class.java
                )
                m.isAccessible = true
                m.invoke(null, cmd, null, null) as Process
            } catch (t: java.lang.reflect.InvocationTargetException) {
                throw RuntimeException("Shizuku.Process gagal: ${t.targetException?.message}")
            }
            try {
                proc.outputStream.apply { write((token + "\n").toByteArray()); flush() }
                val ports = LinkedBlockingQueue<Int>()
                Thread({
                    try {
                        proc.inputStream.bufferedReader().forEachLine { line ->
                            if (line.startsWith("NGIRIM-PORT ")) line.removePrefix("NGIRIM-PORT ").trim().toIntOrNull()?.let { ports.put(it) }
                            else if (line.isNotBlank()) log("host: $line")
                        }
                    } catch (_: Throwable) {}
                }, "ngirim-fallback-out").apply { isDaemon = true }.start()
                val port = ports.poll(12, TimeUnit.SECONDS) ?: throw RuntimeException("proses shell tidak merespons")
                val s = Socket()
                s.connect(InetSocketAddress("127.0.0.1", port), 5000)
                s.tcpNoDelay = true
                DataOutputStream(s.getOutputStream()).apply { Rpc.wStr(this, token); flush() }
                return ShellFallback(proc, s)
            } catch (t: Throwable) {
                try { proc.destroy() } catch (_: Throwable) {}
                throw t
            }
        }
    }
}

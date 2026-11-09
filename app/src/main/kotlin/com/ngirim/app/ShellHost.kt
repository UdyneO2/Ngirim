package com.ngirim.app

import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.security.MessageDigest
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import kotlin.system.exitProcess

/** Referensi callback di sisi kabel: hanya id, objek aslinya tetap di proses aplikasi. */
internal class CbRef(val id: Int)

/** Pengkodean nilai sederhana untuk jalur cadangan (aplikasi <-> proses shell). */
internal object Rpc {
    private const val T_NULL = 0
    private const val T_STR = 1
    private const val T_ARR = 2
    private const val T_BOOL = 3
    private const val T_INT = 4
    private const val T_LONG = 5
    private const val T_CB = 6
    private const val MAX_STR = 32 shl 20
    private const val MAX_ARR = 10_000_000

    fun wStr(o: DataOutputStream, s: String) {
        val b = s.toByteArray(Charsets.UTF_8)
        o.writeInt(b.size); o.write(b)
    }

    fun rStr(i: DataInputStream): String {
        val n = i.readInt()
        if (n < 0 || n > MAX_STR) throw java.io.IOException("string tidak valid")
        val b = ByteArray(n)
        i.readFully(b)
        return String(b, Charsets.UTF_8)
    }

    fun wVal(o: DataOutputStream, v: Any?) {
        when (v) {
            null, is Unit -> o.writeByte(T_NULL)
            is String -> { o.writeByte(T_STR); wStr(o, v) }
            is Array<*> -> {
                o.writeByte(T_ARR); o.writeInt(v.size)
                for (x in v) wStr(o, x as String)
            }
            is Boolean -> { o.writeByte(T_BOOL); o.writeBoolean(v) }
            is Int -> { o.writeByte(T_INT); o.writeInt(v) }
            is Long -> { o.writeByte(T_LONG); o.writeLong(v) }
            is CbRef -> { o.writeByte(T_CB); o.writeInt(v.id) }
            else -> throw IllegalArgumentException("tipe tidak didukung: ${v.javaClass}")
        }
    }

    fun rVal(i: DataInputStream): Any? = when (i.readUnsignedByte()) {
        T_NULL -> null
        T_STR -> rStr(i)
        T_ARR -> {
            val n = i.readInt()
            if (n < 0 || n > MAX_ARR) throw java.io.IOException("array tidak valid")
            Array(n) { rStr(i) }
        }
        T_BOOL -> i.readBoolean()
        T_INT -> i.readInt()
        T_LONG -> i.readLong()
        T_CB -> CbRef(i.readInt())
        else -> throw java.io.IOException("tag tidak dikenal")
    }
}

/**
 * Titik masuk proses cadangan. Dijalankan di uid shell lewat Shizuku.Process:
 *   CLASSPATH=<apk> app_process /system/bin com.ngirim.app.ShellHost
 * Token dibaca dari stdin, port localhost dicetak ke stdout. Proses berhenti sendiri
 * begitu stdin ditutup (aplikasi mati atau memanggil destroy).
 * Method FileService yang sama dipakai persis seperti pada jalur UserService.
 */
object ShellHost {
    @JvmStatic
    fun main(args: Array<String>) {
        val stdin = System.`in`.bufferedReader()
        val token = stdin.readLine() ?: exitProcess(1)
        val ss = ServerSocket(0, 8, InetAddress.getByName("127.0.0.1"))
        println("NGIRIM-PORT ${ss.localPort}")
        System.out.flush()
        Thread {
            try { while (stdin.read() >= 0) { /* tunggu EOF */ } } catch (_: Throwable) {}
            exitProcess(0)
        }.start()
        val svc = FileService()
        val pool = Executors.newCachedThreadPool()
        while (true) {
            val s = try { ss.accept() } catch (_: Throwable) { exitProcess(0) }
            Thread { serve(s, token, svc, pool) }.start()
        }
    }

    private fun serve(s: Socket, token: String, svc: FileService, pool: java.util.concurrent.ExecutorService) {
        try {
            s.tcpNoDelay = true
            val inp = DataInputStream(BufferedInputStream(s.getInputStream()))
            val out = DataOutputStream(BufferedOutputStream(s.getOutputStream()))
            s.soTimeout = 10_000
            val got = Rpc.rStr(inp)
            if (!MessageDigest.isEqual(got.toByteArray(), token.toByteArray())) { s.close(); return }
            s.soTimeout = 0
            val lock = Any()
            val cbs = ConcurrentHashMap<Int, IProgressCallback>()
            fun send(block: DataOutputStream.() -> Unit) {
                try { synchronized(lock) { out.block(); out.flush() } } catch (_: Throwable) { try { s.close() } catch (_: Throwable) {} }
            }
            fun proxy(id: Int): IProgressCallback = cbs.getOrPut(id) {
                object : IProgressCallback.Stub() {
                    override fun onProgress(current: String, done: Long, total: Long, bytesPerSec: Double) =
                        send { writeByte(1); writeInt(id); writeByte(0); Rpc.wStr(this, current); writeLong(done); writeLong(total); writeDouble(bytesPerSec) }
                    override fun onLog(msg: String) =
                        send { writeByte(1); writeInt(id); writeByte(1); Rpc.wStr(this, msg) }
                    override fun onFinish(ok: Boolean, message: String, done: Long, total: Long) =
                        send { writeByte(1); writeInt(id); writeByte(2); writeBoolean(ok); Rpc.wStr(this, message); writeLong(done); writeLong(total) }
                }
            }
            while (true) {
                val callId = inp.readInt()
                val code = inp.readInt()
                val n = inp.readInt()
                if (n < 0 || n > 16) throw java.io.IOException("jumlah argumen tidak valid")
                val a = List(n) { Rpc.rVal(inp).let { v -> if (v is CbRef) proxy(v.id) else v } }
                pool.execute {
                    try {
                        val r = dispatch(svc, code, a)
                        send { writeByte(0); writeInt(callId); writeByte(0); Rpc.wVal(this, r) }
                    } catch (t: Throwable) {
                        send { writeByte(0); writeInt(callId); writeByte(1); Rpc.wStr(this, t.message ?: t.javaClass.simpleName) }
                    }
                }
            }
        } catch (_: Throwable) {
            try { s.close() } catch (_: Throwable) {}
        }
    }

    @Suppress("UNCHECKED_CAST")
    private fun dispatch(f: FileService, code: Int, a: List<Any?>): Any? {
        fun s(i: Int) = a[i] as String
        fun sa(i: Int) = a[i] as Array<String>
        fun cb(i: Int) = a[i] as IProgressCallback
        return when (code) {
            0 -> { f.stopListen(); Thread { Thread.sleep(300); exitProcess(0) }.start(); null }
            1 -> f.list(s(0))
            2 -> f.start(sa(0), sa(1), a[2] as Boolean, cb(3))
            3 -> f.cancel()
            4 -> f.listen(s(0), s(1), s(2), cb(3))
            5 -> f.stopListen()
            6 -> f.send(s(0), a[1] as Int, s(2), s(3), sa(4), cb(5))
            7 -> f.checkPair(s(0), a[1] as Int, s(2), s(3))
            8 -> f.mkdir(s(0))
            9 -> f.browse(s(0))
            10 -> f.remoteList(s(0), a[1] as Int, s(2), s(3), s(4))
            11 -> f.push(s(0), a[1] as Int, s(2), s(3), sa(4), s(5), cb(6))
            12 -> f.pull(s(0), a[1] as Int, s(2), s(3), sa(4), s(5), cb(6))
            13 -> f.setAllowBrowse(a[0] as Boolean)
            14 -> f.listDetailed(s(0))
            15 -> f.deletePaths(sa(0), s(1), cb(2))
            16 -> f.rename(s(0), s(1))
            17 -> f.pathSize(s(0))
            18 -> f.backupXapk(sa(0), cb(1))
            else -> throw IllegalArgumentException("kode tidak dikenal: $code")
        }
    }
}

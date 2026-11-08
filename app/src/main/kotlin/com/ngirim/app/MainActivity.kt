package com.ngirim.app

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.opengl.GLES30
import android.opengl.GLSurfaceView
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.provider.Settings
import android.text.InputType
import android.view.MotionEvent
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowManager
import android.widget.EditText
import android.widget.FrameLayout
import rikka.shizuku.Shizuku
import java.io.File
import java.net.Inet4Address
import java.net.NetworkInterface
import java.security.SecureRandom
import java.util.concurrent.Executors
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

class MainActivity : Activity() {

    private enum class Kind { APK, DIR }
    private class Route(val name: String, val kind: Kind, val src: String?, val dst: String)
    private class ApkEntry(val label: String, val pkg: String, val ver: String, val paths: List<String>, val size: Long)
    private class DirEntry(val name: String, val size: Long, val isDir: Boolean)
    private class Peer(val name: String, val host: String, val port: Int)

    private val base = "/sdcard/Ngirim"
    private val customRoute = 6 // sinkron dengan kCustomRoute di native-lib.cpp
    private val routes = listOf(
        Route("APK - backup aplikasi terpasang", Kind.APK, null, "$base/APK"),
        Route("Foto - DCIM/Camera", Kind.DIR, "/sdcard/DCIM/Camera", "$base/Foto"),
        Route("Game OBB - backup", Kind.DIR, "/sdcard/Android/obb", "$base/obb"),
        Route("Game DATA - backup", Kind.DIR, "/sdcard/Android/data", "$base/data"),
        Route("Game OBB - restore", Kind.DIR, "$base/obb", "/sdcard/Android/obb"),
        Route("Game DATA - restore", Kind.DIR, "$base/data", "/sdcard/Android/data"),
        Route("Kustom - pilih folder sendiri", Kind.DIR, null, ""),
    )

    private val reqShizuku = 1001
    private val reqNearby = 2001
    private val worker = Executors.newSingleThreadExecutor()
    private val myName: String = Build.MODEL ?: "Android"

    @Volatile private var svc: IFileService? = null
    @Volatile private var routeIdx = 0
    @Volatile private var apks: List<ApkEntry> = emptyList()
    @Volatile private var dirs: List<DirEntry> = emptyList()
    @Volatile private var customSrc = ""
    @Volatile private var customDst = ""

    // jaringan
    private val peers = ArrayList<Peer>()
    @Volatile private var paired: Peer? = null
    @Volatile private var pairedPin = ""
    private val sendList = ArrayList<String>()
    @Volatile private var recvDir = ""

    // browser
    @Volatile private var bPurpose = 0
    @Volatile private var bPath = "/sdcard"
    @Volatile private var bEntries: List<DirEntry> = emptyList()

    private lateinit var glView: GLSurfaceView
    private var surfW = 1
    private var surfH = 1

    private val nsdh by lazy {
        NsdHelper(this, { n, h, p -> onPeerFound(n, h, p) }, { Native.log(it) })
    }

    private val userArgs by lazy {
        Shizuku.UserServiceArgs(ComponentName(packageName, FileService::class.java.name))
            .daemon(false)
            .processNameSuffix("fs")
            .debuggable(BuildConfig.DEBUG)
            .version(BuildConfig.VERSION_CODE)
    }

    private val conn = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            if (binder == null || !binder.pingBinder()) return
            svc = IFileService.Stub.asInterface(binder)
            Native.setShizuku(true, "Shizuku terhubung")
            Native.log("OK     service Shizuku aktif")
            refresh()
        }
        override fun onServiceDisconnected(name: ComponentName?) {
            svc = null
            Native.setShizuku(false, "Service terputus")
        }
    }

    private val onBinder = Shizuku.OnBinderReceivedListener { worker.execute { connect() } }
    private val onDead = Shizuku.OnBinderDeadListener {
        svc = null
        Native.setShizuku(false, "Shizuku berhenti")
    }
    private val onPerm = Shizuku.OnRequestPermissionResultListener { _, result ->
        if (result == PackageManager.PERMISSION_GRANTED) worker.execute { bind() }
        else Native.setShizuku(false, "Izin Shizuku ditolak")
    }

    private val cb = object : IProgressCallback.Stub() {
        override fun onProgress(current: String, done: Long, total: Long, bytesPerSec: Double) {
            Native.setProgress(true, done, total, bytesPerSec, current)
        }
        override fun onLog(msg: String) { Native.log(msg) }
        override fun onFinish(ok: Boolean, message: String, done: Long, total: Long) {
            Native.setProgress(false, done, total, 0.0, "")
            Native.log((if (ok) "OK     " else "ERR    ") + message)
            refresh()
        }
    }

    private val netCb = object : IProgressCallback.Stub() {
        override fun onProgress(current: String, done: Long, total: Long, bytesPerSec: Double) {
            Native.setNetProgress(true, done, total, bytesPerSec, current)
        }
        override fun onLog(msg: String) { Native.log(msg) }
        override fun onFinish(ok: Boolean, message: String, done: Long, total: Long) {
            Native.setNetProgress(false, done, total, 0.0, "")
            Native.log((if (ok) "OK     " else "ERR    ") + message)
            if (message.startsWith("Server dihentikan")) {
                nsdh.unregister()
                Native.setNetInfo("", "", recvDir, false, "")
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        Native.setRoutes(routes.map { it.name }.toTypedArray())
        Native.setShizuku(false, "Menunggu Shizuku...")
        Native.log("Ngirim v${BuildConfig.VERSION_NAME} siap")

        glView = GLSurfaceView(this).apply {
            setEGLContextClientVersion(3)
            preserveEGLContextOnPause = true
            setRenderer(object : GLSurfaceView.Renderer {
                override fun onSurfaceCreated(gl: GL10?, c: EGLConfig?) {
                    Native.initGl(resources.displayMetrics.density)
                }
                override fun onSurfaceChanged(gl: GL10?, w: Int, h: Int) {
                    GLES30.glViewport(0, 0, w, h); surfW = w; surfH = h
                }
                override fun onDrawFrame(gl: GL10?) {
                    while (true) {
                        val a = Native.pollAction()
                        if (a.isEmpty()) break
                        handle(a)
                    }
                    GLES30.glClearColor(0.06f, 0.06f, 0.08f, 1f)
                    GLES30.glClear(GLES30.GL_COLOR_BUFFER_BIT)
                    Native.render(surfW, surfH)
                }
            })
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
            setOnTouchListener { _, e ->
                val act = when (e.actionMasked) {
                    MotionEvent.ACTION_DOWN -> 0
                    MotionEvent.ACTION_MOVE -> 1
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> 2
                    else -> -1
                }
                if (act >= 0) {
                    val x = e.x; val y = e.y
                    queueEvent { Native.touch(act, x, y) }
                }
                true
            }
        }

        val root = FrameLayout(this).apply {
            setBackgroundColor(0xFF0F0F14.toInt())
            addView(glView, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
            setOnApplyWindowInsetsListener { v, insets ->
                val b = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                v.setPadding(b.left, b.top, b.right, b.bottom)
                WindowInsets.CONSUMED
            }
        }
        setContentView(root)

        Shizuku.addBinderReceivedListenerSticky(onBinder)
        Shizuku.addBinderDeadListener(onDead)
        Shizuku.addRequestPermissionResultListener(onPerm)
        checkNetPerm()
    }

    override fun onResume() { super.onResume(); glView.onResume(); checkNetPerm() }
    override fun onPause() { glView.onPause(); super.onPause() }

    override fun onDestroy() {
        nsdh.stopDiscovery(); nsdh.unregister()
        try { svc?.stopListen() } catch (_: Throwable) {}
        Shizuku.removeBinderReceivedListener(onBinder)
        Shizuku.removeBinderDeadListener(onDead)
        Shizuku.removeRequestPermissionResultListener(onPerm)
        try { Shizuku.unbindUserService(userArgs, conn, true) } catch (_: Throwable) {}
        super.onDestroy()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == reqNearby) checkNetPerm()
    }

    // ================================================================ Shizuku
    private fun connect() {
        if (!Shizuku.pingBinder()) {
            Native.setShizuku(false, "Shizuku belum berjalan - start dulu lewat app Shizuku")
            return
        }
        when {
            Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED -> bind()
            Shizuku.shouldShowRequestPermissionRationale() ->
                Native.setShizuku(false, "Izin ditolak permanen - aktifkan di app Shizuku")
            else -> Shizuku.requestPermission(reqShizuku)
        }
    }

    private fun bind() {
        if (svc != null) return
        try { Shizuku.bindUserService(userArgs, conn) }
        catch (t: Throwable) { Native.setShizuku(false, "Gagal bind: ${t.message}") }
    }

    // ================================================================ aksi dari UI ImGui
    private fun handle(a: String) {
        val p = a.split("|")
        when (p[0]) {
            "CONNECT" -> worker.execute { connect() }
            "REFRESH" -> refresh()
            "ROUTE" -> { routeIdx = p[1].toInt(); refresh() }
            "CANCEL", "NET_CANCEL" -> Thread { try { svc?.cancel() } catch (_: Throwable) {} }.start()
            "START" -> {
                val idx = p[3].split(",").filter { it.isNotEmpty() }.map { it.toInt() }
                worker.execute { startTransfer(p[1].toInt(), p[2] == "1", idx) }
            }
            // jaringan
            "NET_PERM" -> netPerm()
            "NET_ROLE" -> if (p[1] == "send") { peers.clear(); publishPeers() }
            "NET_SCAN" -> { Native.log("Mencari perangkat di Wi-Fi yang sama..."); nsdh.startDiscovery() }
            "NET_MANUAL" -> askText("Alamat penerima", "contoh 192.168.1.5:45123", false) { v ->
                val i = v.lastIndexOf(':')
                val port = if (i > 0) v.substring(i + 1).toIntOrNull() else null
                if (port == null) Native.log("ERR format harus IP:port")
                else onPeerFound("Manual", v.substring(0, i), port)
            }
            "NET_PAIR" -> pair(p[1].toInt())
            "NET_CLEAR" -> { sendList.clear() }
            "NET_SEND" -> worker.execute { sendNow() }
            "NET_LISTEN" -> worker.execute { listenNow() }
            "NET_STOP" -> worker.execute { stopListenNow() }
            "NET_RESET" -> worker.execute { netReset() }
            // file browser
            "BPURPOSE" -> {
                bPurpose = p[1].toInt()
                browse(when (bPurpose) {
                    2 -> recvDir.ifEmpty { "/sdcard" }
                    3 -> customSrc.ifEmpty { "/sdcard" }
                    4 -> customDst.ifEmpty { "/sdcard" }
                    else -> "/sdcard"
                })
            }
            "BOPEN" -> bEntries.getOrNull(p[1].toInt())?.let { browse(File(bPath, it.name).path) }
            "BUP" -> {
                val parent = File(bPath).parent
                if (parent != null && parent != "/") browse(parent)
            }
            "BGO" -> browse(p[1])
            "BCLOSE" -> closeBrowser()
            "BNEWDIR" -> askText("Nama folder baru", "nama folder", false) { name ->
                if (name.isNotEmpty() && !name.contains('/')) worker.execute {
                    try { svc?.mkdir(File(bPath, name).path) } catch (_: Throwable) {}
                    browse(bPath)
                }
            }
            "BDIR" -> pickDir()
            "BFILES" -> pickFiles(p[1].split(",").filter { it.isNotEmpty() }.map { it.toInt() })
        }
    }

    // ================================================================ file browser
    private fun parseEntry(line: String): DirEntry? {
        val t = line.split("\t")
        if (t.size < 3) return null
        return DirEntry(t.dropLast(2).joinToString("\t"), t[t.size - 2].toLongOrNull() ?: 0, t.last() == "1")
    }

    private fun browse(path: String) = worker.execute {
        val s = svc
        if (s == null) { Native.log("ERR Shizuku belum siap"); return@execute }
        try {
            val list = s.browse(path).mapNotNull { parseEntry(it) }
            bPath = path
            bEntries = list
            Native.setBrowser(true, bPurpose, path,
                list.map { it.name }.toTypedArray(),
                list.map { it.size }.toLongArray(),
                BooleanArray(list.size) { list[it].isDir })
        } catch (t: Throwable) {
            Native.log("ERR browse: ${t.message}")
        }
    }

    private fun closeBrowser() {
        Native.setBrowser(false, 0, "", emptyArray(), LongArray(0), BooleanArray(0))
    }

    private fun pickDir() {
        when (bPurpose) {
            2 -> {
                recvDir = bPath
                Native.setNetInfo("", "", recvDir, false, "")
                Native.log("Folder penerimaan: $recvDir")
            }
            3 -> { customSrc = bPath; Native.setCustom(customSrc, customDst); refresh() }
            4 -> { customDst = bPath; Native.setCustom(customSrc, customDst) }
        }
        closeBrowser()
    }

    private fun pickFiles(idx: List<Int>) {
        for (i in idx) {
            val e = bEntries.getOrNull(i) ?: continue
            val path = File(bPath, e.name).path
            if (path !in sendList) sendList.add(path)
        }
        publishSendList()
        closeBrowser()
    }

    private fun publishSendList() {
        val items = sendList.map { File(it) }
        Native.setSendList(
            sendList.map { (if (File(it).isDirectory) "[DIR] " else "") + it }.toTypedArray(),
            items.map { if (it.isFile) it.length() else 0L }.toLongArray()
        )
    }

    // ================================================================ jaringan
    private fun localIp(): String? {
        try {
            val cm = getSystemService(ConnectivityManager::class.java)
            val net = cm.activeNetwork
            val caps = net?.let { cm.getNetworkCapabilities(it) }
            if (caps != null && (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) ||
                        caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET))) {
                cm.getLinkProperties(net)?.linkAddresses?.map { it.address }
                    ?.firstOrNull { it is Inet4Address && !it.isLoopbackAddress }
                    ?.let { return it.hostAddress }
            }
        } catch (_: Throwable) {}
        return try { // fallback: hotspot / tethering
            NetworkInterface.getNetworkInterfaces().toList()
                .filter { it.isUp && !it.isLoopback &&
                        listOf("wlan", "ap", "swlan", "eth").any { p -> it.name.startsWith(p) } }
                .flatMap { it.inetAddresses.toList() }
                .firstOrNull { it is Inet4Address && it.isSiteLocalAddress }?.hostAddress
        } catch (_: Throwable) { null }
    }

    private fun checkNetPerm() {
        val ip = localIp()
        val nearby = Build.VERSION.SDK_INT < 33 ||
                checkSelfPermission(Manifest.permission.NEARBY_WIFI_DEVICES) == PackageManager.PERMISSION_GRANTED
        val msg = "Wi-Fi: " + (if (ip != null) "terhubung ($ip)" else "TIDAK terhubung") +
                "\nIzin perangkat sekitar: " + (if (nearby) "OK" else "belum diberikan")
        Native.setNetPerm(ip != null && nearby, msg)
    }

    private fun netPerm() = runOnUiThread {
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.NEARBY_WIFI_DEVICES) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.NEARBY_WIFI_DEVICES), reqNearby)
        } else if (localIp() == null) {
            try { startActivity(Intent(Settings.ACTION_WIFI_SETTINGS)) } catch (_: Throwable) {}
        }
        checkNetPerm()
    }

    private fun onPeerFound(name: String, host: String, port: Int) {
        synchronized(peers) {
            if (peers.none { it.host == host && it.port == port }) peers.add(Peer(name, host, port))
        }
        publishPeers()
    }

    private fun publishPeers() {
        val names = synchronized(peers) { peers.map { "${it.name}  (${it.host}:${it.port})" } }
        Native.setPeers(names.toTypedArray())
    }

    private fun pair(i: Int) {
        val peer = synchronized(peers) { peers.getOrNull(i) } ?: return
        askText("PIN dari ${peer.name}", "6 digit", true) { pin ->
            worker.execute {
                val s = svc
                if (s == null) { Native.log("ERR Shizuku belum siap"); return@execute }
                val err = try { s.checkPair(peer.host, peer.port, pin, myName) } catch (t: Throwable) { t.message ?: "error" }
                if (err.isEmpty()) {
                    paired = peer; pairedPin = pin
                    nsdh.stopDiscovery()
                    Native.setNetInfo("", "", recvDir, false, peer.name)
                    Native.log("OK     pairing dengan ${peer.name} (${peer.host}:${peer.port})")
                    Native.setNetStep(3)
                } else {
                    Native.log("ERR pairing gagal: $err")
                }
            }
        }
    }

    private fun sendNow() {
        val s = svc
        val p = paired
        if (s == null || p == null || sendList.isEmpty()) {
            Native.setNetProgress(false, 0, 0, 0.0, ""); return
        }
        try {
            Native.log("Mengirim ${sendList.size} item ke ${p.name}")
            s.send(p.host, p.port, pairedPin, myName, sendList.toTypedArray(), netCb)
        } catch (t: Throwable) {
            Native.log("ERR kirim: ${t.message}")
            Native.setNetProgress(false, 0, 0, 0.0, "")
        }
    }

    private fun listenNow() {
        val s = svc
        if (s == null) { Native.log("ERR Shizuku belum siap"); return }
        if (recvDir.isEmpty()) { Native.log("ERR pilih folder tujuan dulu"); return }
        try {
            val pin = String.format("%06d", SecureRandom().nextInt(1_000_000))
            val port = s.listen(recvDir, pin, myName, netCb)
            if (port < 0) return
            val ip = localIp() ?: "?"
            nsdh.register("Ngirim-$myName", port)
            Native.setNetInfo("$ip:$port", pin, recvDir, true, "")
            Native.log("Menunggu pengirim di $ip:$port - PIN $pin")
        } catch (t: Throwable) {
            Native.log("ERR listen: ${t.message}")
        }
    }

    private fun stopListenNow() {
        try { svc?.stopListen() } catch (_: Throwable) {}
        nsdh.unregister()
        Native.setNetInfo("", "", recvDir, false, "")
        Native.setNetProgress(false, 0, 0, 0.0, "")
    }

    private fun netReset() {
        nsdh.stopDiscovery()
        stopListenNow()
        paired = null; pairedPin = ""
        synchronized(peers) { peers.clear() }
        publishPeers()
    }

    private fun askText(title: String, hint: String, numeric: Boolean, onOk: (String) -> Unit) = runOnUiThread {
        val et = EditText(this).apply {
            this.hint = hint
            setSingleLine()
            if (numeric) inputType = InputType.TYPE_CLASS_NUMBER
        }
        val box = FrameLayout(this).apply {
            setPadding(48, 16, 48, 0)
            addView(et)
        }
        AlertDialog.Builder(this)
            .setTitle(title)
            .setView(box)
            .setPositiveButton("OK") { _, _ -> onOk(et.text.toString().trim()) }
            .setNegativeButton("Batal", null)
            .show()
    }

    // ================================================================ transfer lokal
    private fun refresh() = worker.execute {
        val my = routeIdx
        val r = routes[my]
        try {
            if (r.kind == Kind.APK) {
                val list = loadApks()
                if (my != routeIdx) return@execute
                apks = list
                Native.setItems(list.map { "${it.label} (${it.pkg})" }.toTypedArray(),
                    list.map { it.size }.toLongArray())
            } else {
                val src = if (my == customRoute) customSrc else r.src
                val s = svc
                if (s == null || src.isNullOrEmpty()) {
                    dirs = emptyList()
                    Native.setItems(emptyArray(), LongArray(0)); return@execute
                }
                val list = s.list(src).mapNotNull { parseEntry(it) }
                if (my != routeIdx) return@execute
                dirs = list
                Native.setItems(list.map { (if (it.isDir) "[DIR] " else "") + it.name }.toTypedArray(),
                    list.map { it.size }.toLongArray())
            }
        } catch (t: Throwable) {
            Native.log("ERR gagal memuat daftar: ${t.message}")
        }
    }

    @Suppress("DEPRECATION")
    private fun loadApks(): List<ApkEntry> {
        val pm = packageManager
        return pm.getInstalledApplications(0)
            .filter { it.flags and ApplicationInfo.FLAG_SYSTEM == 0 }
            .sortedBy { pm.getApplicationLabel(it).toString().lowercase() }
            .map { ai ->
                val paths = listOf(ai.sourceDir) + (ai.splitSourceDirs?.toList() ?: emptyList())
                val ver = try { pm.getPackageInfo(ai.packageName, 0).versionName ?: "0" } catch (_: Throwable) { "0" }
                ApkEntry(pm.getApplicationLabel(ai).toString(), ai.packageName,
                    ver.replace(Regex("[^A-Za-z0-9._-]"), "_"), paths, paths.sumOf { File(it).length() })
            }
    }

    private fun startTransfer(routeI: Int, move: Boolean, idx: List<Int>) {
        val s = svc
        if (s == null) {
            Native.log("ERR Shizuku belum siap")
            Native.setProgress(false, 0, 0, 0.0, ""); return
        }
        val r = routes[routeI]
        val srcBase = if (routeI == customRoute) customSrc else r.src
        val dstBase = if (routeI == customRoute) customDst else r.dst
        if (r.kind == Kind.DIR && (srcBase.isNullOrEmpty() || dstBase.isEmpty())) {
            Native.log("ERR sumber/tujuan belum dipilih")
            Native.setProgress(false, 0, 0, 0.0, ""); return
        }
        val srcs = ArrayList<String>()
        val dsts = ArrayList<String>()
        try {
            if (r.kind == Kind.APK) {
                for (i in idx) {
                    val e = apks[i]
                    for (path in e.paths) {
                        srcs += path
                        dsts += "$dstBase/${e.pkg}_${e.ver}/${File(path).name}"
                    }
                }
            } else {
                for (i in idx) {
                    val e = dirs[i]
                    srcs += "$srcBase/${e.name}"
                    dsts += "$dstBase/${e.name}"
                }
            }
            Native.log("Mulai: ${r.name} (${srcs.size} item, ${if (move) "pindah" else "salin"})")
            s.start(srcs.toTypedArray(), dsts.toTypedArray(), move, cb)
        } catch (t: Throwable) {
            Native.log("ERR gagal memulai: ${t.message}")
            Native.setProgress(false, 0, 0, 0.0, "")
        }
    }
}

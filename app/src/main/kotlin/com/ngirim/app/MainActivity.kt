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
import android.os.SystemClock
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
import java.net.InetSocketAddress
import java.net.Socket
import java.security.SecureRandom
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

class MainActivity : Activity() {

    private enum class Kind { APK, DIR }
    private class Route(val name: String, val kind: Kind, val src: String?, val dst: String)
    private class ApkEntry(val label: String, val pkg: String, val ver: String, val paths: List<String>, val size: Long)
    private class DirEntry(val name: String, val size: Long, val isDir: Boolean)
    private class Peer(val name: String, val host: String, val port: Int, val svc: String?) {
        @Volatile var misses = 0
    }
    private class Pane(var path: String, var remote: Boolean) {
        @Volatile var entries: List<DirEntry> = emptyList()
        val ver = AtomicInteger()
        var localPath = path
        var remotePath = "/sdcard"
    }

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
    private val paneIo = Executors.newCachedThreadPool()
    private val probeExec = Executors.newSingleThreadScheduledExecutor()
    private var probeJob: ScheduledFuture<*>? = null
    private val prefs by lazy { getSharedPreferences("ngirim", MODE_PRIVATE) }
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
    @Volatile private var recvDir = "/sdcard/Ngirim/Terima"
    @Volatile private var allowBrowse = false
    @Volatile private var netReady = false
    @Volatile private var myPort = -1

    // FTP
    private val panes = arrayOf(Pane("/sdcard", false), Pane("/sdcard/Download", false))
    private val bookmarks = ArrayList<String>()

    // browser
    @Volatile private var bPurpose = 0
    @Volatile private var bPath = "/sdcard"
    @Volatile private var bEntries: List<DirEntry> = emptyList()

    private lateinit var glView: GLSurfaceView
    private var surfW = 1
    private var surfH = 1

    private val nsdh by lazy {
        NsdHelper(this, { n, h, p -> onPeerFound(n, h, p, n) }, { onPeerLost(it) }, { Native.log(it) })
    }

    private fun makeArgs(suffix: String) =
        Shizuku.UserServiceArgs(ComponentName(packageName, FileService::class.java.name))
            .daemon(false)
            .processNameSuffix(suffix)
            .debuggable(BuildConfig.DEBUG)
            .version(BuildConfig.VERSION_CODE)

    // Watchdog koneksi Shizuku (cadangan bila event binder/izin tidak datang)
    private val shizukuWatch = Executors.newSingleThreadScheduledExecutor()
    @Volatile private var curArgs: Shizuku.UserServiceArgs? = null
    @Volatile private var bindingSince = 0L
    @Volatile private var bindAttempts = 0
    @Volatile private var permAskedAt = 0L

    private val conn = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            if (binder == null || !binder.pingBinder()) return
            svc = IFileService.Stub.asInterface(binder)
            bindingSince = 0L; bindAttempts = 0
            Native.setShizuku(true, "Shizuku terhubung")
            Native.log("OK     service Shizuku aktif")
            try { svc?.setAllowBrowse(allowBrowse) } catch (_: Throwable) {}
            refresh()
            paneLoad(0); paneLoad(1)
        }
        override fun onServiceDisconnected(name: ComponentName?) {
            svc = null
            bindingSince = 0L
            Native.setShizuku(false, "Service terputus - menyambung ulang...")
            myPort = -1; curAddr = ""; curPin = ""
            pushNetInfo()
        }
    }

    private val onBinder = Shizuku.OnBinderReceivedListener { shizukuWatch.execute { shizukuTick() } }
    private val onDead = Shizuku.OnBinderDeadListener {
        svc = null
        bindingSince = 0L
        Native.setShizuku(false, "Shizuku berhenti - start lagi lewat app Shizuku")
    }
    private val onPerm = Shizuku.OnRequestPermissionResultListener { _, result ->
        if (result == PackageManager.PERMISSION_GRANTED) bindingSince = 0L
        shizukuWatch.execute { shizukuTick() }
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
                myPort = -1; curAddr = ""; curPin = ""
                pushNetInfo()
            }
        }
    }

    private val ftpCb = object : IProgressCallback.Stub() {
        override fun onProgress(current: String, done: Long, total: Long, bytesPerSec: Double) {
            Native.setFtpProgress(true, done, total, bytesPerSec, current)
        }
        override fun onLog(msg: String) { Native.log(msg) }
        override fun onFinish(ok: Boolean, message: String, done: Long, total: Long) {
            Native.setFtpProgress(false, done, total, 0.0, "")
            Native.log((if (ok) "OK     " else "ERR    ") + message)
            paneLoad(0); paneLoad(1)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        Native.setRoutes(routes.map { it.name }.toTypedArray())
        Native.setShizuku(false, "Menunggu Shizuku...")
        Native.log("Ngirim v${BuildConfig.VERSION_NAME} siap")
        bookmarks.addAll((prefs.getString("bookmarks", "") ?: "").split("\n").filter { it.isNotEmpty() })
        Native.setBookmarks(bookmarks.toTypedArray())
        pushNetInfo()

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

        shizukuWatch.scheduleWithFixedDelay({ shizukuTick() }, 0, 2, TimeUnit.SECONDS)
        Shizuku.addBinderReceivedListenerSticky(onBinder)
        Shizuku.addBinderDeadListener(onDead)
        Shizuku.addRequestPermissionResultListener(onPerm)
        checkNetPerm()
    }

    override fun onResume() {
        super.onResume(); glView.onResume(); checkNetPerm()
        probeJob = probeExec.scheduleWithFixedDelay({ probePeers() }, 5, 5, TimeUnit.SECONDS)
    }
    override fun onPause() {
        probeJob?.cancel(false); probeJob = null
        nsdh.stopDiscovery()
        glView.onPause(); super.onPause()
    }

    override fun onDestroy() {
        probeExec.shutdownNow(); paneIo.shutdownNow()
        nsdh.stopDiscovery(); nsdh.unregister()
        try { svc?.stopListen() } catch (_: Throwable) {}
        Shizuku.removeBinderReceivedListener(onBinder)
        Shizuku.removeBinderDeadListener(onDead)
        Shizuku.removeRequestPermissionResultListener(onPerm)
        shizukuWatch.shutdownNow()
        curArgs?.let { try { Shizuku.unbindUserService(it, conn, true) } catch (_: Throwable) {} }
        super.onDestroy()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == reqNearby) checkNetPerm()
    }

    // ================================================================ Shizuku
    /**
     * Dipanggil tiap 2 detik dan saat ada event. Urutan: Shizuku hidup -> izin -> bind service.
     * Bila bind tidak tersambung dalam 6 detik, bind ulang (nama proses bergantian fs/fs2 agar
     * proses macet sebelumnya tidak ikut terpakai).
     */
    private fun shizukuTick() {
        try {
            val s = svc
            if (s != null) {
                if (s.asBinder().pingBinder()) return
                svc = null
                bindingSince = 0L
            }
            if (!Shizuku.pingBinder()) {
                bindingSince = 0L
                Native.setShizuku(false, "Shizuku belum berjalan - buka app Shizuku lalu Start")
                return
            }
            if (Shizuku.checkSelfPermission() != PackageManager.PERMISSION_GRANTED) {
                if (Shizuku.shouldShowRequestPermissionRationale()) {
                    Native.setShizuku(false, "Izin Shizuku ditolak - izinkan Ngirim di app Shizuku (otomatis tersambung)")
                    return
                }
                val now = SystemClock.elapsedRealtime()
                if (now - permAskedAt > 15_000) { permAskedAt = now; Shizuku.requestPermission(reqShizuku) }
                Native.setShizuku(false, "Menunggu izin Shizuku - ketuk Izinkan pada dialog")
                return
            }
            val now = SystemClock.elapsedRealtime()
            if (bindingSince == 0L || now - bindingSince > 6_000) startBind()
            else Native.setShizuku(false, if (bindAttempts >= 4)
                "Service Shizuku belum merespons (percobaan $bindAttempts). Stop lalu Start Shizuku, kemudian ketuk Hubungkan Shizuku."
            else "Menghubungkan service Shizuku (percobaan $bindAttempts)...")
        } catch (t: Throwable) {
            Native.setShizuku(false, "Shizuku error: ${t.message}")
        }
    }

    private fun startBind() {
        curArgs?.let { try { Shizuku.unbindUserService(it, conn, true) } catch (_: Throwable) {} }
        bindAttempts++
        val args = makeArgs(if (bindAttempts % 2 == 1) "fs" else "fs2")
        curArgs = args
        bindingSince = SystemClock.elapsedRealtime()
        Native.setShizuku(false, "Menghubungkan service Shizuku (percobaan $bindAttempts)...")
        if (bindAttempts > 1) Native.log("Shizuku: bind ulang (percobaan $bindAttempts)")
        try { Shizuku.bindUserService(args, conn) }
        catch (t: Throwable) { Native.setShizuku(false, "Gagal bind: ${t.message}") }
    }

    // ================================================================ aksi dari UI ImGui
    private fun handle(a: String) {
        val p = a.split("|", limit = 4)
        when (p[0]) {
            "CONNECT" -> {
                bindingSince = 0L; bindAttempts = 0; permAskedAt = 0L
                shizukuWatch.execute { shizukuTick() }
            }
            "OPEN_SHIZUKU" -> runOnUiThread {
                val i = packageManager.getLaunchIntentForPackage("moe.shizuku.privileged.api")
                if (i != null) startActivity(i) else Native.log("ERR app Shizuku tidak ditemukan - pasang dari shizuku.rikka.app")
            }
            "REFRESH" -> refresh()
            "ROUTE" -> { routeIdx = p[1].toInt(); refresh() }
            "CANCEL", "NET_CANCEL" -> Thread { try { svc?.cancel() } catch (_: Throwable) {} }.start()
            "START" -> {
                val idx = p[3].split(",").filter { it.isNotEmpty() }.map { it.toInt() }
                worker.execute { startTransfer(p[1].toInt(), p[2] == "1", idx) }
            }
            // jaringan
            "NET_PERM" -> netPerm()
            "NET_SCAN" -> { synchronized(peers) { peers.removeAll { it.svc != null } }; publishPeers(); nsdh.startDiscovery() }
            "NET_MANUAL" -> askText("Alamat penerima", "contoh 192.168.1.5:45123", false) { v ->
                val i = v.lastIndexOf(':')
                val port = if (i > 0) v.substring(i + 1).toIntOrNull() else null
                if (port == null) Native.log("ERR format harus IP:port")
                else onPeerFound("Manual", v.substring(0, i), port, null)
            }
            "NET_PAIR" -> pair(p[1].toInt())
            "NET_CLEAR" -> { sendList.clear() }
            "NET_SEND" -> worker.execute { sendNow() }
            "NET_LISTEN" -> worker.execute { listenNow() }
            "NET_STOP" -> worker.execute { stopListenNow() }
            "NET_DISCONNECT" -> disconnectPeer()
            "RX_ON" -> worker.execute { listenNow() }
            "RX_OFF" -> worker.execute { stopListenNow() }
            "RX_BROWSE" -> {
                allowBrowse = p[1] == "1"
                try { svc?.setAllowBrowse(allowBrowse) } catch (_: Throwable) {}
            }
            // FTP
            "FP" -> paneAction(p[1].toInt(), p[2], p.getOrNull(3))
            "FP_XFER" -> {
                val idx = p[3].split(",").filter { it.isNotEmpty() }.map { it.toInt() }
                worker.execute { ftpTransfer(p[1].toInt(), p[2] == "1", idx) }
            }
            "FP_MKDIR" -> {
                val i = p[1].toInt()
                askText("Nama folder baru", "nama folder", false) { name ->
                    if (name.isNotEmpty() && !name.contains('/')) paneIo.execute {
                        try { svc?.mkdir(File(panes[i].path, name).path) } catch (_: Throwable) {}
                        paneLoad(i)
                    }
                }
            }
            "BM" -> {
                val path = a.substringAfter("|")
                if (!bookmarks.remove(path)) bookmarks.add(path)
                prefs.edit().putString("bookmarks", bookmarks.joinToString("\n")).apply()
                Native.setBookmarks(bookmarks.toTypedArray())
            }
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
                pushNetInfo()
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
        netReady = ip != null && nearby
        Native.setNetPerm(netReady, msg)
        ensureDiscovery()
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

    private fun display(n: String) = n.removePrefix("Ngirim-")

    private fun onPeerFound(name: String, host: String, port: Int, svcName: String?) {
        if (port == myPort && host == localIp()) return // diri sendiri
        synchronized(peers) {
            val old = peers.firstOrNull { it.host == host && it.port == port }
            if (old == null) peers.add(Peer(display(name), host, port, svcName)) else old.misses = 0
        }
        publishPeers()
    }

    private fun onPeerLost(svcName: String) {
        synchronized(peers) { peers.removeAll { it.svc == svcName } }
        publishPeers()
    }

    private fun publishPeers() {
        val names = synchronized(peers) { peers.map { "${it.name}  (${it.host}:${it.port})" } }
        Native.setPeers(names.toTypedArray())
    }

    /** Cek berkala: perangkat yang tidak lagi menjawab di port-nya dibuang dari daftar aktif. */
    private fun probePeers() {
        val snapshot = synchronized(peers) { peers.filter { it.svc != null } }
        var changed = false
        for (p in snapshot) {
            val alive = try {
                Socket().use { s ->
                    s.connect(InetSocketAddress(p.host, p.port), 1500)
                    s.soTimeout = 1500
                    s.getInputStream().read() >= 0
                }
            } catch (_: Throwable) { false }
            if (alive) p.misses = 0
            else if (++p.misses >= 2) { synchronized(peers) { peers.remove(p) }; changed = true }
        }
        if (changed) publishPeers()
    }

    private fun ensureDiscovery() {
        if (netReady && !nsdh.isDiscovering) nsdh.startDiscovery()
    }

    @Volatile private var curAddr = ""
    @Volatile private var curPin = ""
    private val listeningNow get() = myPort > 0

    private fun pushNetInfo() = Native.setNetInfo(curAddr, curPin, recvDir, listeningNow, paired?.name ?: "")

    private fun pair(i: Int) {
        val peer = synchronized(peers) { peers.getOrNull(i) } ?: return
        askText("PIN dari ${peer.name}", "6 digit", true) { pin ->
            worker.execute {
                val s = svc
                if (s == null) { Native.log("ERR Shizuku belum siap"); return@execute }
                val err = try { s.checkPair(peer.host, peer.port, pin, myName) } catch (t: Throwable) { t.message ?: "error" }
                if (err.isEmpty()) {
                    paired = peer; pairedPin = pin
                    pushNetInfo()
                    Native.log("OK     pairing dengan ${peer.name} (${peer.host}:${peer.port})")
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
            myPort = port
            curAddr = "$ip:$port"; curPin = pin
            nsdh.register("Ngirim-$myName", port)
            pushNetInfo()
            Native.log("Menunggu pengirim di $ip:$port - PIN $pin")
        } catch (t: Throwable) {
            Native.log("ERR listen: ${t.message}")
        }
    }

    private fun stopListenNow() {
        try { svc?.stopListen() } catch (_: Throwable) {}
        nsdh.unregister()
        myPort = -1; curAddr = ""; curPin = ""
        pushNetInfo()
        Native.setNetProgress(false, 0, 0, 0.0, "")
    }

    private fun disconnectPeer() {
        paired = null; pairedPin = ""
        sendList.clear(); publishSendList()
        pushNetInfo()
        panes[1].remote = false
        panes[1].path = panes[1].localPath
        paneLoad(1)
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


    // ================================================================ FTP (dua panel)
    private fun joinPath(dir: String, name: String) = if (dir.endsWith("/")) dir + name else "$dir/$name"

    private fun paneLoad(i: Int) {
        val p = panes[i]
        val v = p.ver.incrementAndGet()
        val path = p.path
        val remote = p.remote
        paneIo.execute {
            val s = svc
            val peer = paired
            val pin = pairedPin
            var err = ""
            var list: List<DirEntry> = emptyList()
            try {
                if (remote) {
                    if (s == null || peer == null) err = "Belum terhubung ke perangkat lain"
                    else {
                        val r = s.remoteList(peer.host, peer.port, pin, myName, path)
                        if (r.isNotEmpty() && r[0] == "OK") list = r.drop(1).mapNotNull { parseEntry(it) }
                        else err = r.firstOrNull() ?: "Gagal memuat"
                    }
                } else if (s == null) {
                    err = "Shizuku belum siap"
                } else {
                    list = s.browse(path).mapNotNull { parseEntry(it) }
                }
            } catch (t: Throwable) { err = t.message ?: "error" }
            if (p.ver.get() != v) return@execute
            p.entries = list
            Native.setPane(i, path, remote, err,
                list.map { it.name }.toTypedArray(),
                list.map { it.size }.toLongArray(),
                BooleanArray(list.size) { list[it].isDir })
        }
    }

    private fun paneAction(i: Int, op: String, arg: String?) {
        val p = panes[i]
        when (op) {
            "OPEN" -> p.entries.getOrNull(arg?.toIntOrNull() ?: -1)?.takeIf { it.isDir }?.let {
                p.path = joinPath(p.path, it.name); paneLoad(i)
            }
            "UP" -> {
                val parent = File(p.path).parent
                if (parent != null && parent != "/" && !(p.remote && !(parent == "/sdcard" || parent.startsWith("/sdcard/")))) {
                    p.path = parent; paneLoad(i)
                }
            }
            "GO" -> if (!arg.isNullOrEmpty()) { p.path = arg; paneLoad(i) }
            "REF" -> paneLoad(i)
            "SRC" -> if (i == 1) {
                val toRemote = arg == "1" && paired != null
                if (toRemote == p.remote) return
                if (p.remote) p.remotePath = p.path else p.localPath = p.path
                p.remote = toRemote
                p.path = if (toRemote) p.remotePath else p.localPath
                paneLoad(1)
            }
        }
    }

    private fun uniqueName(name: String, used: MutableSet<String>): String {
        var n = name
        if (n in used) {
            val dot = name.lastIndexOf('.')
            val base = if (dot > 0) name.substring(0, dot) else name
            val ext = if (dot > 0) name.substring(dot) else ""
            var k = 1
            while ("$base ($k)$ext" in used) k++
            n = "$base ($k)$ext"
        }
        used.add(n)
        return n
    }

    private fun ftpFail(msg: String) {
        Native.log("ERR    $msg")
        Native.setFtpProgress(false, 0, 0, 0.0, "")
    }

    private fun ftpTransfer(from: Int, move: Boolean, idx: List<Int>) {
        val s = svc ?: return ftpFail("Shizuku belum siap")
        val src = panes[from]
        val dst = panes[1 - from]
        val names = idx.mapNotNull { src.entries.getOrNull(it)?.name }
        if (names.isEmpty()) return ftpFail("Tidak ada item terpilih")
        val peer = paired
        try {
            when {
                !src.remote && !dst.remote -> {
                    val used = dst.entries.map { it.name }.toMutableSet()
                    val srcs = ArrayList<String>(); val dsts = ArrayList<String>()
                    for (n in names) {
                        val full = joinPath(src.path, n)
                        if (dst.path == full || dst.path.startsWith("$full/"))
                            return ftpFail("Tidak bisa menyalin folder ke dalam dirinya sendiri: $n")
                        if (move && src.path == dst.path) return ftpFail("Sumber dan tujuan sama")
                        srcs += full
                        dsts += joinPath(dst.path, if (move) n else uniqueName(n, used))
                    }
                    Native.log("Mulai: ${srcs.size} item, ${if (move) "pindah" else "salin"} ke ${dst.path}")
                    s.start(srcs.toTypedArray(), dsts.toTypedArray(), move, ftpCb)
                }
                peer == null -> ftpFail("Belum terhubung ke perangkat lain")
                !src.remote -> {
                    Native.log("Kirim ${names.size} item ke ${peer.name}:${dst.path}")
                    s.push(peer.host, peer.port, pairedPin, myName,
                        names.map { joinPath(src.path, it) }.toTypedArray(), dst.path, ftpCb)
                }
                else -> {
                    Native.log("Ambil ${names.size} item dari ${peer.name} ke ${dst.path}")
                    s.pull(peer.host, peer.port, pairedPin, myName,
                        names.map { joinPath(src.path, it) }.toTypedArray(), dst.path, ftpCb)
                }
            }
        } catch (t: Throwable) {
            ftpFail("gagal memulai: ${t.message}")
        }
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

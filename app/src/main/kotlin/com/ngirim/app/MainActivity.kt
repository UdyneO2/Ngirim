package com.ngirim.app

import android.app.Activity
import android.content.ComponentName
import android.content.ServiceConnection
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.opengl.GLES30
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.os.IBinder
import android.view.MotionEvent
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowManager
import android.widget.FrameLayout
import rikka.shizuku.Shizuku
import java.io.File
import java.util.concurrent.Executors
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

class MainActivity : Activity() {

    private enum class Kind { APK, DIR }
    private class Route(val name: String, val kind: Kind, val src: String?, val dst: String)
    private class ApkEntry(val label: String, val pkg: String, val ver: String, val paths: List<String>, val size: Long)
    private class DirEntry(val name: String, val size: Long, val isDir: Boolean)

    private val base = "/sdcard/Ngirim"
    private val routes = listOf(
        Route("APK - backup aplikasi terpasang", Kind.APK, null, "$base/APK"),
        Route("Foto - DCIM/Camera", Kind.DIR, "/sdcard/DCIM/Camera", "$base/Foto"),
        Route("Game OBB - backup", Kind.DIR, "/sdcard/Android/obb", "$base/obb"),
        Route("Game DATA - backup", Kind.DIR, "/sdcard/Android/data", "$base/data"),
        Route("Game OBB - restore", Kind.DIR, "$base/obb", "/sdcard/Android/obb"),
        Route("Game DATA - restore", Kind.DIR, "$base/data", "/sdcard/Android/data"),
    )

    private val reqCode = 1001
    private val worker = Executors.newSingleThreadExecutor()
    @Volatile private var svc: IFileService? = null
    @Volatile private var routeIdx = 0
    @Volatile private var apks: List<ApkEntry> = emptyList()
    @Volatile private var dirs: List<DirEntry> = emptyList()

    private lateinit var glView: GLSurfaceView
    private var surfW = 1
    private var surfH = 1

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
            Native.log("Service Shizuku aktif")
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
            Native.log(message)
            refresh()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        Native.setRoutes(routes.map { it.name }.toTypedArray())
        Native.setShizuku(false, "Menunggu Shizuku...")

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
    }

    override fun onResume() { super.onResume(); glView.onResume() }
    override fun onPause() { glView.onPause(); super.onPause() }

    override fun onDestroy() {
        Shizuku.removeBinderReceivedListener(onBinder)
        Shizuku.removeBinderDeadListener(onDead)
        Shizuku.removeRequestPermissionResultListener(onPerm)
        try { Shizuku.unbindUserService(userArgs, conn, true) } catch (_: Throwable) {}
        super.onDestroy()
    }

    // ---------- Shizuku ----------
    private fun connect() {
        if (!Shizuku.pingBinder()) {
            Native.setShizuku(false, "Shizuku belum berjalan - start dulu lewat app Shizuku")
            return
        }
        when {
            Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED -> bind()
            Shizuku.shouldShowRequestPermissionRationale() ->
                Native.setShizuku(false, "Izin ditolak permanen - aktifkan di app Shizuku")
            else -> Shizuku.requestPermission(reqCode)
        }
    }

    private fun bind() {
        if (svc != null) return
        try { Shizuku.bindUserService(userArgs, conn) }
        catch (t: Throwable) { Native.setShizuku(false, "Gagal bind: ${t.message}") }
    }

    // ---------- Aksi dari UI ImGui ----------
    private fun handle(a: String) {
        val p = a.split("|")
        when (p[0]) {
            "CONNECT" -> worker.execute { connect() }
            "REFRESH" -> refresh()
            "ROUTE" -> { routeIdx = p[1].toInt(); refresh() }
            "CANCEL" -> Thread { try { svc?.cancel() } catch (_: Throwable) {} }.start()
            "START" -> {
                val idx = p[3].split(",").filter { it.isNotEmpty() }.map { it.toInt() }
                worker.execute { startTransfer(p[1].toInt(), p[2] == "1", idx) }
            }
        }
    }

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
                val s = svc
                if (s == null) { Native.setItems(emptyArray(), LongArray(0)); return@execute }
                val list = s.list(r.src!!).mapNotNull { line ->
                    val t = line.split("\t")
                    if (t.size < 3) null
                    else DirEntry(t.dropLast(2).joinToString("\t"), t[t.size - 2].toLongOrNull() ?: 0, t.last() == "1")
                }
                if (my != routeIdx) return@execute
                dirs = list
                Native.setItems(list.map { (if (it.isDir) "[DIR] " else "") + it.name }.toTypedArray(),
                    list.map { it.size }.toLongArray())
            }
        } catch (t: Throwable) {
            Native.log("Gagal memuat daftar: ${t.message}")
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
            Native.log("Shizuku belum siap")
            Native.setProgress(false, 0, 0, 0.0, ""); return
        }
        val r = routes[routeI]
        val srcs = ArrayList<String>()
        val dsts = ArrayList<String>()
        try {
            if (r.kind == Kind.APK) {
                for (i in idx) {
                    val e = apks[i]
                    for (path in e.paths) {
                        srcs += path
                        dsts += "${r.dst}/${e.pkg}_${e.ver}/${File(path).name}"
                    }
                }
            } else {
                for (i in idx) {
                    val e = dirs[i]
                    srcs += "${r.src}/${e.name}"
                    dsts += "${r.dst}/${e.name}"
                }
            }
            Native.log("Mulai: ${r.name} (${srcs.size} item, ${if (move) "pindah" else "salin"})")
            s.start(srcs.toTypedArray(), dsts.toTypedArray(), move, cb)
        } catch (t: Throwable) {
            Native.log("Gagal memulai: ${t.message}")
            Native.setProgress(false, 0, 0, 0.0, "")
        }
    }
}

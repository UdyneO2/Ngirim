package com.udyneos.ngirim

import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL

/** Cek dan unduh rilis terbaru dari GitHub Releases. */
object Updater {
    const val REPO = "UdyneO2/Ngirim"
    const val RELEASES_PAGE = "https://github.com/$REPO/releases/tag/"

    class Release(val tag: String, val name: String, val body: String, val apkUrl: String, val apkName: String, val size: Long)

    private fun open(url: String): HttpURLConnection {
        val c = URL(url).openConnection() as HttpURLConnection
        c.connectTimeout = 10_000; c.readTimeout = 20_000
        c.setRequestProperty("User-Agent", "Ngirim-Updater")
        c.setRequestProperty("Accept", "application/vnd.github+json")
        return c
    }

    /** Rilis terbaru (bukan pre-release). null bila belum ada rilis atau tanpa APK. */
    fun latest(): Release? {
        val c = open("https://api.github.com/repos/$REPO/releases/latest")
        try {
            if (c.responseCode == 404) return null
            if (c.responseCode != 200) throw java.io.IOException("GitHub membalas HTTP ${c.responseCode}")
            val j = JSONObject(c.inputStream.bufferedReader().readText())
            val assets = j.optJSONArray("assets") ?: return null
            var best: JSONObject? = null
            for (i in 0 until assets.length()) {
                val a = assets.getJSONObject(i)
                if (a.getString("name").endsWith(".apk", ignoreCase = true)) { best = a; break }
            }
            val a = best ?: return null
            return Release(
                j.getString("tag_name"), j.optString("name", ""), j.optString("body", ""),
                a.getString("browser_download_url"), a.getString("name"), a.optLong("size", 0L)
            )
        } finally { c.disconnect() }
    }

    private fun parts(v: String): List<Int> =
        v.trim().removePrefix("v").removePrefix("V").substringBefore('-').split('.').map { it.filter(Char::isDigit).toIntOrNull() ?: 0 }

    /** true bila [remote] lebih baru dari [local]. */
    fun isNewer(remote: String, local: String): Boolean {
        val a = parts(remote); val b = parts(local)
        for (i in 0 until maxOf(a.size, b.size)) {
            val x = a.getOrElse(i) { 0 }; val y = b.getOrElse(i) { 0 }
            if (x != y) return x > y
        }
        return false
    }

    /** Teks catatan rilis tanpa sintaks markdown yang mengganggu. */
    fun plainNotes(md: String): String = md.replace("\r", "").lines().joinToString("\n") { l ->
        var t = l.trimEnd()
        if (t.startsWith("#")) t = t.trimStart('#').trim().uppercase()
        t.replace("**", "").replace("`", "").replace(Regex("^\\s*[-*] "), "• ")
    }.trim()

    fun download(r: Release, dest: File, onProgress: (Long, Long) -> Unit, cancelled: () -> Boolean) {
        dest.parentFile?.mkdirs()
        val c = open(r.apkUrl)
        c.setRequestProperty("Accept", "application/octet-stream")
        try {
            if (c.responseCode != 200) throw java.io.IOException("Unduhan gagal: HTTP ${c.responseCode}")
            val total = c.contentLengthLong.takeIf { it > 0 } ?: r.size
            var done = 0L
            val buf = ByteArray(1 shl 16)
            c.inputStream.use { ins ->
                FileOutputStream(dest).use { os ->
                    while (true) {
                        if (cancelled()) throw java.io.IOException("Dibatalkan")
                        val n = ins.read(buf)
                        if (n < 0) break
                        os.write(buf, 0, n)
                        done += n
                        onProgress(done, total)
                    }
                }
            }
            if (total > 0 && done != total) throw java.io.IOException("Unduhan tidak lengkap ($done dari $total)")
        } catch (t: Throwable) {
            dest.delete()
            throw t
        } finally { c.disconnect() }
    }
}

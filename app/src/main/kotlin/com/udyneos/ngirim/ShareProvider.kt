package com.udyneos.ngirim

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import android.webkit.MimeTypeMap
import java.io.File
import java.io.FileNotFoundException

/**
 * Menyajikan file hasil salinan di cache aplikasi (folder "share") ke aplikasi lain
 * secara read-only, untuk Buka, Bagikan, dan Pasang APK. Tanpa dependensi androidx.
 */
class ShareProvider : ContentProvider() {

    companion object {
        fun root(ctx: android.content.Context): File = File(ctx.externalCacheDir ?: ctx.cacheDir, "share")
        fun mimeOf(name: String): String =
            MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substringAfterLast('.', "").lowercase())
                ?: "application/octet-stream"
    }

    private fun fileFor(uri: Uri): File {
        val ctx = context ?: throw FileNotFoundException("tanpa konteks")
        val base = root(ctx).canonicalFile
        val rel = uri.pathSegments.joinToString("/")
        val f = File(base, rel).canonicalFile
        if (!f.path.startsWith(base.path + "/") || !f.isFile) throw FileNotFoundException(uri.toString())
        return f
    }

    override fun onCreate() = true

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        if (mode != "r") throw SecurityException("hanya baca")
        return ParcelFileDescriptor.open(fileFor(uri), ParcelFileDescriptor.MODE_READ_ONLY)
    }

    override fun getType(uri: Uri): String = mimeOf(uri.lastPathSegment ?: "")

    override fun query(uri: Uri, projection: Array<out String>?, selection: String?, args: Array<out String>?, order: String?): Cursor {
        val f = fileFor(uri)
        val cols = projection ?: arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE)
        val c = MatrixCursor(cols)
        c.addRow(Array<Any?>(cols.size) { i -> if (cols[i] == OpenableColumns.SIZE) f.length() else f.name })
        return c
    }

    override fun insert(uri: Uri, values: ContentValues?): Uri? = throw UnsupportedOperationException()
    override fun delete(uri: Uri, selection: String?, args: Array<out String>?): Int = 0
    override fun update(uri: Uri, values: ContentValues?, selection: String?, args: Array<out String>?): Int = 0
}

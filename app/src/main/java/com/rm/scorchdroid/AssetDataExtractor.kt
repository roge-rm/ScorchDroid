package com.rm.scorchdroid

import android.content.Context
import android.content.res.AssetManager
import java.io.File

/**
 * Upstream Scorched3D's file I/O is plain fopen()/paths (see the porting
 * plan) - it can't read straight out of the APK's compressed asset store.
 * This copies the bundled `data/` asset tree into the app's normal internal
 * storage once, so the native engine can just chdir() there and use its
 * existing path-resolution code completely unmodified.
 */
object AssetDataExtractor {
    private const val VERSION_MARKER = ".extracted_version"
    private const val CURRENT_VERSION = "10"

    fun ensureExtracted(context: Context): File {
        val root = File(context.filesDir, "scorched_root")
        val marker = File(root, VERSION_MARKER)
        if (marker.exists() && marker.readText() == CURRENT_VERSION) {
            return root
        }

        root.deleteRecursively()
        root.mkdirs()
        copyAssetDir(context.assets, "data", File(root, "data"))
        copyAssetFile(context.assets, "scorchdroid_server.xml", File(root, "scorchdroid_server.xml"))
        marker.writeText(CURRENT_VERSION)
        return root
    }

    private fun copyAssetDir(assets: AssetManager, assetPath: String, destDir: File) {
        val entries = assets.list(assetPath) ?: emptyArray()
        if (entries.isEmpty()) {
            copyAssetFile(assets, assetPath, destDir)
            return
        }

        destDir.mkdirs()
        for (entry in entries) {
            copyAssetDir(assets, "$assetPath/$entry", File(destDir, entry))
        }
    }

    private fun copyAssetFile(assets: AssetManager, assetPath: String, destFile: File) {
        destFile.parentFile?.mkdirs()
        assets.open(assetPath).use { input ->
            destFile.outputStream().use { output ->
                input.copyTo(output)
            }
        }
    }
}

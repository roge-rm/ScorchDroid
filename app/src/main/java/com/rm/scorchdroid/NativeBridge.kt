package com.rm.scorchdroid

object NativeBridge {
    init {
        System.loadLibrary("scorchdroid_engine")
    }

    external fun helloFromNative(): String

    /** Points the native engine's cwd/$HOME at the extracted data root (see AssetDataExtractor). */
    external fun initEngine(dataRoot: String): Boolean
}

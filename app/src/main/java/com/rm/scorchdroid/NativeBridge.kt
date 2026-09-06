package com.rm.scorchdroid

object NativeBridge {
    init {
        System.loadLibrary("scorchdroid_engine")
    }

    external fun helloFromNative(): String
}

package com.rm.scorchdroid

object NativeBridge {
    init {
        System.loadLibrary("scorchdroid_engine")
    }

    external fun helloFromNative(): String

    /** Points the native engine's cwd/$HOME at the extracted data root (see AssetDataExtractor). */
    external fun initEngine(dataRoot: String): Boolean

    /** M2 vertical slice: boots a real local game via ScorchedServer::startServer. */
    external fun startLocalGame(): Boolean

    /** Advances the real game simulation by one step (see ServerSimulator). */
    external fun tickEngine()

    /** Debug-only: current ServerState + tank count. */
    external fun getGameStateDebugString(): String
}

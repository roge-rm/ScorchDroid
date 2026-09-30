plugins {
    alias(libs.plugins.kotlin.multiplatform)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.compose.multiplatform)
}

// ScorchDroid in a browser: Compose for Kotlin/Wasm with the shared UI,
// driving the engine built as WebAssembly in ../engine.
//
//   ./gradlew :webApp:wasmJsBrowserDistribution    the page, in web/app/build/dist/wasmJs/productionExecutable

kotlin {
    wasmJs {
        outputModuleName.set("scorchdroid-ui")
        browser {
            commonWebpackConfig { outputFileName = "scorchdroid-ui.js" }
        }
        binaries.executable()
    }
    sourceSets {
        wasmJsMain.dependencies {
            implementation(project(":shared"))
        }
    }
}

/** The engine's WebAssembly and data, built by web/engine/build.sh and served next to the page. */
val engineOut = rootProject.file("web/engine/out")
val buildEngine = tasks.register<Exec>("buildEngine") {
    inputs.dir(rootProject.file("app/src/main/cpp"))
    inputs.dir(rootProject.file("web/engine/src"))
    inputs.dir(rootProject.file("web/engine/include"))
    inputs.files(rootProject.file("web/engine/CMakeLists.txt"), rootProject.file("web/engine/build.sh"))
    outputs.dir(engineOut)
    commandLine(rootProject.file("web/engine/build.sh").absolutePath)
}
kotlin.sourceSets.named("wasmJsMain") { resources.srcDir(files(engineOut).builtBy(buildEngine)) }

/** The version, read from app/build.gradle.kts, and the upstream commit, as the phone's BuildConfig has them. */
val appGradle = rootProject.file("app/build.gradle.kts").readText()
val versionName = Regex("versionName = \"([^\"]+)\"").find(appGradle)!!.groupValues[1]
val upstreamCommit = providers.exec {
    commandLine("git", "-C", rootProject.file("third_party/scorched3d").absolutePath, "rev-parse", "--short=10", "HEAD")
}.standardOutput.asText.map { it.trim() }
val buildInfo = tasks.register("buildInfo") {
    val out = layout.buildDirectory.dir("generated/buildInfo")
    val name = versionName
    val commit = upstreamCommit
    inputs.property("version", name)
    inputs.property("commit", commit)
    outputs.dir(out)
    doLast {
        val file = out.get().file("com/rm/scorchdroid/web/BuildInfo.kt").asFile
        file.parentFile.mkdirs()
        file.writeText(
            "package com.rm.scorchdroid.web\n\n" +
                "internal object BuildInfo {\n" +
                "    const val VERSION_NAME = \"$name\"\n" +
                "    const val UPSTREAM_COMMIT = \"${commit.get()}\"\n" +
                "}\n"
        )
    }
}
kotlin.sourceSets.named("wasmJsMain") { kotlin.srcDir(buildInfo) }


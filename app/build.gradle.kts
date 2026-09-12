import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

val localProperties = Properties().apply {
    val file = rootProject.file("local.properties")
    if (file.exists()) file.inputStream().use { load(it) }
}

// Staged before `android { }` so the assets source set below can point at
// it by task provider rather than by path string - see the srcDir call.
val stageScorchedData = tasks.register<Sync>("stageScorchedData") {
    from(layout.projectDirectory.dir("../third_party/scorched3d/data"))
    into(layout.buildDirectory.dir("generated/assets-staging/data"))
}

// The GPLv2 text itself, shipped inside the APK for the About screen. Staged
// from the repo's own LICENSE rather than committed a second time under
// assets/, so the licence the app displays and the licence the repository
// carries can never drift apart.
val stageLicense = tasks.register<Copy>("stageLicense") {
    from(layout.projectDirectory.file("../LICENSE"))
    into(layout.buildDirectory.dir("generated/assets-staging/licenses"))
    rename { "GPL-2.0.txt" }
}

android {
    namespace = "com.rm.scorchdroid"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "com.rm.scorchdroid"
        minSdk = 26
        targetSdk = 37
        // M9: the exact upstream commit this build's engine came from, shown
        // on the About screen as part of the corresponding-source offer. Read
        // from the submodule rather than typed in, because a stale value here
        // would point people at source that is not what they are running.
        buildConfigField(
            "String",
            "UPSTREAM_COMMIT",
            "\"" + providers.exec {
                // Absolute: providers.exec inherits the daemon's working
                // directory, not the project's, so a relative -C path fails
                // with an unhelpful "exit value 128" at configuration time.
                commandLine(
                    "git", "-C",
                    rootProject.layout.projectDirectory.dir("third_party/scorched3d").asFile.absolutePath,
                    "rev-parse", "--short=10", "HEAD",
                )
            }.standardOutput.asText.get().trim() + "\"",
        )
        versionCode = 16
        versionName = "0.9.2"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    // Release signing credentials come from local.properties (never
    // committed) as scorchdroid.release.{storeFile,storePassword,keyAlias,
    // keyPassword}. Absent them the release build is simply unsigned, so a
    // plain checkout of this repository still builds.
    val releaseStoreFile = localProperties.getProperty("scorchdroid.release.storeFile")

    signingConfigs {
        if (releaseStoreFile != null) {
            create("release") {
                storeFile = file(releaseStoreFile)
                storePassword = localProperties.getProperty("scorchdroid.release.storePassword")
                keyAlias = localProperties.getProperty("scorchdroid.release.keyAlias")
                keyPassword = localProperties.getProperty("scorchdroid.release.keyPassword")
            }
        }
    }

    buildTypes {
        release {
            optimization {
                enable = false
            }
            if (releaseStoreFile != null) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    buildFeatures {
        compose = true
        // For BuildConfig.DEBUG, which gates the on-screen performance
        // readout - see GameHudState.perfLabel.
        buildConfig = true
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }

    // Bundle upstream's data/ directory (weapon/landscape/mod XML, tank
    // meshes, language strings, etc.) straight from the submodule, via a
    // generated staging dir (see stageScorchedData below) rather than
    // duplicating ~90MB into this repo. Extracted to internal storage at
    // first run (see AssetDataExtractor.kt) since upstream's file I/O uses
    // plain fopen()/paths, not AAssetManager.
    sourceSets {
        getByName("main") {
            // builtBy(stageScorchedData), not a bare path: a plain string
            // srcDir tells Gradle where the assets are but not what produces
            // them, so every task that reads the assets directory has to be
            // told to depend on the staging task by hand. That was done for
            // the merge*Assets tasks and missed generateReleaseLintVitalReportModel,
            // which broke the release build outright - Gradle 9 fails the
            // build on an undeclared implicit dependency rather than warning.
            // Declaring the producer once here wires every consumer.
            assets.srcDir(
                files(layout.buildDirectory.dir("generated/assets-staging"))
                    .builtBy(stageScorchedData, stageLicense)
            )
        }
    }
}

// Runs scripts/apply_patches.sh on every build (not just when CMakeLists.txt
// changes - CMake's own execute_process() only re-runs on reconfigure, which
// can silently skip this if the submodule checkout is reset in between -
// see the CMakeLists.txt comment). Always runs: patch application is itself
// idempotent (see the script), so this is cheap on the common no-op path.
val applyScorchedPatches = tasks.register<Exec>("applyScorchedPatches") {
    workingDir = rootDir
    commandLine("bash", "scripts/apply_patches.sh")
    outputs.upToDateWhen { false }
}

tasks.matching { it.name.startsWith("configureCMake") }
    .configureEach { dependsOn(applyScorchedPatches) }

dependencies {
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.core.ktx)
    implementation(libs.material)
    implementation(libs.kotlinx.coroutines.android)

    // M4: the real in-game HUD/menu UI (see the porting plan - this was
    // always meant to be Compose overlaid on the GLSurfaceView, not the
    // plain AlertDialogs/Buttons that shipped as an M4 stopgap while M5
    // multiplayer work took priority).
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.foundation)
    // M6: the HUD is icon-driven rather than a wall of text labels (see
    // GameHud.kt) - material3 doesn't bundle icons, they come from the
    // (BOM-versioned) material icon artifact.
    implementation(libs.androidx.compose.material.icons.extended)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.runtime.compose)
    debugImplementation(libs.androidx.compose.ui.tooling)

    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
}
plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
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
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    buildTypes {
        release {
            optimization {
                enable = false
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    buildFeatures {
        compose = true
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
            assets.srcDirs("build/generated/assets-staging")
        }
    }
}

val stageScorchedData = tasks.register<Sync>("stageScorchedData") {
    from(layout.projectDirectory.dir("../third_party/scorched3d/data"))
    into(layout.buildDirectory.dir("generated/assets-staging/data"))
}

tasks.matching { it.name.startsWith("merge") && it.name.endsWith("Assets") }
    .configureEach { dependsOn(stageScorchedData) }

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
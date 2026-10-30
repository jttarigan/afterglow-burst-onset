// The native code is NOT built here: CMake (preset android-release) builds libafterglow.so with the
// same flags as every other platform and stages the SPIR-V kernels and research levels as assets.
// Gradle only packages them (tools/device/run_android.sh runs both steps). Adapted from NeonTide.
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val nativeBuild = file("../../../build/android-release")

android {
    namespace = "org.afterglow.app"
    compileSdk = 35

    defaultConfig {
        applicationId = "org.afterglow.app"
        minSdk = 31          // ADPF (P3)
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
        ndk { abiFilters += listOf("arm64-v8a") }
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }
    sourceSets["main"].jniLibs.srcDirs(file("$nativeBuild/jniLibs"))
    sourceSets["main"].assets.srcDirs(file("$nativeBuild/assets"))
    androidResources { ignoreAssetsPattern = "!.svn:!.git:!.ds_store:!*.scc:.*:!CVS:!thumbs.db:!*~:!*.spvasm" }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    packaging { jniLibs.useLegacyPackaging = false }
}

tasks.named("preBuild") {
    doFirst {
        check(file("$nativeBuild/jniLibs/arm64-v8a/libafterglow.so").exists()) {
            "libafterglow.so missing: run cmake --build --preset android-release first"
        }
    }
}

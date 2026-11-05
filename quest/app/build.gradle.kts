plugins {
    id("com.android.application")
}

// One version for the Mac app and the headset app: the repository's VERSION file
// (MAJOR.MINOR.PATCH; versionCode = MAJOR * 10000 + MINOR * 100 + PATCH).
val appVersion = rootDir.resolve("../VERSION").readText().trim()
val appVersionCode = appVersion.split(".").map { it.toInt() }.let { (major, minor, patch) ->
    major * 10000 + minor * 100 + patch
}

android {
    namespace = "com.df0.vrscreen"
    compileSdk = 36
    buildToolsVersion = "36.1.0"
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "com.df0.vrscreen"
        minSdk = 32
        targetSdk = 34
        versionCode = appVersionCode
        versionName = appVersion

        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static")
            }
        }
    }

    buildFeatures {
        prefab = true
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }
}

dependencies {
    // Khronos OpenXR loader (prefab package "OpenXR": OpenXR::openxr_loader + OpenXR::headers).
    // Its manifest also merges the OpenXR runtime-broker permissions and <queries>.
    implementation("org.khronos.openxr:openxr_loader_for_android:1.1.65")
}

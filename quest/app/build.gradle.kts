plugins {
    id("com.android.application")
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
        versionCode = 1
        versionName = "0.2.0"

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

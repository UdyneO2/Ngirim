plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Versi bisa di-override dari CI: -PversionName=2.8 -PversionCode=13
val vName = (project.findProperty("versionName") as String?) ?: "2.9"
val vCode = (project.findProperty("versionCode") as String?)?.toIntOrNull() ?: 14

android {
    namespace = "com.udyneos.ngirim"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.udyneos.ngirim"
        minSdk = 30
        targetSdk = 35
        versionCode = vCode
        versionName = vName
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a") }
        externalNativeBuild { cmake { cppFlags += "-std=c++17" } }
    }

    signingConfigs {
        create("release") {
            val path = System.getenv("KEYSTORE_PATH")
            if (path != null && file(path).exists()) {
                storeFile = file(path)
                storePassword = System.getenv("KEYSTORE_PASSWORD")
                keyAlias = System.getenv("KEY_ALIAS")
                keyPassword = System.getenv("KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Lokal tanpa keystore -> fallback ke debug key; di CI selalu pakai keystore rilis
            signingConfig = if (System.getenv("KEYSTORE_PATH") != null)
                signingConfigs.getByName("release") else signingConfigs.getByName("debug")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures { aidl = true; buildConfig = true }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    lint { checkReleaseBuilds = false; abortOnError = false }
}

dependencies {
    implementation("dev.rikka.shizuku:api:13.1.5")
    implementation("dev.rikka.shizuku:provider:13.1.5")
    // Pemindai QR bawaan (kamera + izin ditangani pustaka)
    implementation("com.journeyapps:zxing-android-embedded:4.3.0")
}

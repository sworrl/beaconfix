// BeaconFix Lite: an Android library (AAR) whose core is plain Kotlin/JVM. Built from android/ (settings include
// ":lite"), so it shares the app's Gradle wrapper and plugin versions. It compiles at Kotlin language level 1.9 and
// Java 8 bytecode, minSdk 21, so projects on Kotlin 1.9 / older Android (a Frameo's) can take it as it is.
plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
}

val liteVersion = "1.0.0"
version = liteVersion

android {
    namespace = "org.sworrl.beaconfix.lite"
    compileSdk = 35
    defaultConfig {
        minSdk = 21
        buildConfigField("String", "LITE_VERSION", "\"$liteVersion\"")
    }
    buildFeatures { buildConfig = true }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }
    sourceSets["main"].java.srcDirs("src/main/kotlin")
    sourceSets["test"].java.srcDirs("src/test/kotlin")
    testOptions { unitTests.isReturnDefaultValues = true }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_1_8)
        languageVersion.set(org.jetbrains.kotlin.gradle.dsl.KotlinVersion.KOTLIN_1_9)
        apiVersion.set(org.jetbrains.kotlin.gradle.dsl.KotlinVersion.KOTLIN_1_9)
    }
}

dependencies {
    testImplementation(libs.junit)
}

// The core as a plain jar (no Android classes needed to run it): for the desktop CLI and for dexing onto a device.
tasks.register<Jar>("coreJar") {
    dependsOn("compileReleaseKotlin")
    archiveBaseName.set("beaconfix-lite-core")
    archiveVersion.set(liteVersion)
    from(layout.buildDirectory.dir("tmp/kotlin-classes/release")) { exclude("**/android/**", "**/BuildConfig*") }
    destinationDirectory.set(layout.buildDirectory.dir("dist"))
}

// The sources, for projects that vendor them instead of taking the AAR.
tasks.register<Zip>("sourcesZip") {
    archiveBaseName.set("beaconfix-lite-src")
    archiveVersion.set(liteVersion)
    from("src/main/kotlin")
    from("README.md")
    destinationDirectory.set(layout.buildDirectory.dir("dist"))
}

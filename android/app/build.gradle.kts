import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
    alias(libs.plugins.kotlin.ksp)
    alias(libs.plugins.hilt)
}

// Release signing: secrets never live in the repo. Either export
//   BEACONFIX_KEYSTORE, BEACONFIX_KEYSTORE_PASSWORD, BEACONFIX_KEY_ALIAS, BEACONFIX_KEY_PASSWORD
// in the environment, or put `envFile=/path/to/keystore.env` (a KEY=VALUE file with those four
// variables) into android/keystore.properties (git-ignored).
val signing: Map<String, String> = run {
    val vars = listOf("BEACONFIX_KEYSTORE", "BEACONFIX_KEYSTORE_PASSWORD", "BEACONFIX_KEY_ALIAS", "BEACONFIX_KEY_PASSWORD")
    val fromEnv = vars.associateWith { System.getenv(it) ?: "" }
    if (fromEnv.values.all { it.isNotBlank() }) return@run fromEnv
    val props = Properties()
    val pf = rootProject.file("keystore.properties")
    if (pf.exists()) pf.inputStream().use { props.load(it) }
    val envFile = props.getProperty("envFile")?.let { file(it.replaceFirst("~", System.getProperty("user.home"))) }
    if (envFile != null && envFile.exists()) {
        val map = envFile.readLines().filter { it.contains("=") && !it.trimStart().startsWith("#") }
            .associate { l -> l.substringBefore("=").trim() to l.substringAfter("=").trim().trim('\'', '"') }
        vars.associateWith { map[it] ?: "" }
    } else emptyMap()
}
val canSign = signing.size == 4 && signing.values.all { it.isNotBlank() } && File(signing["BEACONFIX_KEYSTORE"]!!).exists()

android {
    namespace = "org.sworrl.beaconfix"
    compileSdk = 35

    defaultConfig {
        applicationId = "org.sworrl.beaconfix"
        minSdk = 26
        targetSdk = 35
        versionCode = 6
        versionName = "1.3.2"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        vectorDrawables { useSupportLibrary = true }
    }

    signingConfigs {
        if (canSign) {
            create("release") {
                storeFile = file(signing["BEACONFIX_KEYSTORE"]!!)
                storePassword = signing["BEACONFIX_KEYSTORE_PASSWORD"]
                keyAlias = signing["BEACONFIX_KEY_ALIAS"]
                keyPassword = signing["BEACONFIX_KEY_PASSWORD"]
                enableV1Signing = false
                enableV2Signing = true
                enableV3Signing = true
                enableV4Signing = false
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            if (canSign) signingConfig = signingConfigs.getByName("release")
        }
        debug {
            applicationIdSuffix = ".debug"
            isDebuggable = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    buildFeatures { compose = true; buildConfig = true }
    packaging {
        resources {
            excludes += listOf("/META-INF/{AL2.0,LGPL2.1}", "META-INF/versions/9/OSGI-INF/MANIFEST.MF", "META-INF/versions/**", "META-INF/DEPENDENCIES", "META-INF/LICENSE*", "META-INF/NOTICE*", "META-INF/*.kotlin_module", "META-INF/BC*.SF", "META-INF/BC*.RSA")
            pickFirsts += listOf("META-INF/INDEX.LIST")
        }
        jniLibs { useLegacyPackaging = false }
    }
    lint {
        abortOnError = true
        warningsAsErrors = false
        disable += listOf("MissingTranslation")
    }
    testOptions { unitTests.isReturnDefaultValues = true }
    // Room schema history (app/schemas) doubles as the MigrationTest's assets.
    sourceSets["androidTest"].assets.srcDir("$projectDir/schemas")
}

ksp { arg("room.schemaLocation", "$projectDir/schemas") }

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.core.splashscreen)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.lifecycle.service)
    implementation(libs.androidx.activity.compose)

    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.androidx.material3)
    implementation(libs.androidx.material.icons.extended)
    implementation(libs.androidx.compose.adaptive)
    debugImplementation(libs.androidx.ui.tooling)

    implementation(libs.androidx.navigation.compose)
    implementation(libs.androidx.hilt.navigation.compose)
    implementation(libs.hilt.android)
    ksp(libs.hilt.compiler)

    implementation(libs.retrofit)
    implementation(libs.retrofit.kotlinx)
    implementation(libs.okhttp)
    implementation(libs.okhttp.sse)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.kotlinx.coroutines.play)
    implementation(libs.androidx.datastore.preferences)

    implementation(libs.androidx.room.runtime)
    implementation(libs.androidx.room.ktx)
    ksp(libs.androidx.room.compiler)

    implementation(libs.androidx.work.runtime.ktx)
    implementation(libs.androidx.hilt.work)
    ksp(libs.androidx.hilt.work.compiler)

    implementation(libs.androidx.security.crypto)
    implementation(libs.play.services.location)
    implementation(libs.osmdroid)
    implementation(libs.androidx.glance.appwidget)
    implementation(libs.androidx.glance.material3)
    implementation(libs.bouncycastle)
    implementation(libs.zxing.core)
    implementation(libs.gson)
    implementation(libs.androidx.camera.core)
    implementation(libs.androidx.camera.camera2)
    implementation(libs.androidx.camera.lifecycle)
    implementation(libs.androidx.camera.view)

    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.room.testing)
}

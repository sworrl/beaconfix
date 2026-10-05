import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
    alias(libs.plugins.kotlin.ksp)
    alias(libs.plugins.hilt)
    alias(libs.plugins.aboutlibraries)
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
        versionCode = 10
        versionName = "3.10.0"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        vectorDrawables { useSupportLibrary = true }
        // The hub a fresh install assumes before enrolment (an invite carries the real one). Set yours outside the repo:
        // `beaconfixHubUrl=https://hub.your.domain/` in ~/.gradle/gradle.properties.
        buildConfigField("String", "HUB_URL", "\"" + ((project.findProperty("beaconfixHubUrl") as String?) ?: "https://hub.example.com/") + "\"")
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
            excludes += listOf("/META-INF/{AL2.0,LGPL2.1}", "META-INF/versions/9/OSGI-INF/MANIFEST.MF", "META-INF/versions/**", "META-INF/DEPENDENCIES", "META-INF/*.kotlin_module", "META-INF/BC*.SF", "META-INF/BC*.RSA")
            // third-party notices are kept, not dropped: several jars ship a META-INF/LICENSE or NOTICE, so the copies
            // are concatenated instead of colliding; the app shows them all under Settings → Open-source licenses
            merges += listOf("META-INF/LICENSE", "META-INF/LICENSE.txt", "META-INF/LICENSE.md", "META-INF/NOTICE", "META-INF/NOTICE.txt", "META-INF/NOTICE.md")
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

// Settings → Open-source licenses (docs/LICENSING.md): every dependency's license from its POM, collected at build time,
// plus the components that are not Maven dependencies (the ALPR models) from app/config/libraries.
aboutLibraries {
    configPath = "app/config"      // relative to the root project (android/)
    excludeFields = arrayOf("generated")
}
// the plugin does not declare the config directory as an input: an edit there must regenerate the list
tasks.matching { it.name.startsWith("prepareLibraryDefinitions") }.configureEach { inputs.dir("config").withPathSensitivity(PathSensitivity.RELATIVE) }

// ALPR dash cam: the plate detector / reader models, fetched at build time (see alpr_models.gradle.kts)
android.sourceSets["main"].assets.srcDir(layout.buildDirectory.dir("generated/alpr_assets"))
android.buildTypes["release"].proguardFile("alpr-proguard-rules.pro")
// ONNX Runtime ships ~43 MB of x86 / x86_64 libraries for emulators; the dash cam is for ARM phones
android.packaging.jniLibs.excludes += listOf("lib/x86/libonnxruntime*.so", "lib/x86_64/libonnxruntime*.so")
apply(from = "alpr_models.gradle.kts")

// Surveillance signatures (docs/DETECTION.md): ONE file shared with the desktop, data/signatures/surveillance.json,
// copied into the assets at build time (never duplicated by hand). The unit tests read it and the shared cases directly.
val surveillanceSignatures = rootProject.file("../data/signatures/surveillance.json")
val copySurveillanceSignatures = tasks.register<Copy>("copySurveillanceSignatures") {
    from(surveillanceSignatures)
    into(layout.buildDirectory.dir("generated/signature_assets/signatures"))
}
android.sourceSets["main"].assets.srcDir(layout.buildDirectory.dir("generated/signature_assets"))
tasks.named("preBuild") { dependsOn(copySurveillanceSignatures) }
tasks.withType<Test>().configureEach {
    systemProperty("beaconfix.signatures", surveillanceSignatures.absolutePath)
    systemProperty("beaconfix.signatureCases", rootProject.file("../tests/fixtures/surveillance_cases.json").absolutePath)
    inputs.file(surveillanceSignatures).withPathSensitivity(PathSensitivity.RELATIVE)
    inputs.file(rootProject.file("../tests/fixtures/surveillance_cases.json")).withPathSensitivity(PathSensitivity.RELATIVE)
}

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
    implementation(libs.androidx.car.app)
    implementation(libs.androidx.car.app.projected)
    implementation("com.google.guava:guava:33.3.1-android")
    implementation("com.microsoft.onnxruntime:onnxruntime-android:1.30.0")   // ALPR dash cam (MIT)
    // Settings → Open-source licenses: the dependency licenses are collected at build time (AboutLibraries, Apache-2.0)
    implementation(libs.aboutlibraries.core)
    implementation(libs.aboutlibraries.compose.m3)

    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.room.testing)
}

// ALPR dash cam models (ankandrew): downloaded once into build/generated/alpr_assets and bundled as assets.
// Offline builds still succeed without them; the app then downloads them on first use (same SHA-256 checks).
// Licensing (docs/LICENSING.md): the OCR models are MIT and always bundled. The plate DETECTOR's weights were trained
// with GPL-3.0 code (a "grey zone"), so the Apache-2.0 APK does not bundle it by default — the app downloads it on
// first use. Build with -PbundleGplModels=true to bundle it anyway (e.g. a private offline build).
// Everything lives inside the task (configuration-cache safe: no references to this script object).
val alprModels = tasks.register("alprModels") {
    val dir = layout.buildDirectory.dir("generated/alpr_assets/alpr_models").get().asFile
    val bundleGpl = (project.findProperty("bundleGplModels") as String?)?.toBoolean() ?: false
    val detector = listOf(
        listOf("yolo-v9-t-640-license-plates-end2end.onnx",
            "https://github.com/ankandrew/open-image-models/releases/download/assets/yolo-v9-t-640-license-plates-end2end.onnx",
            "c3c1026ca7d0585dd88084d68182dd897113712fa734ae1557ca70174440c076"),
    )
    val specs = (if (bundleGpl) detector else emptyList()) + listOf(
        listOf("cct_xs_v2_global.onnx",
            "https://github.com/ankandrew/fast-plate-ocr/releases/download/arg-plates/cct_xs_v2_global.onnx",
            "8031afb5fdc6b4d80462c9d542f1284ebd2cfddf5dbacd62609848d7e2855f44"),
    )
    outputs.dir(dir)
    inputs.property("bundleGplModels", bundleGpl)
    outputs.upToDateWhen {
        val names = specs.map { it[0] }.toSet()
        (dir.listFiles()?.none { it.isFile && it.name !in names } ?: true) &&
        specs.all { s -> java.io.File(dir, s[0]).let { f -> f.exists() && java.security.MessageDigest.getInstance("SHA-256").digest(f.readBytes()).joinToString("") { "%02x".format(it) } == s[2] } }
    }
    doLast {
        dir.mkdirs()
        val names = specs.map { it[0] }.toSet()
        dir.listFiles()?.filter { it.isFile && it.name !in names }?.forEach { it.delete() }   // e.g. the detector after -PbundleGplModels is dropped
        for (s in specs) {
            val (name, url, sha) = s
            val f = java.io.File(dir, name)
            fun sum(x: java.io.File) = java.security.MessageDigest.getInstance("SHA-256").digest(x.readBytes()).joinToString("") { "%02x".format(it) }
            if (f.exists() && sum(f) == sha) continue
            try {
                var u = java.net.URL(url)
                var conn = u.openConnection() as java.net.HttpURLConnection
                conn.connectTimeout = 20_000; conn.readTimeout = 120_000
                var hops = 0   // GitHub release assets redirect across hosts
                while (conn.responseCode in 300..399 && hops++ < 5) { u = java.net.URL(u, conn.getHeaderField("Location")); conn = u.openConnection() as java.net.HttpURLConnection }
                val tmp = java.io.File(dir, "$name.tmp")
                conn.inputStream.use { input -> tmp.outputStream().use { input.copyTo(it) } }
                check(sum(tmp) == sha) { "checksum mismatch" }
                tmp.renameTo(f)
                logger.lifecycle("ALPR model bundled: $name (${f.length()} bytes)")
            } catch (e: Exception) {
                logger.warn("ALPR model $name not bundled (${e.message}); the app downloads it on first use")
            }
        }
    }
}
tasks.named("preBuild") { dependsOn(alprModels) }

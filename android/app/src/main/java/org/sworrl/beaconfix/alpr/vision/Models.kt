package org.sworrl.beaconfix.alpr.vision

import ai.onnxruntime.OnnxJavaType
import ai.onnxruntime.OnnxTensor
import ai.onnxruntime.OrtEnvironment
import ai.onnxruntime.OrtSession
import android.content.Context
import android.util.Log
import org.sworrl.beaconfix.alpr.core.BoxF
import org.sworrl.beaconfix.alpr.core.CropMath
import org.sworrl.beaconfix.alpr.core.Detection
import org.sworrl.beaconfix.alpr.core.PlateQuality
import org.sworrl.beaconfix.alpr.core.PlateRead
import org.sworrl.beaconfix.alpr.core.PlateText
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.security.MessageDigest

/**
 * The on-device models (code MIT, by ankandrew; see docs/LICENSING.md for the detector weights):
 *  - plate detector: open-image-models `yolo-v9-t-640-license-plates-end2end.onnx` (YOLOv9-t with NMS in the graph),
 *    for the full-frame pass, and the same model trained at 416 px (`yolo-v9-t-416-…`, 2.7× less compute) for the
 *    tiles, the burst windows and the hot full-frame pass. Both take batch 1 only (fixed reshapes in the graph), so
 *    tiles cannot be batched into one run.
 *  - plate recognizer: fast-plate-ocr `cct_xs_v2_global.onnx` (10 slots × 37 symbols, 128×64 RGB uint8 input, plus a
 *    66-way region head), and optionally `cct_s_v2_global.onnx` (same input/outputs; published US plate accuracy 94.6 %
 *    vs 92.9 %, ~8× the compute), the "accurate OCR" setting.
 * Bundled as assets at build time when listed in app/alpr_models.gradle.kts; otherwise (or if a build was made offline)
 * downloaded once from the GitHub releases on first use, SHA-256 checked.
 */
object AlprModels {
    data class Spec(val file: String, val url: String, val sha256: String, val bytes: Long)

    val DETECTOR = Spec("yolo-v9-t-640-license-plates-end2end.onnx",
        "https://github.com/ankandrew/open-image-models/releases/download/assets/yolo-v9-t-640-license-plates-end2end.onnx",
        "c3c1026ca7d0585dd88084d68182dd897113712fa734ae1557ca70174440c076", 7_835_770)
    val DETECTOR_TILE = Spec("yolo-v9-t-416-license-plates-end2end.onnx",
        "https://github.com/ankandrew/open-image-models/releases/download/assets/yolo-v9-t-416-license-plates-end2end.onnx",
        "0469d81f176081671ca4caca1e05f94758eae2f9f659bb8a6c410216f2173fe4", 7_777_518)
    val OCR_ACCURATE = Spec("cct_s_v2_global.onnx",
        "https://github.com/ankandrew/fast-plate-ocr/releases/download/arg-plates/cct_s_v2_global.onnx",
        "384bbbd2cea3ef54761d3df70822ef3a349ee1a112aeafddbe0e3ba06bc6e47b", 5_262_230)
    val OCR = Spec("cct_xs_v2_global.onnx",
        "https://github.com/ankandrew/fast-plate-ocr/releases/download/arg-plates/cct_xs_v2_global.onnx",
        "8031afb5fdc6b4d80462c9d542f1284ebd2cfddf5dbacd62609848d7e2855f44", 3_344_292)

    private const val ASSET_DIR = "alpr_models"

    /** The model bytes: from assets, else the downloaded copy, else download now (blocking; call off the main thread). */
    fun load(ctx: Context, spec: Spec): ByteArray {
        runCatching { ctx.assets.open("$ASSET_DIR/${spec.file}").use { it.readBytes() } }.getOrNull()?.let { return it }
        val dir = File(ctx.noBackupFilesDir, ASSET_DIR).apply { mkdirs() }
        val f = File(dir, spec.file)
        if (f.exists() && sha256(f.readBytes()) == spec.sha256) return f.readBytes()
        Log.i("Alpr", "downloading ${spec.file}")
        val conn = URL(spec.url).openConnection() as HttpURLConnection
        conn.instanceFollowRedirects = true; conn.connectTimeout = 15_000; conn.readTimeout = 60_000
        val bytes = conn.inputStream.use { it.readBytes() }
        check(sha256(bytes) == spec.sha256) { "${spec.file}: checksum mismatch" }
        val tmp = File(dir, spec.file + ".tmp"); tmp.writeBytes(bytes); tmp.renameTo(f)
        return bytes
    }

    fun available(ctx: Context, spec: Spec): Boolean =
        runCatching { ctx.assets.open("$ASSET_DIR/${spec.file}").close(); true }.getOrDefault(false) || File(File(ctx.noBackupFilesDir, ASSET_DIR), spec.file).exists()

    fun sha256(b: ByteArray): String = MessageDigest.getInstance("SHA-256").digest(b).joinToString("") { "%02x".format(it) }

    /** No spinning worker threads (they burn CPU between frames and heat the phone); XNNPACK when asked (much faster on ARM). */
    fun options(threads: Int, xnnpack: Boolean): OrtSession.SessionOptions = OrtSession.SessionOptions().apply {
        setOptimizationLevel(OrtSession.SessionOptions.OptLevel.ALL_OPT)
        addConfigEntry("session.intra_op.allow_spinning", "0")
        if (xnnpack) {
            runCatching { addXnnpack(mapOf("intra_op_num_threads" to threads.toString())); setIntraOpNumThreads(1) }
                .onFailure { setIntraOpNumThreads(threads) }
        } else setIntraOpNumThreads(threads)
    }
}

/** YOLOv9-t plate detector. Not thread-safe: one instance per analysis thread. */
class PlateDetector(private val env: OrtEnvironment, model: ByteArray, threads: Int, xnnpack: Boolean, val size: Int = 640) : AutoCloseable {
    private val session = env.createSession(model, AlprModels.options(threads, xnnpack))
    private val inputName = session.inputNames.first()
    private val input: FloatBuffer = ByteBuffer.allocateDirect(4 * 3 * size * size).order(ByteOrder.nativeOrder()).asFloatBuffer()
    private val px = IntArray(size * size)
    private val chw = FloatArray(3 * size * size)
    /** Time of the last call's sampling + tensor fill, and of the model run (ms; for the status card and tuning). */
    var prepMs = 0L; private set
    var runMs = 0L; private set

    /** Plates in [region] of [frame], returned in frame pixels. */
    fun detect(frame: FrameRgb, region: BoxF, minScore: Float): List<Detection> {
        val t0 = android.os.SystemClock.elapsedRealtime()
        val lb = CropMath.letterbox(region.w.toInt(), region.h.toInt(), size)
        frame.sampleArgb(region, lb.newW, lb.newH, px)
        val plane = size * size; val gray = 114f / 255f
        val left = lb.padLeft; val top = lb.padTop
        java.util.Arrays.fill(chw, gray)
        // CHW, RGB, 0..1, letterbox grey 114
        val inv = 1f / 255f
        for (sy in 0 until lb.newH) {
            val yy = sy + top
            if (yy < 0 || yy >= size) continue
            var o = yy * size + left; var p = sy * lb.newW
            for (sx in 0 until lb.newW) {
                if (sx + left in 0 until size) {
                    val c = px[p]
                    chw[o] = ((c shr 16) and 0xFF) * inv; chw[plane + o] = ((c shr 8) and 0xFF) * inv; chw[2 * plane + o] = (c and 0xFF) * inv
                }
                o++; p++
            }
        }
        input.clear(); input.put(chw)
        input.rewind()
        val t1 = android.os.SystemClock.elapsedRealtime(); prepMs = t1 - t0
        val out = ArrayList<Detection>()
        OnnxTensor.createTensor(env, input, longArrayOf(1, 3, size.toLong(), size.toLong())).use { t ->
            session.run(mapOf(inputName to t)).use { res ->
                @Suppress("UNCHECKED_CAST")
                val rows = res[0].value as Array<FloatArray>
                for (r in rows) {
                    if (r.size < 7 || r[6] < minScore) continue
                    val b = lb.toSource(BoxF(r[1], r[2], r[3], r[4])).offset(region.x1, region.y1).clamp(frame.width, frame.height)
                    if (b.w > 1 && b.h > 1) out += Detection(b, r[6])
                }
            }
        }
        runMs = android.os.SystemClock.elapsedRealtime() - t1
        return out
    }

    override fun close() = session.close()
}

/** One recognizer result: the read and the Tenengrad sharpness of the 128×64 crop it was read from. */
class OcrResult(val read: PlateRead, val tenengrad: Float)

/** fast-plate-ocr recognizer: batches every plate of a frame in one run. */
class PlateOcr(private val env: OrtEnvironment, model: ByteArray, threads: Int, xnnpack: Boolean) : AutoCloseable {
    private val session = env.createSession(model, AlprModels.options(threads, xnnpack))
    private val inputName = session.inputNames.first()
    private val outputs = setOf("plate") + (if ("region" in session.outputNames) setOf("region") else emptySet())
    private val w = 128; private val h = 64
    private val px = IntArray(w * h)

    fun read(frame: FrameRgb, boxes: List<BoxF>): List<OcrResult> {
        if (boxes.isEmpty()) return emptyList()
        val buf = ByteBuffer.allocateDirect(boxes.size * w * h * 3).order(ByteOrder.nativeOrder())
        val sharp = FloatArray(boxes.size)
        for ((k, b) in boxes.withIndex()) {
            frame.sampleArgb(b, w, h, px)
            sharp[k] = PlateQuality.tenengrad(px, w, h)
            for (p in px) { buf.put(((p shr 16) and 0xFF).toByte()); buf.put(((p shr 8) and 0xFF).toByte()); buf.put((p and 0xFF).toByte()) }
        }
        buf.rewind()
        OnnxTensor.createTensor(env, buf, longArrayOf(boxes.size.toLong(), h.toLong(), w.toLong(), 3), OnnxJavaType.UINT8).use { t ->
            session.run(mapOf(inputName to t), outputs).use { res ->
                @Suppress("UNCHECKED_CAST")
                val out = res.get("plate").get().value as Array<Array<FloatArray>>
                @Suppress("UNCHECKED_CAST")
                val region = if ("region" in outputs) res.get("region").get().value as Array<FloatArray> else null
                return out.mapIndexed { k, slots ->
                    val flat = FloatArray(PlateText.MAX_SLOTS * PlateText.ALPHABET.length)
                    for (s in 0 until minOf(PlateText.MAX_SLOTS, slots.size)) System.arraycopy(slots[s], 0, flat, s * PlateText.ALPHABET.length, minOf(PlateText.ALPHABET.length, slots[s].size))
                    val na = region?.get(k)?.let { r -> NORTH_AMERICA.sumOf { i -> r.getOrElse(i) { 0f }.toDouble() }.toFloat() } ?: -1f
                    OcrResult(PlateText.decode(flat).copy(regionProb = na), sharp[k])
                }
            }
        }
    }

    override fun close() = session.close()

    companion object {
        /** Region-head classes of the v2 "global" models (plate_config.yaml order): Canada 14, Mexico 39, United States 61. */
        val NORTH_AMERICA = intArrayOf(14, 39, 61)
    }
}

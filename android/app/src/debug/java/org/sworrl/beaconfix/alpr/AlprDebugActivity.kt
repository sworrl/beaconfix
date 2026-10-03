package org.sworrl.beaconfix.alpr

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.os.Bundle
import android.util.Log
import org.sworrl.beaconfix.alpr.core.AlprJson
import org.sworrl.beaconfix.alpr.core.CropMath
import org.sworrl.beaconfix.alpr.vision.AlprPipeline
import org.sworrl.beaconfix.alpr.vision.ArgbFrame
import org.sworrl.beaconfix.alpr.vision.FrameContext
import org.sworrl.beaconfix.alpr.vision.PassOptions
import java.io.File
import kotlinx.coroutines.launch

/**
 * Debug builds only (adb automation, no UI):
 *   am start -n org.sworrl.beaconfix.debug/org.sworrl.beaconfix.alpr.AlprDebugActivity --es selftest <file in files/> \
 *     [--ei canvas_w 4080 --ei canvas_h 3072 --ef scale 0.35] [--ei iters 5] [--ei tiles 3] [--ez xnnpack true]
 *   … --es dashcam start|stop
 *   … --es pair 'falconeyez://pair?…'   (pairs, then fetches the hotlist)   · --ez forget true
 *   … --ez upload true  (the self-test's events go to the uploader like the dash cam's)
 * The self-test pastes the photo (scaled) into a grey dash-cam-sized canvas at the plate band, runs the pipeline and logs
 * reads and timings under the tag AlprSelfTest. Nothing is queued or uploaded.
 */
class AlprDebugActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        when (intent.getStringExtra("dashcam")) {
            "start" -> { alprEntry().alprSettings().update { it.copy(enabled = true) }; DashCamService.start(this) }
            "stop" -> { alprEntry().alprSettings().update { it.copy(enabled = false) }; DashCamService.stop(this) }
        }
        val e = alprEntry()
        intent.getStringExtra("pair")?.let { qr ->
            val p = org.sworrl.beaconfix.alpr.core.Pairing.parse(qr)
            Log.i(TAG, "pair payload: ${p?.urls} name=${p?.name}")
            if (p != null) kotlinx.coroutines.CoroutineScope(kotlinx.coroutines.Dispatchers.IO).launch {
                Log.i(TAG, "pair: ${e.falconLink().pair(p.urls, p.token, p.name)}")
                Log.i(TAG, "hotlist refresh: ${e.hotlistStore().refresh(force = true)} → ${e.hotlistStore().hotlist.value}")
                Log.i(TAG, "pairing: ${e.falconLink().pairing.value}")
            }
        }
        if (intent.getBooleanExtra("forget", false)) { e.falconLink().forget(); e.hotlistStore().clear(); e.uplink().clearSpill(); Log.i(TAG, "forgot pairing") }
        val upload = intent.getBooleanExtra("upload", false)
        intent.getStringExtra("selftest")?.let { name ->
            val cw = intent.getIntExtra("canvas_w", 0); val ch = intent.getIntExtra("canvas_h", 0)
            val scale = intent.getFloatExtra("scale", 1f); val iters = intent.getIntExtra("iters", 3); val threads = intent.getIntExtra("threads", 4); val per = intent.getIntExtra("per_frame", 99)
            val tiles = intent.getIntExtra("tiles", alprEntry().alprSettings().value.tileCols)
            val xnn = intent.getBooleanExtra("xnnpack", alprEntry().alprSettings().value.xnnpack)
            val app = applicationContext
            val matcher = alprEntry().hotlistStore().matcher
            Thread {
                runCatching {
                    val f = File(name).takeIf { it.isAbsolute } ?: File(filesDir, name)
                    var bmp = BitmapFactory.decodeFile(f.path, BitmapFactory.Options().apply { inPreferredConfig = Bitmap.Config.ARGB_8888 })
                        ?: error("can't decode ${f.path}")
                    if (cw > 0 && ch > 0) {
                        val canvas = Bitmap.createBitmap(cw, ch, Bitmap.Config.ARGB_8888)
                        val c = Canvas(canvas); c.drawColor(Color.rgb(96, 100, 104))
                        val w = (bmp.width * scale).toInt(); val h = (bmp.height * scale).toInt()
                        val scaled = Bitmap.createScaledBitmap(bmp, w, h, true)
                        c.drawBitmap(scaled, (cw - w) * 0.62f, (ch * 0.6f - h / 2f).coerceAtLeast(0f), Paint(Paint.FILTER_BITMAP_FLAG))
                        bmp.recycle(); scaled.recycle(); bmp = canvas
                    }
                    val frame = ArgbFrame.of(bmp); bmp.recycle()
                    Log.i(TAG, "frame ${frame.width}x${frame.height}, tiles=$tiles ${CropMath.tilesFor(frame.width, frame.height, tiles, 416)}, xnnpack=$xnn threads=$threads perFrame=$per")
                    val t0 = System.currentTimeMillis()
                    AlprPipeline(app, threads, xnn).use { p ->
                        Log.i(TAG, "models loaded in ${System.currentTimeMillis() - t0} ms")
                        repeat(iters + 1) { i ->
                            // the frames are one vehicle to the tracker; the extra last round flushes it into its one event
                            val o = if (i == iters) p.flush(matcher)
                                else p.process(frame, FrameContext(System.currentTimeMillis(), null, null), matcher, PassOptions(tileCols = tiles, tilesPerFrame = per))
                            Log.i(TAG, "run $i: ${o.plates.size} plates, total ${o.totalMs} ms (detect ${o.detectMs} = sample ${p.lastPrepMs} + model ${p.lastRunMs}, ocr ${o.ocrMs})")
                            for (s in o.plates) Log.i(TAG, "  ${AlprPipeline.label(s.read)} conf=${s.read?.conf} det=${s.det.score} box=${s.det.box} track=${s.track} fused=${s.fused} matches=${s.matches.map { it.kind.name + ":" + it.entry.plate }}")
                            for (a in o.alerts) Log.i(TAG, "  alert ${a.match.kind} ${a.text} ≈ ${a.match.entry.plate}")
                            if (upload) for (ev in o.events) { e.uplink().start(); e.uplink().submit(ev.event); Log.i(TAG, "submitted ${ev.event.id}") }
                            for (e in o.events) Log.i(TAG, "  event ${e.event.id}: jpeg ${e.event.jpeg.size} B meta=${AlprJson.json.encodeToString(org.sworrl.beaconfix.alpr.core.FrameMeta.serializer(), e.event.meta)}")
                        }
                    }
                }.onFailure { Log.e(TAG, "selftest failed", it) }
            }.start()
        }
        finish()
    }

    companion object { private const val TAG = "AlprSelfTest" }
}

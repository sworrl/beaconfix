package org.sworrl.beaconfix.alpr.ui

import android.content.Context
import android.graphics.Bitmap
import android.graphics.ImageDecoder
import android.net.Uri
import android.os.Build
import android.provider.MediaStore
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.sworrl.beaconfix.alpr.AlprConfig
import org.sworrl.beaconfix.alpr.AlprSettings
import org.sworrl.beaconfix.alpr.AlprStatus
import org.sworrl.beaconfix.alpr.AlprUplink
import org.sworrl.beaconfix.alpr.AlprWorker
import org.sworrl.beaconfix.alpr.Call
import org.sworrl.beaconfix.alpr.DashCamService
import org.sworrl.beaconfix.alpr.FalconLink
import org.sworrl.beaconfix.alpr.HotlistStore
import org.sworrl.beaconfix.alpr.core.MatchKind
import org.sworrl.beaconfix.alpr.core.PairingPayload
import org.sworrl.beaconfix.alpr.vision.AlprPipeline
import org.sworrl.beaconfix.alpr.vision.ArgbFrame
import org.sworrl.beaconfix.alpr.vision.PassOptions
import org.sworrl.beaconfix.alpr.vision.FrameContext
import javax.inject.Inject

/** A pairing payload handed in from outside (a scanned `falconeyez://pair` link) waiting for the user's yes. */
object AlprIncoming { val payload = MutableStateFlow<PairingPayload?>(null) }

data class SelfTestResult(val text: String, val thumbs: List<Pair<String, Bitmap>>)

@HiltViewModel
class AlprViewModel @Inject constructor(
    @ApplicationContext private val ctx: Context,
    val settings: AlprSettings, val status: AlprStatus, val link: FalconLink, val hotlist: HotlistStore, private val uplink: AlprUplink,
) : ViewModel() {
    private val _busy = MutableStateFlow(false); val busy: StateFlow<Boolean> = _busy.asStateFlow()
    private val _msg = MutableStateFlow(""); val message: StateFlow<String> = _msg.asStateFlow()
    private val _test = MutableStateFlow<SelfTestResult?>(null); val selfTest: StateFlow<SelfTestResult?> = _test.asStateFlow()

    fun setEnabled(on: Boolean) {
        settings.update { it.copy(enabled = on) }
        if (on) { DashCamService.start(ctx); if (link.pairing.value.paired) AlprWorker.schedule(ctx) } else DashCamService.stop(ctx)
    }

    fun update(f: (AlprConfig) -> AlprConfig) = settings.update(f)

    fun pair(urls: List<String>, token: String, name: String) = viewModelScope.launch {
        _busy.value = true; _msg.value = "Pairing…"
        _msg.value = when (val r = link.pair(urls, token, name)) {
            is Call.Ok -> {
                AlprWorker.schedule(ctx)
                runCatching { hotlist.refresh(force = true) }
                uplink.start(); uplink.kick()
                "Paired with ${r.value.serverName.ifBlank { name }} as camera ${r.value.cameraName.ifBlank { r.value.cameraId }} (${r.url})"
            }
            is Call.Revoked -> "The server refused the token (401). Make a new pairing code on FalconEyez."
            is Call.Busy -> "The server is busy; try again in ${r.retryAfterS} s."
            is Call.Failed -> if (r.code == 404) "That server has no mobile API yet (404)." else "Pairing failed: ${r.message}"
            is Call.Unreachable -> "No URL answered: ${r.message}"
        }
        _busy.value = false
    }

    fun addUrl(u: String) { _msg.value = if (link.addUrl(u)) "Added $u" else "Not a usable URL: $u" }
    fun removeUrl(u: String) = link.removeUrl(u)

    fun forget() {
        setEnabled(false)
        link.forget(); hotlist.clear(); uplink.clearSpill(); AlprWorker.cancel(ctx)
        _msg.value = "Forgot the FalconEyez server; queued events deleted."
    }

    fun refreshHotlist() = viewModelScope.launch {
        _busy.value = true
        _msg.value = if (hotlist.refresh(force = true)) "Hotlist updated: ${hotlist.hotlist.value.entries.size} entries" else "Hotlist: server not reachable (kept the cached copy)"
        _busy.value = false
    }

    fun uploadNow() { uplink.start(); uplink.kick(); _msg.value = "Uploading…" }

    fun clearMessage() { _msg.value = "" }

    /** Runs the on-device pipeline on a photo (nothing is queued or uploaded) and reports what it read and how fast. */
    fun selfTest(uri: Uri) = viewModelScope.launch {
        _busy.value = true; _msg.value = "Testing…"
        val r = withContext(Dispatchers.Default) {
            runCatching {
                val bmp = decode(uri)
                val frame = ArgbFrame.of(bmp); bmp.recycle()
                AlprPipeline(ctx, 4, settings.value.xnnpack, settings.value.accurateOcr).use { p ->
                    val opt = PassOptions(tileCols = settings.value.tileCols, tilesPerFrame = 99, accurateOcr = settings.value.accurateOcr, makeEvents = false)
                    p.process(frame, FrameContext(System.currentTimeMillis(), null, null), hotlist.matcher, opt)   // warm-up
                    // a fresh time well past the tracker's memory: the measured pass reads the photo as a new vehicle
                    val o = p.process(frame, FrameContext(System.currentTimeMillis() + 3_600_000, null, null), hotlist.matcher, opt)
                    val lines = o.plates.joinToString("\n") { s ->
                        val m = s.matches.firstOrNull()?.let { if (it.kind == MatchKind.EXACT) " · HOTLIST ${it.entry.title}" else " · possible ${it.entry.plate}" }.orEmpty()
                        "${AlprPipeline.label(s.read)} (${"%.2f".format(s.read?.conf ?: 0f)}) · plate ${s.det.box.w.toInt()}×${s.det.box.h.toInt()} px · det ${"%.2f".format(s.det.score)}$m"
                    }
                    SelfTestResult("${frame.width}×${frame.height}: ${o.plates.size} plate(s) in ${o.totalMs} ms (detect ${o.detectMs} ms, OCR ${o.ocrMs} ms)\n$lines",
                        o.plates.map { AlprPipeline.label(it.read) to p.thumb(frame, it.det.box) })
                }
            }
        }
        r.onSuccess { _test.value = it; _msg.value = "" }.onFailure { _msg.value = "Test failed: ${it.message}" }
        _busy.value = false
    }

    private fun decode(uri: Uri): Bitmap {
        val raw = if (Build.VERSION.SDK_INT >= 28) ImageDecoder.decodeBitmap(ImageDecoder.createSource(ctx.contentResolver, uri)) { d, info, _ ->
            d.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
            val long = maxOf(info.size.width, info.size.height)
            if (long > 4096) { val s = 4096f / long; d.setTargetSize((info.size.width * s).toInt(), (info.size.height * s).toInt()) }
        } else @Suppress("DEPRECATION") MediaStore.Images.Media.getBitmap(ctx.contentResolver, uri)
        return if (raw.config == Bitmap.Config.ARGB_8888) raw else raw.copy(Bitmap.Config.ARGB_8888, false)
    }
}

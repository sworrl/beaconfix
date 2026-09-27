package org.sworrl.beaconfix.ui.screens

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import org.sworrl.beaconfix.identity.Qr
import java.util.concurrent.Executors

/** Camera preview that reports the first QR text it decodes (ZXing on the Y plane). */
@Composable
fun QrScanner(hint: String, onResult: (String) -> Unit, modifier: Modifier = Modifier) {
    val ctx = LocalContext.current
    val owner = LocalLifecycleOwner.current
    var granted by remember { mutableStateOf(ContextCompat.checkSelfPermission(ctx, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) }
    val ask = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted = it }
    val executor = remember { Executors.newSingleThreadExecutor() }
    var done by remember { mutableStateOf(false) }
    DisposableEffect(Unit) { onDispose { executor.shutdown() } }
    Box(modifier.fillMaxSize()) {
        if (!granted) {
            Surface(Modifier.align(Alignment.Center).padding(24.dp)) { Button(onClick = { ask.launch(Manifest.permission.CAMERA) }) { Text("Allow the camera to scan a QR code") } }
        } else {
            AndroidView(modifier = Modifier.fillMaxSize(), factory = { c ->
                val view = PreviewView(c)
                val future = ProcessCameraProvider.getInstance(c)
                future.addListener({
                    val provider = future.get()
                    val preview = Preview.Builder().build().also { it.surfaceProvider = view.surfaceProvider }
                    val analysis = ImageAnalysis.Builder().setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST).build()
                    analysis.setAnalyzer(executor) { img ->
                        try {
                            if (!done) {
                                val plane = img.planes[0]; val w = img.width; val h = img.height
                                val buf = plane.buffer; val rs = plane.rowStride
                                val y = ByteArray(w * h)
                                if (rs == w) { buf.get(y, 0, w * h) } else { for (r in 0 until h) { buf.position(r * rs); buf.get(y, r * w, w) } }
                                Qr.decode(y, w, h)?.let { text -> done = true; view.post { onResult(text) } }
                            }
                        } catch (e: Exception) { /* next frame */ } finally { img.close() }
                    }
                    runCatching { provider.unbindAll(); provider.bindToLifecycle(owner, CameraSelector.DEFAULT_BACK_CAMERA, preview, analysis) }
                }, ContextCompat.getMainExecutor(c))
                view
            })
            Surface(Modifier.align(Alignment.BottomCenter).padding(16.dp), tonalElevation = 3.dp, shape = MaterialTheme.shapes.medium) { Text(hint, Modifier.padding(12.dp)) }
        }
    }
}

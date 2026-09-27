package org.sworrl.beaconfix.identity

import android.graphics.Bitmap
import android.graphics.Color
import com.google.zxing.BarcodeFormat
import com.google.zxing.BinaryBitmap
import com.google.zxing.EncodeHintType
import com.google.zxing.PlanarYUVLuminanceSource
import com.google.zxing.common.HybridBinarizer
import com.google.zxing.qrcode.QRCodeReader
import com.google.zxing.qrcode.QRCodeWriter
import com.google.zxing.qrcode.decoder.ErrorCorrectionLevel

/** QR codes without Play services: ZXing for both directions. */
object Qr {
    fun encode(text: String, size: Int = 720): Bitmap {
        val m = QRCodeWriter().encode(text, BarcodeFormat.QR_CODE, size, size, mapOf(EncodeHintType.ERROR_CORRECTION to ErrorCorrectionLevel.M, EncodeHintType.MARGIN to 1))
        val bmp = Bitmap.createBitmap(m.width, m.height, Bitmap.Config.RGB_565)
        val px = IntArray(m.width * m.height)
        for (y in 0 until m.height) for (x in 0 until m.width) px[y * m.width + x] = if (m[x, y]) Color.BLACK else Color.WHITE
        bmp.setPixels(px, 0, m.width, 0, 0, m.width, m.height)
        return bmp
    }
    private val reader = QRCodeReader()
    /** Decode one camera frame (Y plane, row-stride == width after copy). Returns the text or null. */
    fun decode(y: ByteArray, width: Int, height: Int): String? = try {
        val src = PlanarYUVLuminanceSource(y, width, height, 0, 0, width, height, false)
        reader.decode(BinaryBitmap(HybridBinarizer(src))).text
    } catch (e: Exception) { null } finally { reader.reset() }
}

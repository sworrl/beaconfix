package org.sworrl.beaconfix.identity

import android.content.Context
import org.sworrl.beaconfix.R
import java.security.SecureRandom

/** EFF short wordlist (1296 words): six of them make a 32-bit-ish (6 × ~10.3 bits ≈ 62 bits) one-time code. */
object EffWords {
    @Volatile private var cache: List<String>? = null
    fun words(ctx: Context): List<String> = cache ?: ctx.resources.openRawResource(R.raw.eff_short_wordlist).bufferedReader().readLines()
        .map { it.trim() }.filter { it.isNotEmpty() }.also { cache = it }
    /** Six words joined by single spaces, lower-case — the passphrase exactly as typed into another app. */
    fun code(ctx: Context, n: Int = 6): String { val w = words(ctx); val r = SecureRandom(); return (1..n).joinToString(" ") { w[r.nextInt(w.size)] } }
}

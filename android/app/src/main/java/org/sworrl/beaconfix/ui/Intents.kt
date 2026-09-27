package org.sworrl.beaconfix.ui

import android.app.Activity
import android.app.SearchManager
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import android.net.Uri
import android.widget.Toast
import java.net.URLEncoder
import java.util.Locale

/**
 * Hand-offs to other apps, in one place. Dialling is always ACTION_DIAL (the number is filled in, the user presses
 * call) — never ACTION_CALL. Each call adds FLAG_ACTIVITY_NEW_TASK when [Context] is not an Activity, shows a Toast
 * when no app can take the intent, and returns whether an activity was started.
 */
object Intents {
    /** Open the dialer with [number] filled in. "112 / 911" dials the first number; spaces, dashes and brackets are dropped. */
    fun dial(ctx: Context, number: String): Boolean {
        val n = dialable(number)
        if (n.isEmpty()) { toast(ctx, "No phone number"); return false }
        return start(ctx, Intent(Intent.ACTION_DIAL, Uri.fromParts("tel", n, null)))
    }

    /** Directions / a map app at [lat],[lon] with a [label] pin (`geo:0,0?q=lat,lon(label)`). */
    fun navigate(ctx: Context, lat: Double, lon: Double, label: String = ""): Boolean =
        start(ctx, Intent(Intent.ACTION_VIEW, Uri.parse(geoUri(lat, lon, label))))

    /** Open [url] in the browser (https:// is assumed when there is no scheme). */
    fun web(ctx: Context, url: String): Boolean {
        val u = url.trim()
        if (u.isEmpty()) return false
        return start(ctx, Intent(Intent.ACTION_VIEW, Uri.parse(if (u.contains("://")) u else "https://$u")))
    }

    /** The system share sheet with plain [text] (and an optional [subject]). */
    fun shareText(ctx: Context, text: String, subject: String? = null): Boolean {
        val send = Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, text)
        if (!subject.isNullOrBlank()) send.putExtra(Intent.EXTRA_SUBJECT, subject)
        return start(ctx, Intent.createChooser(send, subject))
    }

    /** A web search for [query] (e.g. a hospital's phone number when OSM has none). */
    fun searchWeb(ctx: Context, query: String): Boolean {
        val search = Intent(Intent.ACTION_WEB_SEARCH).putExtra(SearchManager.QUERY, query)
        if (start(ctx, search, quiet = true)) return true
        return start(ctx, Intent(Intent.ACTION_VIEW, Uri.parse("https://www.google.com/search?q=" + URLEncoder.encode(query, "UTF-8"))))
    }

    // ── pure helpers (unit-tested) ────────────────────────────────────────────
    /** "112 / 911" → "112"; "+1 (304) 598-1111" → "+13045981111"; "tel:911" → "911"; "" when there is no digit. */
    fun dialable(number: String): String {
        val first = number.trim().removePrefix("tel:")
            .split('/', ',', ';', '|').map { it.trim() }
            .firstOrNull { part -> part.any { it.isDigit() } } ?: return ""
        val main = first.split(Regex("""\s+or\s+|\s*(?:ext\.?|x)\s*\d.*$""", RegexOption.IGNORE_CASE)).first()
        val kept = main.filter { it.isDigit() || it == '+' || it == '*' || it == '#' }
        return if (kept.startsWith("+")) "+" + kept.drop(1).replace("+", "") else kept.replace("+", "")
    }

    /** `geo:0,0?q=39.635200,-79.955900(label)`; brackets in the label are dropped so map apps parse it. */
    fun geoUri(lat: Double, lon: Double, label: String = ""): String {
        val ll = String.format(Locale.US, "%.6f,%.6f", lat, lon)
        val l = label.replace('(', ' ').replace(')', ' ').trim()
        return if (l.isEmpty()) "geo:0,0?q=$ll" else "geo:0,0?q=$ll(" + URLEncoder.encode(l, "UTF-8").replace("+", "%20") + ")"
    }

    // ── plumbing ──────────────────────────────────────────────────────────────
    private tailrec fun Context.activity(): Activity? = when (this) {
        is Activity -> this
        is ContextWrapper -> baseContext.activity()
        else -> null
    }

    private fun start(ctx: Context, intent: Intent, quiet: Boolean = false): Boolean {
        if (ctx.activity() == null) intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        return try {
            ctx.startActivity(intent); true
        } catch (e: ActivityNotFoundException) {
            if (!quiet) toast(ctx, "No app on this phone can open that"); false
        } catch (e: SecurityException) {
            if (!quiet) toast(ctx, "No app on this phone can open that"); false
        }
    }

    private fun toast(ctx: Context, text: String) = Toast.makeText(ctx.applicationContext ?: ctx, text, Toast.LENGTH_SHORT).show()
}

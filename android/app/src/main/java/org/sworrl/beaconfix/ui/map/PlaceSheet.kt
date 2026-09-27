package org.sworrl.beaconfix.ui.map

import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.data.DesktopCache
import org.sworrl.beaconfix.data.db.FixEntity
import org.sworrl.beaconfix.data.db.PoiEntity
import org.sworrl.beaconfix.estimate.Geo
import org.sworrl.beaconfix.share.ShareText
import org.sworrl.beaconfix.ui.Intents
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.compass
import org.sworrl.beaconfix.ui.map.MapFocus.Target
import org.sworrl.beaconfix.ui.metres
import org.sworrl.beaconfix.ui.theme.Green
import org.sworrl.beaconfix.ui.theme.Magenta
import org.sworrl.beaconfix.ui.theme.Orange
import org.sworrl.beaconfix.ui.theme.Slate
import kotlin.math.roundToInt

/** What the place sheet says (pure; unit-tested). */
object PlaceSheetModel {
    /** The tier / confidence line: always words, never only a colour. [arg] fills `%1$s` (the campus ER's name). */
    enum class Tier(@StringRes val text: Int) {
        PEDS_ER(R.string.map_tier_peds_er), CAMPUS(R.string.map_tier_campus), UNCONFIRMED(R.string.map_tier_unconfirmed),
        PEDS_DEPT(R.string.map_tier_peds_dept), PEDS_URGENT(R.string.map_tier_peds_urgent), URGENT(R.string.map_tier_urgent),
        ER(R.string.map_tier_er), NO_ER(R.string.map_tier_no_er), ER_UNKNOWN(R.string.map_tier_er_unknown),
    }

    fun tier(p: PoiEntity): Tier? = when {
        p.cat == "peds_er" && p.peds == 1 -> Tier.PEDS_ER
        p.cat == "peds_er" && p.campus.isNotBlank() -> Tier.CAMPUS
        p.cat == "peds_er" -> Tier.UNCONFIRMED
        p.cat == "peds_urgent" || p.peds == 4 -> Tier.PEDS_URGENT
        p.cat == "urgent" -> Tier.URGENT
        p.cat != "health" -> null
        p.er == "no" -> Tier.NO_ER
        p.peds == 3 -> Tier.PEDS_DEPT
        p.emergency || p.er == "yes" -> Tier.ER
        else -> Tier.ER_UNKNOWN
    }

    /** Minutes for a drive: straight-line × 1.4 at 70 km/h, rounded to 5 minutes, at least 5 (the shared [org.sworrl.beaconfix.help.DriveEstimate]). */
    fun driveMinutes(straightM: Double): Int = org.sworrl.beaconfix.help.DriveEstimate.seconds(straightM) / 60

    /** A drive time and whether it is measured from this phone ([fromYou]) or from the RV (the desktop's search origin). */
    data class Eta(val minutes: Int, val estimate: Boolean, val fromYou: Boolean)

    /**
     * From this phone when its fix is known: the desktop's own drive time when the desktop searched within 2 km of
     * here, else the straight-line estimate. Without a phone fix: the desktop's drive time, from the RV.
     */
    fun eta(p: PoiEntity, fromLat: Double?, fromLon: Double?): Eta? {
        val desk = p.driveS.takeIf { it > 0 }?.let { ((it / 60.0).roundToInt()).coerceAtLeast(1) }
        if (fromLat != null && fromLon != null) {
            val originKnown = p.originLat != 0.0 || p.originLon != 0.0
            if (desk != null && originKnown && Geo.distanceM(fromLat, fromLon, p.originLat, p.originLon) <= 2000) return Eta(desk, p.driveEst, true)
            return Eta(driveMinutes(Geo.distanceM(fromLat, fromLon, p.lat, p.lon)), true, true)
        }
        return desk?.let { Eta(it, p.driveEst, false) }
    }

    /** A sheet for a map target that is not in the cache (e.g. a Help place the cache lacks). */
    fun synthetic(t: Target, fallbackName: String): PoiEntity =
        PoiEntity(source = "", key = t.poiKey ?: DesktopCache.llKey(t.lat, t.lon, ""), cat = "", name = t.label.ifBlank { fallbackName }, lat = t.lat, lon = t.lon, fetchedAt = 0)
}

private fun tierColor(t: PlaceSheetModel.Tier): Color = when (t) {
    PlaceSheetModel.Tier.PEDS_ER, PlaceSheetModel.Tier.PEDS_DEPT -> Magenta
    PlaceSheetModel.Tier.ER -> Green
    else -> Orange
}

@Composable
private fun etaText(e: PlaceSheetModel.Eta): String {
    val h = e.minutes / 60; val m = e.minutes % 60
    val t = when {
        h == 0 -> stringResource(if (e.estimate) R.string.map_eta_min_est else R.string.map_eta_min, m)
        m == 0 -> stringResource(if (e.estimate) R.string.map_eta_h_est else R.string.map_eta_h, h)
        else -> stringResource(if (e.estimate) R.string.map_eta_hm_est else R.string.map_eta_hm, h, m)
    }
    return stringResource(if (e.fromYou) R.string.map_sheet_from_you else R.string.map_sheet_from_rv_origin, t)
}

/**
 * One place: name, tier / confidence, drive time, address, hours, phone, then Call (or "No phone listed" and a web
 * search), Directions, Share and Website. [from] is the best fix of this phone (null when unknown).
 */
@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun PlaceSheet(place: PoiEntity, from: FixEntity?, onDismiss: () -> Unit) {
    val ctx = LocalContext.current
    val name = place.name.ifBlank { place.label.ifBlank { stringResource(R.string.map_sheet_unnamed) } }
    val phoneFix = from?.takeIf { it.source.startsWith("phone") }
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(horizontal = 20.dp).padding(bottom = 28.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text((if (place.icon.isNotBlank()) place.icon + "  " else "") + name, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.Bold,
                modifier = Modifier.semantics { contentDescription = name })
            val kind = place.label.takeIf { it.isNotBlank() && it != name }
            val dist = phoneFix?.let { f ->
                val d = Geo.distanceM(f.lat, f.lon, place.lat, place.lon)
                stringResource(R.string.map_sheet_distance, metres(d), compass(Geo.bearingDeg(f.lat, f.lon, place.lat, place.lon)))
            }
            listOfNotNull(kind, dist).takeIf { it.isNotEmpty() }?.let { Text(it.joinToString(" · "), color = Slate, style = MaterialTheme.typography.bodyMedium) }
            PlaceSheetModel.tier(place)?.let { t -> Text(stringResource(t.text, place.campus), color = tierColor(t), style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold) }
            if (place.detail.isNotBlank()) Text(place.detail, style = MaterialTheme.typography.bodyMedium)
            PlaceSheetModel.eta(place, phoneFix?.lat, phoneFix?.lon)?.let { Text(etaText(it), style = MaterialTheme.typography.bodyMedium) }
            if (place.address.isNotBlank()) Text(place.address, style = MaterialTheme.typography.bodyMedium)
            if (place.hours.isNotBlank()) Text(stringResource(R.string.map_sheet_hours, place.hours), style = MaterialTheme.typography.bodyMedium)
            if (place.phone.isNotBlank()) Text(place.phone, style = MaterialTheme.typography.bodyMedium)
            else if (place.cat.isNotBlank()) Text(stringResource(R.string.map_sheet_no_phone), color = Slate, style = MaterialTheme.typography.bodyMedium)
            val callCd = stringResource(R.string.map_sheet_cd_call, name)
            val dirCd = stringResource(R.string.map_sheet_cd_directions, name)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                if (place.phone.isNotBlank()) Button(onClick = { Intents.dial(ctx, place.phone) }, modifier = Modifier.heightIn(min = 48.dp).semantics { contentDescription = callCd }) { Text(stringResource(R.string.map_sheet_call)) }
                else if (place.cat.isNotBlank()) OutlinedButton(onClick = { Intents.searchWeb(ctx, listOf(name, place.address, "phone").filter { it.isNotBlank() }.joinToString(" ")) }, modifier = Modifier.heightIn(min = 48.dp)) { Text(stringResource(R.string.map_sheet_search)) }
                OutlinedButton(onClick = { Intents.navigate(ctx, place.lat, place.lon, name) }, modifier = Modifier.heightIn(min = 48.dp).semantics { contentDescription = dirCd }) { Text(stringResource(R.string.map_sheet_directions)) }
                OutlinedButton(onClick = { Intents.shareText(ctx, ShareText.place(name, place.lat, place.lon, place.address, place.phone), name) }, modifier = Modifier.heightIn(min = 48.dp)) { Text(stringResource(R.string.map_sheet_share)) }
                if (place.website.isNotBlank()) OutlinedButton(onClick = { Intents.web(ctx, place.website) }, modifier = Modifier.heightIn(min = 48.dp)) { Text(stringResource(R.string.map_sheet_website)) }
            }
            val src = when {
                place.source.isBlank() -> null
                DesktopCache.isDesktop(place) -> stringResource(R.string.map_sheet_from_rv, ago(place.fetchedAt))
                else -> stringResource(R.string.map_sheet_from_phone, ago(place.fetchedAt))
            }
            val osm = stringResource(R.string.map_sheet_osm_note).takeIf { place.cat.isNotBlank() }      // not for a shared pin
            listOfNotNull(src, osm).takeIf { it.isNotEmpty() }?.let { Text(it.joinToString(" · "), color = Slate, style = MaterialTheme.typography.bodySmall) }
        }
    }
}

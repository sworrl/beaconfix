package org.sworrl.beaconfix.ui.screens

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.trip.Journal
import org.sworrl.beaconfix.trip.JournalDay
import org.sworrl.beaconfix.trip.Stop
import org.sworrl.beaconfix.trip.Stops
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.KeyValue
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.durText
import org.sworrl.beaconfix.ui.metres
import org.sworrl.beaconfix.ui.theme.Gold
import org.sworrl.beaconfix.ui.theme.Slate
import org.sworrl.beaconfix.ui.vm.TripViewModel
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.time.format.FormatStyle
import java.util.Locale
import kotlin.math.roundToInt

private const val DAYS_SHOWN = 7

/** "14:05" today, "Sep 26, 14:05" on another day (local time). */
fun asOfText(ms: Long, now: Long = System.currentTimeMillis(), zone: ZoneId = ZoneId.systemDefault()): String {
    val t = Instant.ofEpochMilli(ms).atZone(zone)
    val sameDay = t.toLocalDate() == Instant.ofEpochMilli(now).atZone(zone).toLocalDate()
    val time = t.format(DateTimeFormatter.ofLocalizedTime(FormatStyle.SHORT))
    return if (sameDay) time else t.format(DateTimeFormatter.ofPattern("MMM d", Locale.getDefault())) + ", " + time
}

/** "as of 14:05" in the accent colour, for anything shown from the offline cache. */
@Composable
fun AsOfLine(ms: Long) {
    if (ms > 0) Text(stringResource(R.string.a9_as_of, asOfText(ms)), color = Gold, style = MaterialTheme.typography.labelMedium)
}

/** The trip journal: days newest first, each stop a row that opens the map there ([onStop]). */
@Composable
fun TripJournal(journal: Journal, asOf: Long, onStop: (Stop) -> Unit) {
    var all by rememberSaveable { mutableStateOf(false) }
    InfoCard(stringResource(R.string.a9_journal_title)) {
        AsOfLine(asOf)
        when (journal.source) {
            Stops.TRACK -> R.string.a9_journal_from_track
            Stops.DESKTOP -> R.string.a9_journal_from_desktop
            Stops.PHONE -> R.string.a9_journal_from_phone
            else -> null
        }?.let { Text(stringResource(it), color = Slate, style = MaterialTheme.typography.bodySmall) }
        if (journal.isEmpty) Text(stringResource(R.string.a9_journal_empty), color = Slate)
        else {
            val days = journal.days.asReversed()
            for (day in if (all) days else days.take(DAYS_SHOWN)) {
                DayHeader(day)
                for (s in day.stops.asReversed()) StopRow(s, onStop)
            }
            if (days.size > DAYS_SHOWN) TextButton(onClick = { all = !all }) {
                Text(if (all) stringResource(R.string.a9_journal_less) else stringResource(R.string.a9_journal_more, days.size))
            }
        }
    }
}

@Composable
private fun DayHeader(day: JournalDay) {
    val today = LocalDate.now()
    val name = when (day.date) {
        today -> stringResource(R.string.a9_today)
        today.minusDays(1) -> stringResource(R.string.a9_yesterday)
        else -> day.date.format(DateTimeFormatter.ofPattern("EEE, MMM d", Locale.getDefault()))
    }
    HorizontalDivider(Modifier.padding(top = 4.dp))
    Row(Modifier.fillMaxWidth().semantics { heading() }, horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
        Text(name, fontWeight = FontWeight.Bold, style = MaterialTheme.typography.titleSmall)
        Text(stringResource(R.string.a9_day_summary, day.stops.size, metres(day.distanceM)), color = Slate, style = MaterialTheme.typography.bodySmall)
    }
}

@Composable
private fun StopRow(s: Stop, onStop: (Stop) -> Unit) {
    val zone = ZoneId.systemDefault()
    val time = Instant.ofEpochMilli(s.arrival).atZone(zone).format(DateTimeFormatter.ofLocalizedTime(FormatStyle.SHORT))
    val name = s.place.ifEmpty { stringResource(R.string.a9_stop_unnamed) }
    val details = buildList {
        if (s.open) add(stringResource(R.string.a9_still_here, time))
        else if (s.dwellS >= 0) add(stringResource(R.string.a9_stayed, durText(s.dwellS)))
        if (s.legM >= 50) add(stringResource(R.string.a9_leg, metres(s.legM)))
        s.elevM?.let { add(stringResource(R.string.a9_elevation, "${it.roundToInt()} m")) }
    }.joinToString(" · ")
    Row(
        Modifier.fillMaxWidth().heightIn(min = 48.dp)
            .clickable(onClickLabel = stringResource(R.string.a9_show_on_map, name)) { onStop(s) }
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(time, fontWeight = FontWeight.Bold, modifier = Modifier.width(64.dp))
        Column(Modifier.weight(1f)) {
            Text(name, maxLines = 2, overflow = TextOverflow.Ellipsis)
            if (details.isNotEmpty()) Text(details, color = Slate, style = MaterialTheme.typography.bodySmall)
        }
        Text("›", color = Slate, style = MaterialTheme.typography.titleMedium)
    }
}

/** This phone's own day, from its logged positions (works with no desktop at all). */
@Composable
fun PhoneTripCard(p: TripViewModel.PhoneSummary) {
    InfoCard(stringResource(R.string.a9_phone_title)) {
        val last = p.last
        if (last == null) Text(stringResource(R.string.a9_phone_none), color = Slate)
        else {
            KeyValue(stringResource(R.string.a9_phone_today), stringResource(R.string.a9_phone_today_value, metres(p.today.distanceM), p.today.stops))
            KeyValue(stringResource(R.string.a9_phone_fixes), "${p.fixes24h}")
            KeyValue(stringResource(R.string.a9_phone_last), ago(last.time) + " · ±${last.acc.toInt()} m")
        }
    }
}

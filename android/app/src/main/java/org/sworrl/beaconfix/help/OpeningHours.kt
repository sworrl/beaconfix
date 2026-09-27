package org.sworrl.beaconfix.help

import java.time.LocalDateTime
import java.util.Locale

/**
 * A small reader for OpenStreetMap `opening_hours` (pure). It understands what places near a road actually carry:
 * `24/7`; weekday ranges and lists (`Mo-Fr`, `Mo,We,Fr`, `Fr-Mo`); several time ranges (`08:00-12:00,13:00-17:00`);
 * rules separated by `;` (a later rule replaces an earlier one for the days it names) or `,` (an additional rule);
 * overnight ranges (`18:00-02:00`, `22:00-26:00`); `off`/`closed`; and `open`. `PH`/`SH` rules are skipped (we
 * cannot know the holidays). Anything else — sunrise/sunset, months, weeks, dates, "by appointment" — makes the
 * answer unknown (null) rather than wrong.
 */
object OpeningHours {
    private const val DAY = 24 * 60
    private val DAYS = listOf("mo", "tu", "we", "th", "fr", "sa", "su")

    /** One parsed rule: [days] (Mo = 0) it applies to, its [intervals] in minutes (end may pass 24:00), [additional] = joined by ','. */
    private class Rule(val days: Set<Int>, val intervals: List<IntRange>, val additional: Boolean)

    /** A week of intervals (minutes from each day's midnight, end exclusive, may exceed 24 h), or null when unknown. */
    private class Week(val days: List<List<IntRange>>, val always: Boolean)

    /** Open at [at]? null = unknown / unparseable. */
    fun isOpen(spec: String?, at: LocalDateTime): Boolean? {
        val w = parse(spec) ?: return null
        if (w.always) return true
        return current(w, at) != null
    }

    /**
     * When the place closes, if it is open at [at]: the end of the current opening (continuing across midnight when
     * the next day opens at 00:00). null when closed, unknown, or never closing (`24/7`).
     */
    fun closesAt(spec: String?, at: LocalDateTime): LocalDateTime? {
        val w = parse(spec) ?: return null
        if (w.always) return null
        var end = current(w, at) ?: return null          // minutes from today's midnight
        val day0 = dayIndex(at)
        repeat(7) {
            if (end % DAY != 0) return at.toLocalDate().atStartOfDay().plusMinutes(end.toLong())
            val next = w.days[(day0 + end / DAY) % 7].filter { it.first == 0 }.maxOfOrNull { it.last + 1 } ?: return at.toLocalDate().atStartOfDay().plusMinutes(end.toLong())
            end = (end / DAY) * DAY + next
        }
        return null                                       // open around the clock all week
    }

    /** The end (minutes from today's midnight) of the interval containing [at], or null when closed. */
    private fun current(w: Week, at: LocalDateTime): Int? {
        val d = dayIndex(at)
        val t = at.hour * 60 + at.minute
        w.days[d].filter { t >= it.first && t <= it.last }.maxOfOrNull { it.last + 1 }?.let { return it }
        // yesterday's ranges that run past midnight
        return w.days[(d + 6) % 7].filter { it.last + 1 > DAY && t + DAY >= it.first && t + DAY <= it.last }.maxOfOrNull { it.last + 1 - DAY }
    }

    private fun dayIndex(at: LocalDateTime) = at.dayOfWeek.value - 1

    private fun parse(spec: String?): Week? {
        val s = spec?.trim()?.takeIf { it.isNotEmpty() } ?: return null
        val norm = s.lowercase(Locale.ROOT)
            .replace('–', '-').replace('—', '-')
            .replace(Regex("""\s*-\s*"""), "-")
            .replace(Regex("""\s*,\s*"""), ",")
            .trim()
        if (norm == "24/7" || norm == "24/7 open" || norm == "mo-su 00:00-24:00" || norm == "00:00-24:00") return Week(List(7) { listOf(0 until DAY) }, true)
        val rules = ArrayList<Rule>()
        for (part in norm.split(';', '|').map { it.trim() }.filter { it.isNotEmpty() }) {
            rules += parseRule(part) ?: return null
        }
        if (rules.isEmpty()) return null
        val week = MutableList(7) { emptyList<IntRange>() }
        for (r in rules) for (d in r.days) week[d] = if (r.additional) week[d] + r.intervals else r.intervals
        val always = week.all { day -> day.any { it.first == 0 && it.last + 1 >= DAY } }
        return Week(week, always)
    }

    private val TIME = Regex("""(\d{1,2}):(\d{2})(?:-(\d{1,2}):(\d{2}))?(\+)?""")
    private val DAYSEL = Regex("""(mo|tu|we|th|fr|sa|su)(?:-(mo|tu|we|th|fr|sa|su))?""")

    /**
     * One `;`-separated rule, which may hold several `,`-joined rules ("Mo-Fr 08:00-17:00,Sa 09:00-12:00").
     * Returns an empty list for a rule we skip on purpose (PH/SH only), null for one we cannot read.
     */
    private fun parseRule(text: String): List<Rule>? {
        if (text.contains('"')) {                          // a comment: fine alongside a rule, unknown on its own
            val rest = text.replace(Regex("\"[^\"]*\""), " ").trim()
            if (rest.isEmpty()) return null
            return parseRule(rest)
        }
        val tokens = text.replace(",", " , ").replace(": ", " ").split(Regex("\\s+")).filter { it.isNotEmpty() }
        val out = ArrayList<Rule>()
        var days: MutableSet<Int>? = null
        var holidays = false
        var times = ArrayList<IntRange>()
        var hasTimes = false
        var off = false
        var additional = false

        fun flush(): Boolean {
            if (days == null && !holidays && !hasTimes && !off) return true
            val skip = holidays && days.isNullOrEmpty()
            if (!skip) {
                val d = days ?: (0..6).toMutableSet()
                val iv = when { off -> emptyList(); hasTimes -> times.toList(); else -> listOf(0 until DAY) }
                out += Rule(d, iv, additional)
            }
            days = null; holidays = false; times = ArrayList(); hasTimes = false; off = false; additional = true
            return true
        }

        for (raw in tokens) {
            val tok = raw.trimEnd(':')
            when {
                tok == "," -> Unit
                tok == "off" || tok == "closed" -> off = true
                tok == "open" -> { hasTimes = true; times.add(0 until DAY) }
                tok == "unknown" -> return null
                tok == "24/7" -> { hasTimes = true; times.add(0 until DAY) }
                tok == "ph" || tok == "sh" -> {
                    if (hasTimes || off) flush()
                    holidays = true
                }
                DAYSEL.matches(tok) -> {
                    if (hasTimes || off) flush()
                    val m = DAYSEL.matchEntire(tok)!!
                    val a = DAYS.indexOf(m.groupValues[1]); val b = m.groupValues[2].takeIf { it.isNotEmpty() }?.let { DAYS.indexOf(it) } ?: a
                    val set = days ?: mutableSetOf<Int>().also { days = it }
                    var i = a
                    while (true) { set += i; if (i == b) break; i = (i + 1) % 7 }
                }
                TIME.matches(tok) -> {
                    val m = TIME.matchEntire(tok)!!
                    val start = m.groupValues[1].toInt() * 60 + m.groupValues[2].toInt()
                    if (start > DAY) return null
                    val end = when {
                        m.groupValues[3].isNotEmpty() -> m.groupValues[3].toInt() * 60 + m.groupValues[4].toInt()
                        m.groupValues[5].isNotEmpty() -> DAY                     // "18:00+": open end, assume midnight
                        else -> return null                                        // a lone time
                    }
                    if (end > 2 * DAY) return null
                    val e = if (end <= start) end + DAY else end                   // overnight
                    times.add(start until e); hasTimes = true
                }
                else -> return null                                                // months, weeks, sunrise, dates…
            }
        }
        flush()
        return out
    }
}

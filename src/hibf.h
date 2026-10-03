// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// HaveIBeenFlocked (docs/SIGHTINGS.md §4): the plate-search watcher's pure parts — the site's hashing and variant
// scheme, the result filter, the leaky-agency list, the schedule. No network here (src/platewatch.cpp does that).
namespace Hibf {

QString fullHash(const QString &variant);       // SHA-256 hex of the variant lowercased and trimmed
QString hashPrefix(const QString &variant);     // its first 8 hex (what is sent: a k-anonymity prefix)
// The site's own O↔0 / I↔1 expansion (upper-cased; the original first, then the others in its depth-first order; ≤ max)
QStringList expandOI(const QString &plate, int max = 10);
// The forms the site would search for a plate the user typed: the display form (whitespace and '&' removed, as the
// site does) and the letters-and-digits-only form
QStringList plateForms(const QString &displayPlate);

struct Variant { QString plate; QString text; QString prefix; QString full; };   // plate: the display plate it came from
QList<Variant> variants(const QStringList &displayPlates);
QStringList prefixes(const QList<Variant> &vars);   // unique, in order

struct Match { QJsonObject row; QString plate; bool verified = false; };
// A result is ours when its license_plate_hash (full SHA-256) equals one of our variants' full hashes; a row without a
// full hash is kept unverified (it matched one of our prefixes, but it may be another plate)
QList<Match> filterResults(const QJsonArray &results, const QList<Variant> &vars);
QJsonObject searchEvent(const Match &m);          // the plate_events row (§4.2)
QString utcToLocalIso(const QString &utc);

// ── §4.4 leaky agencies ──
QStringList nameTokens(const QString &text, QString *state, bool upperStatesOnly);
QStringList fileTokens(const QString &filename, const QString &sourceUrl, QString *state);
// /api/sources/stats → the kv hibf_sources object {fetched, updatedAt, files, agencies:[{tokens, state, files, records, latest, file, url}]}
QJsonObject parseSources(const QByteArray &json, const QString &fetchedIso);
// "" when not leaky, else what matched ("<file> · <url>"). cameraState: two-letter code or "".
QString leakyMatch(const QString &operatorName, const QString &cameraState, const QJsonArray &agencies);
QString stateFromText(const QString &text);       // ", TX" / " TX 75001" at the end of an address → "TX"

// ── §4.5 schedule ──
enum class Mode { Idle, Driving, Flock, Leaky };
qint64  intervalSecs(Mode m);
QString modeName(Mode m);
qint64  backoffSecs(int failures);                 // 1 h, 2 h, 4 h … 24 h
constexpr qint64 kMinGapSecs = 10;                  // between two requests
constexpr int    kMaxPerDay = 30;

} // namespace Hibf

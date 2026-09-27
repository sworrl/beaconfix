#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

class Locator;

// History importers (docs/DATABASE.md "Importing your history"): your Google Timeline / Maps
// exports, WiGLE CSV surveys and GPX / KML tracks become observations (Wi-Fi heard at a known
// position), history fixes (where you were) and visits — all tagged with the importer's device
// name ("timeline", "wigle", "gpx", "kml") so they never touch the live fix, and every AP that
// gains samples is refitted afterwards. Large files are read as a stream: the top-level arrays
// are scanned element by element, so a 700 MB Records.json never lives in memory at once.
namespace Importers {

struct Options {
    QDateTime from, to;                 // inclusive time window (invalid = open)
    bool positions = true, wifi = true, places = true;
    static Options fromJson(const QJsonObject &o);
    QJsonObject toJson() const;
};

struct Summary {                        // the numbers both apps show (same names in the Android app)
    QString format, file, device;
    int positions = 0, wifiScans = 0, observations = 0, visits = 0, tracks = 0, skipped = 0, errors = 0, beaconsTouched = 0;
    qint64 bytes = 0;
    QDateTime first, last;
    double seconds = 0;
    QString error;
    QJsonObject toJson() const;
};

using Progress = std::function<void(int percent, const QString &stage)>;

struct PosSample { qint64 t = 0; double lat = 0, lon = 0, acc = -1, alt = -9999; };
struct WifiRec   { QString bssid; int dbm = -100; };
struct WifiScan  { qint64 t = 0; QList<WifiRec> devices; };
struct Visit     { qint64 start = 0, end = 0; double lat = 0, lon = 0; QString name, type, address; };

// A forward-only scanner for the element objects of a named top-level JSON array
// ("rawSignals", "locations", …). String-escape aware; nothing else is parsed until an
// element is complete, which is then handed to QJsonDocument on its own.
class JsonArrayStream {
public:
    explicit JsonArrayStream(QIODevice *dev);
    bool seekKey(const QByteArray &key);         // position after `"key" : [`
    bool next(QByteArray *element);              // the next complete element (object or value), false at the end of the array
    qint64 pos() const { return m_pos; }
private:
    bool fill();
    QIODevice *m_dev; QByteArray m_buf; int m_at = 0; qint64 m_pos = 0; bool m_eof = false;
};

QString detectFormat(const QString &path);      // timeline | records | semantic | wigle | gpx | kml | beaconfix | ""
QString macFromValue(const QJsonValue &v);      // decimal int64 (Timeline) or hex string → "AA:BB:CC:DD:EE:FF"
bool    parseLatLng(const QString &s, double *lat, double *lon);   // "40.0029°, -75.0681°" / "40.0, -75.1"
qint64  parseTime(const QJsonValue &v);          // ISO string (with offset) or epoch ms/s (number or numeric string) → seconds
QString wigleSecurity(const QString &authMode);  // "[WPA2-PSK-CCMP][ESS]" → wpa2 …

// Pair Wi-Fi scans with positions: nearest position within ±60 s; linear interpolation when both
// neighbours are within 2 min; skipped when the position's accuracy is worse than 100 m.
bool positionAt(const QList<PosSample> &sorted, qint64 t, PosSample *out);

// Parsers. Each emits batches through the sinks (null sinks are fine) and returns true unless the file is unreadable.
struct Sinks {
    std::function<void(const QList<PosSample> &)> positions;
    std::function<void(const QList<WifiScan> &)>  scans;
    std::function<void(const QList<Visit> &)>     visits;
    std::function<void(const QList<WifiRec> &, const PosSample &, const QString &ssid, const QString &security)> wigle;   // one row = one observation
};
bool parseTimeline(QIODevice *dev, const Options &opt, const Sinks &sinks, Summary *sum, Progress progress = nullptr);
bool parseRecords(QIODevice *dev, const Options &opt, const Sinks &sinks, Summary *sum, Progress progress = nullptr);
bool parseSemantic(QIODevice *dev, const Options &opt, const Sinks &sinks, Summary *sum, Progress progress = nullptr);
bool parseWigle(QIODevice *dev, const Options &opt, const Sinks &sinks, Summary *sum, Progress progress = nullptr);
bool parseGpx(QIODevice *dev, const Options &opt, const Sinks &sinks, Summary *sum, Progress progress = nullptr);
bool parseKml(QIODevice *dev, const Options &opt, const Sinks &sinks, Summary *sum, Progress progress = nullptr);

// The whole thing: detect, parse, pair, feed the Locator (batches of 500, refit queued), summary.
bool run(const QString &path, const Options &opt, Locator *loc, Progress progress, Summary *out);

}

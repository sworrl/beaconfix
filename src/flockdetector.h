#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

// Passive detection of Flock Safety hardware and other surveillance / police gear (docs/DETECTION.md). The rules
// live in one versioned file shared with the Android app, data/signatures/surveillance.json: compiled in (qrc), and
// overridable by <data dir>/signatures/surveillance.json when that file is valid and at least as new.
namespace FlockDetector {

struct Detection {
    bool    isFlock = false;      // Flock hardware (class flock / raven) at tier >= detectTier: recorded as a sighting
    bool    informational = false; // other surveillance / police gear at tier >= detectTier: logged, never recorded
    int     tier = -1;            // -1 no match, 0 weak (corroborates only) … 4 conclusive
    QString cls;                  // the signature class: flock, raven, axon, shotspotter, verkada, …
    QString label;                // the class's display name ("Flock Safety", "Axon body / in-car camera gear", …)
    QString cameraType;           // the flock_cameras.camera_type of a sighting (docs/SIGHTINGS.md §2.0): alpr, not_camera
    int     confidence = 0;       // 0 to 100, from the tier
    QString model;                // "Falcon", "Raven", "FS Ext Battery", "Flock device", "" unknown
    QString method;               // "wifi_mac", "wifi_ssid", "wifi_mac+ssid", "wifi_probe", "ble_name", "ble_uuid", "ble_company", …
    QString details;              // one line for the event log
    QStringList rules;            // the rules that fired, e.g. "mac:B41E52", "ssid:flock_serial=bssid", "ble_uuid:3100"
};

// One manufacturer-data record of a BLE advert
struct BleCompany { int id = -1; QByteArray data; };

// The rule set (data/signatures/surveillance.json)
class Signatures;
// The active rule set: the override from the data directory when valid and at least as new, else the compiled-in one
const Signatures &signatures();
// The compiled-in set, or the override when it parses and is at least as new (uncached; the caller owns it)
const Signatures *loadSignatures();
// Parse a rule set (nullptr + *error when invalid). The caller owns it.
const Signatures *parseSignatures(const QByteArray &json, QString *error = nullptr);
// Replace the active rule set (tests; a reload after the override file changed). Takes ownership.
void setSignatures(const Signatures *s);
int  signaturesVersion(const Signatures &s);
QString overridePath();          // <AppDataLocation>/signatures/surveillance.json

// A MAC normalised to 12 upper-case hex digits ("" when it is not one); separators : - . or none
QString normalizeMac(const QString &mac);

// A Wi-Fi access point: its BSSID, SSID and — where probe data exists — the SSIDs this MAC probed for
Detection evaluateWifi(const QString &bssid, const QString &ssid, const QStringList &probedSsids = {});
Detection evaluateWifi(const Signatures &sig, const QString &bssid, const QString &ssid, const QStringList &probedSsids = {});

// A BLE advertisement: its address, name, service UUIDs (16-bit "3100" or 128-bit, either form) and manufacturer data
Detection evaluateBle(const QString &mac, const QString &name, const QStringList &serviceUuids, const QList<BleCompany> &companies = {});
Detection evaluateBle(const Signatures &sig, const QString &mac, const QString &name, const QStringList &serviceUuids, const QList<BleCompany> &companies = {});

} // namespace FlockDetector

struct FlockCamera {
    QString   id;               // e.g. "osm:node/123456" or "det:D8:F3:BC:11:22:33"
    double    lat = 0;
    double    lon = 0;
    QString   source;           // "osm" (Overpass), "deflock", "community" / "3rd Party / Suspected" (flocklocations), "wifi_scan", "ble_scan"
    QString   model;            // the source's model ("Falcon", "Raven", "FS Ext Battery", …); "" unknown
    QString   operatorName;     // the agency / city operating it (OSM operator); "" unknown
    QString   direction;        // heading in degrees or cardinal (e.g. "EB", "WB", "315")
    QString   bssid;            // associated Wi-Fi BSSID if detected
    QString   bleMac;           // associated BLE MAC if detected
    int       confidence = 100; // 0 to 100
    QString   detectionMethod;  // "osm_tag", "wifi_oui", "wifi_ssid", "ble_name", "ble_uuid"
    QDateTime firstSeen;
    QDateTime lastSeen;
    int       sightingCount = 1;
    int       passCount = 0;    // Times user has passed this ALPR camera
    bool      vetted = false;   // true if verified against online database or by field sightings
    QDateTime vettedAt;
    QString   notes;
    qint64    seq = 0;
    QString   cameraType;       // alpr | webcam | ptz | cctv | enforcement | not_camera (docs/SIGHTINGS.md §2.0): only an ALPR reads plates
    QString   tags;             // the OSM tags (compact JSON) when known, "" otherwise
    QString   manufacturer;     // OSM manufacturer / brand (DeFlock "brand"); "" unknown — never guessed
    int       osmVersion = 0;   // the OSM element's version / timestamp when the source gives them (DeFlock, Overpass meta)
    QString   osmTimestamp;
    bool      stale = false;    // no longer confirmed by its source; kept only for the passes it has (docs/DATABASE.md)

    QJsonObject toJson() const;
    static FlockCamera fromJson(const QJsonObject &o);
};

struct LicensePlate {
    QString   plate;          // normalized, e.g. "ABC1234"
    QString   displayPlate;   // e.g. "ABC-1234"
    QString   state;          // "Texas" or "TX"
    QString   vehicleDesc;    // "Blue Toyota Corolla"
    QString   make;           // "Jeep"
    QString   model;          // "Corolla"
    QString   color;          // "Grey"
    bool      active = true;
    QDateTime addedAt;
    QString   notes;

    QJsonObject toJson() const;
    static LicensePlate fromJson(const QJsonObject &o);
};

struct CameraEncounter {
    qint64    id = 0;
    QString   cameraId;
    QDateTime time;
    double    lat = 0;
    double    lon = 0;
    double    distanceM = 0;
    double    speedKmh = 0;
    QString   plate;
    QString   vehicleDesc;
    QString   device;
    int       encounterNum = 1;
    QString   notes;

    QJsonObject toJson() const;
    static CameraEncounter fromJson(const QJsonObject &o);
};

struct PlateAudit {
    qint64    id = 0;
    QString   plate;
    QString   cameraId;
    double    lat = 0;
    double    lon = 0;
    QString   operatorName;
    QDateTime timestamp;
    QString   source;       // "deflock_open_data", "public_transparency_portal", "route_pass"
    int       confidence = 100;
    QString   details;

    QJsonObject toJson() const;
    static PlateAudit fromJson(const QJsonObject &o);
};

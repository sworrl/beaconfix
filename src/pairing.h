#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

struct AccessPoint;
struct Fix;

// Pairing v2 (docs/API.md "Pairing", shared with the Android app): a device that asks for
// access sends an ephemeral X25519 key and what it hears / where it is; we answer with our
// own ephemeral key, and both sides derive the same short authentication string (SAS)
//   SAS = HKDF-SHA256(X25519(ours, theirs), salt = "", info = "beaconfix-pair-sas-v1|" + pairId, 32 B)
// shown as THREE PICTURES from a fixed set of 48. The desktop shows the real triple among two
// decoys; the person picks the one the phone shows. A wrong pick denies the request. Proximity
// (shared beacons, signal levels, distance between the fixes) tells whether the device is
// actually next to this computer; the policy `apiPairProximity` decides how much that matters.
namespace Pairing {

static const int ICON_COUNT = 48;
extern const char *const ICON_NAMES[ICON_COUNT];     // spec order — ids must match the phone
extern const char *const ICON_GLYPHS[ICON_COUNT];    // one emoji per id

QByteArray x25519Generate(QByteArray *pub);           // returns the private key (32 B raw); pub gets 32 B
QByteArray x25519Shared(const QByteArray &priv, const QByteArray &peerPub);   // 32 B or empty
QByteArray hkdfSha256(const QByteArray &ikm, const QByteArray &salt, const QByteArray &info, int len);
QByteArray sas(const QByteArray &shared, const QString &pairId);
QList<int> sasIcons(const QByteArray &sas);           // [sas[0] % 48, sas[2] % 48, sas[4] % 48]
QList<QList<int>> decoys(const QList<int> &real, int count = 2);   // random triples sharing no icon at the same position
QJsonArray tripleJson(const QList<int> &triple);      // [{id, name, glyph} ×3]

struct Proximity {
    int shared = 0, theirs = 0, ours = 0;
    QStringList strongestShared;                      // ssid or bssid ×≤3, by their level
    // Differential signal test over the shared beacons: Δ_i = theirDbm − ourDbm; the median Δ is the two radios'
    // gain offset (gainOffsetDb) and the median absolute residual around it (rssiDelta, "MAD") is how differently
    // the two devices see the same room. Two radios in the same spot agree to a few dB after the offset is removed.
    double rssiDelta = -1;                            // MAD of the residuals (dB), -1 = n/a
    double gainOffsetDb = 0;                          // median Δ (dB)
    double distanceM = -1;                            // between the two fixes, -1 = n/a
    QString verdict = QStringLiteral("unknown");      // adjacent | room | near | far | unknown
    QJsonObject toJson() const;
    static Proximity fromJson(const QJsonObject &o);
};
// theirs: the request's "proximity" object; ours: EVERY beacon of our latest scan — home and travelling APs are the best
// co-location evidence there is (both devices hear the RV's own router at nearly the same level), so nothing is excluded.
Proximity score(const QJsonObject &theirs, const QList<AccessPoint> &ours, const Fix &ourFix);

QString verdictLabel(const QString &verdict);        // "Right next to you" …
bool    verdictAllowed(const QString &verdict, const QString &policy);   // may it be approved without the override?

}

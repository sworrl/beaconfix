#pragma once
// JSON forms of an Estimator::Fit (docs/GRADING.md §6, docs/API.md "Graded estimates"):
//   toStorage / fromStorage   every field, for the estimates.metrics column (round-trips exactly)
//   toApi                     the "fit" object of the AP JSON (D-Bus StateJson / LAN API / sync)
#include "estimator.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>

namespace Estimator {

inline QJsonObject toStorage(const Fit &f)
{
    return QJsonObject{
        {"v", kVersion}, {"valid", f.valid}, {"kind", f.kind}, {"lat", f.lat}, {"lon", f.lon}, {"acc", f.acc}, {"semiMajor", f.semiMajor}, {"semiMinor", f.semiMinor},
        {"orient", f.orientDeg}, {"cxx", f.cxx}, {"cxy", f.cxy}, {"cyy", f.cyy}, {"r95", f.r95}, {"cep50", f.cep50}, {"pWithin25", f.pWithin25}, {"rms", f.rms},
        {"p0", f.p0}, {"pathloss", f.pathloss}, {"fittedN", f.fittedN}, {"n", f.n}, {"vantage", f.vantage}, {"rejected", f.rejected}, {"quality", f.quality},
        {"updated", double(f.updated)}, {"score", f.score}, {"grade", f.grade}, {"pendingGrade", f.pendingGrade}, {"rssDop", f.rssDop}, {"crlbR95", f.crlbR95},
        {"rbar", f.rbar}, {"maxGapDeg", f.maxGapDeg}, {"inHull", f.inHull}, {"linRatio", f.linRatio}, {"chi2nu", f.chi2nu}, {"sigmaDb", f.sigmaDb},
        {"outlierFrac", f.outlierFrac}, {"ess", f.ess}, {"sessions", f.sessions}, {"devices", f.devices}, {"spearman", f.spearman}, {"p0RangeCorr", f.p0RangeCorr},
        {"dminRatio", f.dminRatio}, {"ambiguous", f.ambiguous}, {"altLat", f.altLat}, {"altLon", f.altLon}, {"modes", f.modes}, {"driftD2", f.driftD2},
        {"extD2", f.extD2}, {"jackMax", f.jackMax}, {"nisEwma", f.nisEwma}, {"fadingDb", f.fadingDb}, {"moved", f.moved}, {"newest", double(f.newest)},
        {"suggestLat", f.suggestLat}, {"suggestLon", f.suggestLon}, {"suggestGain", f.suggestGain}, {"cP", f.cP}, {"cG", f.cG}, {"cE", f.cE}, {"cF", f.cF},
        {"cFfit", f.cFfit}, {"cS", f.cS}, {"cT", f.cT}, {"cX", f.cX}, {"groupRef", f.groupRef}, {"groupSize", f.groupSize}};
}

inline Fit fromStorage(const QJsonObject &j)
{
    Fit f;
    f.valid = j["valid"].toBool(); f.kind = j["kind"].toString(QStringLiteral("none")); f.lat = j["lat"].toDouble(); f.lon = j["lon"].toDouble();
    f.acc = j["acc"].toDouble(); f.semiMajor = j["semiMajor"].toDouble(); f.semiMinor = j["semiMinor"].toDouble(); f.orientDeg = j["orient"].toDouble();
    f.cxx = j["cxx"].toDouble(); f.cxy = j["cxy"].toDouble(); f.cyy = j["cyy"].toDouble(); f.r95 = j["r95"].toDouble(); f.cep50 = j["cep50"].toDouble();
    f.pWithin25 = j["pWithin25"].toDouble(); f.rms = j["rms"].toDouble(); f.p0 = j["p0"].toDouble(-40); f.pathloss = j["pathloss"].toDouble(2.4);
    f.fittedN = j["fittedN"].toBool(); f.n = j["n"].toInt(); f.vantage = j["vantage"].toInt(); f.rejected = j["rejected"].toInt();
    f.quality = j["quality"].toString(QStringLiteral("none")); f.updated = qint64(j["updated"].toDouble()); f.score = j["score"].toDouble();
    f.grade = j["grade"].toString(); f.pendingGrade = j["pendingGrade"].toString(); f.rssDop = j["rssDop"].toDouble(); f.crlbR95 = j["crlbR95"].toDouble();
    f.rbar = j["rbar"].toDouble(1); f.maxGapDeg = j["maxGapDeg"].toDouble(360); f.inHull = j["inHull"].toBool(); f.linRatio = j["linRatio"].toDouble();
    f.chi2nu = j["chi2nu"].toDouble(); f.sigmaDb = j["sigmaDb"].toDouble(); f.outlierFrac = j["outlierFrac"].toDouble(); f.ess = j["ess"].toDouble();
    f.sessions = j["sessions"].toInt(); f.devices = j["devices"].toInt(); f.spearman = j["spearman"].toDouble(); f.p0RangeCorr = j["p0RangeCorr"].toDouble();
    f.dminRatio = j["dminRatio"].toDouble(); f.ambiguous = j["ambiguous"].toBool(); f.altLat = j["altLat"].toDouble(); f.altLon = j["altLon"].toDouble();
    f.modes = j["modes"].toInt(); f.driftD2 = j["driftD2"].toDouble(); f.extD2 = j["extD2"].toDouble(-1); f.jackMax = j["jackMax"].toDouble();
    f.nisEwma = j["nisEwma"].toDouble(); f.fadingDb = j["fadingDb"].toDouble(); f.moved = j["moved"].toBool(); f.newest = qint64(j["newest"].toDouble());
    f.suggestLat = j["suggestLat"].toDouble(); f.suggestLon = j["suggestLon"].toDouble(); f.suggestGain = j["suggestGain"].toDouble();
    f.cP = j["cP"].toDouble(); f.cG = j["cG"].toDouble(); f.cE = j["cE"].toDouble(); f.cF = j["cF"].toDouble(); f.cFfit = j["cFfit"].toDouble();
    f.cS = j["cS"].toDouble(); f.cT = j["cT"].toDouble(); f.cX = j["cX"].toDouble(-1); f.groupRef = j["groupRef"].toString(); f.groupSize = j["groupSize"].toInt();
    return f;
}

// Flags a map or card can show without knowing the thresholds (docs/GRADING.md §2.1)
inline QJsonArray flags(const Fit &f)
{
    QJsonArray a;
    if (f.kind == QLatin1String("fix") && !f.inHull) a.append(QStringLiteral("extrapolated"));
    if (f.ambiguous || f.modes >= 2) a.append(QStringLiteral("ambiguous"));
    if (f.moved) a.append(QStringLiteral("moved"));
    if (f.semiMajor > 0 && f.jackMax > 2 * f.semiMajor) a.append(QStringLiteral("fragile"));
    if (f.p0RangeCorr > 0.95) a.append(QStringLiteral("rangeScale"));
    return a;
}

inline QJsonObject toApi(const Fit &f)
{
    QJsonObject o{{"n", f.n}, {"vantage", f.vantage}, {"rms", f.rms}, {"acc", f.acc}, {"p0", f.p0}, {"pathloss", f.pathloss}, {"quality", f.quality},
                  {"rejected", f.rejected}, {"semiMajor", f.semiMajor}, {"semiMinor", f.semiMinor}, {"orient", f.orientDeg},
                  {"updated", f.updated > 0 ? QJsonValue(QDateTime::fromSecsSinceEpoch(f.updated).toString(Qt::ISODate)) : QJsonValue()},
                  {"kind", f.kind}, {"grade", f.grade}, {"score", std::round(f.score * 10) / 10}, {"r95", f.r95}, {"cep50", f.cep50},
                  {"pWithin25", f.pWithin25}, {"cxx", f.cxx}, {"cxy", f.cxy}, {"cyy", f.cyy}, {"lat", f.lat}, {"lon", f.lon},
                  {"devices", f.devices}, {"sessions", f.sessions}, {"inHull", f.inHull}, {"ambiguous", f.ambiguous || f.modes >= 2},
                  {"modes", f.modes}, {"moved", f.moved}, {"flags", flags(f)}};
    if (!f.pendingGrade.isEmpty()) o["pendingGrade"] = f.pendingGrade;
    QJsonObject c{{"P", f.cP}, {"G", f.cG}, {"E", f.cE}, {"F", f.cF}, {"S", f.cS}, {"T", f.cT}};
    if (f.cX >= 0) c["X"] = f.cX;
    o["components"] = c;
    QJsonObject m{{"rssDop", f.rssDop}, {"crlbR95", f.crlbR95}, {"rbar", f.rbar}, {"maxGapDeg", f.maxGapDeg}, {"inHull", f.inHull}, {"linRatio", f.linRatio},
                  {"chi2nu", f.chi2nu}, {"sigmaDb", f.sigmaDb}, {"outlierFrac", f.outlierFrac}, {"ess", f.ess}, {"spearman", f.spearman},
                  {"p0RangeCorr", f.p0RangeCorr}, {"dminRatio", f.dminRatio}, {"modes", f.modes}, {"driftD2", f.driftD2}, {"jackMax", f.jackMax},
                  {"nisEwma", f.nisEwma}, {"fadingDb", f.fadingDb}};
    if (f.extD2 >= 0) m["extD2"] = f.extD2;
    if (f.ambiguous) { m["altLat"] = f.altLat; m["altLon"] = f.altLon; }
    o["metrics"] = m;
    if (f.suggestGain > 0) o["suggest"] = QJsonObject{{"lat", f.suggestLat}, {"lon", f.suggestLon}, {"gain", f.suggestGain}};
    if (!f.groupRef.isEmpty()) o["group"] = QJsonObject{{"ref", f.groupRef}, {"size", f.groupSize}};
    return o;
}

} // namespace Estimator

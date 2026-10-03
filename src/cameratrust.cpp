// SPDX-License-Identifier: Apache-2.0
#include "cameratrust.h"
#include "plateevents.h"
#include <QJsonArray>

namespace CameraTrust {

QString sourceClass(const QString &id, const QString &source, const QString &model)
{
    if (id.startsWith(QLatin1String("det:"))) return QStringLiteral("detection");
    if (PlateEvents::suspectedOnly(source, model)) return QStringLiteral("suspected");
    if (id.startsWith(QLatin1String("osm:")) || source == QLatin1String("osm") || source == QLatin1String("deflock") || source == QLatin1String("openstreetmap"))
        return QStringLiteral("osm");
    if (id.startsWith(QLatin1String("flock:")) || source.contains(QLatin1String("community"), Qt::CaseInsensitive)) return QStringLiteral("community");
    return QStringLiteral("other");
}

double prior(const QString &c)
{
    if (c == QLatin1String("osm")) return kPriorOsm;
    if (c == QLatin1String("community")) return kPriorCommunity;
    if (c == QLatin1String("suspected")) return kPriorSuspected;
    if (c == QLatin1String("detection")) return kPriorDetection;
    return kPriorOther;
}

int tierFromConfidence(int c)
{
    if (c >= 98) return 4;
    if (c >= 85) return 3;
    if (c >= 65) return 2;
    if (c >= 40) return 1;
    return 0;
}

Result compute(const Inputs &in)
{
    Result r;
    QJsonArray terms;
    auto add = [&](const char *term, double v, const QString &note) {
        r.logit += v;
        terms.append(QJsonObject{{"term", QLatin1String(term)}, {"value", std::round(v * 1000) / 1000}, {"note", note}});
    };
    const QString cls = sourceClass(in.id, in.source, in.model);
    add("prior", prior(cls), QStringLiteral("source: %1%2").arg(cls, in.source.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(in.source)));
    QDateTime ts = QDateTime::fromString(in.osmTimestamp, Qt::ISODateWithMs);
    if (!ts.isValid()) ts = QDateTime::fromString(in.osmTimestamp, Qt::ISODate);
    if (ts.isValid() && ts < in.now) {
        const double years = double(ts.secsTo(in.now)) / (365.25 * 86400.0);
        add("age", kPerYear * years, QStringLiteral("OSM edit %1, %2 years ago").arg(ts.toString(Qt::ISODate)).arg(years, 0, 'f', 1));
    }
    if (in.rfTier >= 2) add("rf", kRfStrong, QStringLiteral("Flock hardware detected by RF (tier %1)%2").arg(in.rfTier).arg(in.rfWhat.isEmpty() ? QString() : QStringLiteral(": ") + in.rfWhat));
    else if (in.rfTier >= 0) add("rf", kRfWeak, QStringLiteral("Flock hardware detected by RF at a low tier (%1)%2").arg(in.rfTier).arg(in.rfWhat.isEmpty() ? QString() : QStringLiteral(": ") + in.rfWhat));
    if (in.verdict == QLatin1String("present")) add("user", kConfirmed, QStringLiteral("you confirmed it is there%1").arg(in.verdictAt.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(in.verdictAt)));
    else if (in.verdict == QLatin1String("absent")) add("user", kAbsent, QStringLiteral("you said it is not there%1").arg(in.verdictAt.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(in.verdictAt)));
    r.trust = sigmoid(r.logit);
    r.detail = QJsonObject{{"logit", std::round(r.logit * 1000) / 1000}, {"trust", std::round(r.trust * 1000) / 1000}, {"sourceClass", cls}, {"terms", terms},
                           {"computed", in.now.toString(Qt::ISODate)}, {"weights", QStringLiteral("suggested, not measured (docs/SIGHTINGS.md §2.7)")}};
    if (!std::isnan(in.rfDistanceM)) r.detail["rfDistanceM"] = std::round(in.rfDistanceM * 10) / 10;
    r.detail["osmTimestamp"] = in.osmTimestamp;          // the inputs it was computed from: a change recomputes it
    r.detail["verdict"] = in.verdict;
    return r;
}

} // namespace CameraTrust

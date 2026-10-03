// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <cmath>
#include <limits>

// Camera trust (docs/SIGHTINGS.md §2.7): how sure we are that a mapped ALPR is really there and working, as log-odds.
// Pure. trust = 1 / (1 + e^−logit) is stored per camera (flock_cameras.trust, trust_detail) and multiplies P(read).
// The weights are SUGGESTED starting values, not measured: nobody has published how often a mapped ALPR is missing.
namespace CameraTrust {

constexpr double kPriorOsm = 2.2;          // OpenStreetMap / DeFlock: mapped by someone who saw it (≈ 0.90)
constexpr double kPriorCommunity = 1.0;    // a community report (flocklocations) without an OSM object (≈ 0.73)
constexpr double kPriorSuspected = -0.5;   // a "suspected" list entry (≈ 0.38)
constexpr double kPriorDetection = 0.0;    // our own RF detection: the RF term below carries it
constexpr double kPriorOther = 0.0;
constexpr double kPerYear = -0.25;         // per year since the OSM edit: cameras get moved, removed, contracts end
constexpr double kRfStrong = 1.5;          // Flock hardware detected by RF within 60 m at tier ≥ 2
constexpr double kRfWeak = 0.7;            // … at a lower tier
constexpr double kRfRadiusM = 60.0;
constexpr double kConfirmed = 1.0;         // the user says it is there
constexpr double kAbsent = -1.5;           // the user says it is not there

QString sourceClass(const QString &id, const QString &source, const QString &model);   // osm | community | suspected | detection | other
double  prior(const QString &sourceClass);
int     tierFromConfidence(int confidence);   // docs/DETECTION.md: confidence[tier] = 15 / 40 / 65 / 85 / 98

struct Inputs {
    QString id, source, model;
    QString osmTimestamp;        // ISO, "" unknown
    int     rfTier = -1;         // the best RF detection of Flock hardware within 60 m, -1 none
    double  rfDistanceM = std::numeric_limits<double>::quiet_NaN();
    QString rfWhat;              // what it was (a det: row, or this camera field-confirmed by RF)
    QString verdict;             // present | absent | "" none
    QString verdictAt;
    QDateTime now = QDateTime::currentDateTime();
};
struct Result {
    double logit = 0;
    double trust = 0.5;
    QJsonObject detail;          // {logit, trust, terms:[{term, value, note}], computed, weights:"suggested, not measured"}
};
Result compute(const Inputs &in);
inline double sigmoid(double x) { return 1.0 / (1.0 + std::exp(-x)); }

} // namespace CameraTrust

#include "poiclassify.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QtMath>
#include <cmath>

namespace PoiClassify {

Element Element::fromOverpass(const QJsonObject &el)
{
    Element e;
    e.type = el["type"].toString();
    e.id = qint64(el["id"].toDouble());
    const QJsonObject c = el["center"].toObject();
    e.lat = el.contains("lat") ? el["lat"].toDouble() : c["lat"].toDouble();
    e.lon = el.contains("lon") ? el["lon"].toDouble() : c["lon"].toDouble();
    e.tags = el["tags"].toObject();
    return e;
}

double distanceM(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0, p1 = qDegreesToRadians(lat1), p2 = qDegreesToRadians(lat2);
    const double dp = qDegreesToRadians(lat2 - lat1), dl = qDegreesToRadians(lon2 - lon1);
    const double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * R * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

void driveEstimate(double distM, int *driveS, int *driveM)
{
    const double road = std::max(0.0, distM) * 1.4;              // roads are not straight
    const double secs = road / (70.0 / 3.6);                     // 70 km/h
    if (driveM) *driveM = int(std::lround(road));
    if (driveS) *driveS = std::max(300, int(std::lround(secs / 300.0)) * 300);
}

QString address(const QJsonObject &t)
{
    QString street = t["addr:street"].toString(), hn = t["addr:housenumber"].toString(), unit = t["addr:unit"].toString();
    QString line1 = hn.isEmpty() ? street : (street.isEmpty() ? hn : hn + QLatin1Char(' ') + street);
    if (!unit.isEmpty() && !line1.isEmpty()) line1 += QStringLiteral(" #") + unit;
    QString city = t["addr:city"].toString(); if (city.isEmpty()) city = t["addr:town"].toString(); if (city.isEmpty()) city = t["addr:village"].toString();
    QString state = t["addr:state"].toString(); if (state.isEmpty()) state = t["addr:province"].toString();
    const QString post = t["addr:postcode"].toString();
    QStringList parts;
    if (!line1.isEmpty()) parts << line1;
    if (!city.isEmpty()) parts << city;
    QString tail = state; if (!post.isEmpty()) tail += (tail.isEmpty() ? QString() : QStringLiteral(" ")) + post;
    if (!tail.isEmpty()) parts << tail;
    if (parts.isEmpty() && !t["addr:full"].toString().isEmpty()) parts << t["addr:full"].toString();
    return parts.join(QStringLiteral(", "));
}

QString tierLabel(int peds)
{
    switch (peds) {
    case 1: return QStringLiteral("pediatric ER");
    case 2: return QStringLiteral("children's hospital — ER not confirmed");
    case 3: return QStringLiteral("general ER with a pediatrics dept.");
    case 4: return QStringLiteral("pediatric urgent care — not an ER");
    default: return {};
    }
}

QString baseCategory(const QJsonObject &t)
{
    const QString am = t["amenity"].toString(), shop = t["shop"].toString(), tour = t["tourism"].toString(), hw = t["highway"].toString();
    const QString le = t["leisure"].toString(), hc = t["healthcare"].toString(), off = t["office"].toString(), gov = t["government"].toString();
    const QString name = t["name"].toString().toLower();
    // Emergency & civic
    if (am == "police") return "police";
    if (am == "fire_station") return "fire";
    if (am == "hospital" || hc == "hospital") return "health";
    if (am == "clinic" || am == "doctors" || am == "urgent_care" || hc == "urgent_care" || hc == "clinic" || hc == "doctor") return "urgent";
    if (am == "pharmacy" || shop == "chemist" || hc == "pharmacy") return "pharmacy";
    if (am == "dentist" || hc == "dentist") return "dentist";
    if (am == "veterinary") return "vet";
    if (am == "library") return "library";
    if (am == "townhall") return "townhall";
    if (am == "courthouse") return "court";
    if (off == "government" && (gov == "transportation" || gov == "vehicle_registration" || gov == "driving_license" || name.contains("dmv") || name.contains("motor vehicle") || name.contains("driver")))
        return "dmv";
    if (am == "school" || am == "kindergarten") return "school";
    if (am == "community_centre") return "community";
    // Kids & fun
    if (le == "playground") return "playground";
    if (le == "dog_park") return "dogpark";
    if (le == "park" || le == "garden") return "park";
    if (le == "swimming_pool" || le == "swimming_area" || (le == "sports_centre" && t["sport"].toString().contains("swimming"))) return t["access"].toString() == "private" ? QString() : QStringLiteral("pool");
    if (le == "water_park" || t["playground"].toString() == "splash_pad" || t["playground:splash_pad"].toString() == "yes") return "splash";
    if (tour == "zoo" || tour == "aquarium") return "zoo";
    if (tour == "museum" || tour == "gallery") return "museum";
    if (tour == "theme_park") return "themepark";
    if (am == "ice_cream" || shop == "ice_cream") return "icecream";
    if (am == "cinema") return "cinema";
    if (le == "bowling_alley") return "bowling";
    if (le == "amusement_arcade") return "arcade";
    if (le == "trampoline_park" || (le == "sports_centre" && t["sport"].toString().contains("trampoline"))) return "trampoline";
    if (le == "skatepark" || (le == "pitch" && t["sport"].toString().contains("skateboard"))) return "skate";
    if (t["natural"].toString() == "beach" || le == "beach_resort") return "beach";
    if (tour == "picnic_site" || le == "picnic_table") return "picnic";
    if (hw == "trailhead" || le == "nature_reserve" || t["boundary"].toString() == "national_park") return "trail";
    // Services
    if (am == "fuel") return t["fuel:lpg"].toString() == "yes" && t["fuel:diesel"].toString() != "yes" && t["fuel:octane_87"].toString() != "yes" ? "propane" : "fuel";
    if (shop == "gas" || (shop == "bottled_gas")) return "propane";
    if (am == "charging_station") return "charging";
    if (shop == "supermarket" || shop == "convenience" || shop == "greengrocer" || shop == "wholesale") return "grocery";
    if (am == "restaurant" || am == "fast_food" || am == "food_court" || am == "pub") return "food";
    if (am == "cafe") return "cafe";
    if (tour == "camp_site" || tour == "caravan_site") return "camp";
    if (am == "sanitary_dump_station") return "dump";
    if (am == "drinking_water" || am == "water_point") return "water";
    if (am == "shower") return "shower";
    if (am == "toilets") return t["shower"].toString() == "yes" ? "shower" : "toilets";
    if (shop == "laundry" || am == "laundry") return "laundry";
    if (shop == "car_repair" || shop == "tyres" || shop == "car_parts" || am == "vehicle_inspection") return "repair";
    if (am == "car_wash") return "carwash";
    if (shop == "hardware" || shop == "doityourself" || shop == "outdoor" || shop == "trade") return "hardware";
    if (am == "post_office" || am == "parcel_locker") return "post";
    if (hw == "rest_area" || hw == "services") return "rest";
    const QString ia = t["internet_access"].toString();
    if (ia == "wlan" || ia == "yes") return "wifi";
    return {};
}

bool isUrgentCare(const QString &name, const QString &detail)
{
    static const QRegularExpression re(QStringLiteral("\\burgent\\b|express ?care|after.?hours|walk.?in|immediate ?care|convenient ?care|med ?express"),
                                       QRegularExpression::CaseInsensitiveOption);
    if (detail.contains(QLatin1String("urgent care"), Qt::CaseInsensitive)) return true;
    QString n = name; n.replace(QChar(0x2019), QLatin1Char('\''));
    return re.match(n).hasMatch();
}

static const QString kNotConfirmed = QStringLiteral("ER not confirmed — call ahead");
static const QString kLikely24 = QStringLiteral("likely ER (24/7) — call ahead");

Result classify(const QJsonObject &t)
{
    static const QSet<QString> pedTokens{QStringLiteral("paediatrics"), QStringLiteral("pediatrics"), QStringLiteral("paediatric"), QStringLiteral("pediatric"),
                                         QStringLiteral("paediatrician"), QStringLiteral("pediatrician"), QStringLiteral("specialist_pediatrician")};
    static const QRegularExpression pedNameRe(QStringLiteral("\\b(children'?s?|child|pa?ediatric\\w*|kids?)\\b"));
    static const QRegularExpression notErRe(QStringLiteral("rehab|behavio|psychiat|hospice|home\\b|dental|outpatient|specialty (care|center)|medical office|pavilion|therapy|surgery center|shriners"));
    static const QRegularExpression urgentRe(QStringLiteral("urgent|express care|after.?hours|walk.?in|immediate care"));

    const QString am = t["amenity"].toString(), hc = t["healthcare"].toString(), bld = t["building"].toString();
    QStringList tokens;
    for (const QString &s : t["healthcare:speciality"].toString().toLower().split(QLatin1Char(';'))) { const QString x = s.trimmed(); if (!x.isEmpty()) tokens << x; }
    bool pedSpec = t["health_specialty:paediatrics"].toString() == QLatin1String("yes") || t["emergency:paediatric"].toString() == QLatin1String("yes");
    bool dental = am == QLatin1String("dentist") || hc == QLatin1String("dentist");
    for (const QString &tok : tokens) {
        if (pedTokens.contains(tok)) pedSpec = true;
        else if ((tok.startsWith(QLatin1String("paediatric_")) || tok.startsWith(QLatin1String("pediatric_"))) && !tok.contains(QLatin1String("dent"))) pedSpec = true;
        if (tok.contains(QLatin1String("dent"))) dental = true;
    }
    QString nm = (t["name"].toString() + QLatin1Char(' ') + t["alt_name"].toString() + QLatin1Char(' ') + t["official_name"].toString()).toLower();
    nm.replace(QChar(0x2019), QLatin1Char('\''));
    if (nm.contains(QLatin1String("dent"))) dental = true;
    const bool pedName = pedNameRe.match(nm).hasMatch();
    const bool notEr = notErRe.match(nm).hasMatch();
    const bool isHosp = am == QLatin1String("hospital") || hc == QLatin1String("hospital");
    const bool erYes = t["emergency"].toString() == QLatin1String("yes") || t["emergency:paediatric"].toString() == QLatin1String("yes")
                    || tokens.contains(QLatin1String("emergency")) || tokens.contains(QLatin1String("paediatric_emergency")) || tokens.contains(QLatin1String("pediatric_emergency"));
    const bool erNo = t["emergency"].toString() == QLatin1String("no");
    const bool urgent = t["urgent_care"].toString() == QLatin1String("yes") || tokens.contains(QLatin1String("urgent")) || tokens.contains(QLatin1String("urgent_care")) || urgentRe.match(nm).hasMatch();
    const bool general = tokens.contains(QLatin1String("general"));
    const bool h24 = t["opening_hours"].toString().trimmed() == QLatin1String("24/7");

    Result r;
    r.phone = t["phone"].toString(); if (r.phone.isEmpty()) r.phone = t["contact:phone"].toString();
    r.address = address(t);
    r.hospitalEr = isHosp && erYes;
    auto generalHospital = [&] {                      // rule 5 for a hospital: the general ER view of it
        r.cat = QStringLiteral("health");
        r.er = erYes ? QStringLiteral("yes") : erNo ? QStringLiteral("no") : QString();
    };
    auto finish = [&]() -> Result {
        r.emergency = r.er == QLatin1String("yes") || (r.cat == QLatin1String("health") && h24 && r.er != QLatin1String("no"));
        if (r.cat == QLatin1String("health") && r.detail.isEmpty()) {
            if (r.emergency) r.detail = QStringLiteral("emergency dept.");
            else if (r.er == QLatin1String("no")) r.detail = QStringLiteral("no ER");
        }
        return r;
    };

    // 1. a hospital that is pediatric by speciality or by name (a general hospital with a pediatrics
    //    department is rule 2, unless its name says children's)
    if (isHosp && (pedSpec || pedName) && !(general && !pedName)) {
        if (erNo) { generalHospital(); r.detail = QStringLiteral("children's hospital · no ER"); return finish(); }
        if (notEr) { generalHospital(); return finish(); }
        r.cat = QStringLiteral("peds_er");
        if (erYes) { r.peds = 1; r.er = QStringLiteral("yes"); r.detail = QStringLiteral("pediatric ER"); return finish(); }
        r.peds = 2; r.detail = h24 ? kLikely24 : kNotConfirmed;
        return finish();
    }
    // 2. a general ER with a pediatrics department
    if (isHosp && pedSpec && general && !pedName && erYes) {
        r.cat = QStringLiteral("health"); r.peds = 3; r.er = QStringLiteral("yes"); r.detail = QStringLiteral("ER · pediatrics dept.");
        return finish();
    }
    // 3. a children's hospital mapped only as a building
    if (bld == QLatin1String("hospital") && !isHosp && pedName && !notEr) {
        r.cat = QStringLiteral("peds_er"); r.peds = 2; r.detail = h24 ? kLikely24 : kNotConfirmed;
        return finish();
    }
    // 4. pediatric urgent care (not an ER)
    const bool clinicLike = am == QLatin1String("clinic") || am == QLatin1String("doctors") || am == QLatin1String("urgent_care")
                         || hc == QLatin1String("clinic") || hc == QLatin1String("doctor") || hc == QLatin1String("urgent_care");
    if (clinicLike && (pedSpec || pedName) && urgent && !dental) {
        r.cat = QStringLiteral("peds_urgent"); r.peds = 4; r.detail = QStringLiteral("not an ER");
        return finish();
    }
    // 5. everything else
    r.cat = baseCategory(t);
    if (r.cat == QLatin1String("health")) generalHospital();
    return finish();
}

static QString normName(const QJsonObject &t)
{
    QString n = t["name"].toString().toLower();
    n.replace(QChar(0x2019), QLatin1Char('\''));
    return n.simplified();
}

static bool medical(const QString &cat)
{
    return cat == QLatin1String("peds_er") || cat == QLatin1String("peds_urgent") || cat == QLatin1String("health") || cat == QLatin1String("urgent");
}

QList<Result> classifyAll(const QList<Element> &els)
{
    QList<Result> out;
    out.reserve(els.size());
    for (const Element &e : els) out << classify(e.tags);
    // Campus: a children's hospital with no confirmed ER takes the nearest other ER hospital within 600 m
    for (int i = 0; i < els.size(); ++i) {
        Result &r = out[i];
        if (r.cat != QLatin1String("peds_er") || r.peds != 2 || r.er == QLatin1String("yes")) continue;
        int best = -1; double bd = 600.0;
        for (int j = 0; j < els.size(); ++j) {
            if (j == i || !out[j].hospitalEr || els[j].key() == els[i].key()) continue;
            const QString nm = els[j].tags["name"].toString();
            if (nm.isEmpty() || normName(els[j].tags) == normName(els[i].tags)) continue;   // its own twin is the dedupe's business
            const double d = distanceM(els[i].lat, els[i].lon, els[j].lat, els[j].lon);
            if (d <= bd) { bd = d; best = j; }
        }
        if (best >= 0) {
            r.campus = els[best].tags["name"].toString();
            r.detail = QStringLiteral("ER on campus: %1 — call ahead").arg(r.campus);
        }
    }
    // Dedupe: the same name within 500 m is one place (a hospital drawn twice: the site and the building).
    // The object tagged amenity/healthcare wins; it takes over a phone / address the other one had.
    auto score = [&](int i) {
        const QJsonObject &t = els[i].tags;
        return (t.contains("amenity") || t.contains("healthcare") ? 4 : 0) + (out[i].er == QLatin1String("yes") ? 2 : 0) + (out[i].phone.isEmpty() ? 0 : 1);
    };
    for (int i = 0; i < els.size(); ++i) {
        if (out[i].dropped || !medical(out[i].cat)) continue;
        const QString ni = normName(els[i].tags);
        if (ni.isEmpty()) continue;
        for (int j = i + 1; j < els.size(); ++j) {
            if (out[j].dropped || !medical(out[j].cat) || normName(els[j].tags) != ni) continue;
            if (distanceM(els[i].lat, els[i].lon, els[j].lat, els[j].lon) > 500.0) continue;
            const bool dropI = score(j) > score(i);
            const int keep = dropI ? j : i, drop = dropI ? i : j;
            out[drop].dropped = true;
            if (out[keep].phone.isEmpty()) out[keep].phone = out[drop].phone;
            if (out[keep].address.isEmpty()) out[keep].address = out[drop].address;
            if (dropI) break;
        }
    }
    return out;
}

int pedsRank(int peds, const QString &campus)
{
    if (peds == 1 || (peds == 2 && !campus.isEmpty())) return 0;
    if (peds == 2 || peds == 3) return 1;
    return -1;
}

HelpPicks pickHelp(const QList<HelpCandidate> &c)
{
    auto better = [&](int a, int b) {                 // a nearer than b (b may be -1)
        if (b < 0) return true;
        if (c[a].driveS != c[b].driveS) return c[a].driveS < c[b].driveS;
        return c[a].distM < c[b].distM;
    };
    HelpPicks p;
    int best0 = -1, best1 = -1, hospEr = -1, hospAny = -1;
    for (int i = 0; i < c.size(); ++i) {
        const HelpCandidate &h = c[i];
        const bool pedsSite = h.cat == QLatin1String("peds_er") || (h.cat == QLatin1String("health") && h.peds == 3);
        if (pedsSite) {
            const int rank = pedsRank(h.peds, h.campus);
            if (rank == 0 && better(i, best0)) best0 = i;
            else if (rank == 1 && better(i, best1)) best1 = i;
        }
        if (h.cat == QLatin1String("peds_urgent") && better(i, p.pediatricUrgent)) p.pediatricUrgent = i;
        if (h.cat == QLatin1String("health")) {
            if (h.emergency && (hospEr < 0 || h.distM < c[hospEr].distM)) hospEr = i;
            if (hospAny < 0 || h.distM < c[hospAny].distM) hospAny = i;
        }
    }
    p.pediatric = best0 >= 0 ? best0 : best1;
    if (best0 >= 0 && best1 >= 0 && c[best1].driveS < c[best0].driveS) p.pediatricCloser = best1;
    p.hospital = hospEr >= 0 ? hospEr : hospAny;
    return p;
}

} // namespace PoiClassify

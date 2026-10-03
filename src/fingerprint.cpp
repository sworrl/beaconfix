#include "fingerprint.h"
#include <algorithm>
#include <cmath>

namespace ScanMatch {

void addEpoch(Index &index, Epoch e)
{
    const int id = int(index.epochs.size());
    for (auto it = e.ap.constBegin(); it != e.ap.constEnd(); ++it) index.byAp[it.key()].append(id);
    index.epochs.append(std::move(e));
}

Result locate(const Index &index, const QHash<QString, int> &heard, const Options &opt)
{
    Result r;
    if (index.empty() || heard.size() < opt.minCommon) return r;
    QHash<int, int> shared;                                   // epoch → APs in common
    for (auto it = heard.constBegin(); it != heard.constEnd(); ++it)
        for (int e : index.byAp.value(it.key())) ++shared[e];
    struct Cand { double score; int epoch; int common; };
    QList<Cand> cands;
    std::vector<double> d;
    for (auto it = shared.constBegin(); it != shared.constEnd(); ++it) {
        if (it.value() < opt.minCommon) continue;
        const Epoch &E = index.epochs[it.key()];
        d.clear();
        for (auto h = heard.constBegin(); h != heard.constEnd(); ++h) {
            const auto f = E.ap.constFind(h.key());
            if (f != E.ap.constEnd()) d.push_back(double(h.value() - f.value()));
        }
        std::vector<double> s = d;
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        const double off = s[s.size() / 2];                  // the two radios' offset
        double ss = 0;
        for (double x : d) ss += (x - off) * (x - off);
        int miss = 0;                                         // strong on one side, absent on the other
        for (auto h = heard.constBegin(); h != heard.constEnd(); ++h) if (!E.ap.contains(h.key()) && h.value() > opt.strongDbm) ++miss;
        for (auto f = E.ap.constBegin(); f != E.ap.constEnd(); ++f) if (!heard.contains(f.key()) && f.value() + off > opt.strongDbm) ++miss;
        const double score = -0.5 * ss / (opt.sigmaDb * opt.sigmaDb * double(d.size())) - opt.missWeight * miss / double(heard.size())
                           + 0.5 * std::log(double(d.size()));
        cands.append({score, it.key(), int(d.size())});
    }
    if (cands.isEmpty()) return r;
    const int k = std::min<int>(opt.k, cands.size());
    std::partial_sort(cands.begin(), cands.begin() + k, cands.end(), [](const Cand &a, const Cand &b) { return a.score > b.score; });
    if (cands[0].score < opt.minScore) return r;
    // Softmax-weighted mean in a local metric frame around the best epoch
    const Epoch &b0 = index.epochs[cands[0].epoch];
    const double my = 110574.0, mx = 111320.0 * std::cos(b0.lat * M_PI / 180.0);
    double W = 0, x = 0, y = 0, accSq = 0;
    std::vector<double> w(k);
    for (int i = 0; i < k; ++i) {
        const Epoch &E = index.epochs[cands[i].epoch];
        w[i] = std::exp(cands[i].score - cands[0].score);
        W += w[i]; x += w[i] * (E.lon - b0.lon) * mx; y += w[i] * (E.lat - b0.lat) * my; accSq += w[i] * E.acc * E.acc;
    }
    x /= W; y /= W; accSq /= W;
    double spreadSq = 0;
    for (int i = 0; i < k; ++i) {
        const Epoch &E = index.epochs[cands[i].epoch];
        const double dx = (E.lon - b0.lon) * mx - x, dy = (E.lat - b0.lat) * my - y;
        spreadSq += w[i] * (dx * dx + dy * dy);
    }
    spreadSq /= W;
    r.valid = true;
    r.lat = b0.lat + y / my; r.lon = b0.lon + x / mx;
    r.spread = std::sqrt(spreadSq);
    r.acc = std::max(5.0, std::sqrt(spreadSq + accSq));
    r.score = cands[0].score; r.common = cands[0].common; r.used = k;
    return r;
}

} // namespace ScanMatch

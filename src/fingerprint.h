#pragma once
// Wi-Fi fingerprint positioning (docs/ESTIMATION.md "Locating ourselves"): where were we the last
// time the radio neighbourhood looked like this? Every scan a precisely-located device made (the
// phone with GPS, this host on its surveyed anchor) is an epoch — a position and the levels it
// heard. A new scan is scored against each epoch that shares ≥ 3 APs:
//
//   score = −½·mean((Δ_i − Δ̃)/σ)² − λ·(strong APs heard on one side only)/|H| + ½·ln(common)
//
// Δ_i the level difference on a shared AP and Δ̃ their median (two radios hear the same AP a few dB
// apart: the offset is removed, the shape is compared). The k best epochs are averaged with
// softmax(score) weights. This is RADAR/Horus-style nearest-neighbour matching: it needs no AP
// positions at all, so it is as good as the GPS the epochs were tagged with — on the user's data
// 11.8 m median against 160-525 m for the Wi-Fi geolocation services (2026-10-01, rural WV).
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

namespace ScanMatch {

struct Epoch { double lat = 0, lon = 0, acc = 0; QHash<QString, int> ap; };   // acc: the epoch's own fix (68 %, m)

struct Index {
    QList<Epoch> epochs;
    QHash<QString, QList<int>> byAp;      // BSSID → epochs that heard it
    int obsCount = 0;                     // observations it was built from (staleness)
    bool empty() const { return epochs.isEmpty(); }
};

struct Options {
    double sigmaDb = 8.0;                 // shape mismatch scale (shadowing + body + orientation)
    double strongDbm = -75;               // an AP this strong heard on one side only is evidence against
    double missWeight = 2.0;              // λ
    int    minCommon = 3;
    int    k = 7;
    double minScore = -3.0;               // best epoch worse than this: no fix
};

struct Result {
    bool valid = false;
    double lat = 0, lon = 0;
    double acc = 0;                       // 68 % radius (m): the top epochs' spread ⊕ their own fix error
    double spread = 0;                    // weighted RMS distance of the top epochs from the answer (m)
    double score = 0;                     // the best epoch's score
    int common = 0;                       // APs the best epoch shares with the scan
    int used = 0;                         // epochs averaged
};

// heard: BSSID → dBm of the scan (only the APs that may position: no home / travelling ones)
Result locate(const Index &index, const QHash<QString, int> &heard, const Options &opt = Options());
void addEpoch(Index &index, Epoch e);    // keeps byAp in step

} // namespace ScanMatch

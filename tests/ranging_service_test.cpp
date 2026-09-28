// Unit tests for the pure helpers of src/ranging/rangingservice.{h,cpp}: the calibration's RTT reduction and
// acceptance rule, and BLE levels from partly held samples. The service lives in the tray, so link the app's
// objects (everything but main.cpp.o) after a normal build — from the repository root:
//   OBJS=$(ls build/CMakeFiles/beaconfix.dir/src/*.o build/CMakeFiles/beaconfix.dir/src/ranging/*.o | grep -v main.cpp.o); MOC=build/CMakeFiles/beaconfix.dir/beaconfix_autogen/mocs_compilation.cpp.o
//   g++ -std=c++17 -O2 -Wall -Wextra -fPIC $(pkg-config --cflags Qt6Widgets Qt6DBus Qt6Network Qt6Sql) -Isrc tests/ranging_service_test.cpp $OBJS $MOC -o build/ranging_service_test $(pkg-config --libs Qt6Widgets Qt6DBus Qt6Network Qt6Sql) -lcrypto
//   ./build/ranging_service_test
#include "../src/ranging/rangingservice.h"
#include "../src/ranging/blelink.h"
#include <QCoreApplication>
#include <cmath>
#include <cstdio>

using namespace RangeMath;
static int fails = 0;
#define CHECK(cond, fmt, ...) do { if (!(cond)) { ++fails; std::printf("FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } else { std::printf("ok   " fmt "\n", ##__VA_ARGS__); } } while (0)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // The 0.6 m calibration of 2026-09-27: 8 bursts at 14.4–14.7 m, 7 at 196–408 m (bursts the responder mis-timed)
    const std::vector<double> d = {14.41, 196.2, 14.52, 14.47, 408.0, 14.66, 199.8, 14.58, 210.1, 14.44, 251.3, 14.70, 196.0, 14.49, 300.2};
    std::vector<double> sg;
    for (double x : d) sg.push_back(x > 100 ? 6.0 : 0.32);        // the outliers also report a huge spread
    double c = 0, lo = 0, hi = 0;
    const int n = RangingService::rttCluster(d, RangingService::kCalClusterM, &c, &lo, &hi);
    CHECK(n == 8, "densest 2 m cluster holds the 8 good bursts (got %d)", n);
    CHECK(std::fabs(c - 14.505) < 1e-9, "cluster median 14.505 m (got %.4f)", c);
    CHECK(lo == 14.41 && hi == 14.70, "inlier range [14.41, 14.70] (got [%.2f, %.2f])", lo, hi);
    const double sb = RangingService::rttInlierSigma(d, sg, lo, hi);
    CHECK(std::fabs(sb - 0.32) < 1e-12, "burst σ from the inliers only: 0.32 m, not the outliers' 6 m (got %.3f)", sb);
    // Inliers that scatter more than they claim: the scatter wins
    const std::vector<double> d2 = {10.0, 10.9, 11.8, 10.4, 11.3};
    const std::vector<double> s2(5, 0.3);
    const double sb2 = RangingService::rttInlierSigma(d2, s2, 10.0, 11.8);
    CHECK(sb2 > 0.7 && sb2 < 0.8, "scatter of the inliers (0.71 m) beats their reported 0.3 m (got %.3f)", sb2);
    CHECK(RangingService::rttInlierSigma({}, {}, 0, 1) == kRttFloor, "no inliers → the RTT floor");

    // Acceptance: 3+ bursts that disagree reject the whole window; fewer bursts leave BLE to calibrate alone
    CHECK(RangingService::rttClusterOk(15, 8), "8 of 15 bursts agree → RTT offset calibrated");
    CHECK(!RangingService::rttClusterOk(15, 4), "4 of 15 (< 30 %%) → not calibrated");
    CHECK(!RangingService::rttClusterOk(2, 2), "2 bursts are too few");
    CHECK(RangingService::calibrationFailure(15, 8, false).isEmpty(), "a good RTT cluster calibrates even without BLE");
    CHECK(!RangingService::calibrationFailure(15, 4, true).isEmpty(), "disagreeing bursts fail the calibration even with BLE: %s", qPrintable(RangingService::calibrationFailure(15, 4, true)));
    CHECK(!RangingService::calibrationFailure(3, 1, true).isEmpty(), "3 bursts, none agreeing: fail");
    CHECK(RangingService::calibrationFailure(0, 0, true).isEmpty(), "no bursts (phone in Doze) + BLE → a BLE-only calibration");
    CHECK(RangingService::calibrationFailure(2, 2, true).isEmpty(), "2 bursts + BLE → BLE-only (offset unchanged)");
    CHECK(!RangingService::calibrationFailure(0, 0, false).isEmpty(), "nothing at all → fail");

    // Held samples (BleLink repeats an unchanged RSSI once a second) weight the level but carry no information
    const std::vector<double> r = {-80, -80, -80, -80, -80, -80, -80, -81, -80, -80};
    const Level all = levelFromSamples(r, 3, 10, false);
    const Level two = RangingService::freshLevel(r, 2, 10, false);
    CHECK(std::fabs(two.dbm - all.dbm) < 1e-12, "same level value with or without held repeats (%.3f dBm)", two.dbm);
    CHECK(std::fabs(two.nEff - 2.0) < 1e-12 && std::fabs(two.sigma - kDbPerNeper / std::sqrt(2.0)) < 1e-12, "2 fresh samples → N_eff 2, σ 3.07 dB (got %.2f, %.2f)", two.nEff, two.sigma);
    CHECK(!RangingService::freshLevel(r, 0, 10, false).valid, "only held repeats → no update");
    const Level ten = RangingService::freshLevel(r, 10, 10, false);
    CHECK(ten.nEff == all.nEff && ten.sigma == all.sigma, "all fresh → exactly levelFromSamples (N_eff %.2f)", ten.nEff);

    // A drifted RTT offset: calibrated 13.767 m (0.6 m read 14.37 m); later bursts at 7.1–12.9 m put the range below zero
    std::vector<double> implied;
    for (double b : {10.9, 11.4, 7.1, 12.9, 10.2, 11.8, 9.6, 10.9, 12.1}) implied.push_back(b - 13.767);
    double med = 0;
    CHECK(!RangingService::rttOffsetStale(implied, 0.1, &med), "9 bursts are not enough to judge the offset");
    implied.push_back(10.5 - 13.767);
    CHECK(RangingService::rttOffsetStale(implied, 0.1, &med), "10 bursts, median %.2f m below zero → offset out of date", med);
    CHECK(std::fabs(med - (10.9 - 13.767)) < 1e-9, "median range %.3f m", med);
    // A good offset at contact distance: bursts scatter around 0 m (some negative) — not stale
    std::vector<double> touching;
    for (double b : {-0.4, 0.3, -0.2, 0.1, 0.6, -0.5, 0.2, 0.0, -0.1, 0.4, -0.3, 0.2}) touching.push_back(b);
    CHECK(!RangingService::rttOffsetStale(touching, 0.1), "devices touching, noisy bursts around 0 m → still calibrated");
    // A loosely known offset (σ 0.5 m) needs a median below −1.5 m
    std::vector<double> loose(12, -1.2);
    CHECK(!RangingService::rttOffsetStale(loose, 0.5), "−1.2 m with σ_offset 0.5 m is within 3σ");
    CHECK(RangingService::rttOffsetStale(loose, 0.1), "−1.2 m with σ_offset 0.1 m is not");

    // A stale offset: the BLE models learnt under it go back to the calibration's, and the interval stays wide
    {
        // the 0.6 m calibration: down −78.8 dBm, up −93.9 dBm
        Rls2 down(priorP0Ble(7), kNBle), up(priorP0Ble(-7), kNBle);
        down.update(std::log10(0.6), -78.8, 1.0); up.update(std::log10(0.6), -93.9, 1.0);
        const Rls2 downCal = down, upCal = up;
        // then learnt while a drifted RTT offset held the range at 0.15–0.3 m and the levels read −60 / −75 dBm
        for (int i = 0; i < 300; ++i) {
            const double u = std::log10(i % 2 ? 0.15 : 0.3);
            down.update(u, -60.0, 4.0); up.update(u, -75.0, 4.0);
        }
        CHECK(std::fabs(down.p0 - downCal.p0) > 3.0 && std::fabs(up.p0 - upCal.p0) > 3.0, "the biased learning moved both P0s (%.1f → %.1f, %.1f → %.1f dBm)",
              downCal.p0, down.p0, upCal.p0, up.p0);
        auto tenMinutes = [&](RangeFilter &f) { for (int i = 0; i < 60; ++i) { f.predict(10, false); f.updateRssi(0, -70.0, 2.0, down.p0, down.n); f.updateRssi(1, -85.0, 2.0, up.p0, up.n); } };
        // before: only P_uu was floored, the offsets stayed as tight as the learning had made them
        RangeFilter before(2, std::log10(0.2), 1e-4, 2.0 * kFrozenFadeVar, 4.0);
        before.P[0][0] = std::max(before.P[0][0], 0.25);
        tenMinutes(before);
        RangeFilter f(2, std::log10(0.2), 1e-4, 2.0 * kFrozenFadeVar, 4.0);
        RangingService::forgetSuspectLearning(f, down, up, &downCal, &upCal);
        CHECK(down.p0 == downCal.p0 && down.n == downCal.n && up.p0 == upCal.p0 && up.n == upCal.n
              && down.S[0][0] == downCal.S[0][0] && up.S[1][1] == upCal.S[1][1], "stale → both models back to the calibration's (P0 %.1f / %.1f dBm)", down.p0, up.p0);
        CHECK(f.offsetVar(0) == kOffsetVar0 && f.offsetVar(1) == kOffsetVar0 && f.puu() >= 0.25, "stale → BLE offsets at the prior (σ %.1f dB), P_uu ≥ 0.25", std::sqrt(kOffsetVar0));
        tenMinutes(f);
        CHECK(std::sqrt(f.puu()) > 0.25 && std::sqrt(f.puu()) > 1.5 * std::sqrt(before.puu()),
              "10 min of BLE later the range is still ×/÷ %.2f (only the P_uu floor: ×/÷ %.2f)", std::pow(10.0, std::sqrt(f.puu())), std::pow(10.0, std::sqrt(before.puu())));
        // a ranging.json from before the snapshot: the models cannot be restored, the widening still applies
        Rls2 d2 = down, u2 = up; d2.p0 = -60;
        RangeFilter g(2, std::log10(0.2), 1e-4, 2.0 * kFrozenFadeVar, 4.0);
        RangingService::forgetSuspectLearning(g, d2, u2, nullptr, nullptr);
        CHECK(d2.p0 == -60 && g.offsetVar(0) == kOffsetVar0 && g.offsetVar(1) == kOffsetVar0, "no snapshot → models kept, offsets widened");
    }

    // BLE advert registration errors: only a wrong shape drops the TX-power AD / TxPower; the rest retry
    CHECK(BleLink::shapeError(QStringLiteral("org.bluez.Error.InvalidLength"), QStringLiteral("Advertising data too long")), "too long → shape");
    CHECK(BleLink::shapeError(QStringLiteral("org.bluez.Error.Failed"), QStringLiteral("Failed to parse advertisement.")), "cannot parse (TxPower refused) → shape");
    CHECK(BleLink::shapeError(QStringLiteral("org.bluez.Error.InvalidArguments"), QStringLiteral("Invalid arguments in method call")), "invalid arguments → shape");
    CHECK(!BleLink::shapeError(QStringLiteral("org.freedesktop.DBus.Error.NoReply"), QStringLiteral("Did not receive a reply. Possible causes include: the remote application did not send a reply")),
          "NoReply (bluetoothd busy) → transient, keep TX power");
    CHECK(!BleLink::shapeError(QStringLiteral("org.bluez.Error.AlreadyExists"), QStringLiteral("Already Exists")), "AlreadyExists → transient: unregister and retry");
    CHECK(!BleLink::shapeError(QStringLiteral("org.bluez.Error.NotPermitted"), QStringLiteral("Maximum advertisements reached")), "no free instance → transient");
    CHECK(!BleLink::shapeError(QStringLiteral("org.bluez.Error.Failed"), QStringLiteral("Failed to register advertisement")), "plain Failed → transient");

    std::printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

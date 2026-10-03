// SPDX-License-Identifier: Apache-2.0
// Unit tests for the lossless image pipeline (docs/SIGHTINGS.md §3.1): a JPEG recompressed to JPEG XL comes back
// bit for bit through djxl; a PNG is stored as the smaller lossless JXL / WebP with identical pixels; without cjxl,
// WebP lossless.
#include "../src/imagestore.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QImageWriter>
#include <QPainter>
#include <QRandomGenerator>
#include <cstdio>

static int fails = 0;
#define CHECK(cond, fmt, ...) do { \
    if (!(cond)) { ++fails; std::printf("FAIL %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); } \
    else std::printf("ok   " fmt "\n", ##__VA_ARGS__); \
} while (0)

static QImage scene(bool alpha)
{
    QImage img(320, 200, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    QRandomGenerator rng(42);
    for (int y = 0; y < img.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const int n = int(rng.bounded(24));
            line[x] = qRgba((x * 255 / img.width() + n) & 255, (y * 255 / img.height()) & 255, (128 + n * 3) & 255, alpha ? ((x + y) % 256) : 255);
        }
    }
    QPainter p(&img);                                  // shapes, no text: fonts need a QGuiApplication
    p.fillRect(QRect(10, 10, 120, 40), Qt::white);
    p.setPen(QPen(Qt::black, 3));
    p.drawEllipse(QRect(150, 60, 100, 100));
    return img;
}

static QByteArray encode(const QImage &img, const char *fmt, int quality = -1)
{
    QByteArray out; QBuffer b(&out); b.open(QIODevice::WriteOnly);
    QImageWriter w(&b, fmt);
    if (quality >= 0) w.setQuality(quality);
    w.write(img);
    return out;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);                  // image plugins (jpeg, webp, kimg_jxl)
    const bool jxl = ImageStore::haveCjxl() && ImageStore::haveDjxl();
    // ── JPEG → JPEG XL lossless recompression, bit-exact back through djxl ──
    const QByteArray jpeg = encode(scene(false), "jpeg", 85);
    CHECK(ImageStore::sniffMime(jpeg) == "image/jpeg", "sniff JPEG");
    const ImageStore::Encoded j = ImageStore::encodeLossless(jpeg);
    if (jxl) {
        CHECK(j.ok() && j.mime == "image/jxl" && j.jpegReconstructible, "JPEG stored as JPEG XL (reconstructible): %s", qPrintable(j.mime + j.error));
        CHECK(j.data.size() < jpeg.size(), "smaller: %lld → %lld bytes", (long long)jpeg.size(), (long long)j.data.size());
        CHECK(j.width == 320 && j.height == 200 && j.originalBytes == jpeg.size() && j.originalMime == "image/jpeg" && j.originalSha256.size() == 64, "dimensions and original metadata");
        QString mime;
        const QByteArray back = ImageStore::displayBytes(j.data, j.mime, j.jpegReconstructible, &mime);
        CHECK(mime == "image/jpeg" && back == jpeg, "?as=display gives the original JPEG back bit for bit (%lld bytes)", (long long)back.size());
        CHECK(!ImageStore::decode(j.data, j.mime).isNull(), "the stored JXL decodes (Qt jxl plugin or djxl)");
    } else {
        std::printf("skip cjxl/djxl not installed: JPEG recompression not tested\n");
        CHECK(j.ok() && j.mime == "image/webp", "without cjxl: WebP lossless");
    }
    CHECK(ImageStore::mediaUid(j.data).size() == 32, "media uid = 32 hex");
    // ── PNG: the smaller lossless of JXL / WebP, identical pixels ──
    for (bool alpha : {false, true}) {
        const QImage src = scene(alpha);
        const QByteArray png = encode(src, "png");
        const ImageStore::Encoded p = ImageStore::encodeLossless(png);
        CHECK(p.ok() && (p.mime == "image/jxl" || p.mime == "image/webp") && !p.jpegReconstructible, "PNG%s stored as %s (%lld → %lld bytes)",
              alpha ? " with alpha" : "", qPrintable(p.mime), (long long)png.size(), (long long)p.data.size());
        CHECK(ImageStore::samePixels(ImageStore::decode(p.data, p.mime), src), "PNG%s: decoded pixels equal the input's", alpha ? " with alpha" : "");
        QString mime;
        const QByteArray disp = ImageStore::displayBytes(p.data, p.mime, false, &mime);
        CHECK((mime == "image/png" || mime == "image/webp") && !disp.isEmpty(), "?as=display of a non-JPEG image: %s", qPrintable(mime));
    }
    // ── WebP lossless in (the phone's dash-cam frames) ──
    {
        const QImage src = scene(false);
        const QByteArray webp = encode(src, "webp", 100);
        const ImageStore::Encoded w = ImageStore::encodeLossless(webp);
        CHECK(w.ok() && w.originalMime == "image/webp" && ImageStore::samePixels(ImageStore::decode(w.data, w.mime), src), "WebP lossless in → %s, same pixels", qPrintable(w.mime));
    }
    // ── no cjxl: WebP lossless ──
    qputenv("BEACONFIX_CJXL", "none");
    {
        const ImageStore::Encoded p = ImageStore::encodeLossless(encode(scene(false), "png"));
        CHECK(p.ok() && p.mime == "image/webp" && ImageStore::samePixels(ImageStore::decode(p.data, p.mime), scene(false)), "without cjxl: WebP lossless, same pixels");
    }
    CHECK(!ImageStore::encodeLossless(QByteArray("not an image")).ok(), "junk is refused");
    std::printf("%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

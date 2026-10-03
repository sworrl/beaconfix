// SPDX-License-Identifier: Apache-2.0
#include "imagestore.h"
#include <QBuffer>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace ImageStore {

static QString tool(const char *name)
{
    const QString env = qEnvironmentVariable(QByteArray("BEACONFIX_") + QByteArray(name).toUpper());   // BEACONFIX_CJXL=/path (or "none" in tests)
    if (env == QLatin1String("none")) return {};
    if (!env.isEmpty()) return env;
    return QStandardPaths::findExecutable(QLatin1String(name));
}
bool haveCjxl() { return !tool("cjxl").isEmpty(); }
bool haveDjxl() { return !tool("djxl").isEmpty(); }

static bool run(const QString &exe, const QStringList &args, QString *err)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(exe, args);
    if (!p.waitForStarted(10000)) { if (err) *err = QStringLiteral("%1 did not start").arg(exe); return false; }
    if (!p.waitForFinished(180000)) { p.kill(); p.waitForFinished(2000); if (err) *err = QStringLiteral("%1 timed out").arg(exe); return false; }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (err) *err = QStringLiteral("%1 failed: %2").arg(exe, QString::fromLocal8Bit(p.readAll()).trimmed().right(200));
        return false;
    }
    return true;
}

static bool writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}
static QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QString sniffMime(const QByteArray &b)
{
    if (b.startsWith("\xFF\xD8\xFF")) return QStringLiteral("image/jpeg");
    if (b.startsWith("\x89PNG\r\n\x1A\n")) return QStringLiteral("image/png");
    if (b.size() >= 12 && b.startsWith("RIFF") && b.mid(8, 4) == "WEBP") return QStringLiteral("image/webp");
    if (b.startsWith("\xFF\x0A") || b.startsWith(QByteArray("\x00\x00\x00\x0CJXL \x0D\x0A\x87\x0A", 12))) return QStringLiteral("image/jxl");
    if (b.startsWith("GIF87a") || b.startsWith("GIF89a")) return QStringLiteral("image/gif");
    if (b.startsWith("BM")) return QStringLiteral("image/bmp");
    if (b.size() >= 12 && b.mid(4, 4) == "ftyp" && (b.mid(8, 4) == "avif" || b.mid(8, 4) == "heic")) return QStringLiteral("image/") + QString::fromLatin1(b.mid(8, 4));
    return {};
}

QString mediaUid(const QByteArray &stored)
{
    return QString::fromLatin1(QCryptographicHash::hash(stored, QCryptographicHash::Sha256).toHex().left(32));
}

static QImage normalised(const QImage &in)
{
    QImage i = in;
    i.setColorSpace(QColorSpace());
    return i.convertToFormat(i.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32);
}

bool samePixels(const QImage &a0, const QImage &b0)
{
    if (a0.isNull() || b0.isNull() || a0.size() != b0.size()) return false;
    const bool alpha = a0.hasAlphaChannel() || b0.hasAlphaChannel();
    const QImage a = a0.convertToFormat(alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    const QImage b = b0.convertToFormat(alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    for (int y = 0; y < a.height(); ++y) {
        const QRgb *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            if (pa[x] == pb[x]) continue;
            if (alpha && qAlpha(pa[x]) == 0 && qAlpha(pb[x]) == 0) continue;   // fully transparent: the colour is not part of the image
            if (!alpha && (pa[x] & 0xFFFFFF) == (pb[x] & 0xFFFFFF)) continue;
            return false;
        }
    }
    return true;
}

static QImage readImage(const QByteArray &bytes, const char *format = nullptr)
{
    QBuffer buf;
    buf.setData(bytes);
    buf.open(QIODevice::ReadOnly);
    QImageReader r(&buf, format);
    r.setAutoTransform(false);                    // the stored pixels, as they are
    r.setAllocationLimit(1024);                   // MB: a big phone frame is fine, a decompression bomb is not
    return r.read();
}

static QImage decodeJxl(const QByteArray &jxl)
{
    QImage img = readImage(jxl, "jxl");
    if (!img.isNull() || !haveDjxl()) return img;
    QTemporaryDir dir;
    if (!dir.isValid()) return {};
    if (!writeFile(dir.filePath(QStringLiteral("i.jxl")), jxl)) return {};
    if (!run(tool("djxl"), {dir.filePath(QStringLiteral("i.jxl")), dir.filePath(QStringLiteral("o.png"))}, nullptr)) return {};
    return readImage(readFile(dir.filePath(QStringLiteral("o.png"))), "png");
}

QImage decode(const QByteArray &stored, const QString &mime)
{
    if (mime == QLatin1String("image/jxl") || sniffMime(stored) == QLatin1String("image/jxl")) return decodeJxl(stored);
    return readImage(stored);
}

Encoded encodeLossless(const QByteArray &bytes)
{
    Encoded e;
    e.originalMime = sniffMime(bytes);
    e.originalBytes = bytes.size();
    e.originalSha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    if (bytes.isEmpty()) { e.error = QStringLiteral("empty image"); return e; }
    QTemporaryDir dir;
    const bool cjxl = haveCjxl() && dir.isValid();
    // 1. JPEG: lossless recompression, verified by a bit-exact round trip
    if (e.originalMime == QLatin1String("image/jpeg") && cjxl && haveDjxl()) {
        const QString in = dir.filePath(QStringLiteral("in.jpg")), out = dir.filePath(QStringLiteral("out.jxl")), back = dir.filePath(QStringLiteral("back.jpg"));
        QString err;
        if (writeFile(in, bytes) && run(tool("cjxl"), {in, out, QStringLiteral("--lossless_jpeg=1"), QStringLiteral("-e"), QStringLiteral("7")}, &err)
            && run(tool("djxl"), {out, back}, &err) && readFile(back) == bytes) {
            e.data = readFile(out);
            e.mime = QStringLiteral("image/jxl");
            e.jpegReconstructible = true;
            const QImage probe = readImage(bytes, "jpeg");
            e.width = probe.width(); e.height = probe.height();
            if (e.data.size() < bytes.size() * 2) return e;      // (always: ~20 % smaller) — otherwise fall through to the pixels
            e.data.clear();
        }
    }
    // 2. pixels: the smaller lossless of JXL / WebP whose decoded pixels equal the input's
    const QImage src0 = readImage(bytes);
    if (src0.isNull()) { e.error = QStringLiteral("not a readable image (%1)").arg(e.originalMime.isEmpty() ? QStringLiteral("unknown type") : e.originalMime); return e; }
    const QImage src = normalised(src0);
    e.width = src.width(); e.height = src.height();
    QByteArray best; QString bestMime;
    {   // WebP lossless (Qt's writer: quality 100 = lossless)
        QByteArray webp; QBuffer b(&webp); b.open(QIODevice::WriteOnly);
        QImageWriter w(&b, "webp");
        w.setQuality(100);
        if (w.write(src) && samePixels(readImage(webp, "webp"), src)) { best = webp; bestMime = QStringLiteral("image/webp"); }
    }
    if (cjxl) {
        const QString in = dir.filePath(QStringLiteral("px.png")), out = dir.filePath(QStringLiteral("px.jxl"));
        QByteArray png; QBuffer b(&png); b.open(QIODevice::WriteOnly);
        QImage forPng = src.hasAlphaChannel() ? src : src.convertToFormat(QImage::Format_RGB888);
        if (QImageWriter(&b, "png").write(forPng) && writeFile(in, png)
            && run(tool("cjxl"), {in, out, QStringLiteral("-d"), QStringLiteral("0"), QStringLiteral("-e"), QStringLiteral("7")}, nullptr)) {
            const QByteArray jxl = readFile(out);
            if (!jxl.isEmpty() && (best.isEmpty() || jxl.size() < best.size()) && samePixels(decodeJxl(jxl), src)) { best = jxl; bestMime = QStringLiteral("image/jxl"); }
        }
    }
    if (best.isEmpty()) { e.error = QStringLiteral("no lossless encoder produced the same pixels"); return e; }
    e.data = best; e.mime = bestMime; e.jpegReconstructible = false;
    return e;
}

QByteArray displayBytes(const QByteArray &stored, const QString &mime, bool jpegReconstructible, QString *outMime)
{
    if (mime == QLatin1String("image/jxl")) {
        if (jpegReconstructible && haveDjxl()) {
            QTemporaryDir dir;
            if (dir.isValid() && writeFile(dir.filePath(QStringLiteral("i.jxl")), stored)
                && run(tool("djxl"), {dir.filePath(QStringLiteral("i.jxl")), dir.filePath(QStringLiteral("o.jpg"))}, nullptr)) {
                const QByteArray jpg = readFile(dir.filePath(QStringLiteral("o.jpg")));
                if (!jpg.isEmpty()) { *outMime = QStringLiteral("image/jpeg"); return jpg; }
            }
        }
        const QImage img = decodeJxl(stored);
        if (img.isNull()) { outMime->clear(); return {}; }
        QByteArray png; QBuffer b(&png); b.open(QIODevice::WriteOnly);
        QImageWriter(&b, "png").write(img);
        *outMime = QStringLiteral("image/png");
        return png;
    }
    *outMime = mime;
    return stored;
}

} // namespace ImageStore

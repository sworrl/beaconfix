// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>

// Lossless, smallest image storage for plate-event media (docs/SIGHTINGS.md §3.1).
//  * JPEG in: JPEG XL lossless JPEG recompression (cjxl --lossless_jpeg=1 -e 7), kept only when djxl gives the
//    original file back bit for bit.
//  * anything else: decoded, then both JPEG XL lossless (cjxl -d 0 -e 7) and WebP lossless (Qt's webp writer at
//    quality 100); the smaller one whose decoded pixels equal the input's. Never a lossy result.
// cjxl / djxl (libjxl ≥ 0.7) are runtime tools: without them everything goes WebP lossless.
// Thread-safe (no shared state); runs external processes synchronously — call it off the GUI thread.
namespace ImageStore {

struct Encoded {
    QByteArray data;
    QString mime;                 // image/jxl | image/webp
    int width = 0, height = 0;
    bool jpegReconstructible = false;
    QString originalMime;
    qint64 originalBytes = 0;
    QString originalSha256;
    QString error;
    bool ok() const { return !data.isEmpty(); }
};

bool haveCjxl();
bool haveDjxl();
QString sniffMime(const QByteArray &bytes);     // image/jpeg, image/png, image/webp, image/jxl, image/gif, … or ""
Encoded encodeLossless(const QByteArray &bytes);
// ?as=display: the reconstructed JPEG for a JPEG-recompressed JXL, a PNG for any other JXL (Qt's jxl plugin, else
// djxl), the stored bytes for WebP
QByteArray displayBytes(const QByteArray &stored, const QString &mime, bool jpegReconstructible, QString *outMime);
QImage decode(const QByteArray &stored, const QString &mime);
QString mediaUid(const QByteArray &stored);     // first 32 hex of the SHA-256 of the stored bytes
bool samePixels(const QImage &a, const QImage &b);

} // namespace ImageStore

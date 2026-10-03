#pragma once
// The BeaconFix icon, compiled in from data/icons/hicolor (the same files CMake installs into the
// hicolor theme): the low-detail master at 16–32 px, medium at 48–128 px, high at 256 px.
#include <QIcon>
#include <QString>

inline QString appIconPath(int size)
{
    return QStringLiteral(":/icons/hicolor/%1x%1/apps/beaconfix.png").arg(size);
}

inline QIcon appIcon()
{
    static const QIcon icon = [] {
        QIcon i;
        for (int s : {16, 22, 24, 32, 48, 64, 128, 256}) i.addFile(appIconPath(s), QSize(s, s));
        return i;
    }();
    return icon;
}

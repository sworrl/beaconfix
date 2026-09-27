cmake_minimum_required(VERSION 3.16)
project(beaconfix VERSION 3.8.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network DBus Sql)
find_package(OpenSSL REQUIRED)

add_executable(beaconfix
    src/main.cpp
    src/wifiscanner.cpp
    src/locator.cpp
    src/poiclassify.cpp
    src/beaconview.cpp
    src/tilesource.cpp
    src/apiserver.cpp
    src/mdns.cpp
    src/pairing.cpp
    src/notify.cpp
    src/pairdialog.cpp
    src/importers.cpp
    src/mapdb.cpp
    src/estimator.cpp
    src/identity.cpp
    src/osintegration.cpp
    src/tray.cpp
    src/mainwindow.cpp
    src/anchordialog.cpp
    src/ranging/rangemath.cpp
    src/ranging/anchors.cpp
    src/ranging/blelink.cpp
    src/ranging/rangingservice.cpp
)
target_compile_definitions(beaconfix PRIVATE BEACONFIX_VERSION="${PROJECT_VERSION}")
target_link_libraries(beaconfix PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::DBus Qt6::Sql OpenSSL::Crypto)

# ── Install ──────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
option(BEACONFIX_INSTALL_WIDGET "Install the Plasma widget system-wide (share/plasma/plasmoids)" ON)

install(TARGETS beaconfix RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/beaconfix.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/beaconfix.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
# The D-Bus activation file and the autostart entry need the absolute path of the binary
configure_file(data/org.sworrl.BeaconFix.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.sworrl.BeaconFix.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.sworrl.BeaconFix.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
configure_file(data/beaconfix-tray.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/beaconfix-tray.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/beaconfix-tray.desktop
        DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart)
if(BEACONFIX_INSTALL_WIDGET)
    install(DIRECTORY plasmoid/org.kde.plasma.beaconfix
            DESTINATION ${CMAKE_INSTALL_DATADIR}/plasma/plasmoids)
endif()
# OS integration: the root helper (GeoClue static source) with its polkit action, and the rules
# file that lets active admins set the time zone / publish the position without a prompt.
option(BEACONFIX_INSTALL_POLKIT "Install the beaconfix-osd helper, its polkit action and the prompt-free rules" ON)
if(BEACONFIX_INSTALL_POLKIT)
    set(BEACONFIX_OSD_PATH "${CMAKE_INSTALL_FULL_LIBEXECDIR}/beaconfix-osd")
    configure_file(data/org.sworrl.beaconfix.policy.in ${CMAKE_CURRENT_BINARY_DIR}/org.sworrl.beaconfix.policy @ONLY)
    install(PROGRAMS data/beaconfix-osd DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.sworrl.beaconfix.policy DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
    install(FILES data/50-beaconfix.rules DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/polkit-1/rules.d)
endif()

# Wi-Fi RTT responder (docs/RANGING.md §6): opt-in, run once as root: share/beaconfix/setup-rtt-responder.sh
install(PROGRAMS data/setup-rtt-responder.sh DESTINATION ${CMAKE_INSTALL_DATADIR}/beaconfix)
install(FILES data/beaconfix-rtt-responder.conf.in data/beaconfix-rtt-responder.service data/90-beaconfix-rtt.nm.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/beaconfix)

# ── Packaging: cpack -G DEB (or RPM) in the build directory ─────────────────
set(CPACK_PACKAGE_NAME "beaconfix")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "sworrl")
set(CPACK_PACKAGE_CONTACT "sworrl <github@falcontechnix.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Where am I? Wi-Fi/GPS desktop locator for KDE Plasma with a live beacon map")
set(CPACK_PACKAGE_DESCRIPTION "Position fix from the Starlink dish, your own map database, BeaconDB, Apple's Wi-Fi positioning or IP; a live map of the beacons around you; trip log; a secured LAN API; an encrypted personal Wi-Fi map database; Plasma widget included.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/sworrl/beaconfix")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
set(CPACK_STRIP_FILES ON)
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "sworrl <github@falcontechnix.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6sql6-sqlite, network-manager")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "plasma-workspace, kf6-kpackage, avahi-utils, polkitd, qrencode, geoclue-2.0")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "grpcurl")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-2.0-or-later")
set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtbase-sqlite, NetworkManager")
include(CPack)

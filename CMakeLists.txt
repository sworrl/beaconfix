cmake_minimum_required(VERSION 3.16)
project(beaconfix VERSION 3.10.0 LANGUAGES CXX)

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
    src/linking.cpp
    src/linkdialog.cpp
    src/third_party/qrcodegen.cpp
    src/notify.cpp
    src/importers.cpp
    src/mapdb.cpp
    src/flockdetector.cpp
    src/cameraimport.cpp
    src/estimator.cpp
    src/tracksmoother.cpp
    src/fingerprint.cpp
    src/identity.cpp
    src/osintegration.cpp
    src/tray.cpp
    src/mainwindow.cpp
    src/anchordialog.cpp
    src/ranging/rangemath.cpp
    src/ranging/anchors.cpp
    src/ranging/blelink.cpp
    src/ranging/rangingservice.cpp
    src/webdashboard.cpp
    src/telegrambot.cpp
    src/securechannel.cpp
    src/hub.cpp
    src/hubclient.cpp
    src/hubmain.cpp
    src/plateevents.cpp
    src/hibf.cpp
    src/imagestore.cpp
    src/platewatch.cpp
    src/sightingsview.cpp
    src/roadsnap.cpp
    src/cameratrust.cpp
    src/eyesonflock.cpp
    src/avoidroute.cpp
    src/routeplanner.cpp
)
target_compile_definitions(beaconfix PRIVATE BEACONFIX_VERSION="${PROJECT_VERSION}")
target_link_libraries(beaconfix PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::DBus Qt6::Sql OpenSSL::Crypto)
# The app icon (data/icons/hicolor, rendered from the low/medium/high masters) compiled in for the window, tray and web dashboard
qt_add_resources(beaconfix "icons" PREFIX "/" BASE data FILES
    data/icons/hicolor/16x16/apps/beaconfix.png data/icons/hicolor/22x22/apps/beaconfix.png
    data/icons/hicolor/24x24/apps/beaconfix.png data/icons/hicolor/32x32/apps/beaconfix.png
    data/icons/hicolor/48x48/apps/beaconfix.png data/icons/hicolor/64x64/apps/beaconfix.png
    data/icons/hicolor/128x128/apps/beaconfix.png data/icons/hicolor/256x256/apps/beaconfix.png)
# The surveillance signatures (docs/DETECTION.md), shared with the Android app: compiled in, overridable from the data dir
qt_add_resources(beaconfix "signatures" PREFIX "/" BASE data FILES data/signatures/surveillance.json)

# ── Install ──────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
option(BEACONFIX_INSTALL_WIDGET "Install the Plasma widget system-wide (share/plasma/plasmoids)" ON)

install(TARGETS beaconfix RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/beaconfix.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(DIRECTORY data/icons/hicolor DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)
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
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtbase-sqlite, NetworkManager")
include(CPack)

# ── Tests (cmake -DBEACONFIX_TESTS=ON; then ctest) ──────────────────────────
option(BEACONFIX_TESTS "Build the estimator unit tests and golden vectors" OFF)
if(BEACONFIX_TESTS)
    enable_testing()
    add_executable(estimator_test tests/estimator_test.cpp src/estimator.cpp)
    target_link_libraries(estimator_test PRIVATE Qt6::Core)
    add_test(NAME estimator COMMAND estimator_test)
    add_executable(estimator_golden tests/estimator_golden.cpp src/estimator.cpp)
    target_link_libraries(estimator_golden PRIVATE Qt6::Core)
    add_test(NAME estimator_golden COMMAND estimator_golden --check ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/estimator_golden.json)
    add_executable(flockdetector_test tests/flockdetector_test.cpp src/flockdetector.cpp)
    target_link_libraries(flockdetector_test PRIVATE Qt6::Core)
    qt_add_resources(flockdetector_test "signatures" PREFIX "/" BASE data FILES data/signatures/surveillance.json)
    add_test(NAME flockdetector COMMAND flockdetector_test ${CMAKE_CURRENT_SOURCE_DIR}/data/signatures/surveillance.json
                                                           ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/surveillance_cases.json)
    add_executable(tracksmoother_test tests/tracksmoother_test.cpp src/tracksmoother.cpp)
    target_link_libraries(tracksmoother_test PRIVATE Qt6::Core)
    add_test(NAME tracksmoother COMMAND tracksmoother_test)
    add_executable(securechannel_test tests/securechannel_test.cpp src/securechannel.cpp)
    target_link_libraries(securechannel_test PRIVATE Qt6::Core OpenSSL::Crypto)
    add_test(NAME securechannel COMMAND securechannel_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bfs3_vectors.json)
    add_executable(link_test tests/link_test.cpp src/linking.cpp src/securechannel.cpp)
    target_link_libraries(link_test PRIVATE Qt6::Core OpenSSL::Crypto)
    add_test(NAME link COMMAND link_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/link_vectors.json)
    # Plate events (docs/SIGHTINGS.md): pass detection, the HaveIBeenFlocked watcher's pure parts, the image pipeline
    add_executable(plateevents_test tests/plateevents_test.cpp src/plateevents.cpp)
    target_link_libraries(plateevents_test PRIVATE Qt6::Core)
    add_test(NAME plateevents COMMAND plateevents_test)
    # The camera map's bulk sources (DeFlock, flocklocations community rows): the streaming GeoJSON reader, the row mapping
    add_executable(cameraimport_test tests/cameraimport_test.cpp src/cameraimport.cpp src/flockdetector.cpp src/plateevents.cpp)
    target_link_libraries(cameraimport_test PRIVATE Qt6::Core)
    add_test(NAME cameraimport COMMAND cameraimport_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/deflock_sample.geojson)
    add_executable(hibf_test tests/hibf_test.cpp src/hibf.cpp src/plateevents.cpp)
    target_link_libraries(hibf_test PRIVATE Qt6::Core)
    add_test(NAME hibf COMMAND hibf_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)
    # Phase D (docs/SIGHTINGS.md §2.6, §2.7, §4.6; docs/ROUTING.md): road snapping, camera trust, Eyes on Flock, avoidance routing
    add_executable(roadsnap_test tests/roadsnap_test.cpp src/roadsnap.cpp src/plateevents.cpp)
    target_link_libraries(roadsnap_test PRIVATE Qt6::Core)
    add_test(NAME roadsnap COMMAND roadsnap_test)
    add_executable(camerainfo_test tests/camerainfo_test.cpp src/cameratrust.cpp src/eyesonflock.cpp src/hibf.cpp src/plateevents.cpp)
    target_link_libraries(camerainfo_test PRIVATE Qt6::Core)
    add_test(NAME camerainfo COMMAND camerainfo_test)
    add_executable(avoidroute_test tests/avoidroute_test.cpp src/avoidroute.cpp src/routeplanner.cpp src/plateevents.cpp)
    target_link_libraries(avoidroute_test PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME avoidroute COMMAND avoidroute_test)
    add_executable(imagestore_test tests/imagestore_test.cpp src/imagestore.cpp)
    target_link_libraries(imagestore_test PRIVATE Qt6::Core Qt6::Gui)
    add_test(NAME imagestore COMMAND imagestore_test)
endif()

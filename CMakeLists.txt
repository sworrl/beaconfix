cmake_minimum_required(VERSION 3.16)
project(beaconfix VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network DBus)

add_executable(beaconfix
    src/main.cpp
    src/wifiscanner.cpp
    src/locator.cpp
    src/tilemap.cpp
    src/tray.cpp
    src/mainwindow.cpp
)
target_compile_definitions(beaconfix PRIVATE BEACONFIX_VERSION="${PROJECT_VERSION}")
target_link_libraries(beaconfix PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::DBus)

install(TARGETS beaconfix RUNTIME DESTINATION bin)
install(FILES data/beaconfix.desktop DESTINATION share/applications)
install(FILES data/org.sworrl.BeaconFix.service DESTINATION share/dbus-1/services)

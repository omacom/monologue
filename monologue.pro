QT += core gui qml quick quickcontrols2 multimedia dbus concurrent
!versionAtLeast(QT_VERSION, 6.8.0): error("Monologue requires Qt 6.8 or newer.")
CONFIG += c++17 release
TARGET = monologue
TEMPLATE = app
include(src/common.pri)
SOURCES += src/main.cpp
RESOURCES += src/resources.qrc

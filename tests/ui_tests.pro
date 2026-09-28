QT += core gui qml quick quickcontrols2 multimedia testlib
CONFIG += c++17 console testcase
TARGET = ui_tests
TEMPLATE = app
SOURCES += ui_tests.cpp ../src/theme.cpp
HEADERS += ../src/theme.h
INCLUDEPATH += ../src
RESOURCES += ../src/resources.qrc
# QTest includes all of QtCore, which trips GCC 16 over Qt's own QBitArray.
QMAKE_CXXFLAGS += -Wno-sfinae-incomplete

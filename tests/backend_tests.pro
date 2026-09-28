QT += core gui multimedia testlib dbus concurrent
CONFIG += c++17 console testcase
TARGET = backend_tests
TEMPLATE = app
INCLUDEPATH += ../src
SOURCES += backend_tests.cpp
include(../src/common.pri)
# QTest includes all of QtCore, which trips GCC 16 over Qt's own QBitArray.
QMAKE_CXXFLAGS += -Wno-sfinae-incomplete

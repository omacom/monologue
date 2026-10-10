CONFIG += link_pkgconfig
PKGCONFIG += libpulse wayland-client
INCLUDEPATH += $$PWD/proto
HEADERS += $$PWD/theme.h $$PWD/mediautils.h $$PWD/writer.h $$PWD/backend.h \
    $$PWD/filepicker.h $$PWD/portalfilepicker.h $$PWD/audiocapture.h \
    $$PWD/edit.h $$PWD/thumbnails.h $$PWD/windowlist.h $$PWD/windowcapture.h
SOURCES += $$PWD/theme.cpp $$PWD/mediautils.cpp $$PWD/writer.cpp $$PWD/backend.cpp \
    $$PWD/portalfilepicker.cpp $$PWD/audiocapture.cpp \
    $$PWD/edit.cpp $$PWD/thumbnails.cpp $$PWD/windowlist.cpp $$PWD/windowcapture.cpp \
    $$PWD/proto/hyprland-toplevel-export-v1-protocol.c

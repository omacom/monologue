CONFIG += link_pkgconfig
PKGCONFIG += libpulse libturbojpeg libavcodec libavformat libavutil libswscale libswresample
HEADERS += $$PWD/theme.h $$PWD/mediautils.h $$PWD/encoder.h $$PWD/writer.h $$PWD/backend.h \
    $$PWD/filepicker.h $$PWD/portalfilepicker.h $$PWD/audiocapture.h \
    $$PWD/edit.h $$PWD/thumbnails.h
SOURCES += $$PWD/theme.cpp $$PWD/mediautils.cpp $$PWD/encoder.cpp $$PWD/writer.cpp $$PWD/backend.cpp \
    $$PWD/portalfilepicker.cpp $$PWD/audiocapture.cpp \
    $$PWD/edit.cpp $$PWD/thumbnails.cpp

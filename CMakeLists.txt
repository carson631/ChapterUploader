cmake_minimum_required(VERSION 3.19)
project(ChapterUploader LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets)

qt_standard_project_setup()
set(CMAKE_AUTORCC ON)

qt_add_executable(ChapterUploader
    WIN32
    main.cpp
    mainwindow.h
    mainwindow.cpp
    chapterstore.h
    chapterstore.cpp
    cdpclient.h
    cdpclient.cpp
    uploadworker.h
    uploadworker.cpp
    publishworker.h
    publishworker.cpp
    installpatch.h
    installpatch.cpp
    app.qrc
)

target_link_libraries(ChapterUploader PRIVATE
    Qt::Core Qt::Gui Qt::Widgets Qt::Network Qt::WebSockets
)

install(TARGETS ChapterUploader RUNTIME DESTINATION .)

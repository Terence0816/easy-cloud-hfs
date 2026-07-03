cmake_minimum_required(VERSION 3.24)

project(EasyCloudHFS VERSION 0.1.0 LANGUAGES CXX)

option(EASYCLOUDHFS_PORTABLE_STATIC "Build a true portable single EXE using a static Qt build" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

include(CheckIPOSupported)

if(MSVC AND EASYCLOUDHFS_PORTABLE_STATIC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network)

if(EASYCLOUDHFS_PORTABLE_STATIC)
    get_target_property(QT6_CORE_TARGET_TYPE Qt6::Core TYPE)
    if(NOT QT6_CORE_TARGET_TYPE STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR
            "EASYCLOUDHFS_PORTABLE_STATIC requires a static Qt build. "
            "Current Qt6::Core target type is '${QT6_CORE_TARGET_TYPE}'.")
    endif()
endif()

add_executable(EasyCloudHFS
    assets/app_icon.rc
    assets/resources.qrc
    src/main.cpp
    src/app/Models.cpp
    src/app/DataStore.cpp
    src/app/HttpFileServer.cpp
    src/app/TunnelManager.cpp
    src/app/Controller.cpp
    src/app/QrCodeGenerator.cpp
    src/ui/MainWindow.cpp
)

target_include_directories(EasyCloudHFS PRIVATE src)

target_link_libraries(EasyCloudHFS PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)

check_ipo_supported(RESULT EASYCLOUDHFS_IPO_SUPPORTED OUTPUT EASYCLOUDHFS_IPO_ERROR)
if(EASYCLOUDHFS_IPO_SUPPORTED)
    set_property(TARGET EasyCloudHFS PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET EasyCloudHFS PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
endif()

if(MSVC)
    target_compile_options(EasyCloudHFS PRIVATE
        $<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:/Gw>
        $<$<CONFIG:MinSizeRel>:/Gy>
    )
    target_link_options(EasyCloudHFS PRIVATE
        $<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:/OPT:REF>
        $<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:/OPT:ICF>
        $<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:/INCREMENTAL:NO>
    )
endif()

if(WIN32)
    set_target_properties(EasyCloudHFS PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(EASYCLOUDHFS_PORTABLE_STATIC)
    qt_import_plugins(EasyCloudHFS
        INCLUDE_BY_TYPE platforms Qt::QWindowsIntegrationPlugin
    )

    if(TARGET Qt::QWindowsVistaStylePlugin)
        qt_import_plugins(EasyCloudHFS INCLUDE Qt::QWindowsVistaStylePlugin)
    endif()
endif()

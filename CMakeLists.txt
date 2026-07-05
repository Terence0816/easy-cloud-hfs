cmake_minimum_required(VERSION 3.24)

project(EasyCloudHFS VERSION 1.2.0.0 LANGUAGES CXX)

option(EASYCLOUDHFS_PORTABLE_STATIC "Build a true portable single EXE using a static Qt build" OFF)
set(EASYCLOUDHFS_QT_MAJOR "5" CACHE STRING "Qt major version to use (5 or 6)")
set_property(CACHE EASYCLOUDHFS_QT_MAJOR PROPERTY STRINGS 5 6)

# Qt 5.15 + C++17 is intentionally used by the Win7/10/11 single-EXE build.
# The application itself does not require C++20.
set(CMAKE_CXX_STANDARD 17)
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

if(EASYCLOUDHFS_QT_MAJOR STREQUAL "5")
    find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui Widgets Network)
    set(EASYCLOUDHFS_QT_PREFIX Qt5)
elseif(EASYCLOUDHFS_QT_MAJOR STREQUAL "6")
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network)
    set(EASYCLOUDHFS_QT_PREFIX Qt6)
else()
    message(FATAL_ERROR "EASYCLOUDHFS_QT_MAJOR must be 5 or 6")
endif()

if(EASYCLOUDHFS_PORTABLE_STATIC)
    get_target_property(EASYCLOUDHFS_QT_CORE_TARGET_TYPE ${EASYCLOUDHFS_QT_PREFIX}::Core TYPE)
    if(NOT EASYCLOUDHFS_QT_CORE_TARGET_TYPE STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR
            "EASYCLOUDHFS_PORTABLE_STATIC requires a static Qt build. "
            "Current ${EASYCLOUDHFS_QT_PREFIX}::Core target type is "
            "'${EASYCLOUDHFS_QT_CORE_TARGET_TYPE}'.")
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
    ${EASYCLOUDHFS_QT_PREFIX}::Core
    ${EASYCLOUDHFS_QT_PREFIX}::Gui
    ${EASYCLOUDHFS_QT_PREFIX}::Widgets
    ${EASYCLOUDHFS_QT_PREFIX}::Network
)

# Keep exactly the Windows QPA platform plugin in the static EXE.
# Qt 5.15 and Qt 6 both provide import-plugin CMake helpers; the target names differ.
if(WIN32 AND EASYCLOUDHFS_PORTABLE_STATIC)
    if(EASYCLOUDHFS_QT_MAJOR STREQUAL "5")
        qt5_import_plugins(EasyCloudHFS
            INCLUDE_BY_TYPE platforms Qt5::QWindowsIntegrationPlugin
        )
    else()
        qt_import_plugins(EasyCloudHFS
            INCLUDE_BY_TYPE platforms Qt::QWindowsIntegrationPlugin
        )
    endif()
endif()

# Keep both our code and Windows SDK declarations at the Windows 7 API floor.
# Qt 5.15.2 itself is built with the same definitions by build-qt-static.bat.
if(WIN32)
    target_compile_definitions(EasyCloudHFS PRIVATE
        _WIN32_WINNT=0x0601
        WINVER=0x0601
        NTDDI_VERSION=0x06010000
    )
endif()

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

    # Explicit PE subsystem floor: Windows 7 = 6.01.
    if(WIN32)
        target_link_options(EasyCloudHFS PRIVATE /SUBSYSTEM:WINDOWS,6.01)
    endif()
endif()

if(WIN32)
    set_target_properties(EasyCloudHFS PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

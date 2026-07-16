cmake_minimum_required(VERSION 3.21)
project(unisic-studio VERSION 0.1.0 LANGUAGES CXX)
# Human-facing version. project(VERSION) only takes numeric tuples, so any
# pre-release suffix lives here; this string drives STUDIO_VERSION (binary
# footer) and would drive package/release tags later. Mirrors Unisic's pattern.
set(STUDIO_VERSION_STRING "0.1.0")

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Quick Qml QuickControls2 DBus Concurrent)

qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0001 NEW)
# QTP0004 NEW: auto-generate qmldir for qml/ subdirs so tooling resolves them.
# The policy only exists since Qt 6.8 — setting it unguarded is a hard configure
# error on the declared 6.5/6.6 floor.
if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)
endif()

# The shared foundation library (static lib + Unisic.Kit QML module). It is a
# git submodule; kit changes go through the unisic-kit repo, never edited here.
add_subdirectory(external/unisic-kit)

qt_add_executable(unisic-studio
    src/main.cpp
    src/StudioApp.h src/StudioApp.cpp
    src/StudioSettings.h src/StudioSettings.cpp
    # Project document model (Core+Gui only, no QML — see src/project/*.h). The
    # facade will expose these to QML as QObject* properties; they are not
    # QML_ELEMENT types.
    src/project/CursorTrack.h src/project/CursorTrack.cpp
    src/project/ClickTrack.h src/project/ClickTrack.cpp
    src/project/ZoomTimeline.h src/project/ZoomTimeline.cpp
    src/project/StyleModel.h src/project/StyleModel.cpp
    src/project/StudioProject.h src/project/StudioProject.cpp
)

# qt_standard_project_setup bakes an $ORIGIN install-RPATH into the binary;
# distro packages link system Qt so it is dead weight. Mirrors Unisic.
set_target_properties(unisic-studio PROPERTIES INSTALL_RPATH "")

# Single source of the app version string, exported to C++ from the project()
# call. Mirrors Unisic's UNISIC_VERSION pattern.
target_compile_definitions(unisic-studio PRIVATE
    STUDIO_VERSION="${STUDIO_VERSION_STRING}")

# Developer build: a fully separate app identity (own name, desktop id,
# single-instance socket, config dir) so a build-tree binary never shadows or
# fights an installed stable Studio. Auto ON for local dev builds, OFF for CI
# (which sets STUDIO_BUILD_NUMBER). Mirrors Unisic's UNISIC_DEV_BUILD flag.
if(DEFINED ENV{STUDIO_BUILD_NUMBER} AND NOT "$ENV{STUDIO_BUILD_NUMBER}" STREQUAL "")
    set(_studio_dev_default OFF)
else()
    set(_studio_dev_default ON)
endif()
option(UNISIC_DEV_BUILD "Developer build (separate app identity + config)" ${_studio_dev_default})
if(UNISIC_DEV_BUILD)
    target_compile_definitions(unisic-studio PRIVATE UNISIC_DEV_BUILD)
    message(STATUS "Developer build: UNISIC_DEV_BUILD=ON")
endif()

set_source_files_properties(qml/StudioMain.qml PROPERTIES QT_QML_SINGLETON_TYPE FALSE)

qt_add_qml_module(unisic-studio
    URI UnisicStudio
    VERSION 1.0
    # On a case-insensitive filesystem (this drive is FAT) the default output
    # dir named after the URI would collide with other paths — use a dedicated
    # qmlmod/ tree. Mirrors both Unisic and the kit.
    OUTPUT_DIRECTORY qmlmod/UnisicStudio
    QML_FILES
        qml/StudioMain.qml
)

# qmltyperegistration includes QML_ELEMENT headers by basename.
target_include_directories(unisic-studio PRIVATE src src/project)
target_link_libraries(unisic-studio PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Quick Qt6::Qml Qt6::QuickControls2
    Qt6::DBus Qt6::Concurrent
    # The kit exports its C++ static lib + its generated QML-module plugin.
    unisic-kit unisic-kitplugin
)

# App icon (placeholder, reuses Unisic's mark — see README TODO for a distinct
# icon). PREFIX "/" gives ":/resources/icons/unisic-studio.svg".
qt_add_resources(unisic-studio "icons" PREFIX "/"
    FILES resources/icons/unisic-studio.svg)

# Translations (optional): needs Qt6 LinguistTools. English is the source
# language; only studio_en.ts exists for M0, but the wiring is ready for more.
# `cmake --build build --target update_translations` rescans C++ + QML and
# rewrites the .ts source strings; a plain build compiles them to .qm and
# embeds them under /i18n/ (HAVE_TRANSLATIONS). Without the tools the app still
# builds and falls back to the English source strings. Mirrors Unisic.
find_package(Qt6 QUIET COMPONENTS LinguistTools)
set(STUDIO_TS_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/i18n/studio_en.ts)
if(Qt6LinguistTools_FOUND)
    qt_add_lrelease(unisic-studio TS_FILES ${STUDIO_TS_FILES}
                    QM_FILES_OUTPUT_VARIABLE STUDIO_QM_FILES)
    qt_add_resources(unisic-studio "i18n" PREFIX "/i18n" BASE ${CMAKE_CURRENT_BINARY_DIR}
                     FILES ${STUDIO_QM_FILES})
    target_compile_definitions(unisic-studio PRIVATE HAVE_TRANSLATIONS)
    file(GLOB_RECURSE STUDIO_LUPDATE_SOURCES CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
         ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
         ${CMAKE_CURRENT_SOURCE_DIR}/qml/*.qml)
    # The target-less qt_add_lupdate() signature is Qt 6.7+. On the declared
    # 6.5/6.6 floor the first positional arg is the mandatory target, so pass
    # `unisic-studio` explicitly there.
    if(Qt6_VERSION VERSION_GREATER_EQUAL 6.7)
        qt_add_lupdate(TS_FILES ${STUDIO_TS_FILES} SOURCES ${STUDIO_LUPDATE_SOURCES})
    else()
        qt_add_lupdate(unisic-studio TS_FILES ${STUDIO_TS_FILES} SOURCES ${STUDIO_LUPDATE_SOURCES})
    endif()
    message(STATUS "Translations: enabled (English source only for M0)")
else()
    message(STATUS "Translations: disabled (needs Qt6 LinguistTools) — English source strings only")
endif()

install(TARGETS unisic-studio RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES resources/app.unisic.UnisicStudio.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES resources/app.unisic.UnisicStudio.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES resources/icons/unisic-studio.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    RENAME app.unisic.UnisicStudio.svg)

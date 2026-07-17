cmake_minimum_required(VERSION 3.21)
project(unisic-studio VERSION 0.1.0 LANGUAGES CXX)
# Human-facing version. project(VERSION) only takes numeric tuples, so any
# pre-release suffix lives here; this string drives STUDIO_VERSION (binary
# footer) and would drive package/release tags later. Mirrors Unisic's pattern.
set(STUDIO_VERSION_STRING "0.1.0")

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Quick Qml QuickControls2 DBus Concurrent OpenGL)

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

# Click capture (libinput/libudev) is OPTIONAL, guarded exactly like the kit's
# HAVE_PIPEWIRE: without the -devel packages the build still succeeds (a warning
# is printed) and InputPermission reports NotBuilt / ClickCapture stays inert.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBINPUT IMPORTED_TARGET libinput)
    pkg_check_modules(LIBUDEV IMPORTED_TARGET libudev)
endif()

qt_add_executable(unisic-studio
    src/main.cpp
    src/StudioApp.h src/StudioApp.cpp
    src/StudioSettings.h src/StudioSettings.cpp
    # Recent-projects index + per-window editor lifecycle. Focused helpers the
    # facade wires up (keeping StudioApp thin).
    src/RecentProjects.h src/RecentProjects.cpp
    src/EditorWindowManager.h src/EditorWindowManager.cpp
    # Media helpers: async ffprobe metadata + ffmpeg poster-frame extraction.
    # ffmpeg/ffprobe are shelled out (QStandardPaths-detected), never linked.
    src/media/VideoProbe.h src/media/VideoProbe.cpp
    src/media/PosterExtractor.h src/media/PosterExtractor.cpp
    # Project document model (Core+Gui only, no QML — see src/project/*.h). The
    # facade will expose these to QML as QObject* properties; they are not
    # QML_ELEMENT types.
    src/project/CursorTrack.h src/project/CursorTrack.cpp
    src/project/ClickTrack.h src/project/ClickTrack.cpp
    src/project/ZoomTimeline.h src/project/ZoomTimeline.cpp
    src/project/StyleModel.h src/project/StyleModel.cpp
    src/project/StudioProject.h src/project/StudioProject.cpp
    # Recording engine (M2). StudioRecorder orchestrates portal + PipeWireGrabber
    # + fixed-FPS sampler + ffmpeg; RecordingAssembler builds the tracks/sidecar in
    # its own TU (the kit and project CursorSample structs collide in one). Click
    # capture (ClickCapture/InputPermission) compiles unconditionally with the
    # libinput code fully behind #ifdef HAVE_LIBINPUT — the #else branch is the
    # NotBuilt/inert stub, so no separate stub TU is needed.
    src/capture/RecorderMath.h
    src/capture/InputPermission.h src/capture/InputPermission.cpp
    src/capture/ClickCapture.h src/capture/ClickCapture.cpp
    src/capture/RecordingAssembler.h src/capture/RecordingAssembler.cpp
    src/capture/StudioRecorder.h src/capture/StudioRecorder.cpp
    # Offscreen render + export pipeline (M1 final step). VideoFrameItem and
    # ExportController are QML_ELEMENT types in the UnisicStudio module; the rest
    # is plain C++ the controller wires up. ffmpeg is shelled out, never linked.
    src/render/VideoFrameItem.h src/render/VideoFrameItem.cpp
    src/render/FrameDecoder.h src/render/FrameDecoder.cpp
    src/render/RenderPipeline.h src/render/RenderPipeline.cpp
    src/render/ExportController.h src/render/ExportController.cpp
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

# PreviewVideo.qml `import QtMultimedia` — a RUNTIME-only plugin gated behind
# Studio.capVideoPlayback. Skip ahead-of-time cachegen so a build on a machine
# without the QtMultimedia QML module still succeeds (the import is resolved
# lazily when the Loader instantiates it, or never).
set_source_files_properties(qml/composition/PreviewVideo.qml PROPERTIES QT_QML_SKIP_CACHEGEN ON)

qt_add_qml_module(unisic-studio
    URI UnisicStudio
    VERSION 1.0
    # On a case-insensitive filesystem (this drive is FAT) the default output
    # dir named after the URI would collide with other paths — use a dedicated
    # qmlmod/ tree. Mirrors both Unisic and the kit.
    OUTPUT_DIRECTORY qmlmod/UnisicStudio
    QML_FILES
        qml/StudioMain.qml
        qml/EditorWindow.qml
        qml/InspectorPanel.qml
        qml/ExportDialog.qml
        qml/composition/CompositionRoot.qml
        qml/composition/PreviewVideo.qml
)

# qmltyperegistration includes QML_ELEMENT headers by basename.
target_include_directories(unisic-studio PRIVATE src src/project src/media src/render src/capture)

# Optional click capture: link libinput/libudev and flip HAVE_LIBINPUT only when
# both were found. The capture sources always compile; this just enables the real
# code path (else InputPermission::probe() returns NotBuilt).
if(LIBINPUT_FOUND AND LIBUDEV_FOUND)
    target_link_libraries(unisic-studio PRIVATE PkgConfig::LIBINPUT PkgConfig::LIBUDEV)
    target_compile_definitions(unisic-studio PRIVATE HAVE_LIBINPUT)
    message(STATUS "unisic-studio: click capture enabled (HAVE_LIBINPUT=ON)")
else()
    message(WARNING "libinput/libudev not found — click capture (ClickCapture) will be unavailable")
endif()
target_link_libraries(unisic-studio PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Quick Qt6::Qml Qt6::QuickControls2
    Qt6::DBus Qt6::Concurrent
    # Qt6::OpenGL: public QOpenGLFramebufferObject for the offscreen render target
    # (QQuickRenderTarget::fromOpenGLTexture). A qtbase module, not a new external
    # dependency; the RHI private texture-target headers are not shipped in the SDK.
    Qt6::OpenGL
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

# Project-model unit tests (offscreen-capable, QCoreApplication only). Each test
# compiles just the production .cpp files it exercises — mirrors Unisic's
# per-test executable pattern. Core+Gui+Test (Gui for QColor/QRectF/QSize); no
# QML, no event loop beyond QCoreApplication's.
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

    qt_add_executable(studio_cursor_track_test
        tests/CursorTrackTest.cpp
        src/project/CursorTrack.h src/project/CursorTrack.cpp)
    target_link_libraries(studio_cursor_track_test PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    target_include_directories(studio_cursor_track_test PRIVATE src/project)
    add_test(NAME studio_cursor_track_test COMMAND studio_cursor_track_test)

    qt_add_executable(studio_click_track_test
        tests/ClickTrackTest.cpp
        src/project/ClickTrack.h src/project/ClickTrack.cpp)
    target_link_libraries(studio_click_track_test PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    target_include_directories(studio_click_track_test PRIVATE src/project)
    add_test(NAME studio_click_track_test COMMAND studio_click_track_test)

    qt_add_executable(studio_zoom_timeline_test
        tests/ZoomTimelineTest.cpp
        src/project/ZoomTimeline.h src/project/ZoomTimeline.cpp)
    target_link_libraries(studio_zoom_timeline_test PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    target_include_directories(studio_zoom_timeline_test PRIVATE src/project)
    add_test(NAME studio_zoom_timeline_test COMMAND studio_zoom_timeline_test)

    qt_add_executable(studio_project_test
        tests/StudioProjectTest.cpp
        src/project/CursorTrack.h src/project/CursorTrack.cpp
        src/project/ClickTrack.h src/project/ClickTrack.cpp
        src/project/ZoomTimeline.h src/project/ZoomTimeline.cpp
        src/project/StyleModel.h src/project/StyleModel.cpp
        src/project/StudioProject.h src/project/StudioProject.cpp)
    target_link_libraries(studio_project_test PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    target_include_directories(studio_project_test PRIVATE src/project)
    add_test(NAME studio_project_test COMMAND studio_project_test)

    # Recorder time-math (pure): mono-ns→video-ms mapping, pause-range conversion +
    # merging, and click-position resolution via CursorTrack::sample around excised
    # ranges. RecorderMath is header-only; the click test needs the two track .cpp.
    qt_add_executable(studio_recorder_math_test
        tests/RecorderMathTest.cpp
        src/project/CursorTrack.h src/project/CursorTrack.cpp
        src/project/ClickTrack.h src/project/ClickTrack.cpp)
    target_link_libraries(studio_recorder_math_test PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    target_include_directories(studio_recorder_math_test PRIVATE src/project src/capture)
    add_test(NAME studio_recorder_math_test COMMAND studio_recorder_math_test)
endif()

install(TARGETS unisic-studio RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES resources/app.unisic.UnisicStudio.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES resources/app.unisic.UnisicStudio.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES resources/icons/unisic-studio.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    RENAME app.unisic.UnisicStudio.svg)

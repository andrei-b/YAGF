cmake_minimum_required(VERSION 3.21)
project(yagf_revival VERSION 2.0.0 LANGUAGES CXX)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Concurrent Network)
qt_standard_project_setup()
set(CMAKE_AUTORCC ON)

add_library(yagf_core STATIC
    src/core/ocrdocument.cpp src/core/ocrdocument.h src/core/preprocesssettings.h
    src/core/bookvocabulary.cpp src/core/bookvocabulary.h
    src/core/documentcontext.cpp src/core/documentcontext.h
    src/core/regiongeometry.cpp src/core/regiongeometry.h
    src/core/symbolmatcher.cpp src/core/symbolmatcher.h
    src/core/correctionmemory.cpp src/core/correctionmemory.h
    src/core/projectfile.cpp src/core/projectfile.h
    src/core/wordeditor.cpp src/core/wordeditor.h)
target_link_libraries(yagf_core PUBLIC Qt6::Core Qt6::Gui)
target_include_directories(yagf_core PUBLIC src/core)

add_library(yagf_processing STATIC
    src/processing/specialistocr.cpp src/processing/specialistocr.h
    src/processing/symbolshape.cpp src/processing/symbolshape.h
    src/processing/documentprocessor.cpp src/processing/documentprocessor.h
    src/processing/imageprocessor.cpp src/processing/imageprocessor.h
    src/processing/iocrengine.h
    src/processing/tesseractengine.cpp src/processing/tesseractengine.h
    src/processing/remoteocrengine.cpp src/processing/remoteocrengine.h
    src/processing/scientificbookexporter.cpp src/processing/exportpage.h
    src/processing/pdfexporter.cpp src/processing/pdfexporter.h)
target_link_libraries(yagf_processing PUBLIC yagf_core Qt6::Core Qt6::Gui Qt6::Concurrent Qt6::Network)
target_include_directories(yagf_processing PUBLIC src/processing)

qt_add_executable(yagf-revival
    src/main.cpp src/ui/mainwindow.cpp src/ui/mainwindow.h
    src/ui/preprocessdialog.cpp src/ui/preprocessdialog.h
    src/ui/layoutdialog.cpp src/ui/layoutdialog.h resources/icons.qrc)
target_sources(yagf-revival PRIVATE src/ui/scannerdialog.cpp src/ui/scannerdialog.h)
target_sources(yagf-revival PRIVATE src/ui/documentcommands.cpp src/ui/documentcommands.h)
target_sources(yagf-revival PRIVATE src/ui/aireviewdialog.cpp src/ui/aireviewdialog.h)
target_link_libraries(yagf-revival PRIVATE yagf_core yagf_processing Qt6::Widgets Qt6::Concurrent)

install(TARGETS yagf-revival RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/io.github.andrei_b.YAGF.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES resources/yagf.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/96x96/apps
        RENAME io.github.andrei_b.YAGF.png)
install(FILES packaging/io.github.andrei_b.YAGF.appdata.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES COPYING
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/io.github.andrei_b.YAGF)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Test)
    qt_add_executable(regiongeometry_tests tests/regiongeometry_tests.cpp)
    target_link_libraries(regiongeometry_tests PRIVATE yagf_core)
    add_test(NAME regiongeometry_tests COMMAND regiongeometry_tests)
    qt_add_executable(documentcontext_tests tests/documentcontext_tests.cpp)
    target_link_libraries(documentcontext_tests PRIVATE yagf_core)
    add_test(NAME documentcontext_tests COMMAND documentcontext_tests)
    qt_add_executable(correctionmemory_tests tests/correctionmemory_tests.cpp)
    target_link_libraries(correctionmemory_tests PRIVATE yagf_core)
    add_test(NAME correctionmemory_tests COMMAND correctionmemory_tests)
    qt_add_executable(ocrdocument_tests tests/ocrdocument_tests.cpp)
    target_link_libraries(ocrdocument_tests PRIVATE yagf_core)
    add_test(NAME ocrdocument_tests COMMAND ocrdocument_tests)
    qt_add_executable(documentcommands_tests tests/documentcommands_tests.cpp
        src/ui/documentcommands.cpp src/ui/documentcommands.h)
    target_link_libraries(documentcommands_tests PRIVATE yagf_core Qt6::Gui)
    target_include_directories(documentcommands_tests PRIVATE src/ui)
    add_test(NAME documentcommands_tests COMMAND documentcommands_tests)
    qt_add_executable(mainwindow_tests tests/mainwindow_tests.cpp
        src/ui/mainwindow.cpp src/ui/mainwindow.h
        src/ui/preprocessdialog.cpp src/ui/preprocessdialog.h
        src/ui/layoutdialog.cpp src/ui/layoutdialog.h
        src/ui/scannerdialog.cpp src/ui/scannerdialog.h
        src/ui/documentcommands.cpp src/ui/documentcommands.h resources/icons.qrc)
    target_sources(mainwindow_tests PRIVATE src/ui/aireviewdialog.cpp src/ui/aireviewdialog.h)
    target_link_libraries(mainwindow_tests PRIVATE yagf_processing Qt6::Widgets Qt6::Concurrent Qt6::Test)
    target_include_directories(mainwindow_tests PRIVATE src/ui)
    add_test(NAME mainwindow_tests COMMAND mainwindow_tests)
    set_tests_properties(mainwindow_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(aireviewdialog_tests tests/aireviewdialog_tests.cpp
        src/ui/aireviewdialog.cpp src/ui/aireviewdialog.h)
    target_link_libraries(aireviewdialog_tests PRIVATE yagf_processing Qt6::Widgets Qt6::Network Qt6::Test)
    target_include_directories(aireviewdialog_tests PRIVATE src/ui)
    add_test(NAME aireviewdialog_tests COMMAND aireviewdialog_tests)
    set_tests_properties(aireviewdialog_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(projectfile_tests tests/projectfile_tests.cpp)
    target_link_libraries(projectfile_tests PRIVATE yagf_core)
    add_test(NAME projectfile_tests COMMAND projectfile_tests)
    qt_add_executable(imageprocessor_tests tests/imageprocessor_tests.cpp)
    target_link_libraries(imageprocessor_tests PRIVATE yagf_processing)
    add_test(NAME imageprocessor_tests COMMAND imageprocessor_tests)
    qt_add_executable(scientificbook_tests tests/scientificbook_tests.cpp)
    target_link_libraries(scientificbook_tests PRIVATE yagf_processing)
    add_test(NAME scientificbook_tests COMMAND scientificbook_tests)
    qt_add_executable(specialistocr_tests tests/specialistocr_tests.cpp)
    target_link_libraries(specialistocr_tests PRIVATE yagf_processing)
    add_test(NAME specialistocr_tests COMMAND specialistocr_tests)
    qt_add_executable(adaptiveocr_tests tests/adaptiveocr_tests.cpp)
    target_link_libraries(adaptiveocr_tests PRIVATE yagf_processing)
    add_test(NAME adaptiveocr_tests COMMAND adaptiveocr_tests)
    qt_add_executable(ocrengine_tests tests/ocrengine_tests.cpp)
    target_link_libraries(ocrengine_tests PRIVATE yagf_processing)
    add_test(NAME ocrengine_tests COMMAND ocrengine_tests)
    qt_add_executable(remoteocrengine_tests tests/remoteocrengine_tests.cpp)
    target_link_libraries(remoteocrengine_tests PRIVATE yagf_processing Qt6::Network)
    add_test(NAME remoteocrengine_tests COMMAND remoteocrengine_tests)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME remote_ocr_service_tests
            COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/service/tests)
    endif()
    qt_add_executable(scannerdialog_tests tests/scannerdialog_tests.cpp
        src/ui/scannerdialog.cpp src/ui/scannerdialog.h)
    target_link_libraries(scannerdialog_tests PRIVATE Qt6::Widgets)
    target_include_directories(scannerdialog_tests PRIVATE src/ui)
    add_test(NAME scannerdialog_tests COMMAND scannerdialog_tests)
    set_tests_properties(scannerdialog_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    find_program(PDFTOTEXT_EXECUTABLE pdftotext)
    if(PDFTOTEXT_EXECUTABLE)
        qt_add_executable(pdfexporter_tests tests/pdfexporter_tests.cpp)
        target_link_libraries(pdfexporter_tests PRIVATE yagf_processing Qt6::Gui)
        add_test(NAME pdfexporter_tests COMMAND pdfexporter_tests)
        set_tests_properties(pdfexporter_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()

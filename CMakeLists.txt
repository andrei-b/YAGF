cmake_minimum_required(VERSION 3.21)
project(yagf2 VERSION 2.2.0 LANGUAGES CXX)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4.4 REQUIRED COMPONENTS core imgproc features2d calib3d)
find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Concurrent Network)
qt_standard_project_setup()
set(CMAKE_AUTORCC ON)

# Keep developer checkout paths out of public binaries and diagnostics.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        "-ffile-prefix-map=${PROJECT_SOURCE_DIR}=/usr/src/yagf2"
        "-ffile-prefix-map=${PROJECT_BINARY_DIR}=/usr/src/yagf2/build")
endif()

add_library(yagf_core STATIC
    src/core/recognitionresult.cpp src/core/recognitionresult.h
    src/core/ocrreadings.cpp src/core/ocrreadings.h
    src/core/blockfont.cpp src/core/blockfont.h
    src/core/formulatext.cpp src/core/formulatext.h
    src/core/symbolrules.cpp src/core/symbolrules.h
    src/core/ocrbenchmark.cpp src/core/ocrbenchmark.h
    src/core/spellchecker.cpp src/core/spellchecker.h
    src/core/wordspelling.cpp src/core/wordspelling.h
    src/core/verificationqueue.cpp src/core/verificationqueue.h
    src/core/rendersettings.h src/core/correctionretention.cpp src/core/correctionretention.h
    src/core/ocrdocument.cpp src/core/ocrdocument.h src/core/preprocesssettings.h
    src/core/bookvocabulary.cpp src/core/bookvocabulary.h
    src/core/documentcontext.cpp src/core/documentcontext.h
    src/core/regiongeometry.cpp src/core/regiongeometry.h
    src/core/symbolmatcher.cpp src/core/symbolmatcher.h
    src/core/correctionmemory.cpp src/core/correctionmemory.h
    src/core/projectfile.cpp src/core/projectfile.h
    src/core/wordeditor.cpp src/core/wordeditor.h)
target_link_libraries(yagf_core PUBLIC Qt6::Core)
target_include_directories(yagf_core PUBLIC src/core)

# Image/source operations have no OCR, network, or job-controller dependency.
add_library(yagf_imaging STATIC
    src/processing/pagerestoration.cpp src/processing/pagerestoration.h
    src/processing/pagesource.cpp src/processing/pagesource.h
    src/processing/imageprocessor.cpp src/processing/imageprocessor.h src/processing/pagebend.cpp
    src/processing/symbolshape.cpp src/processing/symbolshape.h)
target_link_libraries(yagf_imaging PUBLIC yagf_core Qt6::Gui PRIVATE ${OpenCV_LIBS})
target_include_directories(yagf_imaging PUBLIC src/processing)

# Symbol augmentation uses source pixels and optional character-box OCR without
# depending on the document controller, export, or widgets.
add_library(yagf_symbols STATIC src/processing/symbolaugmentation.cpp src/processing/symbolaugmentation.h)
target_link_libraries(yagf_symbols PUBLIC yagf_imaging PRIVATE ${OpenCV_LIBS})
target_include_directories(yagf_symbols PUBLIC src/processing)

# Providers and recognition analysis can be used without the async document controller.
add_library(yagf_ocr STATIC
    src/processing/recognitionadapter.cpp src/processing/recognitionadapter.h
    src/processing/ocrservices.cpp src/processing/ocrservices.h
    src/processing/parallelocr.cpp src/processing/parallelocr.h
    src/processing/nativetraining.cpp src/processing/nativetraining.h
    src/processing/aipagelayout.cpp src/processing/aipagelayout.h
    src/processing/pageorientation.cpp src/processing/pageorientation.h
    src/processing/fontestimator.cpp src/processing/fontestimator.h
    src/processing/specialistocr.cpp src/processing/specialistocr.h
    src/processing/spellingformulas.cpp src/processing/spellingformulas.h
    src/processing/compactassignment.h
    src/processing/smartblocks.cpp src/processing/smartblocks.h
    src/processing/mathocrengine.cpp src/processing/mathocrengine.h
    src/processing/iocrengine.h
    src/processing/tesseractengine.cpp src/processing/tesseractengine.h
    src/processing/remoteocrengine.cpp src/processing/remoteocrengine.h)
qt_add_resources(yagf_ocr "local_math" PREFIX "/math" BASE tools FILES tools/math_ocr.py)
target_link_libraries(yagf_ocr PUBLIC yagf_core Qt6::Gui PRIVATE Qt6::Network)
target_include_directories(yagf_ocr PUBLIC src/processing)

add_library(yagf_processing STATIC
    src/processing/documentprocessor.cpp src/processing/documentprocessor.h
    src/processing/textguidedanalysis.cpp src/processing/textguidedanalysis.h)
target_link_libraries(yagf_processing PUBLIC yagf_imaging yagf_symbols yagf_ocr Qt6::Concurrent)
target_include_directories(yagf_processing PUBLIC src/processing)

# Export consumes snapshots and OCR support, never the DocumentProcessor or UI.
add_library(yagf_export STATIC
    src/export/ocrtrainingdataset.cpp src/export/ocrtrainingdataset.h
    src/export/latexdocumentexporter.cpp src/export/latexdocumentexporter.h
    src/export/exporttypes.h
    src/export/exportpage.cpp src/export/exportpage.h
    src/export/pdfexporter.cpp src/export/pdfexporter.h
    src/export/scientificbookexporter.cpp src/export/scientificbookexporter.h
    src/export/verifiedsampleexporter.cpp src/export/verifiedsampleexporter.h
    src/export/detail/exportcommand.cpp src/export/detail/exportcommand.h
    src/export/detail/pagebackground.cpp src/export/detail/pagebackground.h
    src/export/detail/pdfpagelayout.cpp src/export/detail/pdfpagelayout.h
    src/export/detail/pdfformulas.cpp src/export/detail/pdfformulas.h
    src/export/detail/pdfpagepainter.cpp src/export/detail/pdfpagepainter.h)
target_link_libraries(yagf_export PUBLIC yagf_core Qt6::Gui PRIVATE yagf_imaging yagf_ocr)
target_include_directories(yagf_export PUBLIC src/export)

# Experiments use immutable document snapshots and existing provider/source APIs.
add_library(yagf_experiments STATIC src/experiments/experimentrunner.cpp src/experiments/experimentrunner.h)
target_link_libraries(yagf_experiments PUBLIC yagf_core yagf_ocr yagf_imaging yagf_export)
target_include_directories(yagf_experiments PUBLIC src/experiments)

qt_add_executable(yagf2-benchmark src/experiments/main.cpp)
target_link_libraries(yagf2-benchmark PRIVATE yagf_experiments)
target_compile_definitions(yagf2-benchmark PRIVATE YAGF_VERSION="${PROJECT_VERSION}")

qt_add_executable(yagf2
    src/ui/blockminiaturelist.cpp src/ui/blockminiaturelist.h
    src/ui/verificationcontroller.cpp src/ui/verificationcontroller.h
    src/ui/mainwindowreview.cpp src/ui/experimentdialog.cpp src/ui/experimentdialog.h
    src/main.cpp src/ui/mainwindow.cpp src/ui/mainwindow.h src/ui/formulaeditor.cpp src/ui/formulaeditor.h
    src/ui/restorationdialog.cpp src/ui/restorationdialog.h
    src/ui/preprocessdialog.cpp src/ui/preprocessdialog.h
    src/ui/layoutdialog.cpp src/ui/layoutdialog.h resources/icons.qrc)
target_sources(yagf2 PRIVATE src/ui/scannerdialog.cpp src/ui/scannerdialog.h)
target_sources(yagf2 PRIVATE src/ui/documentcommands.cpp src/ui/documentcommands.h)
target_sources(yagf2 PRIVATE src/ui/nativetrainingdialog.cpp src/ui/nativetrainingdialog.h)
target_sources(yagf2 PRIVATE src/ui/ocrservicesdialog.cpp src/ui/ocrservicesdialog.h src/ui/ocrreadingsdialog.cpp src/ui/ocrreadingsdialog.h)
target_sources(yagf2 PRIVATE src/ui/aipagelayoutdialog.cpp src/ui/aipagelayoutdialog.h)
target_sources(yagf2 PRIVATE src/ui/aireviewdialog.cpp src/ui/aireviewdialog.h)
target_sources(yagf2 PRIVATE src/ui/spellhighlighter.cpp src/ui/spellhighlighter.h)
target_sources(yagf2 PRIVATE src/ui/symbolrulesdialog.cpp src/ui/symbolrulesdialog.h)
target_compile_definitions(yagf2 PRIVATE YAGF_VERSION="${PROJECT_VERSION}")
target_link_libraries(yagf2 PRIVATE yagf_experiments yagf_core yagf_processing yagf_export Qt6::Widgets Qt6::Concurrent Qt6::Network)

install(TARGETS yagf2 yagf2-benchmark RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/io.github.andrei_b.YAGF.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES resources/yagf.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/96x96/apps
        RENAME io.github.andrei_b.YAGF.png)
install(FILES resources/yagf-48.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/48x48/apps
        RENAME io.github.andrei_b.YAGF.png)
install(FILES packaging/io.github.andrei_b.YAGF.appdata.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES COPYING
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/io.github.andrei_b.YAGF)
install(FILES README.md ARCHITECTURE.md AUTHORS COPYING
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/yagf2)
install(FILES COPYING DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/yagf2 RENAME copyright)
install(FILES packaging/README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/yagf2/packaging)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/yagf2/docs
        FILES_MATCHING PATTERN "*.md" PATTERN "*.png" PATTERN "*.json")
install(DIRECTORY service/ DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/yagf2/service
        FILES_MATCHING PATTERN "*.md" PATTERN "*.service" PATTERN "*.example.json" PATTERN "*.txt"
        PATTERN "tests" EXCLUDE)
install(DIRECTORY examples/ DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/yagf2/examples
        FILES_MATCHING PATTERN "*.md" PATTERN "*.json" PATTERN "*.png" PATTERN "*.pdf")
install(FILES tests/data/math/equations.pdf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/yagf2/tests/data/math)
install(FILES tools/jetson_metrics.py
        DESTINATION ${CMAKE_INSTALL_DATADIR}/yagf2/tools)
install(DIRECTORY service/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/yagf2/service
        FILES_MATCHING PATTERN "*.py" PATTERN "*.txt" PATTERN "*.example.json"
        PATTERN "*.service" PATTERN "*.md"
        PATTERN "tests" EXCLUDE PATTERN "__pycache__" EXCLUDE)

include(CTest)
if(BUILD_TESTING)
    qt_add_executable(recognitionresult_tests tests/recognitionresult_tests.cpp)
    target_link_libraries(recognitionresult_tests PRIVATE yagf_ocr Qt6::Network Qt6::Test)
    add_test(NAME recognitionresult_tests COMMAND recognitionresult_tests)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Test)
    qt_add_executable(recognitionliveness_tests tests/recognitionliveness_tests.cpp)
    target_link_libraries(recognitionliveness_tests PRIVATE yagf_processing Qt6::Test)
    add_test(NAME recognitionliveness_tests COMMAND recognitionliveness_tests)
    set_tests_properties(recognitionliveness_tests PROPERTIES TIMEOUT 40)
    qt_add_executable(ocrservices_tests tests/ocrservices_tests.cpp src/ui/ocrservicesdialog.cpp src/ui/ocrservicesdialog.h src/ui/ocrreadingsdialog.cpp src/ui/ocrreadingsdialog.h src/ui/documentcommands.cpp src/ui/documentcommands.h)
    target_include_directories(ocrservices_tests PRIVATE src/ui)
    target_link_libraries(ocrservices_tests PRIVATE yagf_processing Qt6::Widgets Qt6::Network Qt6::Concurrent Qt6::Test)
    add_test(NAME ocrservices_tests COMMAND ocrservices_tests)
    set_tests_properties(ocrservices_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 40)
    qt_add_executable(nativetraining_tests tests/nativetraining_tests.cpp src/ui/nativetrainingdialog.cpp src/ui/nativetrainingdialog.h src/ui/documentcommands.cpp src/ui/documentcommands.h)
    target_include_directories(nativetraining_tests PRIVATE src/ui)
    target_link_libraries(nativetraining_tests PRIVATE yagf_processing yagf_export Qt6::Widgets Qt6::Concurrent Qt6::Network Qt6::Test)
    add_test(NAME nativetraining_tests COMMAND nativetraining_tests)
    set_tests_properties(nativetraining_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(aipagelayout_tests tests/aipagelayout_tests.cpp
        src/ui/aipagelayoutdialog.cpp src/ui/aipagelayoutdialog.h
        src/ui/documentcommands.cpp src/ui/documentcommands.h)
    target_include_directories(aipagelayout_tests PRIVATE src/ui)
    target_link_libraries(aipagelayout_tests PRIVATE yagf_processing yagf_export Qt6::Widgets Qt6::Concurrent Qt6::Network Qt6::Test)
    add_test(NAME aipagelayout_tests COMMAND aipagelayout_tests)
    set_tests_properties(aipagelayout_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(blockfont_tests tests/blockfont_tests.cpp)
    target_link_libraries(blockfont_tests PRIVATE yagf_core)
    add_test(NAME blockfont_tests COMMAND blockfont_tests)
    qt_add_executable(formulatext_tests tests/formulatext_tests.cpp src/ui/documentcommands.cpp src/ui/documentcommands.h)
    target_include_directories(formulatext_tests PRIVATE src/ui)
    target_link_libraries(formulatext_tests PRIVATE yagf_core Qt6::Gui)
    add_test(NAME formulatext_tests COMMAND formulatext_tests)
    qt_add_executable(latexdocument_tests tests/latexdocument_tests.cpp)
    target_link_libraries(latexdocument_tests PRIVATE yagf_export)
    add_test(NAME latexdocument_tests COMMAND latexdocument_tests)
    qt_add_executable(symbolaugmentation_tests tests/symbolaugmentation_tests.cpp)
    target_link_libraries(symbolaugmentation_tests PRIVATE yagf_processing Qt6::Widgets)
    add_test(NAME symbolaugmentation_tests COMMAND symbolaugmentation_tests)
    set_tests_properties(symbolaugmentation_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(symbolrulesdialog_tests tests/symbolrulesdialog_tests.cpp src/ui/symbolrulesdialog.cpp src/ui/symbolrulesdialog.h)
    target_link_libraries(symbolrulesdialog_tests PRIVATE yagf_processing yagf_export Qt6::Widgets Qt6::Concurrent Qt6::Network Qt6::Test)
    target_include_directories(symbolrulesdialog_tests PRIVATE src/ui)
    add_test(NAME symbolrulesdialog_tests COMMAND symbolrulesdialog_tests)
    set_tests_properties(symbolrulesdialog_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(spellingformulas_tests tests/spellingformulas_tests.cpp)
    target_link_libraries(spellingformulas_tests PRIVATE yagf_processing Qt6::Test)
    add_test(NAME spellingformulas_tests COMMAND spellingformulas_tests)
    set_tests_properties(spellingformulas_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(smartblocks_tests tests/smartblocks_tests.cpp)
    target_link_libraries(smartblocks_tests PRIVATE yagf_ocr Qt6::Test)
    add_test(NAME smartblocks_tests COMMAND smartblocks_tests)
    set_tests_properties(smartblocks_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(experimentdialog_tests tests/experimentdialog_tests.cpp src/ui/experimentdialog.cpp src/ui/experimentdialog.h)
    target_link_libraries(experimentdialog_tests PRIVATE yagf_experiments Qt6::Widgets Qt6::Concurrent Qt6::Test)
    target_include_directories(experimentdialog_tests PRIVATE src/ui)
    add_test(NAME experimentdialog_tests COMMAND experimentdialog_tests)
    set_tests_properties(experimentdialog_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(experiment_tests tests/experiment_tests.cpp)
    target_link_libraries(experiment_tests PRIVATE yagf_experiments Qt6::Test)
    add_test(NAME experiment_tests COMMAND experiment_tests)
    set_tests_properties(experiment_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(scanneddocument_tests tests/scanneddocument_tests.cpp)
    target_link_libraries(scanneddocument_tests PRIVATE yagf_processing yagf_export)
    target_compile_definitions(scanneddocument_tests PRIVATE YAGF_SCAN_PATH="${CMAKE_CURRENT_SOURCE_DIR}/ocr-examples/Scanned Document.pdf")
    add_test(NAME scanneddocument_tests COMMAND scanneddocument_tests)
    set_tests_properties(scanneddocument_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;OMP_THREAD_LIMIT=2" SKIP_RETURN_CODE 77)
    qt_add_executable(remoteocrtiling_tests tests/remoteocrtiling_tests.cpp)
    target_link_libraries(remoteocrtiling_tests PRIVATE yagf_ocr Qt6::Network Qt6::Test)
    add_test(NAME remoteocrtiling_tests COMMAND remoteocrtiling_tests)
    qt_add_executable(preprocessdialog_tests tests/preprocessdialog_tests.cpp src/ui/preprocessdialog.cpp src/ui/preprocessdialog.h)
    target_link_libraries(preprocessdialog_tests PRIVATE yagf_processing Qt6::Widgets Qt6::Test)
    target_include_directories(preprocessdialog_tests PRIVATE src/ui)
    add_test(NAME preprocessdialog_tests COMMAND preprocessdialog_tests)
    set_tests_properties(preprocessdialog_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(verificationcontroller_tests tests/verificationcontroller_tests.cpp
        src/ui/verificationcontroller.cpp src/ui/verificationcontroller.h)
    target_link_libraries(verificationcontroller_tests PRIVATE yagf_core Qt6::Widgets Qt6::Test)
    target_include_directories(verificationcontroller_tests PRIVATE src/ui)
    add_test(NAME verificationcontroller_tests COMMAND verificationcontroller_tests)
    set_tests_properties(verificationcontroller_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(release21_tests tests/release21_tests.cpp)
    target_link_libraries(release21_tests PRIVATE yagf_processing yagf_export Qt6::Test Qt6::Gui)
    add_test(NAME release21_tests COMMAND release21_tests)
    set_tests_properties(release21_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(pdfpagelayout_tests tests/pdfpagelayout_tests.cpp)
    target_link_libraries(pdfpagelayout_tests PRIVATE yagf_export)
    add_test(NAME pdfpagelayout_tests COMMAND pdfpagelayout_tests)
    set_tests_properties(pdfpagelayout_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(fontestimator_tests tests/fontestimator_tests.cpp)
    target_link_libraries(fontestimator_tests PRIVATE yagf_processing yagf_export Qt6::Gui)
    add_test(NAME fontestimator_tests COMMAND fontestimator_tests)
    set_tests_properties(fontestimator_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" SKIP_RETURN_CODE 77)
    qt_add_executable(djvu_tests tests/djvu_tests.cpp)
    target_link_libraries(djvu_tests PRIVATE yagf_processing yagf_export)
    target_compile_definitions(djvu_tests PRIVATE YAGF_APP_PATH="$<TARGET_FILE:yagf2>")
    add_dependencies(djvu_tests yagf2)
    add_test(NAME djvu_tests COMMAND djvu_tests)
    set_tests_properties(djvu_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" SKIP_RETURN_CODE 77)
    qt_add_executable(spellhighlighter_tests tests/spellhighlighter_tests.cpp
        src/ui/spellhighlighter.cpp src/ui/spellhighlighter.h)
    target_link_libraries(spellhighlighter_tests PRIVATE yagf_core Qt6::Widgets)
    target_include_directories(spellhighlighter_tests PRIVATE src/ui)
    add_test(NAME spellhighlighter_tests COMMAND spellhighlighter_tests)
    set_tests_properties(spellhighlighter_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
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
        src/ui/blockminiaturelist.cpp src/ui/blockminiaturelist.h
        src/ui/verificationcontroller.cpp src/ui/verificationcontroller.h
        src/ui/mainwindowreview.cpp src/ui/experimentdialog.cpp src/ui/experimentdialog.h
        src/ui/mainwindow.cpp src/ui/mainwindow.h src/ui/formulaeditor.cpp src/ui/formulaeditor.h
        src/ui/restorationdialog.cpp src/ui/restorationdialog.h
        src/ui/preprocessdialog.cpp src/ui/preprocessdialog.h
        src/ui/layoutdialog.cpp src/ui/layoutdialog.h
        src/ui/scannerdialog.cpp src/ui/scannerdialog.h
        src/ui/documentcommands.cpp src/ui/documentcommands.h resources/icons.qrc)
    target_sources(mainwindow_tests PRIVATE src/ui/nativetrainingdialog.cpp src/ui/nativetrainingdialog.h)
    target_sources(mainwindow_tests PRIVATE src/ui/ocrservicesdialog.cpp src/ui/ocrservicesdialog.h src/ui/ocrreadingsdialog.cpp src/ui/ocrreadingsdialog.h)
    target_sources(mainwindow_tests PRIVATE src/ui/aipagelayoutdialog.cpp src/ui/aipagelayoutdialog.h)
    target_sources(mainwindow_tests PRIVATE src/ui/aireviewdialog.cpp src/ui/aireviewdialog.h)
    target_sources(mainwindow_tests PRIVATE src/ui/spellhighlighter.cpp src/ui/spellhighlighter.h)
    target_sources(mainwindow_tests PRIVATE src/ui/symbolrulesdialog.cpp src/ui/symbolrulesdialog.h)
    target_link_libraries(mainwindow_tests PRIVATE yagf_experiments yagf_processing yagf_export Qt6::Widgets Qt6::Concurrent Qt6::Network Qt6::Test)
    target_include_directories(mainwindow_tests PRIVATE src/ui)
    add_test(NAME mainwindow_tests COMMAND mainwindow_tests)
    set_tests_properties(mainwindow_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(aireviewdialog_tests tests/aireviewdialog_tests.cpp
        src/ui/aireviewdialog.cpp src/ui/aireviewdialog.h)
    target_link_libraries(aireviewdialog_tests PRIVATE yagf_imaging Qt6::Widgets Qt6::Network Qt6::Test)
    target_include_directories(aireviewdialog_tests PRIVATE src/ui)
    add_test(NAME aireviewdialog_tests COMMAND aireviewdialog_tests)
    set_tests_properties(aireviewdialog_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(projectfile_tests tests/projectfile_tests.cpp)
    target_link_libraries(projectfile_tests PRIVATE yagf_core)
    add_test(NAME projectfile_tests COMMAND projectfile_tests)
    qt_add_executable(pagerestoration_tests tests/pagerestoration_tests.cpp)
    target_link_libraries(pagerestoration_tests PRIVATE yagf_export yagf_imaging)
    add_test(NAME pagerestoration_tests COMMAND pagerestoration_tests)
    set_tests_properties(pagerestoration_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(imageprocessor_tests tests/imageprocessor_tests.cpp)
    target_link_libraries(imageprocessor_tests PRIVATE yagf_imaging)
    add_test(NAME imageprocessor_tests COMMAND imageprocessor_tests)
    qt_add_executable(scientificbook_tests tests/scientificbook_tests.cpp)
    target_link_libraries(scientificbook_tests PRIVATE yagf_export)
    add_test(NAME scientificbook_tests COMMAND scientificbook_tests)
    qt_add_executable(mathrecognition_tests tests/mathrecognition_tests.cpp)
    target_link_libraries(mathrecognition_tests PRIVATE yagf_processing yagf_export Qt6::Test)
    target_compile_definitions(mathrecognition_tests PRIVATE MATH_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/math")
    add_test(NAME mathrecognition_tests COMMAND mathrecognition_tests)
    set_tests_properties(mathrecognition_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;OMP_THREAD_LIMIT=2")
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
    target_link_libraries(remoteocrengine_tests PRIVATE yagf_ocr Qt6::Network)
    add_test(NAME remoteocrengine_tests COMMAND remoteocrengine_tests)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME packaging_tests COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_packaging.py)
        set_tests_properties(packaging_tests PROPERTIES TIMEOUT 30)
        add_test(NAME public_files_tests COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_public_files.py)
        add_test(NAME benchmark_cli_tests COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_cli.py $<TARGET_FILE:yagf2-benchmark>)
        set_tests_properties(benchmark_cli_tests PROPERTIES TIMEOUT 45)
        add_test(NAME jetson_metrics_tests COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_jetson_metrics.py)
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
        target_link_libraries(pdfexporter_tests PRIVATE yagf_export Qt6::Gui)
        add_test(NAME pdfexporter_tests COMMAND pdfexporter_tests)
        set_tests_properties(pdfexporter_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(packaging/CPackConfig.cmake)
endif()

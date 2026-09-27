cmake_minimum_required(VERSION 3.24)
project(TEKITO VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The input method DLL is loaded into every app that takes text; link the
# C++ runtime statically so it never depends on the app's runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(TEKITO_BUILD_TESTS "Build the unit tests" ON)
option(TEKITO_BUILD_BENCHMARKS "Build the engine latency benchmark" OFF)
option(TEKITO_BUILD_EVAL "Build the offline accuracy evaluation" OFF)

set(TEKITO_WARNINGS /W4 /permissive- /EHsc /utf-8)
set(TEKITO_WINDOWS_DEFINES UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# ---------------------------------------------------------------------------
# Engine and user data (no Windows UI)

add_library(tekito_core STATIC
    src/Core/AutoApplyPolicy.cpp
    src/Core/BuiltinLexicon.cpp
    src/Core/CandidateEngine.cpp
    src/Core/CandidateGenerator.cpp
    src/Core/CandidateRanker.cpp
    src/Core/ConversionEngine.cpp
    src/Core/ExternalLexiconProvider.cpp
    src/Core/ExternalRankingProviders.cpp
    src/Core/ExternalSlangProvider.cpp
    src/Core/InputStateMachine.cpp
    src/Core/Japanese/JapaneseComposer.cpp
    src/Core/Japanese/JapaneseConverter.cpp
    src/Core/Japanese/JapaneseDictionary.cpp
    src/Core/Japanese/KanaText.cpp
    src/Core/Japanese/MappedFile.cpp
    src/Core/Japanese/RomajiTable.cpp
    src/Core/LexiconProvider.cpp
    src/Core/MisspellingProvider.cpp
    src/Core/PolicyEngine.cpp
    src/Core/SpaceBoundaryPolicy.cpp
    src/Core/SymSpellIndex.cpp
    src/Core/TypoModel.cpp
    src/Core/UserDictionary.cpp
    src/Core/UserLearning.cpp
    src/Core/UserLexiconProvider.cpp
)
target_include_directories(tekito_core PUBLIC src)
target_compile_options(tekito_core PRIVATE ${TEKITO_WARNINGS})

set(TEKITO_SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/sqlite-amalgamation-3530400")
add_library(tekito_sqlite STATIC "${TEKITO_SQLITE_DIR}/sqlite3.c")
target_include_directories(tekito_sqlite PUBLIC "${TEKITO_SQLITE_DIR}")
target_compile_definitions(tekito_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
target_compile_options(tekito_sqlite PRIVATE /W0)

add_library(tekito_userdata STATIC
    src/UserData/DataPackValidation.cpp
    src/UserData/RuntimeModeState.cpp
    src/UserData/SqliteUserDictionaryRepository.cpp
    src/UserData/UserDictionaryFile.cpp
)
target_link_libraries(tekito_userdata PUBLIC tekito_core tekito_sqlite bcrypt)

add_library(tekito_dictionary STATIC
    src/Dictionary/DictionaryService.cpp
    src/Dictionary/ExternalDictionaryProvider.cpp
)
target_link_libraries(tekito_dictionary PUBLIC tekito_core)

# ---------------------------------------------------------------------------
# The input method

set(TEKITO_TSF_OUTPUT_NAME "Tekito.Tsf" CACHE STRING "File name of the input method DLL")

# The candidate window renders with Windows.UI.Composition (C++/WinRT from
# the Windows SDK) on Direct3D 11 / Direct2D surfaces.
set(TEKITO_CANDIDATE_WINDOW_LIBS d2d1 dwrite dwmapi d3d11 runtimeobject CoreMessaging)

add_library(Tekito.Tsf SHARED
    src/Tsf/CandidateWindow.cpp
    src/Tsf/ClassFactory.cpp
    src/Tsf/DisplayAttributes.cpp
    src/Tsf/DllMain.cpp
    src/Tsf/EditSession.cpp
    src/Tsf/TextServiceJapanese.cpp
    src/Tsf/ModeLangBarItem.cpp
    src/Tsf/Registration.cpp
    src/Tsf/TekitoGuids.cpp
    src/Tsf/TextService.cpp
    src/Tsf/Tekito.Tsf.def
    assets/tekito_tsf.rc
)
target_include_directories(Tekito.Tsf PRIVATE src ${CMAKE_SOURCE_DIR})
target_compile_definitions(Tekito.Tsf PRIVATE ${TEKITO_WINDOWS_DEFINES})
# A DLL named like Tekito.Tsf.Trace writes its diagnostic log without the
# TEKITO_TSF_TRACE environment variable, which hosts rarely pass through.
if(TEKITO_TSF_OUTPUT_NAME MATCHES "(^|\\.)Trace[0-9]*$")
    target_compile_definitions(Tekito.Tsf PRIVATE TEKITO_TSF_TRACE_DEFAULT=1)
endif()
target_compile_options(Tekito.Tsf PRIVATE ${TEKITO_WARNINGS})
target_link_libraries(Tekito.Tsf PRIVATE
    tekito_core tekito_userdata
    ole32 oleaut32 advapi32 user32 gdi32 uuid
    ${TEKITO_CANDIDATE_WINDOW_LIBS} delayimp
)
set_target_properties(Tekito.Tsf PROPERTIES OUTPUT_NAME "${TEKITO_TSF_OUTPUT_NAME}")
# Load the rendering stack only once a candidate list is first shown.
target_link_options(Tekito.Tsf PRIVATE /DELAYLOAD:d3d11.dll /DELAYLOAD:CoreMessaging.dll)

# ---------------------------------------------------------------------------
# TEKITO.exe: the Settings window

add_executable(TEKITO WIN32
    apps/tekito/main.cpp
    apps/tekito-settings/main.cpp
    assets/tekito_app.rc
)
target_include_directories(TEKITO PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} src third_party/webview2/include)
target_compile_definitions(TEKITO PRIVATE ${TEKITO_WINDOWS_DEFINES})
target_compile_options(TEKITO PRIVATE /utf-8)
target_compile_definitions(TEKITO PRIVATE TEKITO_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(TEKITO PRIVATE
    tekito_userdata tekito_dictionary
    user32 gdi32 comctl32 comdlg32 shell32 advapi32
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/lib/x64/WebView2LoaderStatic.lib"
)
# The page itself is built by `npm run build` in apps/tekito-settings/ui.
add_custom_command(TARGET TEKITO POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/tekito-settings/ui/dist"
        "$<TARGET_FILE_DIR:TEKITO>/settings-ui"
)

# ---------------------------------------------------------------------------
# Setup program. The package-release script appends the package to it.

set(TEKITO_SETUP_LIBS
    user32 ole32 shell32 shlwapi advapi32 gdi32 d2d1 dwrite dwmapi windowscodecs
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/lib/x64/WebView2LoaderStatic.lib"
)

add_executable(TekitoSetupStub WIN32 installer/TekitoSetup.cpp installer/TekitoSetup.rc)
target_include_directories(TekitoSetupStub PRIVATE third_party/webview2/include)
target_compile_definitions(TekitoSetupStub PRIVATE ${TEKITO_WINDOWS_DEFINES})
target_compile_options(TekitoSetupStub PRIVATE /utf-8)
target_link_libraries(TekitoSetupStub PRIVATE ${TEKITO_SETUP_LIBS})
set_target_properties(TekitoSetupStub PROPERTIES LINK_FLAGS "/MANIFEST:NO")

# ---------------------------------------------------------------------------
# Tests and tools

if(TEKITO_BUILD_TESTS)
    enable_testing()
    add_executable(tekito_core_tests tests/CoreTests.cpp)
    target_compile_options(tekito_core_tests PRIVATE /utf-8)
    target_link_libraries(tekito_core_tests PRIVATE tekito_userdata tekito_dictionary)
    add_test(NAME tekito_core_tests COMMAND tekito_core_tests)
    set_tests_properties(tekito_core_tests PROPERTIES
        ENVIRONMENT "TEKITO_DATA_PACK_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data")

    add_executable(tekito_japanese_tests tests/JapaneseTests.cpp)
    target_compile_options(tekito_japanese_tests PRIVATE ${TEKITO_WARNINGS})
    target_link_libraries(tekito_japanese_tests PRIVATE tekito_core)
    # A small dictionary built from Mozc for the conversion tests.
    target_compile_definitions(tekito_japanese_tests PRIVATE
        TEKITO_TEST_DATA_DIR=L"${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    add_test(NAME tekito_japanese_tests COMMAND tekito_japanese_tests)
    set_tests_properties(tekito_japanese_tests PROPERTIES
        ENVIRONMENT "TEKITO_DATA_PACK_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data")

    # Renders the candidate window with sample candidates and screenshots it,
    # for reviewing its look without registering the input method.
    add_executable(tekito_candidate_window_preview
        tests/visual/CandidateWindowPreview.cpp
        src/Tsf/CandidateWindow.cpp
        assets/tekito_tsf.rc
    )
    target_include_directories(tekito_candidate_window_preview PRIVATE src ${CMAKE_SOURCE_DIR})
    target_compile_definitions(tekito_candidate_window_preview PRIVATE
        ${TEKITO_WINDOWS_DEFINES} TEKITO_CANDIDATE_WINDOW_PREVIEW)
    target_compile_options(tekito_candidate_window_preview PRIVATE ${TEKITO_WARNINGS})
    target_link_libraries(tekito_candidate_window_preview PRIVATE
        ole32 oleaut32 advapi32 user32 gdi32 ${TEKITO_CANDIDATE_WINDOW_LIBS})

    # Records the frames of the README typing demo with the real engine.
    add_executable(tekito_typing_demo
        tests/visual/TypingDemo.cpp
        src/Tsf/CandidateWindow.cpp
        assets/tekito_tsf.rc
    )
    target_include_directories(tekito_typing_demo PRIVATE src ${CMAKE_SOURCE_DIR})
    target_compile_definitions(tekito_typing_demo PRIVATE
        ${TEKITO_WINDOWS_DEFINES} TEKITO_CANDIDATE_WINDOW_PREVIEW)
    target_compile_options(tekito_typing_demo PRIVATE ${TEKITO_WARNINGS})
    target_link_libraries(tekito_typing_demo PRIVATE
        tekito_core ole32 oleaut32 advapi32 user32 gdi32 ${TEKITO_CANDIDATE_WINDOW_LIBS})

    # Renders one setup screen without elevation and saves a screenshot.
    add_executable(tekito_setup_preview WIN32 installer/TekitoSetup.cpp installer/TekitoSetup.rc)
    target_include_directories(tekito_setup_preview PRIVATE third_party/webview2/include)
    target_compile_definitions(tekito_setup_preview PRIVATE
        ${TEKITO_WINDOWS_DEFINES} TEKITO_SETUP_PREVIEW)
    target_compile_options(tekito_setup_preview PRIVATE /utf-8)
    target_link_libraries(tekito_setup_preview PRIVATE ${TEKITO_SETUP_LIBS})
    set_target_properties(tekito_setup_preview PROPERTIES LINK_FLAGS "/MANIFEST:NO")
endif()

if(TEKITO_BUILD_BENCHMARKS)
    add_executable(tekito_core_benchmark benchmarks/CoreBenchmark.cpp)
    target_compile_definitions(tekito_core_benchmark PRIVATE NOMINMAX)
    target_link_libraries(tekito_core_benchmark PRIVATE tekito_core psapi)

    add_executable(tekito_ja_benchmark benchmarks/JapaneseBenchmark.cpp)
    target_compile_definitions(tekito_ja_benchmark PRIVATE NOMINMAX UNICODE _UNICODE)
    target_compile_options(tekito_ja_benchmark PRIVATE ${TEKITO_WARNINGS})
    target_link_libraries(tekito_ja_benchmark PRIVATE tekito_core psapi)
endif()

if(TEKITO_BUILD_EVAL)
    add_executable(tekito_core_eval eval/EvalHarness.cpp)
    target_compile_definitions(tekito_core_eval PRIVATE NOMINMAX)
    target_link_libraries(tekito_core_eval PRIVATE tekito_core)
endif()

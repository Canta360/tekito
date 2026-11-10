cmake_minimum_required(VERSION 3.24)
project(TEKITO VERSION 0.7.3 LANGUAGES C CXX)

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
option(TEKITO_BUILD_AI "Build TekitoAI.exe, which runs the optional AI models (ONNX Runtime, Windows ML)" ON)

# Without Windows only the engine, its tests and the evaluations build.
if(NOT WIN32)
    include(cmake/NonWindows.cmake)
    return()
endif()

set(TEKITO_WARNINGS /W4 /permissive- /EHsc /utf-8)
set(TEKITO_WINDOWS_DEFINES UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# ---------------------------------------------------------------------------
# Engine and user data (no Windows UI)

add_library(tekito_core STATIC
    src/Core/AutoApplyPolicy.cpp
    src/Core/Japanese/AiJudge.cpp
    src/Core/BuiltinLexicon.cpp
    src/Core/CandidateEngine.cpp
    src/Core/EnglishSpelling.cpp
    src/Core/CandidateGenerator.cpp
    src/Core/CandidateRanker.cpp
    src/Core/ConversionEngine.cpp
    src/Core/ExternalLexiconProvider.cpp
    src/Core/ExternalRankingProviders.cpp
    src/Core/ExternalSlangProvider.cpp
    src/Core/InputStateMachine.cpp
    src/Core/Japanese/JapaneseComposer.cpp
    src/Core/Japanese/JapaneseConverter.cpp
    src/Core/Japanese/JapaneseData.cpp
    src/Core/Japanese/JapaneseDictionary.cpp
    src/Core/Japanese/JapaneseKeys.cpp
    src/Core/Japanese/JapaneseLearning.cpp
    src/Core/Japanese/JapaneseUserDictionary.cpp
    src/Core/Japanese/KanaText.cpp
    src/Core/Japanese/ChunkJudge.cpp
    src/Core/Japanese/KanaSlips.cpp
    src/Core/Japanese/KeyConverter.cpp
    src/Core/Japanese/LanguageModel.cpp
    src/Core/Japanese/Collocations.cpp
    src/Core/Japanese/Compounds.cpp
    src/Core/Japanese/EnglishTies.cpp
    src/Core/Japanese/Homophones.cpp
    src/Core/Japanese/EdgeScorer.cpp
    src/Core/Japanese/LetterOdds.cpp
    src/Core/Japanese/Loanwords.cpp
    src/Core/Japanese/MappedFile.cpp
    src/Core/Japanese/Notation.cpp
    src/Core/Japanese/NeuralMath.cpp
    src/Core/Japanese/PostalCodes.cpp
    src/Core/Japanese/Meanings.cpp
    src/Core/Japanese/KanjiSets.cpp
    src/Core/Japanese/RomajiTable.cpp
    src/Core/Japanese/SentenceReranker.cpp
    src/Core/Japanese/SortedTsv.cpp
    src/Core/Japanese/TopicVectors.cpp
    src/Core/Japanese/WrittenForms.cpp
    src/Core/LexiconProvider.cpp
    src/Core/MisspellingProvider.cpp
    src/Core/PolicyEngine.cpp
    src/Core/RunTogether.cpp
    src/Core/SpaceBoundaryPolicy.cpp
    src/Core/SpecialConversions.cpp
    src/Core/SpellingVariants.cpp
    src/Core/SymSpellIndex.cpp
    src/Core/TypoModel.cpp
    src/Core/UserDictionary.cpp
    src/Core/UserLearning.cpp
    src/Core/UserLexiconProvider.cpp
)
target_include_directories(tekito_core PUBLIC src)
target_compile_options(tekito_core PRIVATE ${TEKITO_WARNINGS})
target_link_libraries(tekito_core PUBLIC advapi32)

# SQLite with page encryption (SQLite3 Multiple Ciphers, the same SQLite).
set(TEKITO_SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite3mc")
add_library(tekito_sqlite STATIC "${TEKITO_SQLITE_DIR}/sqlite3mc_amalgamation.c")
target_include_directories(tekito_sqlite PUBLIC "${TEKITO_SQLITE_DIR}")
target_compile_definitions(tekito_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
target_compile_options(tekito_sqlite PRIVATE /W0)

add_library(tekito_userdata STATIC
    src/UserData/AiModelInstall.cpp
    src/UserData/DataPackValidation.cpp
    src/UserData/ImeDictionaryImport.cpp
    src/UserData/PostalCodeImport.cpp
    src/UserData/RuntimeModeState.cpp
    src/UserData/SqliteUserDictionaryRepository.cpp
    src/UserData/UserDataEncryption.cpp
    src/UserData/UserDictionaryFile.cpp
    third_party/monocypher/monocypher.c
)
# Monocypher (CC0 / BSD-2-Clause): Argon2id and XChaCha20-Poly1305 for the
# recovery code of the user's encrypted database.
target_include_directories(tekito_userdata PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/monocypher")
target_link_libraries(tekito_userdata PUBLIC tekito_core tekito_sqlite bcrypt crypt32)

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
    src/Tsf/CandidateListElement.cpp
    src/Tsf/CandidateWindow.cpp
    src/Tsf/ClassFactory.cpp
    src/Tsf/DisplayAttributes.cpp
    src/Tsf/DllMain.cpp
    src/Tsf/EditSession.cpp
    src/Tsf/JapaneseKeyPress.cpp
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
    ole32 oleaut32 advapi32 user32 gdi32 uuid winmm
    ${TEKITO_CANDIDATE_WINDOW_LIBS} delayimp
)
set_target_properties(Tekito.Tsf PROPERTIES OUTPUT_NAME "${TEKITO_TSF_OUTPUT_NAME}")
# Load the rendering stack only once a candidate list is first shown, and the
# sound library only once Cat plays.
target_link_options(Tekito.Tsf PRIVATE /DELAYLOAD:d3d11.dll /DELAYLOAD:CoreMessaging.dll /DELAYLOAD:winmm.dll)

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
    user32 gdi32 comctl32 comdlg32 shell32 advapi32 urlmon dxgi
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/lib/x64/WebView2LoaderStatic.lib"
)
# The page itself is built by `npm run build` in apps/tekito-settings/ui.
add_custom_command(TARGET TEKITO POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/tekito-settings/ui/dist"
        "$<TARGET_FILE_DIR:TEKITO>/settings-ui"
)

# ---------------------------------------------------------------------------
# TekitoAI.exe: runs the optional AI models for the input method
# (apps/tekito-ai) with ONNX Runtime as Windows ML ships it. The Windows ML
# NuGet package is fetched at a fixed version (only its C/C++ APIs are
# used: no Windows App SDK runtime); its DLLs go next to TekitoAI.exe.

if(TEKITO_BUILD_AI)
    include(FetchContent)
    set(TEKITO_WINML_VERSION "2.5.96")
    FetchContent_Declare(winml
        URL https://api.nuget.org/v3-flatcontainer/microsoft.windows.ai.machinelearning/${TEKITO_WINML_VERSION}/microsoft.windows.ai.machinelearning.${TEKITO_WINML_VERSION}.nupkg
        URL_HASH SHA256=35d6e84feb4142e8cecaf36704fa3ab0fc8251d892850fb80e8341f7d09653a3
        DOWNLOAD_NAME winml.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(winml)
    find_package(microsoft.windows.ai.machinelearning CONFIG REQUIRED PATHS "${winml_SOURCE_DIR}/build/cmake" NO_DEFAULT_PATH)

    add_executable(TekitoAI apps/tekito-ai/main.cpp apps/tekito-ai/ByteBpe.cpp)
    target_compile_definitions(TekitoAI PRIVATE ${TEKITO_WINDOWS_DEFINES})
    target_compile_options(TekitoAI PRIVATE ${TEKITO_WARNINGS})
    target_link_libraries(TekitoAI PRIVATE tekito_core WindowsML::Api WindowsML::OnnxRuntime icu dxgi delayimp)
    # The processor's runtime pack has no Windows ML: its DLL is loaded only
    # when it is there, and DirectML is looked up in ONNX Runtime.
    target_link_options(TekitoAI PRIVATE /DELAYLOAD:Microsoft.Windows.AI.MachineLearning.dll)
    add_custom_command(TARGET TekitoAI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:TekitoAI> "${WINML_DIRECTML_DLL}"
                "$<TARGET_FILE_DIR:TekitoAI>"
        COMMAND_EXPAND_LISTS
    )

    # The processor's runtime pack: TekitoAI.exe with ONNX Runtime's own
    # release (MIT, the same version as Windows ML's), in ai-runtime-cpu/.
    set(TEKITO_ORT_VERSION "1.28.3")
    FetchContent_Declare(ort_cpu
        URL https://github.com/microsoft/onnxruntime/releases/download/v${TEKITO_ORT_VERSION}/onnxruntime-win-x64-${TEKITO_ORT_VERSION}.zip
        URL_HASH SHA256=1d6fab48e85f948436af7c8c971d2c145cf224e2c444755dec894f8b0de11a83
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(ort_cpu)
    add_custom_command(TARGET TekitoAI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TekitoAI>/ai-runtime-cpu"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:TekitoAI>" "${ort_cpu_SOURCE_DIR}/lib/onnxruntime.dll"
                "${ort_cpu_SOURCE_DIR}/LICENSE" "${ort_cpu_SOURCE_DIR}/ThirdPartyNotices.txt"
                "$<TARGET_FILE_DIR:TekitoAI>/ai-runtime-cpu"
    )
endif()

# ---------------------------------------------------------------------------
# Setup program. The package-release script appends the package to it.

set(TEKITO_SETUP_LIBS
    user32 ole32 oleaut32 shell32 shlwapi advapi32 gdi32 d2d1 dwrite dwmapi windowscodecs mfplay
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
        TEKITO_TEST_DATA_DIR=L"${CMAKE_CURRENT_SOURCE_DIR}/tests/data/ja")
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

    # A window that draws the input method's candidate list itself, as
    # full-screen games do, and logs what the list UI element holds.
    add_executable(tekito_uiless_host WIN32 tests/visual/UilessHost.cpp)
    target_compile_definitions(tekito_uiless_host PRIVATE ${TEKITO_WINDOWS_DEFINES})
    target_link_libraries(tekito_uiless_host PRIVATE ole32 oleaut32 user32 uuid)

    # A window to try Japanese typing in without registering the input
    # method: the text service's key handling, the real candidate window.
    add_executable(tekito_testbed WIN32
        tests/visual/Testbed.cpp
        src/Tsf/CandidateWindow.cpp
        src/Tsf/JapaneseKeyPress.cpp
        assets/tekito_tsf.rc
    )
    target_include_directories(tekito_testbed PRIVATE src ${CMAKE_SOURCE_DIR})
    target_compile_definitions(tekito_testbed PRIVATE
        ${TEKITO_WINDOWS_DEFINES} TEKITO_CANDIDATE_WINDOW_PREVIEW)
    target_compile_options(tekito_testbed PRIVATE ${TEKITO_WARNINGS})
    target_link_libraries(tekito_testbed PRIVATE
        tekito_userdata tekito_core imm32 ole32 oleaut32 advapi32 user32 gdi32 shell32 ${TEKITO_CANDIDATE_WINDOW_LIBS})

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
    add_executable(tekito_core_benchmark benchmarks/en/CoreBenchmark.cpp)
    target_compile_definitions(tekito_core_benchmark PRIVATE NOMINMAX)
    target_link_libraries(tekito_core_benchmark PRIVATE tekito_core psapi)

    add_executable(tekito_ja_benchmark benchmarks/ja/JapaneseBenchmark.cpp)
    target_compile_definitions(tekito_ja_benchmark PRIVATE NOMINMAX UNICODE _UNICODE)
    target_compile_options(tekito_ja_benchmark PRIVATE ${TEKITO_WARNINGS})
    target_link_libraries(tekito_ja_benchmark PRIVATE tekito_core psapi)
endif()

if(TEKITO_BUILD_EVAL)
    add_executable(tekito_core_eval eval/en/EvalHarness.cpp eval/en/EnglishTypingStudy.cpp)
    target_compile_definitions(tekito_core_eval PRIVATE NOMINMAX)
    target_link_libraries(tekito_core_eval PRIVATE tekito_core)

    add_executable(tekito_ja_eval eval/ja/JapaneseEval.cpp eval/ja/JapaneseTypingStudy.cpp)
    target_compile_definitions(tekito_ja_eval PRIVATE NOMINMAX)
    target_compile_options(tekito_ja_eval PRIVATE ${TEKITO_WARNINGS})
    target_link_libraries(tekito_ja_eval PRIVATE tekito_core)

    add_executable(tekito_ja_probe eval/ja/JapaneseProbe.cpp)
    target_compile_definitions(tekito_ja_probe PRIVATE NOMINMAX)
    target_compile_options(tekito_ja_probe PRIVATE ${TEKITO_WARNINGS})
    target_link_libraries(tekito_ja_probe PRIVATE tekito_core tekito_userdata)
endif()

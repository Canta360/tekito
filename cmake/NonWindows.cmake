# Builds the engine without Windows (Linux, with GCC or Clang): tekito_core,
# the Japanese unit tests and the offline evaluations, so they can be run
# and measured where there is no Windows. The input method, Settings, the
# setup program and the user data (which uses Windows' bcrypt) are Windows
# only and are not built here.
#
#   cmake -S . -B out/build/linux -DTEKITO_BUILD_EVAL=ON
#   cmake --build out/build/linux -j
#   ctest --test-dir out/build/linux
#
# NFKC comes from ICU here (libicu-dev); Windows has it built in.

find_package(ICU REQUIRED COMPONENTS uc)

set(TEKITO_WARNINGS -Wall -Wextra)

# The same files as tekito_core in CMakeLists.txt: everything in src/Core
# and src/Core/Japanese.
file(GLOB TEKITO_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Core/Japanese/*.cpp")
add_library(tekito_core STATIC ${TEKITO_CORE_SOURCES})
target_include_directories(tekito_core PUBLIC src)
target_link_libraries(tekito_core PUBLIC ICU::uc)

if(TEKITO_BUILD_TESTS)
    enable_testing()
    add_executable(tekito_japanese_tests tests/JapaneseTests.cpp)
    target_link_libraries(tekito_japanese_tests PRIVATE tekito_core)
    target_compile_definitions(tekito_japanese_tests PRIVATE
        TEKITO_TEST_DATA_DIR=L"${CMAKE_CURRENT_SOURCE_DIR}/tests/data/ja")
    add_test(NAME tekito_japanese_tests COMMAND tekito_japanese_tests)
    set_tests_properties(tekito_japanese_tests PROPERTIES
        ENVIRONMENT "TEKITO_DATA_PACK_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data")
endif()

if(TEKITO_BUILD_EVAL)
    add_executable(tekito_core_eval eval/en/EvalHarness.cpp eval/en/EnglishTypingStudy.cpp)
    target_link_libraries(tekito_core_eval PRIVATE tekito_core)

    add_executable(tekito_ja_eval eval/ja/JapaneseEval.cpp eval/ja/JapaneseTypingStudy.cpp)
    target_link_libraries(tekito_ja_eval PRIVATE tekito_core)
endif()

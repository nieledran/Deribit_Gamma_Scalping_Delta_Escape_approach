cmake_minimum_required(VERSION 3.24)
project(DeribitGamma VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INSTALL_BINDIR ".")
if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive- /EHsc)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets WebSockets Network Sql)
qt_standard_project_setup()
configure_file(src/config/build_info.h.in generated/config/build_info.h @ONLY)
add_library(gamma_core src/engine/domain.cpp src/engine/hedger.cpp src/analytics/model.cpp)
target_include_directories(gamma_core PUBLIC src)
target_link_libraries(gamma_core PUBLIC Qt6::Core)
add_library(gamma_runtime
  src/exchange/client.cpp
  src/storage/store.cpp
  src/trading/worker.cpp
  src/trading/connection.cpp
  src/trading/positions.cpp
  src/trading/market_data.cpp
  src/trading/orders.cpp
  src/trading/reconciliation.cpp
  src/trading/profile_reset.cpp
  src/trading/controls.cpp
  src/trading/execution.cpp
  src/trading/hedging.cpp
  src/analytics/ledger.cpp
  src/analytics/ledger_export.cpp
  src/analytics/worker.cpp
  src/analytics/history.cpp)
target_link_libraries(gamma_runtime PUBLIC gamma_core Qt6::WebSockets Qt6::Network Qt6::Sql)
add_library(gamma_ui
  src/ui/window.cpp
  src/ui/window_setup.cpp
  src/ui/window_render.cpp
  src/ui/window_tools.cpp
  src/ui/window_demo.cpp
  src/ui/profile_chart.cpp
  src/ui/theme.cpp
  src/ui/startup.cpp
  src/ui/dialogs.cpp)
target_link_libraries(gamma_ui PUBLIC gamma_runtime Qt6::Widgets)
target_include_directories(gamma_ui PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
qt_add_executable(DeribitGamma WIN32 src/main.cpp)
target_link_libraries(DeribitGamma PRIVATE gamma_ui)
include(CTest)
if(BUILD_TESTING)
  find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
  qt_add_executable(core_tests tests/core_tests.cpp)
  target_link_libraries(core_tests PRIVATE gamma_core Qt6::Test)
  add_test(NAME hedge_and_model COMMAND core_tests)
  qt_add_executable(ledger_tests tests/ledger_tests.cpp)
  target_link_libraries(ledger_tests PRIVATE gamma_runtime Qt6::Test)
  add_test(NAME accounting_and_recovery COMMAND ledger_tests)
  qt_add_executable(client_tests tests/client_tests.cpp src/exchange/client.cpp)
  target_include_directories(client_tests PRIVATE src)
  target_link_libraries(client_tests PRIVATE Qt6::WebSockets Qt6::Network Qt6::Test)
  add_test(NAME websocket_protocol COMMAND client_tests
    -o "${CMAKE_CURRENT_BINARY_DIR}/client-tests.txt,txt")
  qt_add_executable(runtime_tests tests/runtime_tests.cpp)
  target_link_libraries(runtime_tests PRIVATE gamma_ui Qt6::Test)
  add_test(NAME execution_and_ui COMMAND runtime_tests -platform offscreen
    -o "${CMAKE_CURRENT_BINARY_DIR}/runtime-tests.txt,txt")
  set_tests_properties(execution_and_ui PROPERTIES TIMEOUT 300)
  qt_add_executable(performance_tests tests/performance_tests.cpp)
  target_link_libraries(performance_tests PRIVATE gamma_ui Qt6::Test)
  add_test(NAME offline_performance COMMAND performance_tests -platform offscreen
    -o "${CMAKE_CURRENT_BINARY_DIR}/performance-tests.txt,txt")
  set_tests_properties(offline_performance PROPERTIES TIMEOUT 60)
  # The offscreen platform does not discover Windows fonts automatically.
  # Use the same installed fonts as the real Windows UI, not dummy metrics.
  if(WIN32 AND EXISTS "$ENV{SystemRoot}/Fonts")
    file(TO_CMAKE_PATH "$ENV{SystemRoot}/Fonts" gamma_test_fonts)
    set_tests_properties(execution_and_ui offline_performance PROPERTIES
      ENVIRONMENT "QT_QPA_FONTDIR=${gamma_test_fonts}")
  endif()
endif()
install(TARGETS DeribitGamma RUNTIME DESTINATION .)
install(FILES LICENSE README.md THIRD_PARTY.md start.cmd DESTINATION .)
install(FILES docs/BUILDING.md docs/HEDGE_LIMIT_REVIEW.md DESTINATION docs)
install(DIRECTORY licenses DESTINATION .)
qt_generate_deploy_app_script(TARGET DeribitGamma OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})

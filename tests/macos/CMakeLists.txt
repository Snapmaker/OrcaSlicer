cmake_minimum_required(VERSION 3.16)
project(UninstallWatcherLifecycleTests LANGUAGES CXX)

# Standalone checks: no GUI, slicer dependencies, launchctl calls, or real user-data paths.
enable_testing()
add_executable(uninstall_watcher_lifecycle_tests UninstallWatcherLifecycle_test.cpp)
target_compile_features(uninstall_watcher_lifecycle_tests PRIVATE cxx_std_17)
target_include_directories(uninstall_watcher_lifecycle_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src")
add_test(NAME uninstall_watcher_lifecycle COMMAND uninstall_watcher_lifecycle_tests)

add_executable(uninstall_watcher_lifecycle_policy_tests UninstallWatcherLifecyclePolicy_test.cpp)
target_compile_features(uninstall_watcher_lifecycle_policy_tests PRIVATE cxx_std_17)
target_include_directories(uninstall_watcher_lifecycle_policy_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src")
add_test(NAME uninstall_watcher_lifecycle_policy COMMAND uninstall_watcher_lifecycle_policy_tests)

add_executable(atx-ui-model-tests vol_workspace_model_test.cpp)
target_include_directories(atx-ui-model-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(atx-ui-model-tests PRIVATE GTest::gtest_main atx_warnings)
target_compile_features(atx-ui-model-tests PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(atx-ui-model-tests PROPERTIES LABELS atx_ui)


file(GLOB ATX_KB_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

add_executable(atx-kb-tests ${ATX_KB_TEST_SOURCES})
find_package(Threads REQUIRED)
target_link_libraries(atx-kb-tests PRIVATE atx::kb GTest::gtest_main Threads::Threads atx_warnings)

include(GoogleTest)
gtest_discover_tests(atx-kb-tests DISCOVERY_MODE PRE_TEST)

add_test(NAME KnowledgeBaseCli.ListsVectorGenerations
    COMMAND $<TARGET_FILE:atx-kb-cli> vector-list :memory:)
add_test(NAME KnowledgeBaseCli.RecoversVectorGenerations
    COMMAND $<TARGET_FILE:atx-kb-cli> vector-recover :memory: --minimum-age-seconds 0)
add_test(NAME KnowledgeBaseCli.RejectsUnknownCommand
    COMMAND $<TARGET_FILE:atx-kb-cli> unknown :memory:)
set_tests_properties(KnowledgeBaseCli.RejectsUnknownCommand PROPERTIES WILL_FAIL TRUE)

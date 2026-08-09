file(GLOB ATX_DB_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

add_executable(atx-db-tests ${ATX_DB_TEST_SOURCES})
find_package(Threads REQUIRED)
target_link_libraries(atx-db-tests PRIVATE atx::agent-db GTest::gtest_main Threads::Threads atx_warnings)

include(GoogleTest)
gtest_discover_tests(atx-db-tests DISCOVERY_MODE PRE_TEST)

add_test(NAME AgentDatabaseCli.RejectsMissingWorkspaceValue
    COMMAND $<TARGET_FILE:atx-db-cli> init :memory: --workspace)
set_tests_properties(AgentDatabaseCli.RejectsMissingWorkspaceValue PROPERTIES WILL_FAIL TRUE)

add_test(NAME AgentDatabaseCli.RejectsUnknownOption
    COMMAND $<TARGET_FILE:atx-db-cli> init :memory: --unknown value)
set_tests_properties(AgentDatabaseCli.RejectsUnknownOption PROPERTIES WILL_FAIL TRUE)

add_test(NAME AgentDatabaseCli.AllowsDoubleDashInPositionalValue
    COMMAND $<TARGET_FILE:atx-db-cli> run-create :memory: --workspace)

add_test(NAME AgentDatabaseCli.AcceptsRetryBackoffOptions
    COMMAND $<TARGET_FILE:atx-db-cli> consumer-register :memory: retry-auditor
        --max-deliveries 3 --retry-backoff-seconds 1 --retry-backoff-max-seconds 8
        --retry-jitter full --redrive-rate-per-second 10 --redrive-burst-events 20
        --max-redrives 3)

add_test(NAME AgentDatabaseCli.RejectsUnknownRetryJitter
    COMMAND $<TARGET_FILE:atx-db-cli> consumer-register :memory: retry-auditor
        --retry-jitter decorrelated)
set_tests_properties(AgentDatabaseCli.RejectsUnknownRetryJitter PROPERTIES WILL_FAIL TRUE)

add_test(NAME AgentDatabaseCli.RejectsIncompleteRedriveBudget
    COMMAND $<TARGET_FILE:atx-db-cli> consumer-register :memory: retry-auditor
        --redrive-rate-per-second 10)
set_tests_properties(AgentDatabaseCli.RejectsIncompleteRedriveBudget PROPERTIES WILL_FAIL TRUE)

add_test(NAME AgentDatabaseCli.ReportsEmptyConsumerFleet
    COMMAND $<TARGET_FILE:atx-db-cli> consumer-statuses :memory: --workspace cli-fleet)

add_test(NAME AgentDatabaseCli.RejectsConsumerNameForFleetStatus
    COMMAND $<TARGET_FILE:atx-db-cli> consumer-statuses :memory: unexpected-consumer)
set_tests_properties(AgentDatabaseCli.RejectsConsumerNameForFleetStatus PROPERTIES WILL_FAIL TRUE)

add_test(NAME AgentDatabaseCli.RejectsIncompleteConditionalFleetValidator
    COMMAND $<TARGET_FILE:atx-db-cli> consumer-statuses-if-current :memory: 0)
set_tests_properties(AgentDatabaseCli.RejectsIncompleteConditionalFleetValidator
    PROPERTIES WILL_FAIL TRUE)

add_test(NAME AgentDatabaseCli.RejectsNonnumericConditionalFleetValidator
    COMMAND $<TARGET_FILE:atx-db-cli> consumer-statuses-if-current :memory: not-a-number 0)
set_tests_properties(AgentDatabaseCli.RejectsNonnumericConditionalFleetValidator
    PROPERTIES WILL_FAIL TRUE)

add_test(NAME AgentDatabaseCli.ConsumerFleetStatusIsOrderedAndSecretFree
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:atx-db-cli>
        -DDB=${CMAKE_CURRENT_BINARY_DIR}/consumer-statuses-cli.sqlite
        -P ${CMAKE_CURRENT_SOURCE_DIR}/consumer_statuses_cli_test.cmake)

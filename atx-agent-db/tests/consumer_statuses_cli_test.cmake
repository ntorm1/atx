if(NOT DEFINED CLI OR NOT DEFINED DB)
  message(FATAL_ERROR "CLI and DB are required")
endif()

file(REMOVE "${DB}" "${DB}-wal" "${DB}-shm")

function(run_cli)
  execute_process(
      COMMAND "${CLI}" ${ARGN}
      RESULT_VARIABLE result
      OUTPUT_VARIABLE output
      ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "command failed (${result}): ${error}")
  endif()
  set(CLI_OUTPUT "${output}" PARENT_SCOPE)
endfunction()

run_cli(init "${DB}" --workspace fleet-cli)
run_cli(consumer-register "${DB}" zeta --workspace fleet-cli)
run_cli(consumer-register "${DB}" alpha --subject jobs/alpha --workspace fleet-cli)
set(request_secret "request-secret-must-not-leak")
run_cli(consumer-receive "${DB}" zeta owner-z "${request_secret}"
        --lease-seconds 3600 --limit 1 --workspace fleet-cli)
string(REGEX MATCH "\"delivery_token\":\"([^\"]+)\"" delivery_match "${CLI_OUTPUT}")
if(NOT delivery_match)
  message(FATAL_ERROR "receive output did not contain a delivery token")
endif()
set(delivery_secret "${CMAKE_MATCH_1}")

run_cli(consumer-statuses "${DB}" --workspace fleet-cli)
set(fleet "${CLI_OUTPUT}")
if(NOT fleet MATCHES "\"workspace\":\"fleet-cli\"")
  message(FATAL_ERROR "fleet output did not identify its workspace: ${fleet}")
endif()
if(NOT fleet MATCHES "\"consumer_count\":2")
  message(FATAL_ERROR "fleet output did not report both consumers: ${fleet}")
endif()
string(FIND "${fleet}" "\"name\":\"alpha\"" alpha_position)
string(FIND "${fleet}" "\"name\":\"zeta\"" zeta_position)
if(alpha_position LESS 0 OR zeta_position LESS 0 OR NOT alpha_position LESS zeta_position)
  message(FATAL_ERROR "fleet output is not in stable name order: ${fleet}")
endif()

string(REGEX MATCH "\"observed_at\":\"([^\"]+)\"" observed_match "${fleet}")
set(observed_at "${CMAKE_MATCH_1}")
string(REGEX MATCHALL "\"observed_at\":\"${observed_at}\"" observed_matches "${fleet}")
list(LENGTH observed_matches observed_count)
if(NOT observed_count EQUAL 3)
  message(FATAL_ERROR "fleet and nested statuses do not share one observation time: ${fleet}")
endif()
string(REGEX MATCH "\"event_high_watermark\":([0-9]+)" hwm_match "${fleet}")
set(hwm "${CMAKE_MATCH_1}")
string(REGEX MATCHALL "\"event_high_watermark\":${hwm}" hwm_matches "${fleet}")
list(LENGTH hwm_matches hwm_count)
if(NOT hwm_count EQUAL 3)
  message(FATAL_ERROR "fleet and nested statuses do not share one HWM: ${fleet}")
endif()
string(REGEX MATCH "\"consumer_state_revision\":([0-9]+)" revision_match "${fleet}")
set(consumer_state_revision "${CMAKE_MATCH_1}")
if(NOT consumer_state_revision GREATER 0)
  message(FATAL_ERROR "non-empty fleet did not expose a positive consumer state revision: ${fleet}")
endif()
string(REGEX MATCHALL "\"consumer_state_revision\":${consumer_state_revision}"
       revision_matches "${fleet}")
list(LENGTH revision_matches revision_count)
if(NOT revision_count EQUAL 3)
  message(FATAL_ERROR "fleet and nested statuses do not share one consumer state revision: ${fleet}")
endif()
string(REGEX MATCH "\"next_dynamic_transition_at\":\"([^\"]+)\""
       transition_match "${fleet}")
set(next_dynamic_transition_at "${CMAKE_MATCH_1}")
if(next_dynamic_transition_at STREQUAL "")
  message(FATAL_ERROR "leased fleet did not expose its next dynamic transition: ${fleet}")
endif()
string(REGEX MATCHALL
       "\"next_dynamic_transition_at\":\"${next_dynamic_transition_at}\""
       transition_matches "${fleet}")
list(LENGTH transition_matches transition_count)
if(NOT transition_count EQUAL 2)
  message(FATAL_ERROR "fleet transition did not agree with exactly one leased consumer: ${fleet}")
endif()
string(REGEX MATCHALL "\"next_dynamic_transition_at\":\"\"" empty_transition_matches
       "${fleet}")
list(LENGTH empty_transition_matches empty_transition_count)
if(NOT empty_transition_count EQUAL 1)
  message(FATAL_ERROR "idle consumer did not expose an empty dynamic transition: ${fleet}")
endif()

run_cli(consumer-statuses-if-current "${DB}" "${hwm}" "${consumer_state_revision}"
        --next-transition "${next_dynamic_transition_at}" --workspace fleet-cli)
set(cache_hit "${CLI_OUTPUT}")
if(NOT cache_hit MATCHES "\"cache_valid\":true")
  message(FATAL_ERROR "exact fleet validator did not hit the cache fast path: ${cache_hit}")
endif()
if(cache_hit MATCHES "\"snapshot\"")
  message(FATAL_ERROR "cache-valid response serialized an unnecessary snapshot: ${cache_hit}")
endif()
if(NOT cache_hit MATCHES "\"event_high_watermark\":${hwm}" OR
   NOT cache_hit MATCHES "\"consumer_state_revision\":${consumer_state_revision}" OR
   NOT cache_hit MATCHES
       "\"next_dynamic_transition_at\":\"${next_dynamic_transition_at}\"")
  message(FATAL_ERROR "cache-valid response did not return its authoritative validator: ${cache_hit}")
endif()

# Omitting the active lease boundary is a fabricated validator, even when both
# durable markers are exact. The authoritative transition query must force a
# complete current snapshot.
run_cli(consumer-statuses-if-current "${DB}" "${hwm}" "${consumer_state_revision}"
        --workspace fleet-cli)
set(cache_miss "${CLI_OUTPUT}")
if(NOT cache_miss MATCHES "\"cache_valid\":false" OR
   NOT cache_miss MATCHES "\"snapshot\"" OR
   NOT cache_miss MATCHES "\"consumer_count\":2")
  message(FATAL_ERROR "fabricated empty transition did not return a full snapshot: ${cache_miss}")
endif()

foreach(forbidden IN ITEMS "\"delivery_token\"" "\"request_token\""
                           "${delivery_secret}" "${request_secret}")
  string(FIND "${fleet}${cache_hit}${cache_miss}" "${forbidden}" forbidden_position)
  if(NOT forbidden_position EQUAL -1)
    message(FATAL_ERROR "fleet output leaked forbidden capability material: ${forbidden}")
  endif()
endforeach()

file(REMOVE "${DB}" "${DB}-wal" "${DB}-shm")

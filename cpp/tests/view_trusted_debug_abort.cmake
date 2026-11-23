# Runs `PROGRAM CASE` (view_trusted_debug_abort.cpp) and requires that it
# died — did not return 0 — after printing the trusted-view violation
# message. (ctest reports a test that aborts as failed whatever it printed,
# so the abort is checked here.)
execute_process(COMMAND ${PROGRAM} ${CASE}
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
message("${output}")
if(result STREQUAL "0")
   message(FATAL_ERROR "the violation was not caught: '${PROGRAM} ${CASE}' returned normally")
endif()
if(NOT output MATCHES "psio: trusted view over bytes that are not a valid encoding")
   message(FATAL_ERROR "'${PROGRAM} ${CASE}' failed (${result}) without the violation message")
endif()

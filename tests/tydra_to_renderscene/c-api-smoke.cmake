# Verify CLI options reach the public C conversion configuration.
function(check_scene fixture expected)
  execute_process(
    COMMAND "${APP}" "${ROOT}/tests/usda/${fixture}" --next --memstat --nodump ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${fixture}: conversion failed (${result}): ${error}")
  endif()
  if(NOT output MATCHES "${expected}")
    message(FATAL_ERROR "${fixture}: missing '${expected}' in output: ${output}")
  endif()
endfunction()

check_scene(c-core-animation-queries.usda "Animations: 2")
check_scene(c-core-animation-queries.usda "Animations: 0" --no-animation)
check_scene(c-core-mesh-queries.usda "Total vertices: 3, total triangles: 2"
            --trifan --tangent-method mikktspace)

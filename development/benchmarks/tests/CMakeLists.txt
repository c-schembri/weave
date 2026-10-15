foreach(script IN ITEMS automation gate gate_process)
  add_test(NAME ${script}_tests COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/${script}.py")
  set_tests_properties(${script}_tests PROPERTIES TIMEOUT 60 LABELS tooling)
endforeach()

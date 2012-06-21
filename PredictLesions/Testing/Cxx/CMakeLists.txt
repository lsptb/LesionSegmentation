
#-----------------------------------------------------------------------------
set(Launcher_Command ${Slicer_LAUNCH_COMMAND})
set(BASELINE ${CMAKE_CURRENT_SOURCE_DIR}/../../Data/Baseline)
set(INPUT ${CMAKE_CURRENT_SOURCE_DIR}/../../Data/Input)
set(TEMP "${CMAKE_BINARY_DIR}/Testing/Temporary")

set(CLP ${MODULE_NAME})

#-----------------------------------------------------------------------------
add_executable(${CLP}Test ${CLP}Test.cxx)
target_link_libraries(${CLP}Test ${CLP}Lib)
set_target_properties(${CLP}Test PROPERTIES LABELS ${CLP})

#-----------------------------------------------------------------------------
set(testname ${CLP}Test)
add_test(${CLP}Test ${Launcher_Command} ${CLP}Test
  ${CLP}Test
    --xml
  )

#ExternalData_add_test(${CLP}Data NAME ${testname} COMMAND ${Launcher_Command} $<TARGET_FILE:${CLP}Test>
#  --compare DATA{${BASELINE}/${CLP}Test.nhdr,${CLP}Test.raw}
#  ${TEMP}/${CLP}Test.nhdr
#  ModuleEntryPoint
#  --sigma 2.5 DATA{${INPUT}/CTHeadAxial.nhdr,CTHeadAxial.raw.gz} ${TEMP}/${CLP}Test.nhdr
#  )
#set_property(TEST ${testname} PROPERTY LABELS ${CLP})

#-----------------------------------------------------------------------------
#ExternalData_add_target(${CLP}Data)
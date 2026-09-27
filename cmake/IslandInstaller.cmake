set(island_nsis_source "${CMAKE_ROOT}/Modules/Internal/CPack/NSIS.template.in")
if(NOT EXISTS "${island_nsis_source}")
    message(FATAL_ERROR "CMake's bundled NSIS template was not found: ${island_nsis_source}")
endif()
file(READ "${island_nsis_source}" island_nsis_template)

# Fail on upstream changes instead of silently producing a mixed-scope installer.
function(island_replace_nsis needle replacement expected_count)
    string(REPLACE "${needle}" "" remainder "${island_nsis_template}")
    string(LENGTH "${island_nsis_template}" original_length)
    string(LENGTH "${remainder}" remainder_length)
    string(LENGTH "${needle}" needle_length)
    math(EXPR count "(${original_length} - ${remainder_length}) / ${needle_length}")
    if(NOT count EQUAL expected_count)
        message(FATAL_ERROR "Unsupported CMake NSIS template: expected ${expected_count} occurrence(s) of '${needle}', found ${count}")
    endif()
    string(REPLACE "${needle}" "${replacement}" updated "${island_nsis_template}")
    set(island_nsis_template "${updated}" PARENT_SCOPE)
endfunction()

island_replace_nsis("RequestExecutionLevel admin" "RequestExecutionLevel user" 1)
island_replace_nsis("SetShellVarContext all" "SetShellVarContext current" 4)
foreach(callback ".onInit" "un.onInit")
    island_replace_nsis("Function ${callback}\n" "Function ${callback}\n  SetShellVarContext current\n" 1)
endforeach()
foreach(register 0 1)
    set(lookup "ReadRegStr $${register} HKLM \"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\@CPACK_PACKAGE_INSTALL_REGISTRY_KEY@\"")
    string(REPLACE " HKLM " " HKCU " user_lookup "${lookup}")
    island_replace_nsis("${lookup}" "${user_lookup}" 1)
endforeach()

set(island_stop_command [=[
  IfFileExists "$INSTDIR\Island.exe" 0 island_stop_done
    ExecWait '"$INSTDIR\Island.exe" --quit'
    Sleep 500
  island_stop_done:
  ClearErrors
]=])
foreach(hook "CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS" "CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS")
    island_replace_nsis("@${hook}@" "${island_stop_command}\n@${hook}@" 1)
endforeach()

set(island_nsis_directory "${CMAKE_CURRENT_BINARY_DIR}/cpack")
file(MAKE_DIRECTORY "${island_nsis_directory}")
file(WRITE "${island_nsis_directory}/NSIS.template.in" "${island_nsis_template}")
# CPack maps CPACK_MODULE_PATH to CMAKE_MODULE_PATH before FindTemplate().
list(PREPEND CPACK_MODULE_PATH "${island_nsis_directory}")
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "Island")

if(BUILD_TESTING)
    add_test(NAME installer_scope COMMAND "${CMAKE_COMMAND}"
        "-DTEMPLATE_PATH=${island_nsis_directory}/NSIS.template.in"
        -P "${CMAKE_CURRENT_LIST_DIR}/VerifyNsisTemplate.cmake")
endif()

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
set(uninstall_key [=["Software\Microsoft\Windows\CurrentVersion\Uninstall\@CPACK_PACKAGE_INSTALL_REGISTRY_KEY@"]=])
island_replace_nsis("ReadRegStr $0 HKLM ${uninstall_key} \"UninstallString\""
    "ReadRegStr $0 HKCU ${uninstall_key} \"UninstallString\"" 1)

set(display_lookup "ReadRegStr $1 HKLM ${uninstall_key} \"DisplayName\"")
set(user_display_lookup "ReadRegStr $1 HKCU ${uninstall_key} \"DisplayName\"")
string(FIND "${island_nsis_template}" "${display_lookup}" display_lookup_position)
if(NOT display_lookup_position EQUAL -1)
    island_replace_nsis("${display_lookup}" "${user_display_lookup}" 1)
else()
    # CMake 3.31 uses the configured name instead of a DisplayName lookup.
    set(legacy_prompt [=[  MessageBox MB_YESNOCANCEL|MB_ICONEXCLAMATION \
  "@CPACK_NSIS_PACKAGE_NAME@ is already installed. $\n$\nDo you want to uninstall the old version before installing the new one?" \
]=])
    string(REPLACE "@CPACK_NSIS_PACKAGE_NAME@" "$1" user_prompt "${legacy_prompt}")
    island_replace_nsis("${legacy_prompt}" "  ${user_display_lookup}\n${user_prompt}" 1)
endif()

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

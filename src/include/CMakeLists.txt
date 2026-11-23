# Copyright Contributors to the Open Shading Language project.
# SPDX-License-Identifier: BSD-3-Clause
# https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

file (GLOB public_headers OSL/*.h)
file (GLOB public_headers_imathx OSL/Imathx/*.h)

# Only install the testrender session API together with the library that
# implements it.
if (NOT OSL_BUILD_TESTRENDER_LIB)
    list (FILTER public_headers EXCLUDE REGEX "/OSL/testrender\\.h$")
endif ()


option (USE_GENERATED_EXPORT_HEADER "Use CMake-generated export header" OFF)
if (USE_GENERATED_EXPORT_HEADER)
    include (GenerateExportHeader)
    set (exportextras "\n#define OSL_EXPORT OSL_API\n")
    generate_export_header (OSL
          # [BASE_NAME baseName]
          EXPORT_FILE_NAME export.h
          EXPORT_MACRO_NAME OSL_API
          NO_EXPORT_MACRO_NAME OSL_LOCAL
          STATIC_DEFINE OSL_STATIC_DEFINE
          DEPRECATED_MACRO_NAME OSL_DEPRECATED
          # [NO_DEPRECATED_MACRO_NAME noDeprecatedMacroName]
          # [DEFINE_NO_DEPRECATED]
          # [PREFIX_NAME prefix]
          CUSTOM_CONTENT_FROM_VARIABLE exportextras
          )
    list (REMOVE_ITEM public_headers OSL/export.h)
    list (APPEND public_headers "${PROJECT_BINARY_DIR}/src/include/export.h")
endif()


set (versionfile oslversion.h)
message (VERBOSE "Create ${versionfile} from ${versionfile}.in")
configure_file (${PROJECT_NAME}/${versionfile}.in "${PROJECT_BINARY_DIR}/include/${PROJECT_NAME}/${versionfile}" @ONLY)
list (APPEND public_headers "${PROJECT_BINARY_DIR}/include/${PROJECT_NAME}/${versionfile}")

set (oslconfighfile oslconfig.h)
message (VERBOSE "Create ${oslconfighfile} from ${oslconfighfile}.in")
configure_file (${PROJECT_NAME}/${oslconfighfile}.in "${PROJECT_BINARY_DIR}/include/${PROJECT_NAME}/${oslconfighfile}" @ONLY)
list (APPEND public_headers "${PROJECT_BINARY_DIR}/include/${PROJECT_NAME}/${oslconfighfile}")


install (FILES ${public_headers}
         DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}
         COMPONENT developer)
install (FILES ${public_headers_imathx}
         DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}/Imathx
         COMPONENT developer)

set (PROJECT_PUBLIC_HEADERS ${public_headers} ${public_headers_imathx} PARENT_SCOPE)

# Copyright Contributors to the Open Shading Language project.
# SPDX-License-Identifier: BSD-3-Clause
# https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

# The reusable testrender core.  The testrender executable and embedding
# applications (such as MaterialX's graph editor) share this implementation.
set (testrender_core_srcs
     shading.cpp
     simpleraytracer.cpp
     scene.cpp
     bvh.cpp
     session.cpp)

find_package(Threads REQUIRED)

add_library (libtestrender STATIC ${testrender_core_srcs})

# shading.cpp uses the BSDL generated LUT headers.  Ensure they exist before
# compiling the library, including in library-only builds without testrender.
add_dependencies (libtestrender genluts)

set_target_properties (libtestrender
                       PROPERTIES
                       OUTPUT_NAME libtestrender${OSL_LIBNAME_SUFFIX}
                       PREFIX ""
                       POSITION_INDEPENDENT_CODE ON)

target_include_directories (libtestrender
                            BEFORE PRIVATE
                                ${OpenImageIO_INCLUDES}
                                ${CMAKE_CURRENT_SOURCE_DIR})

# BSDL is a header-only interface library used to compile shading.cpp.  Copy
# its include paths instead of exporting it as a link dependency of the
# installed libtestrender target.
get_target_property (_testrender_bsdl_includes BSDL INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories (libtestrender PRIVATE ${_testrender_bsdl_includes})

target_link_libraries (libtestrender
                       PRIVATE
                           Imath::Imath
                       PUBLIC
                           oslexec oslquery oslcomp
                           OpenImageIO::OpenImageIO
                           Threads::Threads)

# The library is always built when the testrender executable is (it is the
# executable's core), but only installed for embedding when explicitly
# requested.  This keeps a normal test-enabled OSL install unchanged.
if (OSL_BUILD_TESTRENDER_LIB)
    install_targets (libtestrender)
endif ()

# The 'testrender' executable
if (OSL_BUILD_TESTS AND BUILD_TESTING)
    set (testrender_srcs
         testrender.cpp
         scene_xml.cpp)

    if (OSL_USE_OPTIX)
        list (APPEND testrender_srcs optixraytracer.cpp)
        set (testrender_cuda_srcs
            cuda/optix_raytracer.cu
            )

        set (testrender_rend_lib_srcs
            cuda/rend_lib.cu
            ../testshade/rs_simplerend.cpp
            )

        # We need to make sure that the PTX files are regenerated whenever these
        # files change.
        set (testrender_cuda_headers
            cuda/rend_lib.h
            background.h
            optics.h
            render_params.h
            raytracer.h
            sampling.h
            shading.h
            shading.cpp
            simpleraytracer.cpp
            )

        # Generate PTX for all of the CUDA files
        foreach (cudasrc ${testrender_cuda_srcs})
            NVCC_COMPILE ( ${cudasrc} "${testrender_cuda_headers}" ptx_generated "" BSDL)
            list (APPEND ptx_list ${ptx_generated})
        endforeach ()

        # Compile the renderer-supplied shadeops (rend_lib) to LLVM bitcode and PTX
        add_compile_definitions (OSL_LLVM_CUDA_BITCODE)
        CUDA_SHADEOPS_COMPILE ( "rend_lib_testrender"
            rend_lib_bc
            rend_lib_ptx
            "${testrender_rend_lib_srcs}"
            "${testrender_cuda_headers}"
        )

        # Serialize the linked bitcode into a CPP file to be embedded in the current target binary
        set (rend_lib_bc_cuda_cpp "${CMAKE_CURRENT_BINARY_DIR}/rend_lib_cuda.bc.cpp")
        MAKE_EMBEDDED_CPP( "rend_lib_llvm_compiled_ops" ${rend_lib_bc_cuda_cpp} ${rend_lib_bc} )
        list (APPEND testrender_srcs ${rend_lib_bc_cuda_cpp})
        list (APPEND ptx_list ${rend_lib_ptx})

        add_custom_target (testrender_ptx ALL
            DEPENDS ${ptx_list} ${testrender_cuda_headers}
            SOURCES ${testrender_cuda_srcs} )

        # Install the PTX files in a fixed location so that they can be
        # loaded at run time
        install (FILES ${ptx_list}
                 DESTINATION ${OSL_PTX_INSTALL_DIR})
    endif()

    if (CMAKE_COMPILER_IS_INTELCLANG)
        # To better match existing test results
        add_compile_options("-fp-model=precise")
        # Better performance is likely by not requiring a precise floating point
        # model, although with slightly different numerical results.
    endif ()

    add_executable (testrender ${testrender_srcs})

    target_include_directories (testrender BEFORE PRIVATE ${OpenImageIO_INCLUDES})
    target_include_directories (testrender PRIVATE ${_testrender_bsdl_includes})

    target_link_libraries (testrender
        PRIVATE
            libtestrender
            BSDL
            Imath::Imath
            pugixml::pugixml
            Threads::Threads)

    if (OSL_USE_OPTIX)
        add_dependencies(testrender testrender_ptx)
    endif ()

    osl_optix_target (testrender)

    install ( TARGETS testrender RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} )

    add_executable (testrender_session_test session_test.cpp)
    target_link_libraries (testrender_session_test PRIVATE libtestrender)
    target_compile_definitions (testrender_session_test PRIVATE
        TESTRENDER_STDOSL_PATH="${PROJECT_SOURCE_DIR}/src/shaders/stdosl.h")
    set_target_properties (testrender_session_test PROPERTIES FOLDER "Unit Tests")
    add_test (NAME unit_testrender_session COMMAND testrender_session_test)
endif ()

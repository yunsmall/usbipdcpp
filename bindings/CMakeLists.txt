find_package(pybind11 CONFIG REQUIRED)

file(GLOB BINDING_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/devices/*.cpp
)

# 内部编译目标，真正产出 .pyd
pybind11_add_module(_usbipdcpp_py ${BINDING_SOURCES})

set_target_properties(_usbipdcpp_py PROPERTIES OUTPUT_NAME usbipdcpp)
set_target_properties(_usbipdcpp_py PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/python_package
)

target_link_libraries(_usbipdcpp_py PRIVATE
    usbipdcpp
    usbipdcpp_virtual_device
)

target_include_directories(_usbipdcpp_py PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

# 将依赖的共享库 DLL 复制到 python_package，确保 .pyd 能直接加载
if(USBIPDCPP_BUILD_SHARED_LIBS AND WIN32)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/.copy_dlls_done"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:usbipdcpp>"
            "${CMAKE_BINARY_DIR}/python_package"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:usbipdcpp_virtual_device>"
            "${CMAKE_BINARY_DIR}/python_package"
        COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/.copy_dlls_done"
        COMMENT "Copying DLLs to python_package"
        DEPENDS "$<TARGET_FILE:_usbipdcpp_py>" "$<TARGET_FILE:usbipdcpp>" "$<TARGET_FILE:usbipdcpp_virtual_device>"
    )
    add_custom_target(_copy_python_dlls DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/.copy_dlls_done")
    set(STUBGEN_DEP "_copy_python_dlls")
else()
    set(STUBGEN_DEP "_usbipdcpp_py")
endif()

# 构建后自动生成 .pyi 存根文件，方便 IDE 代码补全
find_program(PYBIND11_STUBGEN_EXE pybind11-stubgen)
if(PYBIND11_STUBGEN_EXE)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/.stubgen_done"
        COMMAND ${CMAKE_COMMAND} -E env
                "PYTHONPATH=${CMAKE_BINARY_DIR}/python_package"
                ${PYBIND11_STUBGEN_EXE} usbipdcpp
                -o "${CMAKE_BINARY_DIR}/python_package"
                --ignore-all-errors
        COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/.stubgen_done"
        COMMENT "Generating .pyi stubs with pybind11-stubgen"
        DEPENDS ${STUBGEN_DEP}
    )
    add_custom_target(_python_stubgen DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/.stubgen_done")
    set(USBPY_FINAL_DEP _python_stubgen)
else()
    if(USBIPDCPP_BUILD_SHARED_LIBS AND WIN32)
        set(USBPY_FINAL_DEP _copy_python_dlls)
    else()
        set(USBPY_FINAL_DEP _usbipdcpp_py)
    endif()
    message(WARNING "pybind11-stubgen not found, .pyi stubs will not be generated. "
                    "Install with: pip install pybind11-stubgen")
endif()

# 对外目标，构建 --target usbipdcpp_python 即触发整个链
add_custom_target(usbipdcpp_python DEPENDS ${USBPY_FINAL_DEP})

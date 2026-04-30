find_package(pybind11 CONFIG REQUIRED)

file(GLOB BINDING_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/devices/*.cpp
)

pybind11_add_module(usbipdcpp_python ${BINDING_SOURCES})

# Python 导入名为 usbipdcpp，需要将输出文件名设为 usbipdcpp
set_target_properties(usbipdcpp_python PROPERTIES OUTPUT_NAME usbipdcpp)

target_link_libraries(usbipdcpp_python PRIVATE
    usbipdcpp
    usbipdcpp_virtual_device
)

target_include_directories(usbipdcpp_python PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

# Python 模块输出目录
set_target_properties(usbipdcpp_python PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/python_package
)

# 构建后自动生成 .pyi 存根文件，方便 IDE 代码补全
find_program(PYBIND11_STUBGEN_EXE pybind11-stubgen)
if(PYBIND11_STUBGEN_EXE)
    add_custom_command(TARGET usbipdcpp_python POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E env
                "PYTHONPATH=${CMAKE_BINARY_DIR}/python_package"
                ${PYBIND11_STUBGEN_EXE} usbipdcpp
                -o "${CMAKE_BINARY_DIR}/python_package"
                --ignore-all-errors
        COMMENT "Generating .pyi type stubs with pybind11-stubgen"
    )
else()
    message(WARNING "pybind11-stubgen not found, .pyi stubs will not be generated. "
                    "Install with: pip install pybind11-stubgen")
endif()

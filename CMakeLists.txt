cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.0.0")
set(PROJECT_VER_NUMBER 2)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(instacare-matter-sensor)

# ESP-Matter 需要在所有 C++ 组件中加载其生成的配置头文件。
# 该设置与 Espressif 官方 managed_component_light 示例保持一致。
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)

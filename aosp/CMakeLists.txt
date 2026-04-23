# AOSP subset for layerviewer. All targets link against aosp_platform, which
# sets up include paths for the shim headers (liblog, libcutils) and AOSP
# source-layout includes.

# Platform abstraction: shim headers come FIRST so system <android/log.h>,
# <cutils/...> etc resolve to our no-op versions rather than Android's.
add_library(aosp_platform INTERFACE)
target_include_directories(aosp_platform INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/shim/include
    # libbase headers are used by libutils too, so expose here.
    ${CMAKE_CURRENT_SOURCE_DIR}/libbase/include
)
target_compile_definitions(aosp_platform INTERFACE
    # Drop Android-specific Bionic detection so libbase/libutils take host paths.
    __STDC_FORMAT_MACROS=1
)
# Tell libbase/libutils we're NOT on Android/Bionic. They have !defined(__BIONIC__)
# and !defined(__ANDROID__) host fallbacks.

# Suppress the wall of warnings from ported AOSP code.
if(NOT MSVC)
    target_compile_options(aosp_platform INTERFACE
        -Wno-unused-parameter
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-unused-private-field
        -Wno-missing-field-initializers
        -Wno-sign-compare
        -Wno-deprecated-declarations
        -Wno-gnu-statement-expression
        -Wno-c99-designator
        -Wno-format-security
    )
endif()

# ---- libutils ----
file(GLOB LIBUTILS_SRC CONFIGURE_DEPENDS libutils/src/*.cpp)
add_library(aosp_libutils STATIC ${LIBUTILS_SRC})
target_include_directories(aosp_libutils PUBLIC
    libutils/include
)
target_link_libraries(aosp_libutils PUBLIC aosp_platform)

# ---- libbase ----
file(GLOB LIBBASE_SRC CONFIGURE_DEPENDS libbase/src/*.cpp)
add_library(aosp_libbase STATIC ${LIBBASE_SRC})
target_include_directories(aosp_libbase PUBLIC
    libbase/include
)
target_include_directories(aosp_libbase PRIVATE
    libbase/src
)
target_link_libraries(aosp_libbase PUBLIC aosp_platform)

# ---- libmath (header-only) ----
add_library(aosp_libmath INTERFACE)
target_include_directories(aosp_libmath INTERFACE libmath/include)

# ---- arect (header-only NDK) ----
add_library(aosp_arect INTERFACE)
target_include_directories(aosp_arect INTERFACE arect/include)

# ---- ftl (header-only template lib from frameworks/native/include/ftl) ----
add_library(aosp_ftl INTERFACE)
target_include_directories(aosp_ftl INTERFACE ftl/include)
target_link_libraries(aosp_ftl INTERFACE aosp_platform aosp_libbase)

# ---- libui value types (Rect, Region, Transform, PixelFormat, ColorSpace) ----
file(GLOB LIBUI_SRC CONFIGURE_DEPENDS libui/src/*.cpp)
add_library(aosp_libui STATIC ${LIBUI_SRC})
target_include_directories(aosp_libui PUBLIC libui/include)
target_link_libraries(aosp_libui PUBLIC
    aosp_platform aosp_libutils aosp_libbase aosp_libmath aosp_arect)

# ---- libtonemap ----
add_library(aosp_tonemap STATIC tonemap/src/tonemap.cpp)
target_include_directories(aosp_tonemap PUBLIC tonemap/include)
target_link_libraries(aosp_tonemap PUBLIC aosp_platform aosp_libmath aosp_libui)

# ---- libshaders ----
add_library(aosp_shaders STATIC shaders/src/shaders.cpp)
target_include_directories(aosp_shaders PUBLIC shaders/include)
target_link_libraries(aosp_shaders PUBLIC aosp_platform aosp_libmath aosp_libui aosp_tonemap)

# ---- Perfetto SF layer-trace protos ----
add_subdirectory(perfetto_protos)

# ---- RenderEngine (Skia/Ganesh + our Desktop-GL variant) ----
add_subdirectory(renderengine)

# ---- CompositionEngine (turns LayerSnapshots into LayerSettings for RE) ----
add_subdirectory(compositionengine)

# Convenience aggregate.
add_library(aosp INTERFACE)
target_link_libraries(aosp INTERFACE
    aosp_libutils aosp_libbase aosp_libmath aosp_arect aosp_ftl aosp_libui
    aosp_perfetto_protos aosp_renderengine)

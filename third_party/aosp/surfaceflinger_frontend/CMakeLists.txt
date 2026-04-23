# SurfaceFlinger FrontEnd port. Consumes transactions + layer traces and
# produces LayerSnapshots that CompositionEngine drives.

file(GLOB SF_FRONTEND_SRC CONFIGURE_DEPENDS FrontEnd/*.cpp)

add_library(aosp_surfaceflinger_frontend STATIC ${SF_FRONTEND_SRC})
target_include_directories(aosp_surfaceflinger_frontend PUBLIC
    .           # so "FrontEnd/X.h" resolves
    FrontEnd    # so sibling "RequestedLayerState.h" resolves
)
target_link_libraries(aosp_surfaceflinger_frontend PUBLIC
    aosp_platform
    aosp_libutils aosp_libbase aosp_libmath aosp_ftl aosp_libui
    aosp_compositionengine aosp_renderengine
)
if(NOT MSVC)
    target_compile_options(aosp_surfaceflinger_frontend PRIVATE
        -Wno-unused-parameter
        -Wno-unused-variable
        -Wno-missing-field-initializers
        -Wno-sign-compare
        -Wno-deprecated-declarations
    )
endif()

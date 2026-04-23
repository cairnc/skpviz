# CompositionEngine port. Sits between FrontEnd (LayerSnapshots) and
# RenderEngine (LayerSettings + drawLayers), driving Output/OutputLayer
# state machines.

file(GLOB CE_SRC CONFIGURE_DEPENDS
    src/*.cpp
    src/planner/*.cpp
)

add_library(aosp_compositionengine STATIC ${CE_SRC})
target_include_directories(aosp_compositionengine PUBLIC include)
target_include_directories(aosp_compositionengine PRIVATE src)
target_link_libraries(aosp_compositionengine PUBLIC
    aosp_platform
    aosp_libutils aosp_libbase aosp_libmath aosp_ftl aosp_libui
    aosp_renderengine
    skia
)
target_include_directories(aosp_compositionengine PRIVATE
    ${SKIA_DIR}
    ${SKIA_DIR}/include/core
)
if(NOT MSVC)
    target_compile_options(aosp_compositionengine PRIVATE
        -Wno-unused-parameter
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-missing-field-initializers
        -Wno-sign-compare
        -Wno-deprecated-declarations
    )
endif()

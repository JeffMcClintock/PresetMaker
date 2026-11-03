cmake_minimum_required(VERSION 3.20)
project(PresetMaker CXX)

# Fits a VST3 instrument's parameters to a sampled GM instrument (Windows gm.dls).
# pmcli = command line; the gmpi_ui front end is a later target.

if(NOT WIN32)
    message(FATAL_ERROR "PresetMaker is Windows-only for now.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# VST3 SDK: reuse the CPM cache the adaptors already populated, else fetch.
set(VST3_SDK_FOLDER_OVERRIDE "" CACHE PATH "Local vst3sdk checkout, or blank to find/fetch one.")
if(NOT "${VST3_SDK_FOLDER_OVERRIDE}" STREQUAL "")
    set(VST3_SDK "${VST3_SDK_FOLDER_OVERRIDE}")
else()
    file(GLOB _cached "$ENV{USERPROFILE}/.cpm/vst3_sdk/*/vst3_sdk/public.sdk/source/vst/hosting/plugprovider.h")
    set(_first "")
    if(_cached)
        list(GET _cached 0 _first)
    endif()
    if(_first)
        get_filename_component(VST3_SDK "${_first}/../../../../.." ABSOLUTE)
    else()
        include(FetchContent)
        FetchContent_Declare(vst3_sdk GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk
            GIT_TAG v3.8.0_build_66 GIT_SHALLOW ON SOURCE_SUBDIR do-not-configure)
        FetchContent_MakeAvailable(vst3_sdk)
        set(VST3_SDK ${vst3_sdk_SOURCE_DIR})
    endif()
endif()
message(STATUS "PresetMaker: VST3 SDK = ${VST3_SDK}")

set(vst3_srcs
    ${VST3_SDK}/base/source/baseiids.cpp
    ${VST3_SDK}/base/source/fbuffer.cpp
    ${VST3_SDK}/base/source/fdebug.cpp
    ${VST3_SDK}/base/source/fobject.cpp
    ${VST3_SDK}/base/source/fstring.cpp
    ${VST3_SDK}/base/source/updatehandler.cpp
    ${VST3_SDK}/base/thread/source/flock.cpp
    ${VST3_SDK}/pluginterfaces/base/coreiids.cpp
    ${VST3_SDK}/pluginterfaces/base/funknown.cpp
    ${VST3_SDK}/pluginterfaces/base/ustring.cpp
    ${VST3_SDK}/public.sdk/source/common/commoniids.cpp
    ${VST3_SDK}/public.sdk/source/common/commonstringconvert.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstinitiids.cpp
    ${VST3_SDK}/public.sdk/source/vst/utility/stringconvert.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/eventlist.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/module.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/module_win32.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/parameterchanges.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${VST3_SDK}/public.sdk/source/vst/hosting/processdata.cpp
    ${VST3_SDK}/public.sdk/source/common/threadchecker_win32.cpp
    ${VST3_SDK}/public.sdk/source/common/memorystream.cpp
    ${VST3_SDK}/public.sdk/source/vst/vstpresetfile.cpp
)

add_library(pm_core STATIC
    src/Dls.cpp
    src/GmRender.cpp
    src/Reference.cpp
    src/Vst3Host.cpp
    src/Metrics.cpp
    src/Fit.cpp
    ${vst3_srcs}
)
target_include_directories(pm_core PUBLIC src)
target_include_directories(pm_core SYSTEM PUBLIC ${VST3_SDK})
target_compile_definitions(pm_core PUBLIC UNICODE _UNICODE NOMINMAX
    $<$<CONFIG:Debug>:_DEBUG DEBUG=1>
    $<$<CONFIG:Release>:NDEBUG DEBUG=0>
    $<$<CONFIG:RelWithDebInfo>:NDEBUG DEBUG=0>)
target_compile_options(pm_core PUBLIC /utf-8 /bigobj /Zc:char8_t-)
target_link_libraries(pm_core PUBLIC ole32 shell32)

add_executable(pmcli src/cli.cpp)
target_link_libraries(pmcli PRIVATE pm_core)

add_executable(pmcal src/calibrate.cpp)
target_link_libraries(pmcal PRIVATE pm_core)

# The desktop app: a gmpi_ui form over the same fitting code. gmpi_ui and GMPI come from sibling
# checkouts when there are some, else GitHub.
option(PM_GUI "Build PresetMaker.exe (needs gmpi_ui + GMPI)" OFF)
if(PM_GUI)
    include(FetchContent)
    foreach(dep gmpi_ui GMPI)
        string(TOUPPER ${dep} DEP)
        set(${DEP}_FOLDER_OVERRIDE "" CACHE PATH "Local ${dep} checkout, or blank to use ../${dep} or fetch.")
        if(NOT "${${DEP}_FOLDER_OVERRIDE}" STREQUAL "")
            set(${DEP}_DIR "${${DEP}_FOLDER_OVERRIDE}")
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../${dep}/experimental/forms.h" OR EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../${dep}/Core/GmpiApiEditor.h")
            get_filename_component(${DEP}_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../${dep}" ABSOLUTE)
        else()
            FetchContent_Declare(${dep} GIT_REPOSITORY https://github.com/JeffMcClintock/${dep} GIT_TAG origin/main
                GIT_SHALLOW ON SOURCE_SUBDIR do-not-configure)
            FetchContent_MakeAvailable(${dep})
            set(${DEP}_DIR ${${dep}_SOURCE_DIR})
        endif()
        message(STATUS "PresetMaker: ${dep} = ${${DEP}_DIR}")
    endforeach()

    add_executable(PresetMaker WIN32
        gui/MainWin32.cpp
        gui/FitForm.cpp
        gui/FitForm.h
        ${GMPI_UI_DIR}/experimental/builders.cpp
        ${GMPI_UI_DIR}/experimental/forms.cpp
        ${GMPI_UI_DIR}/experimental/grid.cpp
        ${GMPI_UI_DIR}/experimental/primatives.cpp
        ${GMPI_UI_DIR}/experimental/ui_facades.cpp
        ${GMPI_UI_DIR}/helpers/Timer.cpp
        ${GMPI_UI_DIR}/backends/DrawingFrameCommon.cpp
        ${GMPI_UI_DIR}/backends/DrawingFrameWin.cpp
        ${GMPI_UI_DIR}/backends/DirectXGfx.cpp
    )
    target_include_directories(PresetMaker PRIVATE gui gui/compat ${GMPI_DIR} ${GMPI_DIR}/Core ${GMPI_UI_DIR})
    target_link_libraries(PresetMaker PRIVATE pm_core d3d11 d2d1 dwrite windowscodecs Dwmapi dxguid)
endif()

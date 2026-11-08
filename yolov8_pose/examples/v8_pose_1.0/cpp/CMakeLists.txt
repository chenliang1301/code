cmake_minimum_required(VERSION 3.10)

project(rknn_yolov8_pose_demo)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (ENABLE_ASAN)
	message(STATUS "BUILD WITH ADDRESS SANITIZER")
	set (CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
	set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
	set (CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
endif ()

set(rknpu_yolov8-pose_file rknpu2/yolov8-pose.cc)
if (TARGET_SOC STREQUAL "rv1106" OR TARGET_SOC STREQUAL "rv1103")
    add_definitions(-DRV1106_1103)
    set(rknpu_yolov8-pose_file rknpu2/yolov8-pose_rv1106_1103.cc)
    #dma
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../3rdparty/allocator/dma)
elseif(TARGET_SOC STREQUAL "rk1808" OR TARGET_SOC STREQUAL "rv1109" OR TARGET_SOC STREQUAL "rv1126")
    add_definitions(-DRKNPU1)
    set(rknpu_yolov8-pose_file rknpu1/yolov8-pose.cc)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if (NOT SYSROOT AND CMAKE_SYSROOT)
        set(SYSROOT "${CMAKE_SYSROOT}/usr/include")
    endif()
    if (NOT SYSROOT)
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -print-sysroot
            OUTPUT_VARIABLE COMPILER_SYSROOT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if (COMPILER_SYSROOT AND EXISTS "${COMPILER_SYSROOT}/usr/include/opencv4/opencv2/opencv.hpp")
            set(SYSROOT "${COMPILER_SYSROOT}/usr/include")
        endif()
    endif()
    if (NOT SYSROOT AND DEFINED ENV{GCC_COMPILER})
        get_filename_component(GCC_COMPILER_DIR "$ENV{GCC_COMPILER}" DIRECTORY)
        if (GCC_COMPILER_DIR)
            get_filename_component(TOOLCHAIN_ROOT "${GCC_COMPILER_DIR}/.." ABSOLUTE)
            set(SYSROOT "${TOOLCHAIN_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux-gnu/sysroot/usr/include")
        endif()
    endif()
    if (NOT EXISTS "${SYSROOT}/opencv4/opencv2/opencv.hpp")
        message(FATAL_ERROR "OpenCV headers were not found at ${SYSROOT}/opencv4. Set SYSROOT to the target sysroot include directory, as used by yolov5_base.")
    endif()
    add_definitions(-DENABLE_OPENCV)
    include_directories(${SYSROOT} ${SYSROOT}/opencv4)
    set(OPENCV_LIBS opencv_core opencv_flann opencv_videoio opencv_video opencv_highgui opencv_imgcodecs opencv_imgproc)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../3rdparty/ 3rdparty.out)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../utils/ utils.out)

set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")

file(GLOB SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)


add_executable(${PROJECT_NAME}
    main.cc
    tennis_analyzer.cc
    postprocess.cc
    ${rknpu_yolov8-pose_file}
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(${PROJECT_NAME} PRIVATE mjpeg_server.cc)
endif()

target_link_libraries(${PROJECT_NAME}
    imageutils
    fileutils
    imagedrawing    
    ${LIBRKNNRT}
    dl
    ${OPENCV_LIBS}
)

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(${PROJECT_NAME}
    log
)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} Threads::Threads)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBRKNNRT_INCLUDES}
)

install(TARGETS ${PROJECT_NAME} DESTINATION .)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../model/bus.jpg DESTINATION ./model)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../model/yolov8_pose_labels_list.txt DESTINATION ./model)
file(GLOB RKNN_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../model/*.rknn")
install(FILES ${RKNN_FILES} DESTINATION model)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../configs DESTINATION .)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tennis_control.py DESTINATION tools)

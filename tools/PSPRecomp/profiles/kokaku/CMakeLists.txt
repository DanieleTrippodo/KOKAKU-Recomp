set(KOKAKU_PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(modloader)

file(GLOB KOKAKU_GENERATED CONFIGURE_DEPENDS "${KOKAKU_PROFILE_DIR}/generated/*.cpp")

if(MSVC)
    set_source_files_properties(${KOKAKU_GENERATED} PROPERTIES COMPILE_OPTIONS
        "-O2;/bigobj")
else()
    set_source_files_properties(${KOKAKU_GENERATED} PROPERTIES COMPILE_OPTIONS
        "-O2;-g0")
endif()

set(KOKAKU_HOST_SOURCES
    host/vcs_bootstrap_paths.cpp
    host/kokaku_profile.cpp
    host/native_font.cpp
    host/native_boot_screens.cpp
    host/vcs_native_fast_paths.cpp
    host/framebuffer_capture.cpp
    host/display_window.cpp
    host/kokaku_controls.cpp
    host/kokaku_controls_ui.cpp
    host/kokaku_native_menu.cpp
    host/kokaku_frame_rate.cpp
    host/kokaku_controls_tests.cpp
    host/kokaku_mod_capture.cpp
    host/kokaku_mod_runtime.cpp
    host/kokaku_mouse.cpp
    host/kokaku_pc_balance.cpp
    host/kokaku_atrac.cpp
    host/audio_output.cpp
    host/vcs_config.cpp
    host/vcs_camera_input.cpp
    host/vcs_vehicle_input.cpp
    host/vcs_fps_overlay.cpp
    host/audio_output.cpp
    host/vcs_config.cpp
    host/vcs_camera_input.cpp
    host/vcs_vehicle_input.cpp
    host/vcs_fps_overlay.cpp
    host/vcs_media_decoder.cpp
    host/vcs_project2dfx.cpp
    host/vcs_project2dfx_lights.cpp
    host/vcs_draw_distance_patch.cpp
    host/vcs_runtime_log.cpp
    host/vcs_hdr_post_dx12_stub.cpp
    host/ge_renderer.cpp
    host/ge_gpu_backend_dx12.cpp
    host/kokaku_rtx.cpp
    host/dx12_presenter.cpp
    host/kokaku_net.cpp
    host/kokaku_online.cpp
    host/kokaku_transport.cpp
    host/kokaku_lobby.cpp
)

function(kokaku_target_common target)
    target_include_directories(${target} PRIVATE
        "${KOKAKU_PROFILE_DIR}/host"
        "${KOKAKU_PROFILE_DIR}/generated"
        "${KOKAKU_PROFILE_DIR}/third_party/ffmpeg/include")
    target_link_libraries(${target} PRIVATE psprecomp_core ${CMAKE_DL_LIBS})
    if(WIN32)
        foreach(KOKAKU_FFMPEG_LIB avcodec avformat avutil swresample swscale)
            target_link_libraries(${target} PRIVATE
                "${KOKAKU_PROFILE_DIR}/third_party/ffmpeg/lib/${KOKAKU_FFMPEG_LIB}.lib")
        endforeach()
        target_link_libraries(${target} PRIVATE winmm d3d12 dxgi d3dcompiler dxguid ws2_32 iphlpapi winhttp)
        target_link_libraries(${target} PRIVATE kokaku_mod_model)
    endif()
    psprecomp_enable_host_avx(${target})
endfunction()

add_executable(KokakuNative
    host/main.cpp
    ${KOKAKU_HOST_SOURCES}
    ${KOKAKU_GENERATED}
)

kokaku_target_common(KokakuNative)

if(PSPRECOMP_BUILD_PROFILE_TESTS)
    enable_testing()
    add_test(NAME kokaku_controls_tests COMMAND KokakuNative --self-test-controls)
    add_test(NAME kokaku_native_menu_tests COMMAND KokakuNative --self-test-native-menu)
    add_test(NAME kokaku_frame_rate_tests COMMAND KokakuNative --self-test-frame-rate)
    add_test(NAME kokaku_palette_cache_tests COMMAND KokakuNative --self-test-palette-cache)
    add_test(NAME kokaku_atrac_tests COMMAND KokakuNative --self-test-atrac)
    add_test(NAME kokaku_mouse_tests COMMAND KokakuNative --self-test-mouse)
    add_test(NAME kokaku_pc_balance_tests COMMAND KokakuNative --self-test-pc-balance)
    add_test(NAME kokaku_memory_tests COMMAND KokakuNative --self-test-memory)
    add_test(NAME kokaku_vblank_tests COMMAND KokakuNative --self-test-vblank)
    add_test(NAME kokaku_splash_tests COMMAND KokakuNative --self-test-splash)
    add_test(NAME kokaku_savedata_tests COMMAND KokakuNative --self-test-savedata)
    add_test(NAME kokaku_net_tests COMMAND KokakuNative --self-test-net)
    add_test(NAME kokaku_transport_tests COMMAND KokakuNative --self-test-transport)
    add_test(NAME kokaku_lobby_tests COMMAND KokakuNative --self-test-lobby)
    add_test(NAME kokaku_online_tests COMMAND KokakuNative --self-test-online)
    if(WIN32)
        add_test(NAME kokaku_font_tests COMMAND KokakuNative --self-test-font)
        add_test(NAME kokaku_audio_output_tests COMMAND KokakuNative --self-test-audio-output)
    endif()
endif()

set_target_properties(KokakuNative PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "KokakuNative"
)

if(WIN32)
    add_custom_command(TARGET KokakuNative POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${KOKAKU_PROFILE_DIR}/third_party/dxc/dxcompiler.dll"
            "${KOKAKU_PROFILE_DIR}/third_party/dxc/dxil.dll" "$<TARGET_FILE_DIR:KokakuNative>"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:KokakuNative>/licenses/dxc"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${KOKAKU_PROFILE_DIR}/third_party/dxc/LICENSE-MS.txt"
            "${KOKAKU_PROFILE_DIR}/third_party/dxc/LICENSE-LLVM.txt"
            "${KOKAKU_PROFILE_DIR}/third_party/dxc/LICENCE-MIT.txt"
            "${KOKAKU_PROFILE_DIR}/third_party/dxc/README.md" "$<TARGET_FILE_DIR:KokakuNative>/licenses/dxc"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${KOKAKU_PROFILE_DIR}/third_party/ffmpeg/bin" "$<TARGET_FILE_DIR:KokakuNative>"
        COMMAND ${CMAKE_COMMAND}
            "-DSOURCE=${KOKAKU_PROFILE_DIR}/config/KokakuNative.ini"
            "-DDESTINATION=$<TARGET_FILE_DIR:KokakuNative>/KokakuNative.ini"
            -P "${KOKAKU_PROFILE_DIR}/copy_default_config.cmake"
    )
endif()

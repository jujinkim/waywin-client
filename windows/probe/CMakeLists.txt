add_executable(waywin-windows-probe WIN32 main.cpp probe.rc)
target_compile_features(waywin-windows-probe PRIVATE cxx_std_17)
target_compile_definitions(waywin-windows-probe PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(waywin-windows-probe PRIVATE user32 gdi32 shell32 ole32)
if(MSVC)
  target_compile_options(waywin-windows-probe PRIVATE /W4 /WX /utf-8)
  # probe.rc embeds the manifest; a second, linker-generated one is a duplicate resource.
  target_link_options(waywin-windows-probe PRIVATE /MANIFEST:NO)
else()
  target_compile_options(waywin-windows-probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(MINGW)
  target_link_options(waywin-windows-probe PRIVATE -municode -static -static-libgcc -static-libstdc++
    -Wl,--dynamicbase,--nxcompat,--high-entropy-va)
  target_compile_options(waywin-windows-probe PRIVATE -fstack-protector-strong)
endif()

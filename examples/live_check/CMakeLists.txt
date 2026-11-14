# The live session's manual check against OpenAI's GPT-Live (docs/live.md, "The manual check"): a command line program
# without a window (its --talk mode opens the real microphone and output: a person's run), macOS only (the NSURLSession WebSocket backend; the other desktops have no WebSocket backend yet).
add_executable(quench_live_check LiveCheck.cpp Talk.cpp)
quench_configure_target(quench_live_check)
target_link_libraries(quench_live_check PRIVATE quench_assistant quench_audio_device)
set_target_properties(quench_live_check PROPERTIES OUTPUT_NAME quench-live-check)

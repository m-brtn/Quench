# The assistant's manual check against a real provider (docs/assistant.md, "The manual check"): a command line program
# without a window, macOS only (the NSURLSession backend; the other desktops have no network backend yet).
add_executable(quench_assistant_check AssistantCheck.cpp)
quench_configure_target(quench_assistant_check)
target_link_libraries(quench_assistant_check PRIVATE quench_assistant quench_audio)
set_target_properties(quench_assistant_check PROPERTIES OUTPUT_NAME quench-assistant-check)

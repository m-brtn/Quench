# The cook's command line (ctest: cook_cli): a texture and a model from the demo's sources, cooked twice into separate
# directories; the outputs must be byte-identical, the depfile must name every input, and bad input must fail.
# cmake -DCOOK=<quench_cook> -DSOURCES=<examples/demo/assets_src> -DOUT=<scratch directory> -P CookCli.cmake
# With -DPYTHON and -DCHARACTER=<tools/assets/make_demo_character.py>: the demo's character too (skeleton, clips and
# the skinned model).

file(REMOVE_RECURSE "${OUT}")
foreach(run a b)
    set(dir "${OUT}/${run}")
    file(MAKE_DIRECTORY "${dir}/models")
    execute_process(COMMAND "${COOK}" texture "${SOURCES}/tiles/tile_3.png" "${dir}/tile_3.ktx2"
                    RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "texture cook failed (${result})")
    endif()
    execute_process(COMMAND "${COOK}" model "${SOURCES}/models/crate.gltf" "${dir}/models/crate.qmodel"
                            --asset-root "${dir}" --depfile "${dir}/crate.d"
                    RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "model cook failed (${result})")
    endif()
endforeach()

foreach(file tile_3.ktx2 models/crate.qmodel models/crate_0.ktx2)
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/a/${file}" "${OUT}/b/${file}"
                    RESULT_VARIABLE different)
    if(different)
        message(FATAL_ERROR "${file} differs between two cooks")
    endif()
endforeach()

file(READ "${OUT}/a/crate.d" depfile)
foreach(input crate.gltf crate.bin crate_planks.png)
    string(FIND "${depfile}" "${input}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "the depfile does not name ${input}:\n${depfile}")
    endif()
endforeach()

execute_process(COMMAND "${COOK}" texture "${SOURCES}/CREDITS.md" "${OUT}/bad.ktx2" RESULT_VARIABLE result
                ERROR_VARIABLE error)
if(result EQUAL 0 OR EXISTS "${OUT}/bad.ktx2")
    message(FATAL_ERROR "a text file cooked as a texture did not fail")
endif()

# Sounds (-DSOUNDS=<tests/audio/data>): a copy in the same format, a 16-bit WAV, and a text file that fails.
execute_process(COMMAND "${COOK}" sound "${SOUNDS}/tone.flac" "${OUT}/tone.flac" --depfile "${OUT}/tone.d"
                RESULT_VARIABLE result)
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${SOUNDS}/tone.flac" "${OUT}/tone.flac"
                RESULT_VARIABLE different)
if(NOT result EQUAL 0 OR different)
    message(FATAL_ERROR "a FLAC cooked to FLAC is not a copy (${result})")
endif()
execute_process(COMMAND "${COOK}" sound "${SOUNDS}/tone.mp3" "${OUT}/tone.wav" --channels 2 RESULT_VARIABLE result)
file(SIZE "${OUT}/tone.wav" size)
if(NOT result EQUAL 0 OR size LESS 44100)
    message(FATAL_ERROR "the MP3 cooked to a stereo WAV failed (${result}, ${size} bytes)")
endif()
execute_process(COMMAND "${COOK}" sound "${SOURCES}/CREDITS.md" "${OUT}/bad.wav" RESULT_VARIABLE result
                ERROR_VARIABLE sound_error)
if(result EQUAL 0 OR EXISTS "${OUT}/bad.wav")
    message(FATAL_ERROR "a text file cooked as a sound did not fail")
endif()
# The character: generated once, cooked twice.
if(PYTHON AND CHARACTER)
    execute_process(COMMAND "${PYTHON}" "${CHARACTER}" "${OUT}/character" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "make_demo_character.py failed (${result})")
    endif()
    set(source "${OUT}/character/mannequin.gltf")
    foreach(run a b)
        set(dir "${OUT}/${run}")
        execute_process(COMMAND "${COOK}" skeleton "${source}" "${dir}/mannequin.qskel" RESULT_VARIABLE r1)
        execute_process(COMMAND "${COOK}" animation "${source}" "${dir}/mannequin.qanim" --depfile "${dir}/anim.d"
                        RESULT_VARIABLE r2)
        execute_process(COMMAND "${COOK}" model "${source}" "${dir}/models/mannequin.qmodel" --asset-root "${dir}"
                        RESULT_VARIABLE r3)
        if(NOT r1 EQUAL 0 OR NOT r2 EQUAL 0 OR NOT r3 EQUAL 0)
            message(FATAL_ERROR "character cook failed (skeleton ${r1}, animation ${r2}, model ${r3})")
        endif()
    endforeach()
    foreach(file mannequin.qskel mannequin.qanim models/mannequin.qmodel)
        execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}/a/${file}" "${OUT}/b/${file}"
                        RESULT_VARIABLE different)
        if(different)
            message(FATAL_ERROR "${file} differs between two cooks")
        endif()
    endforeach()
    file(READ "${OUT}/a/anim.d" depfile)
    foreach(input mannequin.gltf mannequin.bin mannequin.anim.json)
        string(FIND "${depfile}" "${input}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "the animation depfile does not name ${input}:\n${depfile}")
        endif()
    endforeach()
    execute_process(COMMAND "${COOK}" animation "${source}" "${OUT}/bad.qanim" --clips "${SOURCES}/CREDITS.md"
                    RESULT_VARIABLE result ERROR_VARIABLE animation_error)
    if(result EQUAL 0 OR EXISTS "${OUT}/bad.qanim")
        message(FATAL_ERROR "a text file as clip settings did not fail")
    endif()
endif()

message(STATUS "cook_cli: outputs identical, depfile complete, bad input rejected: ${error}${sound_error}${animation_error}")

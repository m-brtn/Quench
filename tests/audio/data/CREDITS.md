# Credits

Test sounds of the audio module, generated for this repository; CC0 1.0 (public domain dedication). Nothing here
comes from another source.

| File        | Content                                     | Made with                                                                       |
|-------------|---------------------------------------------|---------------------------------------------------------------------------------|
| `tone.flac` | 0.5 s 440 Hz sine, mono, 22050 Hz, 16-bit   | `sox -n -r 22050 -c 1 -b 16 tone.flac synth 0.5 sine 440 vol 0.5 fade 0.01 0.5 0.01` |
| `tone.mp3`  | the same tone as a 32 kbit/s MP3            | the same `sox` command into `tone.wav`, then `lame --silent -b 32 tone.wav tone.mp3` |

The tests embed both files (`tests/CMakeLists.txt`, `AudioTestData.cpp`) and write their other sounds at run time.

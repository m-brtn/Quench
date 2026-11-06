// miniaudio's implementation with its Vorbis decoder: stb_vorbis (extras/stb_vorbis.c in miniaudio's archive) is
// declared before miniaudio, which enables miniaudio's Vorbis backend, and defined after it, in the order of
// miniaudio's own tools/audioconverter. Built in the third-party target quench_miniaudio (cmake/Dependencies.cmake).

#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

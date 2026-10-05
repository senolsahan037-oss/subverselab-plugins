#pragma once

/*  The HAVA engine and its factory moods, included exactly as the prototype
    ships them. Neither file is edited here:

      Source/YayEngine.h   verbatim copy of prototypes/YAY/Source/YayEngine.h
                           md5 febbaf66bc0ae7f80caefeaed2defa76
      tools/presets.h      verbatim copy of prototypes/YAY/tools/presets.h
                           md5 13c3a89920b18db562337db34217dc5f

    The engine compiles clean under -Wall -Wextra (tools/render, the prototype
    flags). The plugin targets add JUCE's much stricter recommended set
    (-Wconversion, -Wfloat-equal, -Wshadow-all ...); those are silenced for these
    two verbatim headers only, so every warning the build prints is ours to fix.
*/
#if defined (__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Weverything"
#elif defined (__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wconversion"
 #pragma GCC diagnostic ignored "-Wsign-conversion"
 #pragma GCC diagnostic ignored "-Wfloat-equal"
 #pragma GCC diagnostic ignored "-Wshadow"
 #pragma GCC diagnostic ignored "-Wunused-variable"
#endif

#include "YayEngine.h"
#include "../tools/presets.h"

#if defined (__clang__)
 #pragma clang diagnostic pop
#elif defined (__GNUC__)
 #pragma GCC diagnostic pop
#endif

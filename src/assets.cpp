#include "gfx.hpp"
// truetype font embedded in a header:
// Downloaded from:
// https://github.com/edx/edx-fonts/blob/master/open-sans/fonts/Regular/OpenSans-Regular.ttf
// Converted with:
// https://codewitch-honey-crisis.github.io/gfx_web/header/index.html
#define DE_VALENCIA_IMPLEMENTATION
#include "de_valencia.hpp"
#undef DE_VALENCIA_IMPLEMENTATION
gfx::const_buffer_stream& text_font_stream = de_valencia;

// generated using https://codewitch-honey-crisis.github.io/gfx_web/icon-pack/index.html
#define ICONS_IMPLEMENTATION
#include "icons.hpp"
#undef ICONS_IMPLEMENTATION
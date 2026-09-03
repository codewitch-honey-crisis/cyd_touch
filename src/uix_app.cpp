#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include "cyd28.hpp"
#include "icons.hpp"
#include "text_font_stream.hpp"
#include "piano_box.hpp"

#define BL_FULL 100

// import the gfx and uix namespaces since we'll be using them all over
using namespace gfx;
using namespace uix;
// UIX uses RGBA8888 so create a color enum for that:
using uix_color_t = color<uix_pixel>;

static size16 icon_dim(0,0);
static uint8_t* icon_data = nullptr;
static gfx::rectf correct_aspect(const rect16& r, float aspect) {
    rect16 result = r;
    if (aspect>=1.f) {
        result.y2 /= aspect;
    } else {
        result.x2 *= aspect;
    }
    return (rectf)result;
}
// Take the SVG icons, scale them to the target size and rasterize them
static bitmap<alpha_pixel<8>> create_icon_data(const_buffer_stream& icon_stream, sizef icon_dimensions, uint16_t icon_size) {
    icon_dim = size16(icon_size,icon_size);

    // create a new bitmap in 4-bit grayscale
    auto bmp = create_bitmap<gsc_pixel<8>>(icon_dim);
    canvas cvs(icon_dim);
    rectf corrected = correct_aspect(bmp.bounds(), icon_dimensions.aspect_ratio());
    matrix fit;
    const size_t size = bitmap<gsc_pixel<8>>::sizeof_buffer(bmp.dimensions());
    if(bmp.begin()==nullptr) {
        // out of memory
        goto error;
    }
    // fill with white
    bmp.fill(bmp.bounds(),gsc_pixel<8>(255));
    // assign the bitmap array entry to the bitmap's buffer
    icon_data=bmp.begin();
    
    if(gfx_result::success!=cvs.initialize()) {
        // out of memory
        goto error;
    }
    // link the canvas and the bitmap so the canvas can draw on it
    if(gfx_result::success!=draw::canvas(bmp,cvs)) {
        // can't imagine why this would fail
        puts("Failed to bind canvas to bitmap");
        goto error;
    }
    corrected.center_inplace((gfx::rectf)bmp.bounds());
    fit = matrix::create_fit_to(icon_dimensions,corrected);
    icon_stream.seek(0); // make sure we're at the beginning
    if(gfx_result::success!=cvs.render_svg(icon_stream,fit)) {
        puts("Error rasterizing SVG");
        goto error;
    }
    cvs.deinitialize();
    // invert, because it's black on white, but we need an alpha transparency map
    for(size_t j = 0;j<size;++j) {
        uint8_t d = icon_data[j];
        icon_data[j]=255-d;
    }
    
    return bitmap<alpha_pixel<8>>(bmp.dimensions(),icon_data);
error:
    if(icon_data!=nullptr) {
        free(icon_data);
        icon_data = nullptr;
    }

    puts("Error creating icon");
    return bitmap<alpha_pixel<8>>({0,0},nullptr);
}

// Rounded frequencies for octave 4 (C4..B4), in Hz.
// C4 D4  E   F   G   A   B, chromatic including sharps.
const uint16_t note_freqs[12] = {
    262,  // C4
    277,  // C#4
    294,  // D4
    311,  // D#4
    330,  // E4
    349,  // F4
    370,  // F#4
    392,  // G4
    415,  // G#4
    440,  // A4
    466,  // A#4
    494,  // B4
};

// note: 0 = C, 1 = C#, ... 11 = B
// octave: 4 = the base octave. Higher octaves double the frequency.
static uint16_t note_to_freq(uint8_t note_num) {
    uint8_t note = note_num % 12;
    uint8_t octave = note_num / 12;
    if (note > 11) return 0;          // guard
    uint16_t freq = note_freqs[note];
    if (octave >= 4) {
        freq <<= (octave - 4);        // double per octave up
    } else {
        freq >>= (4 - octave);        // halve per octave down
    }
    return freq;
}
using label_t = label<cyd28_surface_t>;
using button_t = painter<cyd28_surface_t>;
using toucher_t = painter<cyd28_surface_t>;
using color_picker_t = color_picker<cyd28_surface_t>;
using piano_box_t = piano_box<cyd28_surface_t>;

static bitmap<alpha_pixel<8>> back_icon, piano_icon, image_icon, picker_icon;
static tt_font text_font(text_font_stream,30,font_size_units::px);
cyd28_screen_t piano_screen,picker_screen,image_screen;
static label_t title;
static button_t piano_back,picker_back;
static button_t piano_choose,picker_choose,image_choose;
static color_picker_t picker;
static piano_box_t piano;
static toucher_t image_toucher;
typedef struct {
    bitmap<alpha_pixel<8>>* icon;
    uix_pixel color;
    cyd28_screen_t* screen;
    bool skip_invalidate;
} button_state_t;
static button_state_t piano_state,image_state,picker_state;
static mask_draw_cache draw_cache;

typedef struct stat stat_t;
typedef struct dirent dirent_t;
static int my_stricmp(const char* lhs, const char* rhs) {
    int result = 0;
    while (!result && *lhs && *rhs) {
        result = tolower(*lhs++) - tolower(*rhs++);
    }
    if (!result) {
        if (*lhs) {
            return 1;
        } else if (*rhs) {
            return -1;
        }
        return 0;
    }
    return result;
}

static stat_t fs_stat(const char* path) {
    stat_t s;
    stat(path, &s);
    return s;
}

static stat_t fs_st; 
static dirent_t* fs_de;
static DIR* fs_dir;
static char fs_path[256];
static file_stream fs_stm;
static jpg_image fs_jpg;
static int jpg_divisor = 1;
static size16 fs_jpg_dim;
static bool fs_valid_jpg=false;
static bool fs_has_jpgs = false;

static volatile bool image_mode = false;
static volatile int  image_flushing = 0;
static volatile int  image_buf_idx = 0;
static const size_t  image_buf_size = cyd28_lcd_transfer_buffer_size();

// centering + strip-batching state for the slideshow
static int  jpg_ox = 0, jpg_oy = 0;   // offset to center the image on the panel
static int  jpg_iw = 0, jpg_ih = 0;   // current (post-scale) image dimensions
static int  strip_y0 = -1;            // image-space y of the pending strip's top (-1 = none)
static int  strip_h  = 0;             // rows accumulated in the pending strip
static int  strip_cap = 0;            // rows the transfer buffer can hold at image width
static bool jpg_aborted = false;      // set when image_mode drops mid-frame
static cyd28_lcd_metrics_t screen_metrics;
static int jpg_last_iw = -1, jpg_last_ih = -1;

static int jpg_src_w = 0, jpg_src_h = 0;   // true 1:1 dimensions
// jpg_divisor already declared

static bool bl_fade(int from, int to, uint32_t ms) {
    const int steps = 16;
    if (ms < (uint32_t)steps) ms = steps;
    for (int i = 0; i <= steps; ++i) {
        cyd28_backlight((uint8_t)(from + (to - from) * i / steps));
        cyd28_update();
        if (!image_mode) return false;
        vTaskDelay(pdMS_TO_TICKS(ms / steps));
    }
    return true;
}

static bool jpg_fits(int divisor) {
    // ceiling division keeps us conservative on odd sizes so we never overshoot
    int sw = image_screen.dimensions().width;
    int sh = image_screen.dimensions().height;
    return (fs_jpg_dim.width  + divisor - 1) / divisor <= sw &&
           (fs_jpg_dim.height + divisor - 1) / divisor <= sh;
}

static bool jpg_next() {
    fs_valid_jpg=false;
    if(fs_dir==nullptr) return false;
    while((fs_de = readdir(fs_dir))!=nullptr) {
        size_t len = strlen(fs_de->d_name);
        if(len>4 && 0==my_stricmp(fs_de->d_name+(len-4),".jpg")) {
            strcpy(fs_path,"/sdcard/");
            strcat(fs_path,fs_de->d_name);
            fs_st = fs_stat(fs_path);
            bool is_dir = (fs_st.st_mode & S_IFMT)==S_IFDIR;
            if(!is_dir) {
                fs_stm = file_stream(fs_path);
                fs_jpg = jpg_image(fs_stm);
                if(fs_jpg.initialize()!=gfx_result::success) {
                    fs_stm.close();
                    return false;
                }
                fs_jpg_dim = fs_jpg.dimensions();
                jpg_src_w = fs_jpg_dim.width;    // read before any scale is applied
                jpg_src_h = fs_jpg_dim.height;
                jpg_scale scale = jpg_scale::scale_1_1;
                int div = 1;
                if(fs_jpg_dim.width>image_screen.dimensions().width || fs_jpg_dim.height>image_screen.dimensions().height) {
                    if(jpg_fits(2))      { scale = jpg_scale::scale_1_2; div = 2; }
                    else if(jpg_fits(4)) { scale = jpg_scale::scale_1_4; div = 4; }
                    else                 { scale = jpg_scale::scale_1_8; div = 8; }
                }
                jpg_divisor = div;  
                if(scale!=jpg_scale::scale_1_1) {
                    fs_jpg.deinitialize();
                    fs_stm.seek(0);
                    fs_jpg = jpg_image(fs_stm,scale);
                    if(gfx_result::success==fs_jpg.initialize()) {
                        fs_jpg_dim = fs_jpg.dimensions();
                        fs_has_jpgs = true;
                        fs_valid_jpg=true;
                        return true;
                    }
                } else {
                    // already fits at 1:1 — display as-is
                    fs_has_jpgs = true;
                    fs_valid_jpg=true;
                    return true;
                }
                fs_stm.close();
                fs_jpg.deinitialize();
            }
        }
    }
    return false;
}

static inline uint8_t* jpg_fill_buffer() {
    return image_buf_idx ? cyd28_lcd_transfer_buffer2() : cyd28_lcd_transfer_buffer();
}

static void jpg_fill_black(int x1,int y1,int x2,int y2) {
    if (x1 > x2 || y1 > y2) return;
    int w = x2 - x1 + 1;
    int rows = (int)(image_buf_size / ((size_t)w * 2));
    if (rows < 1) rows = 1;
    for (int y = y1; y <= y2; ) {
        int fh = (y + rows - 1 <= y2) ? rows : (y2 - y + 1);
        while (image_flushing) portYIELD();
        uint8_t* p = jpg_fill_buffer();
        memset(p, 0, (size_t)w * fh * 2);
        image_flushing = 1;
        cyd28_lcd_flush_bitmap(x1, y, x2, y + fh - 1, p);
        image_buf_idx ^= 1;
        y += fh;
    }
}

static void jpg_fill_borders() {
    if (jpg_iw == jpg_last_iw && jpg_ih == jpg_last_ih) return;  // layout unchanged
    jpg_last_iw = jpg_iw; jpg_last_ih = jpg_ih;
    int l = jpg_ox, t = jpg_oy, r = jpg_ox + jpg_iw - 1, b = jpg_oy + jpg_ih - 1;
    jpg_fill_black(0, 0, screen_metrics.width-1, t-1);                     // top
    jpg_fill_black(0, b+1, screen_metrics.width-1, screen_metrics.height-1);    // bottom
    jpg_fill_black(0, t, l-1, b);                                     // left
    jpg_fill_black(r+1, t, screen_metrics.width-1, b);                     // right
}
void cyd28_on_lcd_flush_complete() {
    image_flushing = 0;               // buffer swap is driven by the draw code now
}

static void jpg_begin_frame() {
    jpg_iw = (jpg_src_w + jpg_divisor - 1) / jpg_divisor;   // ceil
    jpg_ih = (jpg_src_h + jpg_divisor - 1) / jpg_divisor;
    if (jpg_iw > screen_metrics.width)  jpg_iw = screen_metrics.width;   // safety clamp
    if (jpg_ih > screen_metrics.height) jpg_ih = screen_metrics.height;
    jpg_ox = (screen_metrics.width  - jpg_iw) / 2; if (jpg_ox < 0) jpg_ox = 0;
    jpg_oy = (screen_metrics.height - jpg_ih) / 2; if (jpg_oy < 0) jpg_oy = 0;
    strip_cap = jpg_iw > 0 ? (int)(image_buf_size / ((size_t)jpg_iw * 2)) : 1;
    if (strip_cap < 1) strip_cap = 1;
    if (strip_cap > jpg_ih) strip_cap = jpg_ih;
    strip_y0 = -1; strip_h = 0; jpg_aborted = false;
    
}

// Flush the pending strip as one rectangle. cyd28_update() + the image_mode
// check live here so they run once per flush, not once per MCU.
static void jpg_flush_strip() {
    if (strip_y0 < 0 || strip_h <= 0) return;
    int fh = strip_h;
    if (strip_y0 + fh > jpg_ih) fh = jpg_ih - strip_y0;   // never push off-image rows
    if (fh <= 0) { strip_y0 = -1; strip_h = 0; return; }

    while (image_flushing) portYIELD();   // wait out the previous strip's DMA
    cyd28_update();                        // service UI/touch (may set image_mode=false)
    if (!image_mode) {                     // user left the slideshow -> abort
        jpg_aborted = true;
        strip_y0 = -1; strip_h = 0;
        return;
    }
     if (!image_mode) {                     // user left the slideshow -> abort
        jpg_aborted = true;
        strip_y0 = -1; strip_h = 0;
        return;
    }
    // Ramp the backlight up in step with how far down the image we've drawn,
    // so the picture "develops" from dark to full as it loads.
    if (jpg_ih > 0) {
        int done = strip_y0 + fh;                       // image rows done incl. this strip
        int pct = (int)((long)done * BL_FULL / jpg_ih);
        if (pct > BL_FULL) pct = BL_FULL;
        cyd28_backlight((uint8_t)pct);
    }
    uint8_t* p = jpg_fill_buffer();
    image_flushing = 1;
    cyd28_lcd_flush_bitmap(jpg_ox, jpg_oy + strip_y0,
                           jpg_ox + jpg_iw - 1, jpg_oy + strip_y0 + fh - 1, p);
    image_buf_idx ^= 1;                     // next strip fills the other buffer
    strip_y0 = -1; strip_h = 0;
}

static gfx_result jpg_image_draw_callback(const image_data& data, void* state) {
    using img_px_t = rgba_pixel<32>;
    const const_bitmap<img_px_t>& src_bmp = *data.bitmap.region;
    const size16  dim = src_bmp.dimensions();
    const point16 loc = data.bitmap.location;
    const int w = dim.width, h = dim.height;
    // Flush first if this MCU would overflow the pending strip's capacity.
    if (strip_y0 >= 0 && (loc.y + h - strip_y0) > strip_cap) {
        jpg_flush_strip();
        if (jpg_aborted) return gfx_result::canceled;
    }
    if (strip_y0 < 0) strip_y0 = loc.y;    // start a fresh strip at this row

    // Blit the MCU into the full-width strip buffer at its position.
    // band_bmp's width is the stride; draw::bitmap clips edge overrun for us.
    uint8_t* p = jpg_fill_buffer();
    bitmap<CYD28_PIXEL> band_bmp({(uint16_t)jpg_iw, (uint16_t)strip_cap}, p);
    rect16 dst((uint16_t)loc.x, (uint16_t)(loc.y - strip_y0),
               (uint16_t)(loc.x + w - 1), (uint16_t)(loc.y - strip_y0 + h - 1));
    draw::bitmap(band_bmp, dst, src_bmp, src_bmp.bounds());

    int need = loc.y + h - strip_y0;
    if (need > strip_cap) need = strip_cap; // guard against a too-small buffer
    if (need > strip_h) strip_h = need;
    return gfx_result::success;
}
static void jpg_end_frame() {
    jpg_flush_strip();   // flush the final partial strip
    while (image_flushing) portYIELD();  // wait for its DMA before we tear down the stream
}
static void picker_on_color_changed(uix_pixel color, void* state) {
    cyd28_led(color);
}
static void loop_task(void* arg) {
    TickType_t wdt_ts = xTaskGetTickCount();
    TickType_t jpg_ts = 0;
    bool in_image = false;   // have we set up this slideshow session yet
    while (1) {
        if (xTaskGetTickCount() >= wdt_ts + pdMS_TO_TICKS(200)) {
            wdt_ts = xTaskGetTickCount();
            vTaskDelay(5);
        }
        if(image_mode) {
            bool draw_now = false;
            if(!in_image) {
                // just entered the image screen: (re)start the slideshow
                in_image = true;
                if(fs_dir!=nullptr) closedir(fs_dir);
                fs_dir = opendir("/sdcard/");
                draw_now = true;                 // show first image right away
                jpg_ts = xTaskGetTickCount();
            }

            if(draw_now || xTaskGetTickCount() >= jpg_ts + pdMS_TO_TICKS(5000)) {
                jpg_ts = xTaskGetTickCount();
                // Fade the current image out before swapping. On the very first
                // image nothing's shown yet, so just cut straight to dark.
                if(draw_now) {
                    cyd28_backlight(0);
                } else if(!bl_fade(BL_FULL, 0, 300)) {
                    cyd28_backlight(BL_FULL);   // user tapped out mid-fade
                    continue;
                }
                bool got_jpg = false;
                if(jpg_next()) {
                    got_jpg = true;
                } else if(fs_has_jpgs) {
                    // reached the end - rewind and start over
                    closedir(fs_dir);
                    fs_dir = opendir("/sdcard/");
                    got_jpg = jpg_next();
                }
                if(got_jpg) {
                    jpg_begin_frame();
                    jpg_fill_borders();          // borders fill while the panel is dark
                    // Clear the image rect too, so the previous frame doesn't
                    // ghost through the not-yet-drawn rows during the ramp.
                    jpg_fill_black(jpg_ox, jpg_oy, jpg_ox + jpg_iw - 1, jpg_oy + jpg_ih - 1);
                    gfx_result r = fs_jpg.draw((rect16)image_screen.bounds(), jpg_image_draw_callback);
                    if (r != gfx_result::canceled && !jpg_aborted) jpg_end_frame();
                    fs_stm.close();
                    fs_jpg.deinitialize();
                    if (r == gfx_result::canceled || jpg_aborted) {
                        cyd28_backlight(BL_FULL);   // don't leave the menu dark
                        image_mode = false; in_image = false;
                        cyd28_display.active_screen(cyd28_default_screen);
                        continue;
                    }
                    cyd28_backlight(BL_FULL);       // pin to full; last strip may round short
                } else {
                    cyd28_backlight(BL_FULL);       // nothing to show; don't sit dark
                }
            }
        }
        cyd28_update();
    }
}
static void toucher_on_release(void* state) {
    image_mode = false;
    cyd28_display.active_screen(cyd28_default_screen);
}

static void piano_on_pressed(uint8_t key, bool pressed, void* state) {
    if(pressed) {
        int freq = note_to_freq(key+12*3);
        cyd28_tone(freq);
        return;
    }
    cyd28_tone();
}
static void back_button_on_paint(cyd28_surface_t& destination, const srect16& clip, void* state) {
    draw::icon(destination,spoint16::zero(),back_icon,cyd28_color_t::blue);
}
static void back_button_on_release(void* state) {
    cyd28_display.active_screen(cyd28_default_screen);
}
static void choose_button_on_paint(cyd28_surface_t& destination, const srect16& clip, void* state) {
    button_state_t& s = *(button_state_t*)state;
    draw::icon(destination,spoint16::zero(),*s.icon,s.color);
}
static bool button_on_touch(size_t locations_size,const spoint16* locations,void* state) {
    return true;
}
static void choose_button_on_release(void* state) {
    button_state_t& s = *(button_state_t*)state;
    cyd28_display.active_screen(*s.screen);
    if(s.skip_invalidate) {
        s.screen->validate_all();
    }
    image_mode = (s.screen == &image_screen);
}

extern "C" void app_main(void) {
    // initialize the CYD28
    cyd28_init();
    // set to whatever rotation you like (0-3 in 90 degree increments from default)
    cyd28_lcd_rotation(2); // landscape, flipped
    cyd28_backlight(BL_FULL);
    cyd28_lcd_metrics(&screen_metrics);
    cyd28_led_calibrate(200, 130, 255);        // gains only, gamma defaults to 2.8
    
    // preallocate our draw cache (not necessary, but slightly better performance)
    draw_cache.ensure(cyd28_default_screen.dimensions().width);
    bool sd_mounted = false;
    if(cyd28_sd_mount()) {
        sd_mounted = true;
        puts("SD card mounted");
    } else {
        puts("SD card not mounted");
    }
    back_icon = create_icon_data(icons_circle_arrow_left,icons_circle_arrow_left_dimensions,cyd28_default_screen.dimensions().height/10);
    piano_icon = create_icon_data(icons_music,icons_music_dimensions,cyd28_default_screen.dimensions().height/5);
    picker_icon = create_icon_data(icons_lightbulb,icons_lightbulb_dimensions,cyd28_default_screen.dimensions().height/5);
    image_icon = create_icon_data(icons_image,icons_image_dimensions,cyd28_default_screen.dimensions().height/5);
    text_font.initialize();
    // set up the screen and controls
    cyd28_default_screen.background_color(cyd28_color_t::white);

    title.bounds(srect16(0,0,cyd28_default_screen.dimensions().width-1,text_font.line_height()-1).offset(0,text_font.line_height()));
    title.color(uix_color_t::dark_goldenrod);
    title.font(text_font);
    title.text_justify(uix_justify::bottom_middle);
    title.text("CYD 2.8\"");
    cyd28_default_screen.register_control(title);

    piano_choose.bounds(((srect16)piano_icon.bounds()).center_vertical(cyd28_default_screen.bounds()).offset(10,0));
    piano_state.icon = &piano_icon;
    piano_state.color = uix_color_t::blue;
    piano_state.screen = &piano_screen;
    piano_state.skip_invalidate = false;
    piano_choose.on_paint_callback(choose_button_on_paint,&piano_state);
    piano_choose.on_touch_callback(button_on_touch,&piano_state);
    piano_choose.on_release_callback(choose_button_on_release,&piano_state);
    cyd28_default_screen.register_control(piano_choose);

    picker_choose.bounds(((srect16)picker_icon.bounds()).center_vertical(cyd28_default_screen.bounds()).offset(cyd28_default_screen.dimensions().width-10-picker_icon.dimensions().width,0));
    picker_state.icon = &picker_icon;
    picker_state.color = uix_color_t::red;
    picker_state.screen = &picker_screen;
    picker_state.skip_invalidate = false;
    picker_choose.on_paint_callback(choose_button_on_paint,&picker_state);
    picker_choose.on_touch_callback(button_on_touch,&picker_state);
    picker_choose.on_release_callback(choose_button_on_release,&picker_state);
    cyd28_default_screen.register_control(picker_choose);
    
    image_choose.bounds(((srect16)image_icon.bounds()).center(cyd28_default_screen.bounds()));
    image_state.icon = &image_icon;
    image_state.color = uix_color_t::green;
    image_state.screen = &image_screen;
    image_state.skip_invalidate = true;
    image_choose.on_paint_callback(choose_button_on_paint,&image_state);
    image_choose.on_touch_callback(button_on_touch,&image_state);
    image_choose.on_release_callback(choose_button_on_release,&image_state);
    image_choose.visible(sd_mounted);
    cyd28_default_screen.register_control(image_choose);
    
    piano_screen.dimensions(cyd28_default_screen.dimensions());
    piano_screen.background_color(cyd28_color_t::white);
    srect16 sr = piano_screen.bounds();
    sr.y2/=2;
    sr.center_vertical_inplace(piano_screen.bounds());
    piano.bounds(sr);
    piano.octaves(2);
    piano.draw_cache(draw_cache);
    piano.on_pressed(piano_on_pressed);
    piano_screen.register_control(piano);
    
    piano_back.bounds(srect16(spoint16::zero(),(ssize16)back_icon.dimensions()));
    piano_back.bounds(srect16(spoint16::zero(),(ssize16)back_icon.dimensions()));
    piano_back.on_paint_callback(back_button_on_paint);
    piano_back.on_touch_callback(button_on_touch);
    piano_back.on_release_callback(back_button_on_release);
    piano_screen.register_control(piano_back);

    picker_screen.dimensions(cyd28_default_screen.dimensions());
    picker_screen.background_color(cyd28_color_t::white);
    
    sr = picker_screen.bounds();
    sr.inflate_inplace(-piano_back.dimensions().width,-piano_back.dimensions().height);
    picker.bounds(sr);
    picker.on_color_changed_callback(picker_on_color_changed);
    picker_screen.register_control(picker);
    
    picker_back.bounds(srect16(spoint16::zero(),(ssize16)back_icon.dimensions()));
    picker_back.on_paint_callback(back_button_on_paint);
    picker_back.on_touch_callback(button_on_touch);
    picker_back.on_release_callback(back_button_on_release);
    picker_screen.register_control(picker_back);
    
    // we don't actually use this screen directly
    // it serves as a stub to prevent other screens
    // from refreshing the display while we're writing
    // images to it.
    image_screen.dimensions(cyd28_default_screen.dimensions());
    image_screen.background_color(cyd28_color_t::black);
    image_toucher.bounds(image_screen.bounds());
    image_toucher.on_touch_callback(button_on_touch); // reuse this since it's trivial
    image_toucher.on_release_callback(toucher_on_release);
    image_screen.register_control(image_toucher);
    image_screen.validate_all();
    cyd28_display.commit();
    fs_dir = nullptr;
    if(sd_mounted) {
        fs_dir = opendir("/sdcard/");
        
    }
    // start the app loop
    TaskHandle_t loop_handle;
    xTaskCreate(loop_task, "loop_task", 4096, nullptr, uxTaskPriorityGet(xTaskGetCurrentTaskHandle()), &loop_handle);
}

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include "cyd28.hpp"
#include "icons.hpp"
#include "text_font_stream.hpp"
#include "color_picker.hpp"
#include "piano_box.hpp"
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
using button_t = painter<cyd28_surface_t>;
using color_picker_t = color_picker<cyd28_surface_t>;
using piano_box_t = piano_box<cyd28_surface_t>;

static bitmap<alpha_pixel<8>> back_icon, piano_icon, picker_icon;
cyd28_screen_t piano_screen,picker_screen;
static button_t piano_back,picker_back;
static button_t piano_choose,picker_choose;
static color_picker_t picker;
static piano_box_t piano;

static mask_draw_cache draw_cache;

static void loop_task(void* arg) {
    TickType_t wdt_ts = xTaskGetTickCount();
    uix::uix_pixel old_color;
    while (1) {
        // feed the watchdog timer
        if (xTaskGetTickCount() >= wdt_ts + pdMS_TO_TICKS(200)) {
            wdt_ts = xTaskGetTickCount();
            vTaskDelay(5);
        }
        if(old_color!=picker.color()) {
            old_color = picker.color();
            cyd28_led(old_color);
        }
        // must be called in app loop
        cyd28_update();
    }
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
    bitmap<alpha_pixel<8>>* bmp = (bitmap<alpha_pixel<8>>*)state;
    draw::icon(destination,spoint16::zero(),*bmp,cyd28_color_t::red);
}
static bool button_on_touch(size_t locations_size,const spoint16* locations,void* state) {
    return true;
}
static void choose_button_on_release(void* state) {
    cyd28_display.active_screen(*(screen_base*)state);
}
extern "C" void app_main(void) {
    // initialize the CYD28
    cyd28_init();
    // preallocate our draw cache (not necessary, but slightly better performance)
    draw_cache.ensure(cyd28_default_screen.dimensions().width);

    if(cyd28_sd_mount()) {
        puts("SD card mounted");
    } else {
        puts("SD card not mounted");
    }
    back_icon = create_icon_data(icons_circle_arrow_left,icons_circle_arrow_left_dimensions,cyd28_default_screen.dimensions().height/10);
    piano_icon = create_icon_data(icons_music,icons_music_dimensions,cyd28_default_screen.dimensions().height/5);
    picker_icon = create_icon_data(icons_lightbulb,icons_lightbulb_dimensions,cyd28_default_screen.dimensions().height/5);
    // set up the screen and controls
    cyd28_default_screen.background_color(cyd28_color_t::white);
    piano_choose.bounds(((srect16)piano_icon.bounds()).center_vertical(cyd28_default_screen.bounds()).offset(10,0));
    piano_choose.on_paint_callback(choose_button_on_paint,&piano_icon);
    piano_choose.on_touch_callback(button_on_touch);
    piano_choose.on_release_callback(choose_button_on_release,&piano_screen);
    cyd28_default_screen.register_control(piano_choose);

    picker_choose.bounds(((srect16)picker_icon.bounds()).center_vertical(cyd28_default_screen.bounds()).offset(cyd28_default_screen.dimensions().width-10-picker_icon.dimensions().width,0));
    picker_choose.on_paint_callback(choose_button_on_paint,&picker_icon);
    picker_choose.on_touch_callback(button_on_touch);
    picker_choose.on_release_callback(choose_button_on_release,&picker_screen);
    cyd28_default_screen.register_control(picker_choose);
    
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
    picker_screen.register_control(picker);
    
    picker_back.bounds(srect16(spoint16::zero(),(ssize16)back_icon.dimensions()));
    picker_back.on_paint_callback(back_button_on_paint);
    picker_back.on_touch_callback(button_on_touch);
    picker_back.on_release_callback(back_button_on_release);
    picker_screen.register_control(picker_back);
    cyd28_display.commit();
    // start the app loop
    TaskHandle_t loop_handle;
    xTaskCreate(loop_task, "loop_task", 4096, nullptr, uxTaskPriorityGet(xTaskGetCurrentTaskHandle()), &loop_handle);
}

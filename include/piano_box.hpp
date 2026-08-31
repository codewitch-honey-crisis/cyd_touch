#pragma once
#include <memory.h>
#include <uix.hpp>
#include <gfx.hpp>

template<typename ControlSurfaceType>
class piano_box : public uix::control<ControlSurfaceType> {
public:
    using type = piano_box;
    using control_surface_type = ControlSurfaceType;
    using pixel_type = typename ControlSurfaceType::pixel_type;
    using palette_type = typename ControlSurfaceType::palette_type;
    typedef void(*on_pressed_callback_type)(uint8_t key, bool pressed, void* state);
private:
    using base_type = uix::control<ControlSurfaceType>;
    static constexpr const uint8_t white_semi[7] = {0,2,4,5,7,9,11};
    gfx::mask_draw_cache* m_dc;
    gfx::mask_draw_cache m_local_dc;
    uix::uix_pixel m_pressed_color;
    size_t m_octaves;
    uint8_t m_pressed_key;
    int16_t m_key_width;
    int16_t m_x_offset;
    on_pressed_callback_type m_on_pressed_callback;
    void* m_on_pressed_callback_state;
    void recompute_sizing() {
        m_pressed_key=0xFF;
        m_key_width = this->dimensions().width/m_octaves/7;
        // leftover from integer truncation, split evenly to center the block
        const int16_t used = (int16_t)(m_key_width * 7 * (int)m_octaves);
        m_x_offset = (int16_t)((this->dimensions().width - used) / 2);
    }
public:
    piano_box() : m_dc(&m_local_dc),m_octaves(1) {
        m_x_offset = 0;
        // orange, 50% opacity
        m_pressed_color = uix::uix_pixel(255,127,0,127);
        m_pressed_key=0xFF;
        m_on_pressed_callback = nullptr;
    }
    gfx::mask_draw_cache& draw_cache() const {
        return *m_dc;
    }
    void draw_cache(gfx::mask_draw_cache& value) {
        m_dc = &value;
    }
    uix::uix_pixel pressed_color() const {
        return m_pressed_color;
    }
    void pressed_color(uix::uix_pixel value) {
        if(value!=m_pressed_color) {
            m_pressed_color = value;
            this->invalidate();
        }
    }
    size_t octaves() const {
        return m_octaves;
    }
    void octaves(size_t value) {
        value = gfx::math::clamp<size_t>(value,1,5);
        if(value!=m_octaves) {
            m_octaves=value;
            recompute_sizing();
            this->invalidate();
        }
    }
    void on_pressed(on_pressed_callback_type callback,void* state = nullptr) {
        m_on_pressed_callback = callback;
        m_on_pressed_callback_state = state;
    }
    on_pressed_callback_type on_pressed() const {
        return m_on_pressed_callback;
    }
    void* on_pressed_state() const {
        return m_on_pressed_callback_state;
    }
protected:
    virtual void on_after_resize() override {
        recompute_sizing();
        this->invalidate(); // may be redundant
    }
    virtual void on_before_paint() override {
        
    }
    virtual void on_paint(control_surface_type& destination, const gfx::srect16& clip) override {
        gfx::draw::filled_rectangle(destination,destination.bounds(),uix::uix_pixel(0xFF,0xFF,0xFF,0xFF));
        gfx::srect16 sr(m_x_offset,0,m_x_offset+m_key_width-1,destination.dimensions().height-1);
        static constexpr const uix::uix_pixel gray(31,31,31,0xFF);
        static constexpr const uix::uix_pixel black(0,0,0,0xFF);
        static constexpr const uix::uix_pixel pressed(0x7F,0x7F,0xFF,0xFF);   // whatever you like
        
        const size_t key_count = 7 * m_octaves;
        size_t actual_key = 0;

        // white keys
        for(size_t i = 0; i < key_count; ++i) {
            actual_key = (i/7)*12 + white_semi[i%7];
            const bool is_pressed = (actual_key == m_pressed_key);
            gfx::draw::aa_rounded_rectangle(destination,sr,gray,2,m_key_width/10,m_dc);
            if(is_pressed) {
                // e.g. fill the white key body to show it's down
                gfx::draw::aa_filled_rounded_rectangle(destination,sr,m_pressed_color,2,m_dc);
            }
            sr.offset_inplace(m_key_width,0);
        }

        sr.y2*=10;
        sr.y2/=16;
        sr.x1 = m_x_offset + m_key_width/2+(m_key_width/4);
        sr.x2 = sr.x1+(m_key_width/2);
        int key_skip = 0;
        // black keys
        for(size_t i = 0; i < key_count - 1; ++i) {
            ++key_skip;
            if(key_skip != 3 && key_skip != 7) {   // skip E->F and B->C gaps
                actual_key = (i/7)*12 + white_semi[i%7] + 1;   // sharp of white key i
                const bool is_pressed = (actual_key == m_pressed_key);
                gfx::draw::aa_filled_rounded_rectangle(destination, sr, black, 2, m_dc);
                if(is_pressed) {
                    gfx::draw::aa_filled_rounded_rectangle(destination, sr, m_pressed_color, 2, m_dc);
                }
            }
            if(key_skip == 7) {
                key_skip = 0;
            }
            sr.offset_inplace(m_key_width, 0);
        }
    }
    virtual bool on_touch(size_t locations_size, const gfx::spoint16* locations) override {
        uint8_t result = 0xFF;
        if (locations_size > 0) {
            const int16_t w = (int16_t)m_key_width;
            const int16_t H = (int16_t)this->dimensions().height;   // control height
            const size_t  white_count = 7 * m_octaves;

            const int16_t x = locations[0].x;   // primary touch
            const int16_t y = locations[0].y;

            // Black keys sit on top, so test them first -- but only in the
            // upper band where they exist (mirrors the 10/16 height in on_paint).
            const int16_t black_bottom = (int16_t)(((int32_t)(H - 1) * 10) / 16);

            if (y >= 0 && y <= black_bottom) {
                int     key_skip = 0;
                int16_t bx1 = m_x_offset + w/2 + w/4;
                for (size_t i = 0; i + 1 < white_count; ++i) {
                    ++key_skip;
                    if (key_skip != 3 && key_skip != 7) {    // no black after E or B
                        const int16_t bx2 = bx1 + w/2;
                        if (x >= bx1 && x <= bx2) {
                            // this black key is the sharp of white key i => whiteChromatic + 1
                            result = (uint8_t)((i/7)*12 + white_semi[i%7] + 1);
                            break;
                        }
                    }
                    if (key_skip == 7) key_skip = 0;
                    bx1 += w;
                }
            }

            // No black hit -> the white key column underneath.
            if (result == 0xFF && x >= m_x_offset && y >= 0 && y <= (H - 1)) {
                const size_t wi = (size_t)((x - m_x_offset) / w);
                if (wi < white_count) {
                    result = (uint8_t)((wi/7)*12 + white_semi[wi%7]);
                }
            }
        }

        m_pressed_key = result;
        this->invalidate();
        if(m_pressed_key!=255 && m_on_pressed_callback!=nullptr) {
            m_on_pressed_callback(m_pressed_key,true, m_on_pressed_callback_state);
        }
        return true;
    }
virtual void on_release() override {
    uint8_t key = m_pressed_key;
    m_pressed_key = 0xFF;
    this->invalidate();   
    if(key!=255 && m_on_pressed_callback!=nullptr) {
        m_on_pressed_callback(key,false, m_on_pressed_callback_state);
    }
}
};
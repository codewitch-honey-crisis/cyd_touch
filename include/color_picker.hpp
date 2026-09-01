#pragma once
#include <memory.h>
#include <uix.hpp>
#include <gfx.hpp>

template<typename ControlSurfaceType>
class color_picker : public uix::control<ControlSurfaceType> {
public:
    using type = color_picker;
    using control_surface_type = ControlSurfaceType;
    using pixel_type = typename ControlSurfaceType::pixel_type;
    using palette_type = typename ControlSurfaceType::palette_type;
private:
    using base_type = uix::control<ControlSurfaceType>;
    gfx::mask_draw_cache* m_dc;
    gfx::mask_draw_cache m_local_dc;
    uint8_t m_hue;
    uint8_t m_saturation;
    uint8_t m_value;
    uix::uix_pixel m_color;
    gfx::spoint16 m_pick;
    bool m_pick_visible;
    static const constexpr int16_t pick_size = 16;
public:
    color_picker() : m_dc(&m_local_dc),m_hue(0),m_saturation(0),m_value(0),m_color(0,true),m_pick(0,0),m_pick_visible(false) {
    }
    gfx::mask_draw_cache& draw_cache() const {
        return *m_dc;
    }
    void draw_cache(gfx::mask_draw_cache& value) {
        m_dc = &value;
    }
    uix::uix_pixel color() const {
        return m_color;
    }
protected:
    
    virtual void on_before_paint() override {
        
    }
    virtual void on_paint(control_surface_type& destination, const gfx::srect16& clip) override {
        const int16_t bar_size = destination.dimensions().height / 10;
        const int16_t width    = destination.dimensions().width;
        const int16_t height   = destination.dimensions().height;
        const int16_t region_h = height - bar_size;   // SV rows: 0 .. region_h-1

        // normalize clip bounds (in case x1>x2 / y1>y2) and clamp to the control
        int16_t cx1 = clip.x1 < clip.x2 ? clip.x1 : clip.x2;
        int16_t cx2 = clip.x1 < clip.x2 ? clip.x2 : clip.x1;
        int16_t cy1 = clip.y1 < clip.y2 ? clip.y1 : clip.y2;
        int16_t cy2 = clip.y1 < clip.y2 ? clip.y2 : clip.y1;
        if (cx1 < 0) { cx1 = 0; }
        if (cx2 > width - 1)  { cx2 = width - 1; }
        if (cy1 < 0) { cy1 = 0;  }
        if (cy2 > height - 1) {cy2 = height - 1;}

        // --- 2D S/V gradient: clipped, drawn as runs of equal color per row ---
        const int16_t sv_y1 = cy1;
        const int16_t sv_y2 = (cy2 < region_h - 1) ? cy2 : (int16_t)(region_h - 1);
        using px_t = typename control_surface_type::pixel_type;   // surface's native format
        for (int16_t y = sv_y1; y <= sv_y2; ++y) {
            uint8_t v = (region_h > 1)
                ? (uint8_t)(((long)(region_h - 1 - y) * 255) / (region_h - 1))
                : 255;

            int16_t run_start = cx1;
            px_t run_px;
            bool have_run = false;
            for (int16_t x = cx1; x <= cx2; ++x) {
                uint8_t s = (width > 1)
                    ? (uint8_t)(((long)x * 255) / (width - 1))
                    : 0;
                gfx::hsv_pixel<24> hsv(m_hue, s, v);
                px_t native;
                gfx::convert(hsv, &native);        // one conversion; compare in native space

                if (!have_run) {
                    run_start = x; run_px = native; have_run = true;
                } else if (native != run_px) {
                    gfx::draw::filled_rectangle(
                        destination,
                        gfx::srect16(run_start, y, (int16_t)(x - 1), y),
                        run_px);
                    run_start = x; run_px = native;
                }
            }
            if (have_run) {
                gfx::draw::filled_rectangle(
                    destination,
                    gfx::srect16(run_start, y, cx2, y),
                    run_px);
            }
        }

        // --- horizontal hue bar: skip entirely if clip misses its rows ---
        const int16_t hbar_y1 = height - bar_size;
        const int16_t hbar_y2 = height - 1;
        if (cy2 >= hbar_y1 && cy1 <= hbar_y2) {
            int16_t prev_x = 0;
            for (int h = 0; h <= 255; ++h) {
                int16_t next_x = (int16_t)(((long)(h + 1) * width) / 256);
                if (next_x > prev_x) {
                    // this block spans [prev_x, next_x-1]; draw only if it overlaps clip x
                    if (next_x - 1 >= cx1 && prev_x <= cx2) {
                        gfx::hsv_pixel<24> color(h, 255, 127);
                        gfx::draw::filled_rectangle(
                            destination,
                            gfx::srect16(prev_x, hbar_y1, (int16_t)(next_x - 1), hbar_y2),
                            color);
                    }
                    prev_x = next_x;
                }
            }
        }
        if(m_pick_visible) {
            const gfx::srect16 pick(m_pick.x,m_pick.y,m_pick.x+pick_size-1,m_pick.y+pick_size-1);
            gfx::draw::rectangle(destination,pick,uix::uix_pixel(0,0,0,0xFF));
            gfx::draw::filled_rectangle(destination,pick.inflate(-1,-1),m_color);
        }
    }
    virtual bool on_touch(size_t locations_size, const gfx::spoint16* locations) override {
        const int16_t width    = this->dimensions().width;
        const int16_t height   = this->dimensions().height;
        const int16_t bar_size = height / 10;
        const int16_t region_h = height - bar_size;   // SV plane rows: 0 .. region_h-1

        gfx::spoint16 pt = locations[0];
        
        if (pt.y >= height - bar_size) {
            // --- hue bar: inverse of  x = hue * width / 256 ---
            int h = ((int)pt.x * 256) / width;
            if (h > 255) h = 255;
            m_hue = (uint8_t)h;
            m_pick_visible = true;
            m_pick = locations[0].offset(4,-pick_size-4);
            gfx::hsv_pixel<24> hsv(m_hue,m_saturation,m_value);
            gfx::convert(hsv,&m_color);
        
            this->invalidate(); // force a repaint
        } else {
            // --- SV plane: inverse of the on_paint ramps ---
            // S min at left, max at right:  s = x * 255 / (width-1)
            m_saturation = (width > 1)
                ? (uint8_t)(((long)pt.x * 255) / (width - 1))
                : 0;
            // V max at top (y=0), min at bottom:  v = (region_h-1-y) * 255 / (region_h-1)
            int16_t yy = pt.y;
            if (yy > region_h - 1) yy = region_h - 1;   // in case bar rounding overlaps
            m_value = (region_h > 1)
                ? (uint8_t)(((long)(region_h - 1 - yy) * 255) / (region_h - 1))
                : 255;
            gfx::hsv_pixel<24> hsv(m_hue,m_saturation,m_value);
            gfx::convert(hsv,&m_color);
        
            if(m_pick_visible) {
                this->invalidate(gfx::srect16(m_pick.x,m_pick.y,m_pick.x+pick_size-1,m_pick.y+pick_size-1));
            }
            m_pick_visible = true;
            m_pick = locations[0].offset(4,-pick_size-4);
            if(m_pick.x<0) {
                m_pick.x += (pick_size+4);
            }
            if(m_pick.y<0) {
                m_pick.y += (pick_size+4);
            }
            if(m_pick.x+pick_size>this->dimensions().width) {
                m_pick.x = this->dimensions().width - pick_size;
            }
            if(m_pick.y+pick_size>this->dimensions().height) {
                m_pick.y = this->dimensions().height - pick_size;
            }
            this->invalidate(gfx::srect16(m_pick.x,m_pick.y,m_pick.x+pick_size-1,m_pick.y+pick_size-1));
        }

        return true;
    }
    virtual void on_release() override {
        if(m_pick_visible) { // should always be true could assert it i suppose
            this->invalidate(gfx::srect16(m_pick.x,m_pick.y,m_pick.x+pick_size-1,m_pick.y+pick_size-1));
        }
        m_pick_visible = false;
    }  
};
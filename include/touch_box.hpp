#pragma once
#include <uix.hpp>
#include <gfx.hpp>
template<typename ControlSurfaceType>
class touch_box : public uix::control<ControlSurfaceType> {
public:
    using type = touch_box;
    using control_surface_type = ControlSurfaceType;
    using pixel_type = typename ControlSurfaceType::pixel_type;
    using palette_type = typename ControlSurfaceType::palette_type;
private:
    using base_type = uix::control<ControlSurfaceType>;
    bool m_touched;
    gfx::spoint16 m_point;
    bool m_commit_touched;
    gfx::spoint16 m_commit_point;
    gfx::mask_draw_cache* m_dc;
    gfx::mask_draw_cache m_local_dc;
    uix::uix_pixel m_color;
    
public:
    touch_box() : m_touched(false),m_point(gfx::spoint16::zero()),m_dc(&m_local_dc) {

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
    void color(uix::uix_pixel value) {
        if(value!=m_color) {
            m_color = value;
            this->invalidate();
        }
    }
protected:
    virtual void on_before_paint() override {
        m_commit_touched = m_touched;
        m_commit_point = m_point;
    }
    virtual void on_paint(control_surface_type& destination, const gfx::srect16& clip) override {
        if(m_commit_touched) {
            gfx::draw::aa_filled_arc(destination, gfx::srect16(m_commit_point,20),m_color,0,360,m_dc,&clip);
        }
    }
    virtual bool on_touch(size_t locations_size, const gfx::spoint16* locations) override {
        m_touched = true;
        m_point = locations[0];
        this->invalidate();
        return true;
    }
    virtual void on_release() override {
        m_touched = false;
        this->invalidate();
    }
};
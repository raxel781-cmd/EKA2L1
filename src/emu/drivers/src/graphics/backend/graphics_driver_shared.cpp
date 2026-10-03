/*
 * Copyright (c) 2019 EKA2L1 Team.
 * 
 * This file is part of EKA2L1 project
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <common/algorithm.h>
#include <common/log.h>
#include <common/platform.h>

#include <cstdint>
#include <limits>
#include <vector>

#include <drivers/graphics/backend/graphics_driver_shared.h>
#include <drivers/graphics/buffer.h>
#include <drivers/graphics/shader.h>

#if !EKA2L1_PLATFORM(IOS)
#include <glad/glad.h>
#endif

namespace eka2l1::drivers {
    static void translate_bpp_to_format(const int bpp, texture_format &internal_format, texture_format &format,
        texture_data_type &data_type, const bool stricted) {
        // Hope the driver likes this, it always does.
        internal_format = texture_format::rgba;
        data_type = texture_data_type::ubyte;

        switch (bpp) {
        case 8:
#if EKA2L1_PLATFORM(IOS)
            // The iOS GLES simulator samples single-channel R8 / GL_RED
            // textures as 0 (black) even though the byte data uploads without
            // any GL error (and renders fine on desktop GL). This breaks every
            // AVKON glyph/icon alpha mask (gray256), which S60v5 apps lean on
            // heavily — the masked text ended up as solid black blocks. Promote
            // 8bpp bitmaps to RGBA8 instead; the source bytes are expanded to
            // (v,v,v,v) at upload time (see update_bitmap), so the mask shader's
            // maskValue.r still reads the coverage.
            format = texture_format::rgba;
            internal_format = texture_format::rgba;
#else
            format = texture_format::r;

            if (stricted)
                internal_format = texture_format::r8;
#endif

            break;

        case 12:
            format = texture_format::rgba;
            internal_format = texture_format::rgba4;
            data_type = texture_data_type::ushort_4_4_4_4;

            break;

        case 16:
#if EKA2L1_PLATFORM(IOS)
            // Same iOS GLES story as the 8bpp case: a GL_RGB / RGB565 texture
            // that AVKON renders glyph runs into (window-server backing bitmaps)
            // comes back black on the simulator. Promote to RGBA8 and unpack the
            // 565 source to RGBA at upload time (see update_bitmap).
            format = texture_format::rgba;
            internal_format = texture_format::rgba;
#else
            format = texture_format::rgb;
            data_type = texture_data_type::ushort_5_6_5;

            if (stricted)
                internal_format = texture_format::rgb;
#endif

            break;

        case 24:
#if EKA2L1_PLATFORM(IOS)
            format = texture_format::rgba;
            internal_format = texture_format::rgba;
#else
            if (stricted) {
                format = texture_format::rgb;
                internal_format = texture_format::rgb;
            } else {
                format = texture_format::bgr;
            }
#endif

            break;

        case 32:
#if EKA2L1_PLATFORM(IOS)
            format = texture_format::rgba;
            internal_format = texture_format::rgba;
#else
            format = texture_format::bgra;

            if (stricted)
                internal_format = texture_format::bgra;
#endif

            break;

        default:
            break;
        }
    }

    static texture_ptr instantiate_suit_color_bitmap_texture(graphics_driver *driver, const eka2l1::vec2 &size, const int bpp) {
        auto texture = make_texture(driver);

        texture_format internal_format = texture_format::none;
        texture_format data_format = texture_format::none;
        texture_data_type data_type = texture_data_type::ubyte;

        translate_bpp_to_format(bpp, internal_format, data_format, data_type, driver->is_stricted());

        texture->create(driver, 2, 0, eka2l1::vec3(size.x, size.y, 0), internal_format, data_format, data_type, nullptr, 0);
        texture->set_filter_minmag(false, drivers::filter_option::linear);
        texture->set_filter_minmag(true, drivers::filter_option::linear);

        if (bpp == 12) {
            texture->set_channel_swizzle({ channel_swizzle::green, channel_swizzle::blue,
                channel_swizzle::alpha, channel_swizzle::one });
        }

        return texture;
    }

    static texture_ptr instantiate_bitmap_depth_stencil_texture(graphics_driver *driver, const eka2l1::vec2 &size) {
        auto ds_tex = make_texture(driver);
        ds_tex->create(driver, 2, 0, eka2l1::vec3(size.x, size.y, 0),
            texture_format::depth24_stencil8, texture_format::depth_stencil, texture_data_type::uint_24_8,
            nullptr, 0);

        return ds_tex;
    }

    bitmap::bitmap(graphics_driver *driver, const eka2l1::vec2 &size, const int initial_bpp)
        : bpp(initial_bpp)
        , logical_size(size) {
        // Make color buffer for our bitmap!
        tex = std::move(instantiate_suit_color_bitmap_texture(driver, size, bpp));
    }

    bitmap::~bitmap() {
        tex.reset();
        ds_tex.reset();
        fb.reset();
    }

    void bitmap::resize(graphics_driver *driver, const eka2l1::vec2 &new_size) {
    // Keep the logical game/framebuffer resolution unchanged.
    logical_size = new_size;

    // Framebuffers use the physical internal render resolution.
    const eka2l1::vec2 physical_size =
        fb ? eka2l1::vec2{
                 new_size.x * render_scale,
                 new_size.y * render_scale
             }
           : new_size;

    auto tex_to_replace = std::move(
        instantiate_suit_color_bitmap_texture(driver, physical_size, bpp));

    auto ds_tex_to_replace = std::move(
        instantiate_bitmap_depth_stencil_texture(driver, physical_size));

    if (fb) {
        auto new_fb = make_framebuffer(
            driver,
            { tex_to_replace.get() },
            { 0 },
            ds_tex_to_replace.get(),
            0,
            ds_tex_to_replace.get(),
            0);

        fb->bind(driver, framebuffer_bind_read);
        new_fb->bind(driver, framebuffer_bind_draw);

        eka2l1::rect source_region;
        source_region.top = { 0, 0 };
        source_region.size = tex->get_size();

        eka2l1::rect dest_region;
        dest_region.top = { 0, 0 };
        dest_region.size = physical_size;

        fb->blit(
            source_region,
            dest_region,
            draw_buffer_bit_color_buffer,
            filter_option::linear);

        new_fb->unbind(driver);
        fb->unbind(driver);

        fb = std::move(new_fb);
    }

    tex = std::move(tex_to_replace);
    ds_tex = std::move(ds_tex_to_replace);
}


    void bitmap::init_fb(graphics_driver *driver) {
    // Internal framebuffer scale: 2x for the first validation build.
    render_scale = 2;

    // Keep logical game resolution separate from physical GPU resolution.
    const eka2l1::vec2 physical_size = {
        logical_size.x * render_scale,
        logical_size.y * render_scale
    };

    // Allocate the actual internal-resolution color buffer.
    tex = std::move(
        instantiate_suit_color_bitmap_texture(driver, physical_size, bpp));

    // Allocate matching depth/stencil storage.
    ds_tex = std::move(
        instantiate_bitmap_depth_stencil_texture(driver, physical_size));

    fb = make_framebuffer(
        driver,
        { tex.get() },
        { 0 },
        ds_tex.get(),
        0,
        ds_tex.get(),
        0);
}

}

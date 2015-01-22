/*****************************************************************************
 *
 * This file is part of Mapnik (c++ mapping toolkit)
 *
 * Copyright (C) 2014 Artem Pavlenko
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

// mapnik
#include <mapnik/warp.hpp>
#include <mapnik/config.hpp>
#include <mapnik/image_data.hpp>
#include <mapnik/image_scaling_traits.hpp>
#include <mapnik/image_util.hpp>
#include <mapnik/box2d.hpp>
#include <mapnik/view_transform.hpp>
#include <mapnik/raster.hpp>
#include <mapnik/proj_transform.hpp>

// agg
#include "agg_image_filters.h"
#include "agg_trans_bilinear.h"
#include "agg_span_interpolator_linear.h"
#include "agg_span_image_filter_rgba.h"
#include "agg_rendering_buffer.h"
#include "agg_pixfmt_rgba.h"
#include "agg_rasterizer_scanline_aa.h"
#include "agg_basics.h"
#include "agg_scanline_bin.h"
#include "agg_renderer_scanline.h"
#include "agg_span_allocator.h"
#include "agg_image_accessors.h"
#include "agg_renderer_scanline.h"

namespace mapnik {

template <typename T>
MAPNIK_DECL void warp_image (T & target, T const& source, proj_transform const& prj_trans,
                 box2d<double> const& target_ext, box2d<double> const& source_ext,
                 double offset_x, double offset_y, unsigned mesh_size, scaling_method_e scaling_method, double filter_factor)
{
    using image_data_type = T;
    using pixel_type = typename image_data_type::pixel_type;
    using pixfmt_pre = typename detail::agg_scaling_traits<image_data_type>::pixfmt_pre;
    using color_type = typename detail::agg_scaling_traits<image_data_type>::color_type;
    using renderer_base = agg::renderer_base<pixfmt_pre>;
    using interpolator_type = typename detail::agg_scaling_traits<image_data_type>::interpolator_type;

    constexpr std::size_t pixel_size = sizeof(pixel_type);

    view_transform ts(source.width(), source.height(),
                      source_ext);
    view_transform tt(target.width(), target.height(),
                      target_ext, offset_x, offset_y);

    std::size_t mesh_nx = std::ceil(source.width()/double(mesh_size) + 1);
    std::size_t mesh_ny = std::ceil(source.height()/double(mesh_size) + 1);

    image_data<double> xs(mesh_nx, mesh_ny);
    image_data<double> ys(mesh_nx, mesh_ny);

    // Precalculate reprojected mesh
    for(std::size_t j = 0; j < mesh_ny; ++j)
    {
        for (std::size_t i=0; i<mesh_nx; ++i)
        {
            xs(i,j) = std::min(i*mesh_size,source.width());
            ys(i,j) = std::min(j*mesh_size,source.height());
            ts.backward(&xs(i,j), &ys(i,j));
        }
    }
    prj_trans.backward(xs.getData(), ys.getData(), nullptr, mesh_nx*mesh_ny);

    agg::rasterizer_scanline_aa<> rasterizer;
    agg::scanline_bin scanline;
    agg::rendering_buffer buf(target.getBytes(),
                              target.width(),
                              target.height(),
                              target.width() * pixel_size);
    pixfmt_pre pixf(buf);
    renderer_base rb(pixf);
    rasterizer.clip_box(0, 0, target.width(), target.height());
    agg::rendering_buffer buf_tile(
        const_cast<unsigned char*>(source.getBytes()),
        source.width(),
        source.height(),
        source.width() * pixel_size);

    pixfmt_pre pixf_tile(buf_tile);

    using img_accessor_type = agg::image_accessor_clone<pixfmt_pre>;
    img_accessor_type ia(pixf_tile);

    agg::span_allocator<color_type> sa;
    // Project mesh cells into target interpolating raster inside each one
    for (std::size_t j = 0; j < mesh_ny - 1; ++j)
    {
        for (std::size_t i = 0; i < mesh_nx - 1; ++i)
        {
            double polygon[8] = {xs(i,j), ys(i,j),
                                 xs(i+1,j), ys(i+1,j),
                                 xs(i+1,j+1), ys(i+1,j+1),
                                 xs(i,j+1), ys(i,j+1)};
            tt.forward(polygon+0, polygon+1);
            tt.forward(polygon+2, polygon+3);
            tt.forward(polygon+4, polygon+5);
            tt.forward(polygon+6, polygon+7);

            rasterizer.reset();
            rasterizer.move_to_d(std::floor(polygon[0]), std::floor(polygon[1]));
            rasterizer.line_to_d(std::floor(polygon[2]), std::floor(polygon[3]));
            rasterizer.line_to_d(std::floor(polygon[4]), std::floor(polygon[5]));
            rasterizer.line_to_d(std::floor(polygon[6]), std::floor(polygon[7]));

            std::size_t x0 = i * mesh_size;
            std::size_t y0 = j * mesh_size;
            std::size_t x1 = (i+1) * mesh_size;
            std::size_t y1 = (j+1) * mesh_size;
            x1 = std::min(x1, source.width());
            y1 = std::min(y1, source.height());
            agg::trans_affine tr(polygon, x0, y0, x1, y1);
            if (tr.is_valid())
            {
                interpolator_type interpolator(tr);
                if (scaling_method == SCALING_NEAR)
                {
                    using span_gen_type = typename detail::agg_scaling_traits<image_data_type>::span_image_filter;
                    span_gen_type sg(ia, interpolator);
                    agg::render_scanlines_bin(rasterizer, scanline, rb, sa, sg);
                }
                else
                {
                    using span_gen_type = typename detail::agg_scaling_traits<image_data_type>::span_image_resample_affine;
                    agg::image_filter_lut filter;
                    detail::set_scaling_method(filter, scaling_method, filter_factor);
                    span_gen_type sg(ia, interpolator, filter);
                    agg::render_scanlines_bin(rasterizer, scanline, rb, sa, sg);
                }
            }

        }
    }
}

namespace detail {

struct warp_image_visitor
{
    warp_image_visitor (raster & target_raster, proj_transform const& prj_trans, box2d<double> const& source_ext,
                        double offset_x, double offset_y, unsigned mesh_size,
                        scaling_method_e scaling_method, double filter_factor)
        : target_raster_(target_raster),
          prj_trans_(prj_trans),
          source_ext_(source_ext),
          offset_x_(offset_x),
          offset_y_(offset_y),
          mesh_size_(mesh_size),
          scaling_method_(scaling_method),
        filter_factor_(filter_factor) {}

    void operator() (image_data_null const&) {}

    template <typename T>
    void operator() (T const& source)
    {
        using image_data_type = T;
        //source and target image data types must match
        if (target_raster_.data_.template is<image_data_type>())
        {
            image_data_type & target = util::get<image_data_type>(target_raster_.data_);
            warp_image (target, source, prj_trans_, target_raster_.ext_, source_ext_,
                        offset_x_, offset_y_, mesh_size_, scaling_method_, filter_factor_);
        }
    }

    raster & target_raster_;
    proj_transform const& prj_trans_;
    box2d<double> const& source_ext_;
    double offset_x_;
    double offset_y_;
    unsigned mesh_size_;
    scaling_method_e scaling_method_;
    double filter_factor_;
};

}

void reproject_and_scale_raster(raster & target, raster const& source,
                                proj_transform const& prj_trans,
                                double offset_x, double offset_y,
                                unsigned mesh_size,
                                scaling_method_e scaling_method)
{
    detail::warp_image_visitor warper(target, prj_trans, source.ext_, offset_x, offset_y, mesh_size,
                                      scaling_method, source.get_filter_factor());
    util::apply_visitor(warper, source.data_);
}

template MAPNIK_DECL void warp_image (image_rgba8&, image_rgba8 const&, proj_transform const&,
                                      box2d<double> const&, box2d<double> const&, double, double, unsigned, scaling_method_e, double);

template MAPNIK_DECL void warp_image (image_gray8&, image_gray8 const&, proj_transform const&,
                                      box2d<double> const&, box2d<double> const&, double, double, unsigned, scaling_method_e, double);

template MAPNIK_DECL void warp_image (image_gray16&, image_gray16 const&, proj_transform const&,
                                      box2d<double> const&, box2d<double> const&, double, double, unsigned, scaling_method_e, double);

template MAPNIK_DECL void warp_image (image_gray32f&, image_gray32f const&, proj_transform const&,
                                      box2d<double> const&, box2d<double> const&, double, double, unsigned, scaling_method_e, double);


}// namespace mapnik

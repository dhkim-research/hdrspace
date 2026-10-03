/*
 * HEIF codec.
 * Copyright (c) 2017-2025 Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of libheif.
 *
 * libheif is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libheif is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libheif.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "heif_image_handle.h"
#include "api_structs.h"
#include <climits>
#include <string>


void heif_image_handle_release(const heif_image_handle* handle)
{
  delete handle;
}


int heif_image_handle_is_primary_image(const heif_image_handle* handle)
{
  return handle->image->is_primary();
}


heif_item_id heif_image_handle_get_item_id(const heif_image_handle* handle)
{
  return handle->image->get_id();
}


int heif_image_handle_get_width(const heif_image_handle* handle)
{
  if (handle && handle->image) {
    uint32_t w = handle->image->get_width();
    if (w > INT_MAX) {
      return 0;
    }
    else {
      return static_cast<int>(w);
    }
  }
  else {
    return 0;
  }
}


int heif_image_handle_get_height(const heif_image_handle* handle)
{
  if (handle && handle->image) {
    uint32_t h = handle->image->get_height();
    if (h > INT_MAX) {
      return 0;
    }
    else {
      return static_cast<int>(h);
    }
  }
  else {
    return 0;
  }
}


int heif_image_handle_has_alpha_channel(const heif_image_handle* handle)
{
  // TODO: for now, also scan the grid tiles for alpha information (issue #708), but depending about
  // how the discussion about this structure goes forward, we might remove this again.

  return handle->context->has_alpha(handle->image->get_id()); // handle case in issue #708
  //return handle->image->get_alpha_channel() != nullptr;       // old alpha check that fails on alpha in grid tiles
}


int heif_image_handle_is_premultiplied_alpha(const heif_image_handle* handle)
{
  // TODO: what about images that have the alpha in the grid tiles (issue #708) ?
  return handle->image->is_premultiplied_alpha();
}


int heif_image_handle_get_luma_bits_per_pixel(const heif_image_handle* handle)
{
  return handle->image->get_luma_bits_per_pixel();
}


int heif_image_handle_get_chroma_bits_per_pixel(const heif_image_handle* handle)
{
  return handle->image->get_chroma_bits_per_pixel();
}


heif_error heif_image_handle_get_preferred_decoding_colorspace(const heif_image_handle* image_handle,
                                                               heif_colorspace* out_colorspace,
                                                               heif_chroma* out_chroma)
{
  Error err = image_handle->image->get_coded_image_colorspace(out_colorspace, out_chroma);
  if (err) {
    return err.error_struct(image_handle->image.get());
  }

  return heif_error_success;
}


int heif_image_handle_get_ispe_width(const heif_image_handle* handle)
{
  if (handle && handle->image) {
    return handle->image->get_ispe_width();
  }
  else {
    return 0;
  }
}


int heif_image_handle_get_ispe_height(const heif_image_handle* handle)
{
  if (handle && handle->image) {
    return handle->image->get_ispe_height();
  }
  else {
    return 0;
  }
}


int heif_image_handle_get_pixel_aspect_ratio(const heif_image_handle* handle, uint32_t* aspect_h, uint32_t* aspect_v)
{
  auto pasp = handle->image->get_property<Box_pasp>();
  if (pasp) {
    *aspect_h = pasp->hSpacing;
    *aspect_v = pasp->vSpacing;
    return 1;
  }
  else {
    *aspect_h = 1;
    *aspect_v = 1;
    return 0;
  }
}


heif_context* heif_image_handle_get_context(const heif_image_handle* handle)
{
  auto ctx = new heif_context();
  ctx->context = handle->context;
  return ctx;
}


const char* heif_image_handle_get_gimi_content_id(const heif_image_handle* handle)
{
  if (!handle->image->has_gimi_sample_content_id()) {
    return nullptr;
  }

  std::string id = handle->image->get_gimi_sample_content_id();
  char* idstring = new char[id.size() + 1];
  strcpy(idstring, id.c_str());
  return idstring;
}

// ------------------------- gain map images -------------------------

struct heif_error heif_image_handle_get_gain_map_image_handle(
    const struct heif_image_handle* handle, struct heif_image_handle** gain_map_handle) {
  if (!gain_map_handle) {
    return {heif_error_Usage_error, heif_suberror_Null_pointer_argument,
            "NULL gain_map_handle passed to heif_image_handle_get_gain_map_image_handle()"};
  }

  std::shared_ptr<ImageItem> gain_map_image = handle->image->get_gain_map();
  if (!gain_map_image) {
    Error err(heif_error_Usage_error, heif_suberror_Nonexisting_item_referenced,
              "base image handle is not associated with a gain map image");
    return err.error_struct(handle->image.get());
  }

  *gain_map_handle = new heif_image_handle();
  (*gain_map_handle)->image = gain_map_image;
  (*gain_map_handle)->context = handle->context;

  return Error::Ok.error_struct(handle->image.get());
}

size_t heif_image_handle_get_gain_map_metadata_size(const struct heif_image_handle* handle) {
  std::shared_ptr<ImageMetadata> metadata = handle->image->get_gain_map_metadata();

  if (metadata) {
    return metadata->m_data.size();
  }

  return 0;
}

struct heif_error heif_image_handle_get_gain_map_metadata(const struct heif_image_handle* handle,
                                                          void* out_data) {
  if (!out_data) {
    return {heif_error_Usage_error, heif_suberror_Null_pointer_argument,
            "NULL out_data passed to heif_image_handle_get_gain_map_metadata()"};
  }

  std::shared_ptr<ImageMetadata> metadata = handle->image->get_gain_map_metadata();
  if (!metadata) {
    Error err(heif_error_Invalid_input, heif_suberror_No_item_data,
              "base image handle is not associated with a gain map image");
    return err.error_struct(handle->image.get());
  }

  std::vector<uint8_t>& buffer = metadata->m_data;
  memcpy(out_data, buffer.data(), buffer.size());

  return heif_error_success;
}

struct heif_error heif_image_handle_get_derived_image_nclx_color_profile(
    const struct heif_image_handle* handle, struct heif_color_profile_nclx** out_data) {
  if (!out_data) {
    return {heif_error_Usage_error, heif_suberror_Null_pointer_argument,
            "NULL out_data passed to heif_image_handle_get_derived_image_nclx_color_profile()"};
  }

  auto nclx_profile = handle->image->get_derived_img_color_profile_nclx();
  Error err = nclx_profile.get_nclx_color_profile(out_data);

  return err.error_struct(handle->image.get());
}

size_t heif_image_handle_get_derived_image_raw_color_profile_size(
    const struct heif_image_handle* handle) {
  auto profile_icc = handle->image->get_derived_img_color_profile_icc();
  if (profile_icc) {
    return profile_icc->get_data().size();
  } else {
    return 0;
  }
}

struct heif_error heif_image_handle_get_derived_image_raw_color_profile(
    const struct heif_image_handle* handle, void* out_data) {
  if (!out_data) {
    return {heif_error_Usage_error, heif_suberror_Null_pointer_argument,
            "NULL out_data passed to heif_image_handle_get_derived_image_raw_color_profile()"};
  }

  auto raw_profile = handle->image->get_derived_img_color_profile_icc();
  if (raw_profile) {
    memcpy(out_data, raw_profile->get_data().data(), raw_profile->get_data().size());
  } else {
    Error err(heif_error_Color_profile_does_not_exist, heif_suberror_Unspecified);
    return err.error_struct(handle->image.get());
  }

  return Error::Ok.error_struct(handle->image.get());
}

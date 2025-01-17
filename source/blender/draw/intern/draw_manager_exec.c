/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2016, Blender Foundation.
 */

/** \file
 * \ingroup draw
 */

#include "draw_manager.h"

#include "BLI_math.h"
#include "BLI_math_bits.h"
#include "BLI_memblock.h"

#include "BKE_global.h"

#include "GPU_extensions.h"
#include "intern/gpu_shader_private.h"
#include "intern/gpu_primitive_private.h"

#ifdef USE_GPU_SELECT
#  include "GPU_select.h"
#endif

void DRW_select_load_id(uint id)
{
#ifdef USE_GPU_SELECT
  BLI_assert(G.f & G_FLAG_PICKSEL);
  DST.select_id = id;
#endif
}

#define DEBUG_UBO_BINDING

typedef struct DRWCommandsState {
  GPUBatch *batch;
  int resource_chunk;
  int base_inst;
  int inst_count;
  int v_first;
  int v_count;
  bool neg_scale;
  /* Resource location. */
  int obmats_loc;
  int obinfos_loc;
  int baseinst_loc;
  int chunkid_loc;
  /* Legacy matrix support. */
  int obmat_loc;
  int obinv_loc;
  int mvp_loc;
  /* Selection ID state. */
  GPUVertBuf *select_buf;
  uint select_id;
  /* Drawing State */
  DRWState drw_state_enabled;
  DRWState drw_state_disabled;
} DRWCommandsState;

/* -------------------------------------------------------------------- */
/** \name Draw State (DRW_state)
 * \{ */

void drw_state_set(DRWState state)
{
  if (DST.state == state) {
    return;
  }

#define CHANGED_TO(f) \
  ((DST.state_lock & (f)) ? \
       0 : \
       (((DST.state & (f)) ? ((state & (f)) ? 0 : -1) : ((state & (f)) ? 1 : 0))))

#define CHANGED_ANY(f) (((DST.state & (f)) != (state & (f))) && ((DST.state_lock & (f)) == 0))

#define CHANGED_ANY_STORE_VAR(f, enabled) \
  (((DST.state & (f)) != (enabled = (state & (f)))) && (((DST.state_lock & (f)) == 0)))

  /* Depth Write */
  {
    int test;
    if ((test = CHANGED_TO(DRW_STATE_WRITE_DEPTH))) {
      if (test == 1) {
        glDepthMask(GL_TRUE);
      }
      else {
        glDepthMask(GL_FALSE);
      }
    }
  }

  /* Stencil Write */
  {
    DRWState test;
    if (CHANGED_ANY_STORE_VAR(DRW_STATE_WRITE_STENCIL_ENABLED, test)) {
      /* Stencil Write */
      if (test) {
        glStencilMask(0xFF);
        if (test & DRW_STATE_WRITE_STENCIL) {
          glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        }
        else if (test & DRW_STATE_WRITE_STENCIL_SHADOW_PASS) {
          glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
          glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
        }
        else if (test & DRW_STATE_WRITE_STENCIL_SHADOW_FAIL) {
          glStencilOpSeparate(GL_BACK, GL_KEEP, GL_DECR_WRAP, GL_KEEP);
          glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
        }
        else {
          BLI_assert(0);
        }
      }
      else {
        glStencilMask(0x00);
      }
    }
  }

  /* Color Write */
  {
    int test;
    if ((test = CHANGED_TO(DRW_STATE_WRITE_COLOR))) {
      if (test == 1) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      }
      else {
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
      }
    }
  }

  /* Raster Discard */
  {
    if (CHANGED_ANY(DRW_STATE_RASTERIZER_ENABLED)) {
      if ((state & DRW_STATE_RASTERIZER_ENABLED) != 0) {
        glDisable(GL_RASTERIZER_DISCARD);
      }
      else {
        glEnable(GL_RASTERIZER_DISCARD);
      }
    }
  }

  /* Cull */
  {
    DRWState test;
    if (CHANGED_ANY_STORE_VAR(DRW_STATE_CULL_BACK | DRW_STATE_CULL_FRONT, test)) {
      if (test) {
        glEnable(GL_CULL_FACE);

        if ((state & DRW_STATE_CULL_BACK) != 0) {
          glCullFace(GL_BACK);
        }
        else if ((state & DRW_STATE_CULL_FRONT) != 0) {
          glCullFace(GL_FRONT);
        }
        else {
          BLI_assert(0);
        }
      }
      else {
        glDisable(GL_CULL_FACE);
      }
    }
  }

  /* Depth Test */
  {
    DRWState test;
    if (CHANGED_ANY_STORE_VAR(DRW_STATE_DEPTH_TEST_ENABLED, test)) {
      if (test) {
        glEnable(GL_DEPTH_TEST);

        if (state & DRW_STATE_DEPTH_LESS) {
          glDepthFunc(GL_LESS);
        }
        else if (state & DRW_STATE_DEPTH_LESS_EQUAL) {
          glDepthFunc(GL_LEQUAL);
        }
        else if (state & DRW_STATE_DEPTH_EQUAL) {
          glDepthFunc(GL_EQUAL);
        }
        else if (state & DRW_STATE_DEPTH_GREATER) {
          glDepthFunc(GL_GREATER);
        }
        else if (state & DRW_STATE_DEPTH_GREATER_EQUAL) {
          glDepthFunc(GL_GEQUAL);
        }
        else if (state & DRW_STATE_DEPTH_ALWAYS) {
          glDepthFunc(GL_ALWAYS);
        }
        else {
          BLI_assert(0);
        }
      }
      else {
        glDisable(GL_DEPTH_TEST);
      }
    }
  }

  /* Stencil Test */
  {
    int test;
    if (CHANGED_ANY_STORE_VAR(DRW_STATE_STENCIL_TEST_ENABLED, test)) {
      DST.stencil_mask = STENCIL_UNDEFINED;
      if (test) {
        glEnable(GL_STENCIL_TEST);
      }
      else {
        glDisable(GL_STENCIL_TEST);
      }
    }
  }

  /* Wire Width */
  {
    int test;
    if ((test = CHANGED_TO(DRW_STATE_WIRE_SMOOTH))) {
      if (test == 1) {
        GPU_line_width(2.0f);
        GPU_line_smooth(true);
      }
      else {
        GPU_line_width(1.0f);
        GPU_line_smooth(false);
      }
    }
  }

  /* Blending (all buffer) */
  {
    int test;
    if (CHANGED_ANY_STORE_VAR(DRW_STATE_BLEND_ALPHA | DRW_STATE_BLEND_ALPHA_PREMUL |
                                  DRW_STATE_BLEND_ADD | DRW_STATE_BLEND_MUL |
                                  DRW_STATE_BLEND_ADD_FULL | DRW_STATE_BLEND_OIT |
                                  DRW_STATE_BLEND_ALPHA_UNDER_PREMUL | DRW_STATE_BLEND_CUSTOM,
                              test)) {
      if (test) {
        glEnable(GL_BLEND);

        if ((state & DRW_STATE_BLEND_ALPHA) != 0) {
          glBlendFuncSeparate(GL_SRC_ALPHA,
                              GL_ONE_MINUS_SRC_ALPHA, /* RGB */
                              GL_ONE,
                              GL_ONE_MINUS_SRC_ALPHA); /* Alpha */
        }
        else if ((state & DRW_STATE_BLEND_ALPHA_UNDER_PREMUL) != 0) {
          glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE);
        }
        else if ((state & DRW_STATE_BLEND_ALPHA_PREMUL) != 0) {
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        else if ((state & DRW_STATE_BLEND_MUL) != 0) {
          glBlendFunc(GL_DST_COLOR, GL_ZERO);
        }
        else if ((state & DRW_STATE_BLEND_OIT) != 0) {
          glBlendFuncSeparate(GL_ONE,
                              GL_ONE, /* RGB */
                              GL_ZERO,
                              GL_ONE_MINUS_SRC_ALPHA); /* Alpha */
        }
        else if ((state & DRW_STATE_BLEND_ADD) != 0) {
          /* Do not let alpha accumulate but premult the source RGB by it. */
          glBlendFuncSeparate(GL_SRC_ALPHA,
                              GL_ONE, /* RGB */
                              GL_ZERO,
                              GL_ONE); /* Alpha */
        }
        else if ((state & DRW_STATE_BLEND_ADD_FULL) != 0) {
          /* Let alpha accumulate. */
          glBlendFunc(GL_ONE, GL_ONE);
        }
        else if ((state & DRW_STATE_BLEND_CUSTOM) != 0) {
          /* Custom blend parameters using dual source blending.
           * Can only be used with one Draw Buffer. */
          glBlendFunc(GL_ONE, GL_SRC1_COLOR);
        }
        else {
          BLI_assert(0);
        }
      }
      else {
        glDisable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE); /* Don't multiply incoming color by alpha. */
      }
    }
  }

  /* Shadow Bias */
  {
    int test;
    if ((test = CHANGED_TO(DRW_STATE_SHADOW_OFFSET))) {
      if (test == 1) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glEnable(GL_POLYGON_OFFSET_LINE);
        /* 2.0 Seems to be the lowest possible slope bias that works in every case. */
        glPolygonOffset(2.0f, 1.0f);
      }
      else {
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDisable(GL_POLYGON_OFFSET_LINE);
      }
    }
  }

  /* Clip Planes */
  {
    int test;
    if ((test = CHANGED_TO(DRW_STATE_CLIP_PLANES))) {
      if (test == 1) {
        for (int i = 0; i < DST.view_active->clip_planes_len; i++) {
          glEnable(GL_CLIP_DISTANCE0 + i);
        }
      }
      else {
        for (int i = 0; i < MAX_CLIP_PLANES; i++) {
          glDisable(GL_CLIP_DISTANCE0 + i);
        }
      }
    }
  }

  /* Program Points Size */
  {
    int test;
    if ((test = CHANGED_TO(DRW_STATE_PROGRAM_POINT_SIZE))) {
      if (test == 1) {
        GPU_program_point_size(true);
      }
      else {
        GPU_program_point_size(false);
      }
    }
  }

  /* Provoking Vertex */
  {
    int test;
    if ((test = CHANGED_TO(DRW_STATE_FIRST_VERTEX_CONVENTION))) {
      if (test == 1) {
        glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
      }
      else {
        glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
      }
    }
  }

#undef CHANGED_TO
#undef CHANGED_ANY
#undef CHANGED_ANY_STORE_VAR

  DST.state = state;
}

static void drw_stencil_set(uint mask)
{
  if (DST.stencil_mask != mask) {
    DST.stencil_mask = mask;
    if ((DST.state & DRW_STATE_STENCIL_ALWAYS) != 0) {
      glStencilFunc(GL_ALWAYS, mask, 0xFF);
    }
    else if ((DST.state & DRW_STATE_STENCIL_EQUAL) != 0) {
      glStencilFunc(GL_EQUAL, mask, 0xFF);
    }
    else if ((DST.state & DRW_STATE_STENCIL_NEQUAL) != 0) {
      glStencilFunc(GL_NOTEQUAL, mask, 0xFF);
    }
  }
}

/* Reset state to not interfer with other UI drawcall */
void DRW_state_reset_ex(DRWState state)
{
  DST.state = ~state;
  drw_state_set(state);
}

static void drw_state_validate(void)
{
  /* Cannot write to stencil buffer without stencil test. */
  if ((DST.state & DRW_STATE_WRITE_STENCIL_ENABLED)) {
    BLI_assert(DST.state & DRW_STATE_STENCIL_TEST_ENABLED);
  }
  /* Cannot write to depth buffer without depth test. */
  if ((DST.state & DRW_STATE_WRITE_DEPTH)) {
    BLI_assert(DST.state & DRW_STATE_DEPTH_TEST_ENABLED);
  }
}

/**
 * Use with care, intended so selection code can override passes depth settings,
 * which is important for selection to work properly.
 *
 * Should be set in main draw loop, cleared afterwards
 */
void DRW_state_lock(DRWState state)
{
  DST.state_lock = state;
}

void DRW_state_reset(void)
{
  DRW_state_reset_ex(DRW_STATE_DEFAULT);

  GPU_point_size(5);

  /* Reset blending function */
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Culling (DRW_culling)
 * \{ */

static bool draw_call_is_culled(const DRWResourceHandle *handle, DRWView *view)
{
  DRWCullingState *culling = DRW_memblock_elem_from_handle(DST.vmempool->cullstates, handle);
  return (culling->mask & view->culling_mask) != 0;
}

/* Set active view for rendering. */
void DRW_view_set_active(DRWView *view)
{
  DST.view_active = (view) ? view : DST.view_default;
}

/* Return True if the given BoundSphere intersect the current view frustum */
static bool draw_culling_sphere_test(const BoundSphere *frustum_bsphere,
                                     const float (*frustum_planes)[4],
                                     const BoundSphere *bsphere)
{
  /* Bypass test if radius is negative. */
  if (bsphere->radius < 0.0f) {
    return true;
  }

  /* Do a rough test first: Sphere VS Sphere intersect. */
  float center_dist_sq = len_squared_v3v3(bsphere->center, frustum_bsphere->center);
  float radius_sum = bsphere->radius + frustum_bsphere->radius;
  if (center_dist_sq > SQUARE(radius_sum)) {
    return false;
  }
  /* TODO we could test against the inscribed sphere of the frustum to early out positively. */

  /* Test against the 6 frustum planes. */
  /* TODO order planes with sides first then far then near clip. Should be better culling
   * heuristic when sculpting. */
  for (int p = 0; p < 6; p++) {
    float dist = plane_point_side_v3(frustum_planes[p], bsphere->center);
    if (dist < -bsphere->radius) {
      return false;
    }
  }
  return true;
}

static bool draw_culling_box_test(const float (*frustum_planes)[4], const BoundBox *bbox)
{
  /* 6 view frustum planes */
  for (int p = 0; p < 6; p++) {
    /* 8 box vertices. */
    for (int v = 0; v < 8; v++) {
      float dist = plane_point_side_v3(frustum_planes[p], bbox->vec[v]);
      if (dist > 0.0f) {
        /* At least one point in front of this plane.
         * Go to next plane. */
        break;
      }
      else if (v == 7) {
        /* 8 points behind this plane. */
        return false;
      }
    }
  }
  return true;
}

static bool draw_culling_plane_test(const BoundBox *corners, const float plane[4])
{
  /* Test against the 8 frustum corners. */
  for (int c = 0; c < 8; c++) {
    float dist = plane_point_side_v3(plane, corners->vec[c]);
    if (dist < 0.0f) {
      return true;
    }
  }
  return false;
}

/* Return True if the given BoundSphere intersect the current view frustum.
 * bsphere must be in world space. */
bool DRW_culling_sphere_test(const DRWView *view, const BoundSphere *bsphere)
{
  view = view ? view : DST.view_default;
  return draw_culling_sphere_test(&view->frustum_bsphere, view->frustum_planes, bsphere);
}

/* Return True if the given BoundBox intersect the current view frustum.
 * bbox must be in world space. */
bool DRW_culling_box_test(const DRWView *view, const BoundBox *bbox)
{
  view = view ? view : DST.view_default;
  return draw_culling_box_test(view->frustum_planes, bbox);
}

/* Return True if the view frustum is inside or intersect the given plane.
 * plane must be in world space. */
bool DRW_culling_plane_test(const DRWView *view, const float plane[4])
{
  view = view ? view : DST.view_default;
  return draw_culling_plane_test(&view->frustum_corners, plane);
}

/* Return True if the given box intersect the current view frustum.
 * This function will have to be replaced when world space bb per objects is implemented. */
bool DRW_culling_min_max_test(const DRWView *view, float obmat[4][4], float min[3], float max[3])
{
  view = view ? view : DST.view_default;
  float tobmat[4][4];
  transpose_m4_m4(tobmat, obmat);
  for (int i = 6; i--;) {
    float frustum_plane_local[4], bb_near[3], bb_far[3];
    mul_v4_m4v4(frustum_plane_local, tobmat, view->frustum_planes[i]);
    aabb_get_near_far_from_plane(frustum_plane_local, min, max, bb_near, bb_far);

    if (plane_point_side_v3(frustum_plane_local, bb_far) < 0.0f) {
      return false;
    }
  }

  return true;
}

void DRW_culling_frustum_corners_get(const DRWView *view, BoundBox *corners)
{
  view = view ? view : DST.view_default;
  *corners = view->frustum_corners;
}

void DRW_culling_frustum_planes_get(const DRWView *view, float planes[6][4])
{
  view = view ? view : DST.view_default;
  memcpy(planes, view->frustum_planes, sizeof(float) * 6 * 4);
}

static void draw_compute_culling(DRWView *view)
{
  view = view->parent ? view->parent : view;

  /* TODO(fclem) multithread this. */
  /* TODO(fclem) compute all dirty views at once. */
  if (!view->is_dirty) {
    return;
  }

  BLI_memblock_iter iter;
  BLI_memblock_iternew(DST.vmempool->cullstates, &iter);
  DRWCullingState *cull;
  while ((cull = BLI_memblock_iterstep(&iter))) {
    if (cull->bsphere.radius < 0.0) {
      cull->mask = 0;
    }
    else {
      bool culled = !draw_culling_sphere_test(
          &view->frustum_bsphere, view->frustum_planes, &cull->bsphere);

#ifdef DRW_DEBUG_CULLING
      if (G.debug_value != 0) {
        if (culled) {
          DRW_debug_sphere(
              cull->bsphere.center, cull->bsphere.radius, (const float[4]){1, 0, 0, 1});
        }
        else {
          DRW_debug_sphere(
              cull->bsphere.center, cull->bsphere.radius, (const float[4]){0, 1, 0, 1});
        }
      }
#endif

      if (view->visibility_fn) {
        culled = !view->visibility_fn(!culled, cull->user_data);
      }

      SET_FLAG_FROM_TEST(cull->mask, culled, view->culling_mask);
    }
  }

  view->is_dirty = false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Draw (DRW_draw)
 * \{ */

BLI_INLINE void draw_legacy_matrix_update(DRWShadingGroup *shgroup,
                                          DRWResourceHandle *handle,
                                          float obmat_loc,
                                          float obinv_loc,
                                          float mvp_loc)
{
  /* Still supported for compatibility with gpu_shader_* but should be forbidden. */
  DRWObjectMatrix *ob_mats = DRW_memblock_elem_from_handle(DST.vmempool->obmats, handle);
  if (obmat_loc != -1) {
    GPU_shader_uniform_vector(shgroup->shader, obmat_loc, 16, 1, (float *)ob_mats->model);
  }
  if (obinv_loc != -1) {
    GPU_shader_uniform_vector(shgroup->shader, obinv_loc, 16, 1, (float *)ob_mats->modelinverse);
  }
  /* Still supported for compatibility with gpu_shader_* but should be forbidden
   * and is slow (since it does not cache the result). */
  if (mvp_loc != -1) {
    float mvp[4][4];
    mul_m4_m4m4(mvp, DST.view_active->storage.persmat, ob_mats->model);
    GPU_shader_uniform_vector(shgroup->shader, mvp_loc, 16, 1, (float *)mvp);
  }
}

BLI_INLINE void draw_geometry_bind(DRWShadingGroup *shgroup, GPUBatch *geom)
{
  /* XXX hacking #GPUBatch. we don't want to call glUseProgram! (huge performance loss) */
  if (DST.batch) {
    DST.batch->program_in_use = false;
  }

  DST.batch = geom;

  GPU_batch_program_set_no_use(
      geom, GPU_shader_get_program(shgroup->shader), GPU_shader_get_interface(shgroup->shader));

  geom->program_in_use = true; /* XXX hacking #GPUBatch */

  GPU_batch_bind(geom);
}

BLI_INLINE void draw_geometry_execute(DRWShadingGroup *shgroup,
                                      GPUBatch *geom,
                                      int vert_first,
                                      int vert_count,
                                      int inst_first,
                                      int inst_count,
                                      int baseinst_loc)
{
  /* inst_count can be -1. */
  inst_count = max_ii(0, inst_count);

  if (baseinst_loc != -1) {
    /* Fallback when ARB_shader_draw_parameters is not supported. */
    GPU_shader_uniform_vector_int(shgroup->shader, baseinst_loc, 1, 1, (int *)&inst_first);
    /* Avoids VAO reconfiguration on older hardware. (see GPU_batch_draw_advanced) */
    inst_first = 0;
  }

  /* bind vertex array */
  if (DST.batch != geom) {
    draw_geometry_bind(shgroup, geom);
  }

  GPU_batch_draw_advanced(geom, vert_first, vert_count, inst_first, inst_count);
}

BLI_INLINE void draw_indirect_call(DRWShadingGroup *shgroup, DRWCommandsState *state)
{
  if (state->inst_count == 0) {
    return;
  }
  if (state->baseinst_loc == -1) {
    /* bind vertex array */
    if (DST.batch != state->batch) {
      GPU_draw_list_submit(DST.draw_list);
      draw_geometry_bind(shgroup, state->batch);
    }
    GPU_draw_list_command_add(
        DST.draw_list, state->v_first, state->v_count, state->base_inst, state->inst_count);
  }
  /* Fallback when unsupported */
  else {
    draw_geometry_execute(shgroup,
                          state->batch,
                          state->v_first,
                          state->v_count,
                          state->base_inst,
                          state->inst_count,
                          state->baseinst_loc);
  }
}

enum {
  BIND_NONE = 0,
  BIND_TEMP = 1,    /* Release slot after this shading group. */
  BIND_PERSIST = 2, /* Release slot only after the next shader change. */
};

static void set_bound_flags(uint64_t *slots, uint64_t *persist_slots, int slot_idx, char bind_type)
{
  uint64_t slot = 1llu << (unsigned long)slot_idx;
  *slots |= slot;
  if (bind_type == BIND_PERSIST) {
    *persist_slots |= slot;
  }
}

static int get_empty_slot_index(uint64_t slots)
{
  uint64_t empty_slots = ~slots;
  /* Find first empty slot using bitscan. */
  if (empty_slots != 0) {
    if ((empty_slots & 0xFFFFFFFFlu) != 0) {
      return (int)bitscan_forward_uint(empty_slots);
    }
    else {
      return (int)bitscan_forward_uint(empty_slots >> 32) + 32;
    }
  }
  else {
    /* Greater than GPU_max_textures() */
    return 99999;
  }
}

static void bind_texture(GPUTexture *tex, char bind_type)
{
  int idx = GPU_texture_bound_number(tex);
  if (idx == -1) {
    /* Texture isn't bound yet. Find an empty slot and bind it. */
    idx = get_empty_slot_index(DST.RST.bound_tex_slots);

    if (idx < GPU_max_textures()) {
      GPUTexture **gpu_tex_slot = &DST.RST.bound_texs[idx];
      /* Unbind any previous texture. */
      if (*gpu_tex_slot != NULL) {
        GPU_texture_unbind(*gpu_tex_slot);
      }
      GPU_texture_bind(tex, idx);
      *gpu_tex_slot = tex;
    }
    else {
      printf("Not enough texture slots! Reduce number of textures used by your shader.\n");
      return;
    }
  }
  else {
    /* This texture slot was released but the tex
     * is still bound. Just flag the slot again. */
    BLI_assert(DST.RST.bound_texs[idx] == tex);
  }
  set_bound_flags(&DST.RST.bound_tex_slots, &DST.RST.bound_tex_slots_persist, idx, bind_type);
}

static void bind_ubo(GPUUniformBuffer *ubo, char bind_type)
{
  int idx = GPU_uniformbuffer_bindpoint(ubo);
  if (idx == -1) {
    /* UBO isn't bound yet. Find an empty slot and bind it. */
    idx = get_empty_slot_index(DST.RST.bound_ubo_slots);

    /* [0..1] are reserved ubo slots. */
    idx += 2;

    if (idx < GPU_max_ubo_binds()) {
      GPUUniformBuffer **gpu_ubo_slot = &DST.RST.bound_ubos[idx];
      /* Unbind any previous UBO. */
      if (*gpu_ubo_slot != NULL) {
        GPU_uniformbuffer_unbind(*gpu_ubo_slot);
      }
      GPU_uniformbuffer_bind(ubo, idx);
      *gpu_ubo_slot = ubo;
    }
    else {
      /* printf so user can report bad behavior */
      printf("Not enough ubo slots! This should not happen!\n");
      /* This is not depending on user input.
       * It is our responsibility to make sure there is enough slots. */
      BLI_assert(0);
      return;
    }
  }
  else {
    BLI_assert(idx < 64);
    /* This UBO slot was released but the UBO is
     * still bound here. Just flag the slot again. */
    BLI_assert(DST.RST.bound_ubos[idx] == ubo);
  }
  /* Remove offset for flag bitfield. */
  idx -= 2;
  set_bound_flags(&DST.RST.bound_ubo_slots, &DST.RST.bound_ubo_slots_persist, idx, bind_type);
}

#ifndef NDEBUG
/**
 * Opengl specification is strict on buffer binding.
 *
 * " If any active uniform block is not backed by a
 * sufficiently large buffer object, the results of shader
 * execution are undefined, and may result in GL interruption or
 * termination. " - Opengl 3.3 Core Specification
 *
 * For now we only check if the binding is correct. Not the size of
 * the bound ubo.
 *
 * See T55475.
 * */
static bool ubo_bindings_validate(DRWShadingGroup *shgroup)
{
  bool valid = true;
#  ifdef DEBUG_UBO_BINDING
  /* Check that all active uniform blocks have a non-zero buffer bound. */
  GLint program = 0;
  GLint active_blocks = 0;

  glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  glGetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &active_blocks);

  for (uint i = 0; i < active_blocks; i++) {
    int binding = 0;
    int buffer = 0;

    glGetActiveUniformBlockiv(program, i, GL_UNIFORM_BLOCK_BINDING, &binding);
    glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, binding, &buffer);

    if (buffer == 0) {
      char blockname[64];
      glGetActiveUniformBlockName(program, i, sizeof(blockname), NULL, blockname);

      if (valid) {
        printf("Trying to draw with missing UBO binding.\n");
        valid = false;
      }

      DRWPass *parent_pass = DRW_memblock_elem_from_handle(DST.vmempool->passes,
                                                           &shgroup->pass_handle);

      printf("Pass : %s, Shader : %s, Block : %s\n",
             parent_pass->name,
             shgroup->shader->name,
             blockname);
    }
  }
#  endif
  return valid;
}
#endif

static void release_texture_slots(bool with_persist)
{
  if (with_persist) {
    DST.RST.bound_tex_slots = 0;
    DST.RST.bound_tex_slots_persist = 0;
  }
  else {
    DST.RST.bound_tex_slots &= DST.RST.bound_tex_slots_persist;
  }
}

static void release_ubo_slots(bool with_persist)
{
  if (with_persist) {
    DST.RST.bound_ubo_slots = 0;
    DST.RST.bound_ubo_slots_persist = 0;
  }
  else {
    DST.RST.bound_ubo_slots &= DST.RST.bound_ubo_slots_persist;
  }
}

static void draw_update_uniforms(DRWShadingGroup *shgroup,
                                 DRWCommandsState *state,
                                 bool *use_tfeedback)
{
  for (DRWUniformChunk *unichunk = shgroup->uniforms; unichunk; unichunk = unichunk->next) {
    DRWUniform *uni = unichunk->uniforms;
    for (int i = 0; i < unichunk->uniform_used; i++, uni++) {
      GPUTexture *tex;
      GPUUniformBuffer *ubo;
      if (uni->location == -2) {
        uni->location = GPU_shader_get_uniform_ensure(shgroup->shader,
                                                      DST.uniform_names.buffer + uni->name_ofs);
        if (uni->location == -1) {
          continue;
        }
      }
      const void *data = uni->pvalue;
      if (ELEM(uni->type, DRW_UNIFORM_INT_COPY, DRW_UNIFORM_FLOAT_COPY)) {
        data = uni->fvalue;
      }
      switch (uni->type) {
        case DRW_UNIFORM_INT_COPY:
        case DRW_UNIFORM_INT:
          GPU_shader_uniform_vector_int(
              shgroup->shader, uni->location, uni->length, uni->arraysize, data);
          break;
        case DRW_UNIFORM_FLOAT_COPY:
        case DRW_UNIFORM_FLOAT:
          GPU_shader_uniform_vector(
              shgroup->shader, uni->location, uni->length, uni->arraysize, data);
          break;
        case DRW_UNIFORM_TEXTURE:
          tex = (GPUTexture *)uni->pvalue;
          BLI_assert(tex);
          bind_texture(tex, BIND_TEMP);
          GPU_shader_uniform_texture(shgroup->shader, uni->location, tex);
          break;
        case DRW_UNIFORM_TEXTURE_PERSIST:
          tex = (GPUTexture *)uni->pvalue;
          BLI_assert(tex);
          bind_texture(tex, BIND_PERSIST);
          GPU_shader_uniform_texture(shgroup->shader, uni->location, tex);
          break;
        case DRW_UNIFORM_TEXTURE_REF:
          tex = *((GPUTexture **)uni->pvalue);
          BLI_assert(tex);
          bind_texture(tex, BIND_TEMP);
          GPU_shader_uniform_texture(shgroup->shader, uni->location, tex);
          break;
        case DRW_UNIFORM_BLOCK:
          ubo = (GPUUniformBuffer *)uni->pvalue;
          bind_ubo(ubo, BIND_TEMP);
          GPU_shader_uniform_buffer(shgroup->shader, uni->location, ubo);
          break;
        case DRW_UNIFORM_BLOCK_PERSIST:
          ubo = (GPUUniformBuffer *)uni->pvalue;
          bind_ubo(ubo, BIND_PERSIST);
          GPU_shader_uniform_buffer(shgroup->shader, uni->location, ubo);
          break;
        case DRW_UNIFORM_BLOCK_OBMATS:
          state->obmats_loc = uni->location;
          ubo = DST.vmempool->matrices_ubo[0];
          GPU_uniformbuffer_bind(ubo, 0);
          GPU_shader_uniform_buffer(shgroup->shader, uni->location, ubo);
          break;
        case DRW_UNIFORM_BLOCK_OBINFOS:
          state->obinfos_loc = uni->location;
          ubo = DST.vmempool->obinfos_ubo[0];
          GPU_uniformbuffer_bind(ubo, 1);
          GPU_shader_uniform_buffer(shgroup->shader, uni->location, ubo);
          break;
        case DRW_UNIFORM_RESOURCE_CHUNK:
          state->chunkid_loc = uni->location;
          GPU_shader_uniform_int(shgroup->shader, uni->location, 0);
          break;
        case DRW_UNIFORM_TFEEDBACK_TARGET:
          BLI_assert(data && (*use_tfeedback == false));
          *use_tfeedback = GPU_shader_transform_feedback_enable(shgroup->shader,
                                                                ((GPUVertBuf *)data)->vbo_id);
          break;
          /* Legacy/Fallback support. */
        case DRW_UNIFORM_BASE_INSTANCE:
          state->baseinst_loc = uni->location;
          break;
        case DRW_UNIFORM_MODEL_MATRIX:
          state->obmat_loc = uni->location;
          break;
        case DRW_UNIFORM_MODEL_MATRIX_INVERSE:
          state->obinv_loc = uni->location;
          break;
        case DRW_UNIFORM_MODELVIEWPROJECTION_MATRIX:
          state->mvp_loc = uni->location;
          break;
      }
    }
  }

  BLI_assert(ubo_bindings_validate(shgroup));
}

BLI_INLINE void draw_select_buffer(DRWShadingGroup *shgroup,
                                   DRWCommandsState *state,
                                   GPUBatch *batch,
                                   const DRWResourceHandle *handle)
{
  const bool is_instancing = (batch->inst != NULL);
  int start = 0;
  int count = 1;
  int tot = is_instancing ? batch->inst->vertex_len : batch->verts[0]->vertex_len;
  /* Hack : get "vbo" data without actually drawing. */
  int *select_id = (void *)state->select_buf->data;

  /* Batching */
  if (!is_instancing) {
    /* FIXME: Meh a bit nasty. */
    if (batch->gl_prim_type == convert_prim_type_to_gl(GPU_PRIM_TRIS)) {
      count = 3;
    }
    else if (batch->gl_prim_type == convert_prim_type_to_gl(GPU_PRIM_LINES)) {
      count = 2;
    }
  }

  while (start < tot) {
    GPU_select_load_id(select_id[start]);
    if (is_instancing) {
      draw_geometry_execute(shgroup, batch, 0, 0, start, count, state->baseinst_loc);
    }
    else {
      draw_geometry_execute(
          shgroup, batch, start, count, DRW_handle_id_get(handle), 0, state->baseinst_loc);
    }
    start += count;
  }
}

typedef struct DRWCommandIterator {
  int cmd_index;
  DRWCommandChunk *curr_chunk;
} DRWCommandIterator;

static void draw_command_iter_begin(DRWCommandIterator *iter, DRWShadingGroup *shgroup)
{
  iter->curr_chunk = shgroup->cmd.first;
  iter->cmd_index = 0;
}

static DRWCommand *draw_command_iter_step(DRWCommandIterator *iter, eDRWCommandType *cmd_type)
{
  if (iter->curr_chunk) {
    if (iter->cmd_index == iter->curr_chunk->command_len) {
      iter->curr_chunk = iter->curr_chunk->next;
      iter->cmd_index = 0;
    }
    if (iter->curr_chunk) {
      *cmd_type = command_type_get(iter->curr_chunk->command_type, iter->cmd_index);
      if (iter->cmd_index < iter->curr_chunk->command_used) {
        return iter->curr_chunk->commands + iter->cmd_index++;
      }
    }
  }
  return NULL;
}

static void draw_call_resource_bind(DRWCommandsState *state, const DRWResourceHandle *handle)
{
  /* Front face is not a resource but it is inside the resource handle. */
  bool neg_scale = DRW_handle_negative_scale_get(handle);
  if (neg_scale != state->neg_scale) {
    glFrontFace((neg_scale) ? GL_CW : GL_CCW);
    state->neg_scale = neg_scale;
  }

  int chunk = DRW_handle_chunk_get(handle);
  if (state->resource_chunk != chunk) {
    if (state->chunkid_loc != -1) {
      GPU_shader_uniform_int(NULL, state->chunkid_loc, chunk);
    }
    if (state->obmats_loc != -1) {
      GPU_uniformbuffer_unbind(DST.vmempool->matrices_ubo[state->resource_chunk]);
      GPU_uniformbuffer_bind(DST.vmempool->matrices_ubo[chunk], 0);
    }
    if (state->obinfos_loc != -1) {
      GPU_uniformbuffer_unbind(DST.vmempool->obinfos_ubo[state->resource_chunk]);
      GPU_uniformbuffer_bind(DST.vmempool->obinfos_ubo[chunk], 1);
    }
    state->resource_chunk = chunk;
  }
}

static void draw_call_batching_flush(DRWShadingGroup *shgroup, DRWCommandsState *state)
{
  draw_indirect_call(shgroup, state);
  GPU_draw_list_submit(DST.draw_list);

  state->batch = NULL;
  state->inst_count = 0;
  state->base_inst = -1;
}

static void draw_call_single_do(DRWShadingGroup *shgroup,
                                DRWCommandsState *state,
                                GPUBatch *batch,
                                DRWResourceHandle handle,
                                int vert_first,
                                int vert_count,
                                int inst_count)
{
  draw_call_batching_flush(shgroup, state);

  draw_call_resource_bind(state, &handle);

  /* TODO This is Legacy. Need to be removed. */
  if (state->obmats_loc == -1 &&
      (state->obmat_loc != -1 || state->obinv_loc != -1 || state->mvp_loc != -1)) {
    draw_legacy_matrix_update(
        shgroup, &handle, state->obmat_loc, state->obinv_loc, state->mvp_loc);
  }

  if (G.f & G_FLAG_PICKSEL) {
    if (state->select_buf != NULL) {
      draw_select_buffer(shgroup, state, batch, &handle);
      return;
    }
    else {
      GPU_select_load_id(state->select_id);
    }
  }

  draw_geometry_execute(shgroup,
                        batch,
                        vert_first,
                        vert_count,
                        DRW_handle_id_get(&handle),
                        inst_count,
                        state->baseinst_loc);
}

static void draw_call_batching_start(DRWCommandsState *state)
{
  state->neg_scale = false;
  state->resource_chunk = 0;
  state->base_inst = 0;
  state->inst_count = 0;
  state->v_first = 0;
  state->v_count = 0;
  state->batch = NULL;

  state->select_id = -1;
  state->select_buf = NULL;
}

/* NOTE: Does not support batches with instancing VBOs. */
static void draw_call_batching_do(DRWShadingGroup *shgroup,
                                  DRWCommandsState *state,
                                  DRWCommandDraw *call)
{
  /* If any condition requires to interupt the merging. */
  bool neg_scale = DRW_handle_negative_scale_get(&call->handle);
  int chunk = DRW_handle_chunk_get(&call->handle);
  int id = DRW_handle_id_get(&call->handle);
  if ((state->neg_scale != neg_scale) ||  /* Need to change state. */
      (state->resource_chunk != chunk) || /* Need to change UBOs. */
      (state->batch != call->batch)       /* Need to change VAO. */
  ) {
    draw_call_batching_flush(shgroup, state);

    state->batch = call->batch;
    state->v_first = (call->batch->elem) ? call->batch->elem->index_start : 0;
    state->v_count = (call->batch->elem) ? call->batch->elem->index_len :
                                           call->batch->verts[0]->vertex_len;
    state->inst_count = 1;
    state->base_inst = id;

    draw_call_resource_bind(state, &call->handle);

    GPU_draw_list_init(DST.draw_list, state->batch);
  }
  /* Is the id consecutive? */
  else if (id != state->base_inst + state->inst_count) {
    /* We need to add a draw command for the pending instances. */
    draw_indirect_call(shgroup, state);
    state->inst_count = 1;
    state->base_inst = id;
  }
  /* We avoid a drawcall by merging with the precedent
   * drawcall using instancing. */
  else {
    state->inst_count++;
  }
}

/* Flush remaining pending drawcalls. */
static void draw_call_batching_finish(DRWShadingGroup *shgroup, DRWCommandsState *state)
{
  draw_call_batching_flush(shgroup, state);

  /* Reset state */
  if (state->neg_scale) {
    glFrontFace(GL_CCW);
  }
  if (state->obmats_loc != -1) {
    GPU_uniformbuffer_unbind(DST.vmempool->matrices_ubo[state->resource_chunk]);
  }
  if (state->obinfos_loc != -1) {
    GPU_uniformbuffer_unbind(DST.vmempool->obinfos_ubo[state->resource_chunk]);
  }
}

static void draw_shgroup(DRWShadingGroup *shgroup, DRWState pass_state)
{
  BLI_assert(shgroup->shader);

  DRWCommandsState state = {
      .obmats_loc = -1,
      .obinfos_loc = -1,
      .baseinst_loc = -1,
      .chunkid_loc = -1,
      .obmat_loc = -1,
      .obinv_loc = -1,
      .mvp_loc = -1,
      .drw_state_enabled = 0,
      .drw_state_disabled = 0,
  };

  const bool shader_changed = (DST.shader != shgroup->shader);
  bool use_tfeedback = false;

  if (shader_changed) {
    if (DST.shader) {
      GPU_shader_unbind();
    }
    GPU_shader_bind(shgroup->shader);
    DST.shader = shgroup->shader;
    /* XXX hacking gawain */
    if (DST.batch) {
      DST.batch->program_in_use = false;
    }
    DST.batch = NULL;
  }

  release_ubo_slots(shader_changed);
  release_texture_slots(shader_changed);

  draw_update_uniforms(shgroup, &state, &use_tfeedback);

  drw_state_set(pass_state);

  /* Rendering Calls */
  {
    DRWCommandIterator iter;
    DRWCommand *cmd;
    eDRWCommandType cmd_type;

    draw_command_iter_begin(&iter, shgroup);

    draw_call_batching_start(&state);

    while ((cmd = draw_command_iter_step(&iter, &cmd_type))) {

      switch (cmd_type) {
        case DRW_CMD_DRWSTATE:
        case DRW_CMD_STENCIL:
          draw_call_batching_flush(shgroup, &state);
          break;
        case DRW_CMD_DRAW:
        case DRW_CMD_DRAW_PROCEDURAL:
        case DRW_CMD_DRAW_INSTANCE:
          if (draw_call_is_culled(&cmd->instance.handle, DST.view_active)) {
            continue;
          }
          break;
        default:
          break;
      }

      switch (cmd_type) {
        case DRW_CMD_CLEAR:
          GPU_framebuffer_clear(
#ifndef NDEBUG
              GPU_framebuffer_active_get(),
#else
              NULL,
#endif
              cmd->clear.clear_channels,
              (float[4]){cmd->clear.r / 255.0f,
                         cmd->clear.g / 255.0f,
                         cmd->clear.b / 255.0f,
                         cmd->clear.a / 255.0f},
              cmd->clear.depth,
              cmd->clear.stencil);
          break;
        case DRW_CMD_DRWSTATE:
          state.drw_state_enabled |= cmd->state.enable;
          state.drw_state_disabled |= cmd->state.disable;
          drw_state_set((pass_state & ~state.drw_state_disabled) | state.drw_state_enabled);
          break;
        case DRW_CMD_STENCIL:
          drw_stencil_set(cmd->stencil.mask);
          break;
        case DRW_CMD_SELECTID:
          state.select_id = cmd->select_id.select_id;
          state.select_buf = cmd->select_id.select_buf;
          break;
        case DRW_CMD_DRAW:
          if (!USE_BATCHING || state.obmats_loc == -1 || (G.f & G_FLAG_PICKSEL) ||
              cmd->draw.batch->inst) {
            draw_call_single_do(shgroup, &state, cmd->draw.batch, cmd->draw.handle, 0, 0, 0);
          }
          else {
            draw_call_batching_do(shgroup, &state, &cmd->draw);
          }
          break;
        case DRW_CMD_DRAW_PROCEDURAL:
          draw_call_single_do(shgroup,
                              &state,
                              cmd->procedural.batch,
                              cmd->procedural.handle,
                              0,
                              cmd->procedural.vert_count,
                              1);
          break;
        case DRW_CMD_DRAW_INSTANCE:
          draw_call_single_do(shgroup,
                              &state,
                              cmd->instance.batch,
                              cmd->instance.handle,
                              0,
                              0,
                              cmd->instance.inst_count);
          break;
        case DRW_CMD_DRAW_RANGE:
          draw_call_single_do(shgroup,
                              &state,
                              cmd->range.batch,
                              (DRWResourceHandle)0,
                              cmd->range.vert_first,
                              cmd->range.vert_count,
                              1);
          break;
      }
    }

    draw_call_batching_finish(shgroup, &state);
  }

  if (use_tfeedback) {
    GPU_shader_transform_feedback_disable(shgroup->shader);
  }
}

static void drw_update_view(void)
{
  /* TODO(fclem) update a big UBO and only bind ranges here. */
  DRW_uniformbuffer_update(G_draw.view_ubo, &DST.view_active->storage);

  /* TODO get rid of this. */
  DST.view_storage_cpy = DST.view_active->storage;

  draw_compute_culling(DST.view_active);
}

static void drw_draw_pass_ex(DRWPass *pass,
                             DRWShadingGroup *start_group,
                             DRWShadingGroup *end_group)
{
  if (start_group == NULL) {
    return;
  }

  DST.shader = NULL;

  BLI_assert(DST.buffer_finish_called &&
             "DRW_render_instance_buffer_finish had not been called before drawing");

  if (DST.view_previous != DST.view_active || DST.view_active->is_dirty) {
    drw_update_view();
    DST.view_active->is_dirty = false;
    DST.view_previous = DST.view_active;
  }

  /* GPU_framebuffer_clear calls can change the state outside the DRW module.
   * Force reset the affected states to avoid problems later. */
  drw_state_set(DST.state | DRW_STATE_WRITE_DEPTH | DRW_STATE_WRITE_COLOR);

  drw_state_set(pass->state);
  drw_state_validate();

  DRW_stats_query_start(pass->name);

  for (DRWShadingGroup *shgroup = start_group; shgroup; shgroup = shgroup->next) {
    draw_shgroup(shgroup, pass->state);
    /* break if upper limit */
    if (shgroup == end_group) {
      break;
    }
  }

  /* Clear Bound textures */
  for (int i = 0; i < DST_MAX_SLOTS; i++) {
    if (DST.RST.bound_texs[i] != NULL) {
      GPU_texture_unbind(DST.RST.bound_texs[i]);
      DST.RST.bound_texs[i] = NULL;
    }
  }

  /* Clear Bound Ubos */
  for (int i = 0; i < DST_MAX_SLOTS; i++) {
    if (DST.RST.bound_ubos[i] != NULL) {
      GPU_uniformbuffer_unbind(DST.RST.bound_ubos[i]);
      DST.RST.bound_ubos[i] = NULL;
    }
  }

  if (DST.shader) {
    GPU_shader_unbind();
    DST.shader = NULL;
  }

  if (DST.batch) {
    DST.batch->program_in_use = false;
    DST.batch = NULL;
  }

  /* Fix T67342 for some reason. AMD Pro driver bug. */
  if ((DST.state & DRW_STATE_BLEND_CUSTOM) != 0 &&
      GPU_type_matches(GPU_DEVICE_ATI, GPU_OS_ANY, GPU_DRIVER_OFFICIAL)) {
    drw_state_set(DST.state & ~DRW_STATE_BLEND_CUSTOM);
  }

  /* HACK: Rasterized discard can affect clear commands which are not
   * part of a DRWPass (as of now). So disable rasterized discard here
   * if it has been enabled. */
  if ((DST.state & DRW_STATE_RASTERIZER_ENABLED) == 0) {
    drw_state_set((DST.state & ~DRW_STATE_RASTERIZER_ENABLED) | DRW_STATE_DEFAULT);
  }

  DRW_stats_query_end();
}

void DRW_draw_pass(DRWPass *pass)
{
  drw_draw_pass_ex(pass, pass->shgroups.first, pass->shgroups.last);
}

/* Draw only a subset of shgroups. Used in special situations as grease pencil strokes */
void DRW_draw_pass_subset(DRWPass *pass, DRWShadingGroup *start_group, DRWShadingGroup *end_group)
{
  drw_draw_pass_ex(pass, start_group, end_group);
}

/** \} */

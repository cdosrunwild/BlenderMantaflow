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
 * The Original Code is Copyright (C) Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup bke
 */

/* Part of the code copied from elbeem fluid library, copyright by Nils Thuerey */

#include "MEM_guardedalloc.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h> /* memset */

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_kdopbvh.h"
#include "BLI_threads.h"
#include "BLI_utildefines.h"

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_constraint_types.h"
#include "DNA_customdata_types.h"
#include "DNA_light_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_particle_types.h"
#include "DNA_scene_types.h"
#include "DNA_manta_types.h"

#include "BKE_appdir.h"
#include "BKE_animsys.h"
#include "BKE_armature.h"
#include "BKE_bvhutils.h"
#include "BKE_collision.h"
#include "BKE_colortools.h"
#include "BKE_constraint.h"
#include "BKE_customdata.h"
#include "BKE_deform.h"
#include "BKE_effect.h"
#include "BKE_library.h"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_modifier.h"
#include "BKE_object.h"
#include "BKE_particle.h"
#include "BKE_pointcache.h"
#include "BKE_scene.h"
#include "BKE_manta.h"
#include "BKE_texture.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "RE_shader_ext.h"

#include "GPU_glew.h"

/* UNUSED so far, may be enabled later */
/* #define USE_SMOKE_COLLISION_DM */

//#define DEBUG_TIME

#include "manta_fluid_API.h"

#ifdef DEBUG_TIME
#  include "PIL_time.h"
#endif

#  include "BLI_task.h"
#  include "BLI_kdtree.h"
#  include "BLI_voxel.h"

static ThreadMutex object_update_lock = BLI_MUTEX_INITIALIZER;

struct Mesh;
struct Object;
struct Scene;
struct MantaModifierData;

// timestep default value for nice appearance 0.1f
#  define DT_DEFAULT 0.1f

#  define ADD_IF_LOWER_POS(a, b) (min_ff((a) + (b), max_ff((a), (b))))
#  define ADD_IF_LOWER_NEG(a, b) (max_ff((a) + (b), min_ff((a), (b))))
#  define ADD_IF_LOWER(a, b) (((b) > 0) ? ADD_IF_LOWER_POS((a), (b)) : ADD_IF_LOWER_NEG((a), (b)))

void BKE_manta_reallocate_fluid(MantaDomainSettings *mds, int res[3], int free_old)
{
  if (free_old && mds->fluid) {
    manta_free(mds->fluid);
  }
  if (!min_iii(res[0], res[1], res[2])) {
    mds->fluid = NULL;
    return;
  }

  mds->fluid = manta_init(res, mds->mmd);

  mds->res_noise[0] = res[0] * mds->noise_scale;
  mds->res_noise[1] = res[1] * mds->noise_scale;
  mds->res_noise[2] = res[2] * mds->noise_scale;
}

void BKE_manta_reallocate_copy_fluid(MantaDomainSettings *mds,
                                     int o_res[3],
                                     int n_res[3],
                                     int o_min[3],
                                     int n_min[3],
                                     int o_max[3],
                                     int o_shift[3],
                                     int n_shift[3])
{
  int x, y, z;
  struct MANTA *fluid_old = mds->fluid;
  const int block_size = mds->noise_scale;
  int new_shift[3] = {0};
  sub_v3_v3v3_int(new_shift, n_shift, o_shift);

  /* allocate new fluid data */
  BKE_manta_reallocate_fluid(mds, n_res, 0);

  int o_total_cells = o_res[0] * o_res[1] * o_res[2];
  int n_total_cells = n_res[0] * n_res[1] * n_res[2];

  /* boundary cells will be skipped when copying data */
  int bwidth = mds->boundary_width;

  /* copy values from old fluid to new */
  if (o_total_cells > 1 && n_total_cells > 1) {
    /* base smoke */
    float *o_dens, *o_react, *o_flame, *o_fuel, *o_heat, *o_vx, *o_vy, *o_vz,
      *o_r, *o_g, *o_b;
    float *n_dens, *n_react, *n_flame, *n_fuel, *n_heat, *n_vx, *n_vy, *n_vz,
      *n_r, *n_g, *n_b;
    float dummy, *dummy_s;
    int *dummy_p;
    /* noise smoke */
    int wt_res_old[3];
    float *o_wt_dens, *o_wt_react, *o_wt_flame, *o_wt_fuel, *o_wt_tcu, *o_wt_tcv, *o_wt_tcw,
      *o_wt_tcu2, *o_wt_tcv2, *o_wt_tcw2, *o_wt_r, *o_wt_g, *o_wt_b;
    float *n_wt_dens, *n_wt_react, *n_wt_flame, *n_wt_fuel, *n_wt_tcu, *n_wt_tcv, *n_wt_tcw,
      *n_wt_tcu2, *n_wt_tcv2, *n_wt_tcw2, *n_wt_r, *n_wt_g, *n_wt_b;

    if (mds->flags & FLUID_DOMAIN_USE_NOISE) {
      manta_smoke_turbulence_export(fluid_old,
                  &o_wt_dens,
                  &o_wt_react,
                  &o_wt_flame,
                  &o_wt_fuel,
                  &o_wt_r,
                  &o_wt_g,
                  &o_wt_b,
                  &o_wt_tcu,
                  &o_wt_tcv,
                  &o_wt_tcw,
                  &o_wt_tcu2,
                  &o_wt_tcv2,
                  &o_wt_tcw2);
      manta_smoke_turbulence_get_res(fluid_old, wt_res_old);
      manta_smoke_turbulence_export(mds->fluid,
                  &n_wt_dens,
                  &n_wt_react,
                  &n_wt_flame,
                  &n_wt_fuel,
                  &n_wt_r,
                  &n_wt_g,
                  &n_wt_b,
                  &n_wt_tcu,
                  &n_wt_tcv,
                  &n_wt_tcw,
                  &n_wt_tcu2,
                  &n_wt_tcv2,
                  &n_wt_tcw2);
    }

    manta_smoke_export(fluid_old,
         &dummy,
         &dummy,
         &o_dens,
         &o_react,
         &o_flame,
         &o_fuel,
         &o_heat,
         &o_vx,
         &o_vy,
         &o_vz,
         &o_r,
         &o_g,
         &o_b,
         &dummy_p,
         &dummy_s);
    manta_smoke_export(mds->fluid,
         &dummy,
         &dummy,
         &n_dens,
         &n_react,
         &n_flame,
         &n_fuel,
         &n_heat,
         &n_vx,
         &n_vy,
         &n_vz,
         &n_r,
         &n_g,
         &n_b,
         &dummy_p,
         &dummy_s);

    for (x = o_min[0]; x < o_max[0]; x++) {
      for (y = o_min[1]; y < o_max[1]; y++) {
        for (z = o_min[2]; z < o_max[2]; z++) {
          /* old grid index */
          int xo = x - o_min[0];
          int yo = y - o_min[1];
          int zo = z - o_min[2];
          int index_old = manta_get_index(xo, o_res[0], yo, o_res[1], zo);
          /* new grid index */
          int xn = x - n_min[0] - new_shift[0];
          int yn = y - n_min[1] - new_shift[1];
          int zn = z - n_min[2] - new_shift[2];
          int index_new = manta_get_index(xn, n_res[0], yn, n_res[1], zn);

          /* skip if outside new domain */
          if (xn < 0 || xn >= n_res[0] || yn < 0 || yn >= n_res[1] || zn < 0 || zn >= n_res[2]) {
            continue;
          }
          /* skip if trying to copy from old boundary cell */
          if (xo < bwidth || yo < bwidth || zo < bwidth ||
              xo >= o_res[0]-bwidth || yo >= o_res[1]-bwidth || zo >= o_res[2]-bwidth) {
              continue;
          }
          /* skip if trying to copy into new boundary cell */
          if (xn < bwidth || yn < bwidth || zn < bwidth ||
              xn >= n_res[0]-bwidth || yn >= n_res[1]-bwidth || zn >= n_res[2]-bwidth) {
              continue;
          }

          /* copy data */
          if (mds->flags & FLUID_DOMAIN_USE_NOISE) {
            int i, j, k;
            /* old grid index */
            int xx_o = xo * block_size;
            int yy_o = yo * block_size;
            int zz_o = zo * block_size;
            /* new grid index */
            int xx_n = xn * block_size;
            int yy_n = yn * block_size;
            int zz_n = zn * block_size;

            /* insert old texture values into new texture grids */
            n_wt_tcu[index_new] = o_wt_tcu[index_old];
            n_wt_tcv[index_new] = o_wt_tcv[index_old];
            n_wt_tcw[index_new] = o_wt_tcw[index_old];

            n_wt_tcu2[index_new] = o_wt_tcu2[index_old];
            n_wt_tcv2[index_new] = o_wt_tcv2[index_old];
            n_wt_tcw2[index_new] = o_wt_tcw2[index_old];

            for (i = 0; i < block_size; i++) {
              for (j = 0; j < block_size; j++) {
                for (k = 0; k < block_size; k++) {
                  int big_index_old = manta_get_index(
                    xx_o + i, wt_res_old[0], yy_o + j, wt_res_old[1], zz_o + k);
                  int big_index_new = manta_get_index(
                    xx_n + i, mds->res_noise[0], yy_n + j, mds->res_noise[1], zz_n + k);
                  /* copy data */
                  n_wt_dens[big_index_new] = o_wt_dens[big_index_old];
                  if (n_wt_flame && o_wt_flame) {
                    n_wt_flame[big_index_new] = o_wt_flame[big_index_old];
                    n_wt_fuel[big_index_new] = o_wt_fuel[big_index_old];
                    n_wt_react[big_index_new] = o_wt_react[big_index_old];
                  }
                  if (n_wt_r && o_wt_r) {
                    n_wt_r[big_index_new] = o_wt_r[big_index_old];
                    n_wt_g[big_index_new] = o_wt_g[big_index_old];
                    n_wt_b[big_index_new] = o_wt_b[big_index_old];
                  }
                }
              }
            }
          }

          n_dens[index_new] = o_dens[index_old];
          /* heat */
          if (n_heat && o_heat) {
            n_heat[index_new] = o_heat[index_old];
          }
          /* fuel */
          if (n_fuel && o_fuel) {
            n_flame[index_new] = o_flame[index_old];
            n_fuel[index_new] = o_fuel[index_old];
            n_react[index_new] = o_react[index_old];
          }
          /* color */
          if (o_r && n_r) {
            n_r[index_new] = o_r[index_old];
            n_g[index_new] = o_g[index_old];
            n_b[index_new] = o_b[index_old];
          }
          n_vx[index_new] = o_vx[index_old];
          n_vy[index_new] = o_vy[index_old];
          n_vz[index_new] = o_vz[index_old];

        }
      }
    }
  }
  manta_free(fluid_old);
}

/* convert global position to domain cell space */
static void manta_pos_to_cell(MantaDomainSettings *mds, float pos[3])
{
  mul_m4_v3(mds->imat, pos);
  sub_v3_v3(pos, mds->p0);
  pos[0] *= 1.0f / mds->cell_size[0];
  pos[1] *= 1.0f / mds->cell_size[1];
  pos[2] *= 1.0f / mds->cell_size[2];
}

/* set domain transformations and base resolution from object mesh */
static void manta_set_domain_from_mesh(MantaDomainSettings *mds,
                                       Object *ob,
                                       Mesh *me,
                                       bool init_resolution)
{
  size_t i;
  float min[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, max[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
  float size[3];
  MVert *verts = me->mvert;
  float scale = 0.0;
  int res;

  res = mds->maxres;

  // get BB of domain
  for (i = 0; i < me->totvert; i++) {
    // min BB
    min[0] = MIN2(min[0], verts[i].co[0]);
    min[1] = MIN2(min[1], verts[i].co[1]);
    min[2] = MIN2(min[2], verts[i].co[2]);

    // max BB
    max[0] = MAX2(max[0], verts[i].co[0]);
    max[1] = MAX2(max[1], verts[i].co[1]);
    max[2] = MAX2(max[2], verts[i].co[2]);
  }

  /* set domain bounds */
  copy_v3_v3(mds->p0, min);
  copy_v3_v3(mds->p1, max);
  mds->dx = 1.0f / res;

  /* calculate domain dimensions */
  sub_v3_v3v3(size, max, min);
  if (init_resolution) {
    zero_v3_int(mds->base_res);
    copy_v3_v3(mds->cell_size, size);
  }
  /* apply object scale */
  for (i = 0; i < 3; i++) {
    size[i] = fabsf(size[i] * ob->scale[i]);
  }
  copy_v3_v3(mds->global_size, size);
  copy_v3_v3(mds->dp0, min);

  invert_m4_m4(mds->imat, ob->obmat);

  // prevent crash when initializing a plane as domain
  if (!init_resolution || (size[0] < FLT_EPSILON) || (size[1] < FLT_EPSILON) ||
      (size[2] < FLT_EPSILON)) {
    return;
  }

  /* define grid resolutions from longest domain side */
  if (size[0] >= MAX2(size[1], size[2])) {
    scale = res / size[0];
    mds->scale = size[0] / fabsf(ob->scale[0]);
    mds->base_res[0] = res;
    mds->base_res[1] = max_ii((int)(size[1] * scale + 0.5f), 4);
    mds->base_res[2] = max_ii((int)(size[2] * scale + 0.5f), 4);
  }
  else if (size[1] >= MAX2(size[0], size[2])) {
    scale = res / size[1];
    mds->scale = size[1] / fabsf(ob->scale[1]);
    mds->base_res[0] = max_ii((int)(size[0] * scale + 0.5f), 4);
    mds->base_res[1] = res;
    mds->base_res[2] = max_ii((int)(size[2] * scale + 0.5f), 4);
  }
  else {
    scale = res / size[2];
    mds->scale = size[2] / fabsf(ob->scale[2]);
    mds->base_res[0] = max_ii((int)(size[0] * scale + 0.5f), 4);
    mds->base_res[1] = max_ii((int)(size[1] * scale + 0.5f), 4);
    mds->base_res[2] = res;
  }

  /* set cell size */
  mds->cell_size[0] /= (float)mds->base_res[0];
  mds->cell_size[1] /= (float)mds->base_res[1];
  mds->cell_size[2] /= (float)mds->base_res[2];
}

static void manta_set_domain_gravity(Scene *scene, MantaDomainSettings *mds)
{
  float gravity[3] = {0.0f, 0.0f, -1.0f};
  float gravity_mag;

  /* use global gravity if enabled */
  if (scene->physics_settings.flag & PHYS_GLOBAL_GRAVITY) {
    copy_v3_v3(gravity, scene->physics_settings.gravity);
    /* map default value to 1.0 */
    mul_v3_fl(gravity, 1.0f / 9.810f);

    /* convert gravity to domain space */
    gravity_mag = len_v3(gravity);
    mul_mat3_m4_v3(mds->imat, gravity);
    normalize_v3(gravity);
    mul_v3_fl(gravity, gravity_mag);

    mds->gravity[0] = gravity[0];
    mds->gravity[1] = gravity[1];
    mds->gravity[2] = gravity[2];
  }
}

static int mantaModifier_init(
    MantaModifierData *mmd, Depsgraph *depsgraph, Object *ob, Scene *scene, Mesh *me)
{
  int scene_framenr = (int)DEG_get_ctime(depsgraph);

  if ((mmd->type & MOD_MANTA_TYPE_DOMAIN) && mmd->domain && !mmd->domain->fluid) {
    MantaDomainSettings *mds = mmd->domain;
    int res[3];
    /* set domain dimensions from mesh */
    manta_set_domain_from_mesh(mds, ob, me, true);
    /* set domain gravity */
    manta_set_domain_gravity(scene, mds);
    /* reset domain values */
    zero_v3_int(mds->shift);
    zero_v3(mds->shift_f);
    add_v3_fl(mds->shift_f, 0.5f);
    zero_v3(mds->prev_loc);
    mul_m4_v3(ob->obmat, mds->prev_loc);
    copy_m4_m4(mds->obmat, ob->obmat);

    /* set resolutions */
    if (mmd->domain->type == FLUID_DOMAIN_TYPE_GAS &&
        mmd->domain->flags & FLUID_DOMAIN_USE_ADAPTIVE_DOMAIN)
    {
      res[0] = res[1] = res[2] = 1; /* use minimum res for adaptive init */
    }
    else {
      copy_v3_v3_int(res, mds->base_res);
    }
    copy_v3_v3_int(mds->res, res);
    mds->total_cells = mds->res[0] * mds->res[1] * mds->res[2];
    mds->res_min[0] = mds->res_min[1] = mds->res_min[2] = 0;
    copy_v3_v3_int(mds->res_max, res);

    /* set time, frame length = 0.1 is at 25fps */
    float fps = scene->r.frs_sec / scene->r.frs_sec_base;
    mds->frame_length = DT_DEFAULT * (25.0f / fps) * mds->time_scale;
    /* initially dt is equal to frame length (dt can change with adaptive-time stepping though) */
    mds->dt = mds->frame_length;
    mds->time_per_frame = 0;
    mds->time_total = (scene_framenr-1) * mds->frame_length;

    /* allocate fluid */
    BKE_manta_reallocate_fluid(mds, mds->res, 0);

    mmd->time = scene_framenr;

    return 1;
  }
  else if (mmd->type & MOD_MANTA_TYPE_FLOW) {
    if (!mmd->flow) {
      mantaModifier_createType(mmd);
    }
    mmd->time = scene_framenr;
    return 1;
  }
  else if (mmd->type & MOD_MANTA_TYPE_EFFEC) {
    if (!mmd->effec) {
      mantaModifier_createType(mmd);
    }
    mmd->time = scene_framenr;
    return 1;
  }
  return 0;
}

static void mantaModifier_freeDomain(MantaModifierData *mmd)
{
  if (mmd->domain) {
    if (mmd->domain->fluid) {
      manta_free(mmd->domain->fluid);
    }

    if (mmd->domain->fluid_mutex) {
      BLI_rw_mutex_free(mmd->domain->fluid_mutex);
    }

    if (mmd->domain->effector_weights) {
      MEM_freeN(mmd->domain->effector_weights);
    }
    mmd->domain->effector_weights = NULL;

    if (!(mmd->modifier.flag & eModifierFlag_SharedCaches)) {
      BKE_ptcache_free_list(&(mmd->domain->ptcaches[0]));
      mmd->domain->point_cache[0] = NULL;
    }

    if (mmd->domain->mesh_velocities) {
      MEM_freeN(mmd->domain->mesh_velocities);
    }
    mmd->domain->mesh_velocities = NULL;

    if (mmd->domain->coba) {
      MEM_freeN(mmd->domain->coba);
    }

    MEM_freeN(mmd->domain);
    mmd->domain = NULL;
  }
}

static void mantaModifier_freeFlow(MantaModifierData *mmd)
{
  if (mmd->flow) {
    if (mmd->flow->mesh) {
      BKE_id_free(NULL, mmd->flow->mesh);
    }
    mmd->flow->mesh = NULL;

    if (mmd->flow->verts_old) {
      MEM_freeN(mmd->flow->verts_old);
    }
    mmd->flow->verts_old = NULL;
    mmd->flow->numverts = 0;

    MEM_freeN(mmd->flow);
    mmd->flow = NULL;
  }
}

static void mantaModifier_freeCollision(MantaModifierData *mmd)
{
  if (mmd->effec) {
    if (mmd->effec->mesh) {
      BKE_id_free(NULL, mmd->effec->mesh);
    }
    mmd->effec->mesh = NULL;

    if (mmd->effec->verts_old) {
      MEM_freeN(mmd->effec->verts_old);
    }
    mmd->effec->verts_old = NULL;
    mmd->effec->numverts = 0;

    MEM_freeN(mmd->effec);
    mmd->effec = NULL;
  }
}

static void mantaModifier_reset_ex(struct MantaModifierData *mmd, bool need_lock)
{
  if (mmd) {
    if (mmd->domain) {
      if (mmd->domain->fluid) {
        if (need_lock) {
          BLI_rw_mutex_lock(mmd->domain->fluid_mutex, THREAD_LOCK_WRITE);
        }

        manta_free(mmd->domain->fluid);
        mmd->domain->fluid = NULL;

        if (need_lock) {
          BLI_rw_mutex_unlock(mmd->domain->fluid_mutex);
        }
      }

      mmd->time = -1;
      mmd->domain->total_cells = 0;
      mmd->domain->active_fields = 0;
    }
    else if (mmd->flow) {
      if (mmd->flow->verts_old) {
        MEM_freeN(mmd->flow->verts_old);
      }
      mmd->flow->verts_old = NULL;
      mmd->flow->numverts = 0;
    }
    else if (mmd->effec) {
      if (mmd->effec->verts_old) {
        MEM_freeN(mmd->effec->verts_old);
      }
      mmd->effec->verts_old = NULL;
      mmd->effec->numverts = 0;
    }
  }
}

void mantaModifier_reset(struct MantaModifierData *mmd)
{
  mantaModifier_reset_ex(mmd, true);
}

void mantaModifier_free(MantaModifierData *mmd)
{
  if (mmd) {
    mantaModifier_freeDomain(mmd);
    mantaModifier_freeFlow(mmd);
    mantaModifier_freeCollision(mmd);
  }
}

void mantaModifier_createType(struct MantaModifierData *mmd)
{
  if (mmd) {
    if (mmd->type & MOD_MANTA_TYPE_DOMAIN) {
      if (mmd->domain) {
        mantaModifier_freeDomain(mmd);
      }

      /* domain object data */
      mmd->domain = MEM_callocN(sizeof(MantaDomainSettings), "MantaDomain");
      mmd->domain->mmd = mmd;
      mmd->domain->effector_weights = BKE_effector_add_weights(NULL);
      mmd->domain->fluid = NULL;
      mmd->domain->fluid_mutex = BLI_rw_mutex_alloc();
      mmd->domain->eff_group = NULL;
      mmd->domain->fluid_group = NULL;
      mmd->domain->coll_group = NULL;

      /* adaptive domain options */
      mmd->domain->adapt_margin = 4;
      mmd->domain->adapt_res = 0;
      mmd->domain->adapt_threshold = 0.02f;

      /* fluid domain options */
      mmd->domain->maxres = 64;
      mmd->domain->solver_res = 3;
      mmd->domain->border_collisions = 0;  // open domain
      mmd->domain->flags = FLUID_DOMAIN_USE_DISSOLVE_LOG | FLUID_DOMAIN_USE_ADAPTIVE_TIME;
      mmd->domain->gravity[0] = 0.0f;
      mmd->domain->gravity[1] = 0.0f;
      mmd->domain->gravity[2] = -1.0f;
      mmd->domain->active_fields = 0;
      mmd->domain->type = FLUID_DOMAIN_TYPE_GAS;
      mmd->domain->boundary_width = 1;

      /* smoke domain options */
      mmd->domain->alpha = 1.0f;
      mmd->domain->beta = 1.0f;
      mmd->domain->diss_speed = 5;
      mmd->domain->vorticity = 0;
      mmd->domain->active_color[0] = 0.0f;
      mmd->domain->active_color[1] = 0.0f;
      mmd->domain->active_color[2] = 0.0f;
      mmd->domain->highres_sampling = SM_HRES_FULLSAMPLE;

      /* flame options */
      mmd->domain->burning_rate = 0.75f;
      mmd->domain->flame_smoke = 1.0f;
      mmd->domain->flame_vorticity = 0.5f;
      mmd->domain->flame_ignition = 1.5f;
      mmd->domain->flame_max_temp = 3.0f;
      mmd->domain->flame_smoke_color[0] = 0.7f;
      mmd->domain->flame_smoke_color[1] = 0.7f;
      mmd->domain->flame_smoke_color[2] = 0.7f;

      /* noise options */
      mmd->domain->noise_strength = 1.0;
      mmd->domain->noise_pos_scale = 2.0f;
      mmd->domain->noise_time_anim = 0.1f;
      mmd->domain->noise_scale = 2;
      mmd->domain->noise_type = FLUID_NOISE_TYPE_WAVELET;

      /* liquid domain options */
      mmd->domain->particle_randomness = 0.1f;
      mmd->domain->particle_number = 2;
      mmd->domain->particle_minimum = 8;
      mmd->domain->particle_maximum = 16;
      mmd->domain->particle_radius = 1.8f;
      mmd->domain->particle_band_width = 3.0f;

      /* diffusion options*/
      mmd->domain->surface_tension = 0.0f;
      mmd->domain->viscosity_base = 1.0f;
      mmd->domain->viscosity_exponent = 6.0f;
      mmd->domain->domain_size = 0.5f;

      /* mesh options */
      mmd->domain->mesh_velocities = NULL;
      mmd->domain->mesh_concave_upper = 3.5f;
      mmd->domain->mesh_concave_lower = 0.4f;
      mmd->domain->mesh_smoothen_pos = 1;
      mmd->domain->mesh_smoothen_neg = 1;
      mmd->domain->mesh_scale = 2;
      mmd->domain->totvert = 0;
      mmd->domain->mesh_generator = FLUID_DOMAIN_MESH_IMPROVED;

      /* secondary particle options */
      mmd->domain->sndparticle_tau_min_wc = 2.0;
      mmd->domain->sndparticle_tau_max_wc = 8.0;
      mmd->domain->sndparticle_tau_min_ta = 5.0;
      mmd->domain->sndparticle_tau_max_ta = 20.0;
      mmd->domain->sndparticle_tau_min_k = 1.0;
      mmd->domain->sndparticle_tau_max_k = 5.0;
      mmd->domain->sndparticle_k_wc = 200;
      mmd->domain->sndparticle_k_ta = 40;
      mmd->domain->sndparticle_k_b = 0.5;
      mmd->domain->sndparticle_k_d = 0.6;
      mmd->domain->sndparticle_l_min = 10.0;
      mmd->domain->sndparticle_l_max = 25.0;
      mmd->domain->sndparticle_boundary = SNDPARTICLE_BOUNDARY_DELETE;
      mmd->domain->sndparticle_combined_export = SNDPARTICLE_COMBINED_EXPORT_OFF;
      mmd->domain->sndparticle_potential_radius = 2;
      mmd->domain->sndparticle_update_radius = 2;
      mmd->domain->particle_type = 0;
      mmd->domain->particle_scale = 1;

      /* fluid guiding options */
      mmd->domain->guiding_parent = NULL;
      mmd->domain->guiding_alpha = 2.0f;
      mmd->domain->guiding_beta = 5;
      mmd->domain->guiding_vel_factor = 2.0f;
      mmd->domain->guide_res = NULL;
      mmd->domain->guiding_source = FLUID_DOMAIN_GUIDING_SRC_DOMAIN;

      /* cache options */
      mmd->domain->cache_frame_start = 1;
      mmd->domain->cache_frame_end = 50;
      mmd->domain->cache_frame_pause_data = 0;
      mmd->domain->cache_frame_pause_noise = 0;
      mmd->domain->cache_frame_pause_mesh = 0;
      mmd->domain->cache_frame_pause_particles = 0;
      mmd->domain->cache_frame_pause_guiding = 0;
      mmd->domain->cache_flag = 0;
      mmd->domain->cache_type = FLUID_DOMAIN_CACHE_MODULAR;
      mmd->domain->cache_mesh_format = FLUID_DOMAIN_FILE_BIN_OBJECT;
      mmd->domain->cache_data_format = FLUID_DOMAIN_FILE_UNI;
      mmd->domain->cache_particle_format = FLUID_DOMAIN_FILE_UNI;
      mmd->domain->cache_noise_format = FLUID_DOMAIN_FILE_UNI;
      modifier_path_init(mmd->domain->cache_directory,
                         sizeof(mmd->domain->cache_directory),
                         FLUID_DOMAIN_DIR_DEFAULT);

      /* time options */
      mmd->domain->time_scale = 1.0;
      mmd->domain->cfl_condition = 4.0;

      /* display options */
      mmd->domain->slice_method = FLUID_DOMAIN_SLICE_VIEW_ALIGNED;
      mmd->domain->axis_slice_method = AXIS_SLICE_FULL;
      mmd->domain->slice_axis = 0;
      mmd->domain->interp_method = 0;
      mmd->domain->draw_velocity = false;
      mmd->domain->slice_per_voxel = 5.0f;
      mmd->domain->slice_depth = 0.5f;
      mmd->domain->display_thickness = 1.0f;
      mmd->domain->coba = NULL;
      mmd->domain->vector_scale = 1.0f;
      mmd->domain->vector_draw_type = VECTOR_DRAW_NEEDLE;
      mmd->domain->use_coba = false;
      mmd->domain->coba_field = FLUID_DOMAIN_FIELD_DENSITY;

      /* -- Deprecated / unsed options (below)-- */

      /* pointcache options */
      BLI_listbase_clear(&mmd->domain->ptcaches[1]);
      mmd->domain->point_cache[0] = BKE_ptcache_add(&(mmd->domain->ptcaches[0]));
      mmd->domain->point_cache[0]->flag |= PTCACHE_DISK_CACHE;
      mmd->domain->point_cache[0]->step = 1;
      mmd->domain->point_cache[1] = NULL; /* Deprecated */
      mmd->domain->cache_comp = SM_CACHE_LIGHT;
      mmd->domain->cache_high_comp = SM_CACHE_LIGHT;

      /* OpenVDB cache options */
#ifdef WITH_OPENVDB_BLOSC
      mmd->domain->openvdb_comp = VDB_COMPRESSION_BLOSC;
#else
      mmd->domain->openvdb_comp = VDB_COMPRESSION_ZIP;
#endif
      mmd->domain->clipping = 1e-3f;
      mmd->domain->data_depth = 0;
    }
    else if (mmd->type & MOD_MANTA_TYPE_FLOW) {
      if (mmd->flow) {
        mantaModifier_freeFlow(mmd);
      }

      /* flow object data */
      mmd->flow = MEM_callocN(sizeof(MantaFlowSettings), "MantaFlow");
      mmd->flow->mmd = mmd;
      mmd->flow->mesh = NULL;
      mmd->flow->psys = NULL;
      mmd->flow->noise_texture = NULL;

      /* initial velocity */
      mmd->flow->verts_old = NULL;
      mmd->flow->numverts = 0;
      mmd->flow->vel_multi = 1.0f;
      mmd->flow->vel_normal = 0.0f;
      mmd->flow->vel_random = 0.0f;
      mmd->flow->vel_coord[0] = 0.0f;
      mmd->flow->vel_coord[1] = 0.0f;
      mmd->flow->vel_coord[2] = 0.0f;

      /* emission */
      mmd->flow->density = 1.0f;
      mmd->flow->color[0] = 0.7f;
      mmd->flow->color[1] = 0.7f;
      mmd->flow->color[2] = 0.7f;
      mmd->flow->fuel_amount = 1.0f;
      mmd->flow->temp = 1.0f;
      mmd->flow->volume_density = 0.0f;
      mmd->flow->surface_distance = 1.5f;
      mmd->flow->particle_size = 1.0f;
      mmd->flow->subframes = 0;

      /* texture control */
      mmd->flow->source = FLUID_FLOW_SOURCE_MESH;
      mmd->flow->texture_size = 1.0f;

      mmd->flow->type = FLUID_FLOW_TYPE_SMOKE;
      mmd->flow->behavior = FLUID_FLOW_BEHAVIOR_GEOMETRY;
      mmd->flow->type = FLUID_FLOW_TYPE_SMOKE;
      mmd->flow->flags = FLUID_FLOW_ABSOLUTE | FLUID_FLOW_USE_PART_SIZE | FLUID_FLOW_USE_INFLOW;
    }
    else if (mmd->type & MOD_MANTA_TYPE_EFFEC) {
      if (mmd->effec) {
        mantaModifier_freeCollision(mmd);
      }

      /* effector object data */
      mmd->effec = MEM_callocN(sizeof(MantaCollSettings), "MantaColl");
      mmd->effec->mmd = mmd;
      mmd->effec->mesh = NULL;
      mmd->effec->verts_old = NULL;
      mmd->effec->numverts = 0;
      mmd->effec->surface_distance = 0.5f;
      mmd->effec->type = FLUID_EFFECTOR_TYPE_COLLISION;

      /* guiding options */
      mmd->effec->guiding_mode = FLUID_EFFECTOR_GUIDING_MAXIMUM;
      mmd->effec->vel_multi = 1.0f;
    }
  }
}

void mantaModifier_copy(const struct MantaModifierData *mmd,
                        struct MantaModifierData *tmmd,
                        const int flag)
{
  tmmd->type = mmd->type;
  tmmd->time = mmd->time;

  mantaModifier_createType(tmmd);

  if (tmmd->domain) {
    MantaDomainSettings *tmds = tmmd->domain;
    MantaDomainSettings *mds = mmd->domain;

    /* domain object data */
    tmds->fluid_group = mds->fluid_group;
    tmds->eff_group = mds->eff_group;
    tmds->coll_group = mds->coll_group;
    MEM_freeN(tmds->effector_weights);
    tmds->effector_weights = MEM_dupallocN(mds->effector_weights);

    /* adaptive domain options */
    tmds->adapt_margin = mds->adapt_margin;
    tmds->adapt_res = mds->adapt_res;
    tmds->adapt_threshold = mds->adapt_threshold;

    /* fluid domain options */
    tmds->maxres = mds->maxres;
    tmds->solver_res = mds->solver_res;
    tmds->border_collisions = mds->border_collisions;
    tmds->flags = mds->flags;
    tmds->gravity[0] = mds->gravity[0];
    tmds->gravity[1] = mds->gravity[1];
    tmds->gravity[2] = mds->gravity[2];
    tmds->active_fields = mds->active_fields;
    tmds->type = mds->type;
    tmds->boundary_width = mds->boundary_width;

    /* smoke domain options */
    tmds->alpha = mds->alpha;
    tmds->beta = mds->beta;
    tmds->diss_speed = mds->diss_speed;
    tmds->vorticity = mds->vorticity;
    tmds->highres_sampling = mds->highres_sampling;

    /* flame options */
    tmds->burning_rate = mds->burning_rate;
    tmds->flame_smoke = mds->flame_smoke;
    tmds->flame_vorticity = mds->flame_vorticity;
    tmds->flame_ignition = mds->flame_ignition;
    tmds->flame_max_temp = mds->flame_max_temp;
    copy_v3_v3(tmds->flame_smoke_color, mds->flame_smoke_color);

    /* noise options */
    tmds->noise_strength = mds->noise_strength;
    tmds->noise_pos_scale = mds->noise_pos_scale;
    tmds->noise_time_anim = mds->noise_time_anim;
    tmds->noise_scale = mds->noise_scale;
    tmds->noise_type = mds->noise_type;

    /* liquid domain options */
    tmds->particle_randomness = mds->particle_randomness;
    tmds->particle_number = mds->particle_number;
    tmds->particle_minimum = mds->particle_minimum;
    tmds->particle_maximum = mds->particle_maximum;
    tmds->particle_radius = mds->particle_radius;
    tmds->particle_band_width = mds->particle_band_width;

    /* diffusion options*/
    tmds->surface_tension = mds->surface_tension;
    tmds->viscosity_base = mds->viscosity_base;
    tmds->viscosity_exponent = mds->viscosity_exponent;
    tmds->domain_size = mds->domain_size;

    /* mesh options */
    if (mds->mesh_velocities) {
      tmds->mesh_velocities = MEM_dupallocN(mds->mesh_velocities);
    }
    tmds->mesh_concave_upper = mds->mesh_concave_upper;
    tmds->mesh_concave_lower = mds->mesh_concave_lower;
    tmds->mesh_smoothen_pos = mds->mesh_smoothen_pos;
    tmds->mesh_smoothen_neg = mds->mesh_smoothen_neg;
    tmds->mesh_scale = mds->mesh_scale;
    tmds->totvert = mds->totvert;
    tmds->mesh_generator = mds->mesh_generator;

    /* secondary particle options */
    tmds->sndparticle_k_b = mds->sndparticle_k_b;
    tmds->sndparticle_k_d = mds->sndparticle_k_d;
    tmds->sndparticle_k_ta = mds->sndparticle_k_ta;
    tmds->sndparticle_k_wc = mds->sndparticle_k_wc;
    tmds->sndparticle_l_max = mds->sndparticle_l_max;
    tmds->sndparticle_l_min = mds->sndparticle_l_min;
    tmds->sndparticle_tau_max_k = mds->sndparticle_tau_max_k;
    tmds->sndparticle_tau_max_ta = mds->sndparticle_tau_max_ta;
    tmds->sndparticle_tau_max_wc = mds->sndparticle_tau_max_wc;
    tmds->sndparticle_tau_min_k = mds->sndparticle_tau_min_k;
    tmds->sndparticle_tau_min_ta = mds->sndparticle_tau_min_ta;
    tmds->sndparticle_tau_min_wc = mds->sndparticle_tau_min_wc;
    tmds->sndparticle_boundary = mds->sndparticle_boundary;
    tmds->sndparticle_combined_export = mds->sndparticle_combined_export;
    tmds->sndparticle_potential_radius = mds->sndparticle_potential_radius;
    tmds->sndparticle_update_radius = mds->sndparticle_update_radius;
    tmds->particle_type = mds->particle_type;
    tmds->particle_scale = mds->particle_scale;

    /* fluid guiding options */
    tmds->guiding_parent = mds->guiding_parent;
    tmds->guiding_alpha = mds->guiding_alpha;
    tmds->guiding_beta = mds->guiding_beta;
    tmds->guiding_vel_factor = mds->guiding_vel_factor;
    tmds->guide_res = mds->guide_res;
    tmds->guiding_source = mds->guiding_source;

    /* cache options */
    tmds->cache_frame_start = mds->cache_frame_start;
    tmds->cache_frame_end = mds->cache_frame_end;
    tmds->cache_frame_pause_data = mds->cache_frame_pause_data;
    tmds->cache_frame_pause_noise = mds->cache_frame_pause_noise;
    tmds->cache_frame_pause_mesh = mds->cache_frame_pause_mesh;
    tmds->cache_frame_pause_particles = mds->cache_frame_pause_particles;
    tmds->cache_frame_pause_guiding = mds->cache_frame_pause_guiding;
    tmds->cache_flag = mds->cache_flag;
    tmds->cache_type = mds->cache_type;
    tmds->cache_mesh_format = mds->cache_mesh_format;
    tmds->cache_data_format = mds->cache_data_format;
    tmds->cache_particle_format = mds->cache_particle_format;
    tmds->cache_noise_format = mds->cache_noise_format;
    BLI_strncpy(tmds->cache_directory, mds->cache_directory, sizeof(tmds->cache_directory));

    /* time options */
    tmds->time_scale = mds->time_scale;
    tmds->cfl_condition = mds->cfl_condition;

    /* display options */
    tmds->slice_method = mds->slice_method;
    tmds->axis_slice_method = mds->axis_slice_method;
    tmds->slice_axis = mds->slice_axis;
    tmds->interp_method = mds->interp_method;
    tmds->draw_velocity = mds->draw_velocity;
    tmds->slice_per_voxel = mds->slice_per_voxel;
    tmds->slice_depth = mds->slice_depth;
    tmds->display_thickness = mds->display_thickness;
    if (mds->coba) {
      tmds->coba = MEM_dupallocN(mds->coba);
    }
    tmds->vector_scale = mds->vector_scale;
    tmds->vector_draw_type = mds->vector_draw_type;
    tmds->use_coba = mds->use_coba;
    tmds->coba_field = mds->coba_field;

    /* -- Deprecated / unsed options (below)-- */

    /* pointcache options */
    BKE_ptcache_free_list(&(tmds->ptcaches[0]));
    if (flag & LIB_ID_CREATE_NO_MAIN) {
      /* Share the cache with the original object's modifier. */
      tmmd->modifier.flag |= eModifierFlag_SharedCaches;
      tmds->point_cache[0] = mds->point_cache[0];
      tmds->ptcaches[0] = mds->ptcaches[0];
    }
    else {
      tmds->point_cache[0] = BKE_ptcache_copy_list(
          &(tmds->ptcaches[0]), &(mds->ptcaches[0]), flag);
    }

    /* OpenVDB cache options */
    tmds->openvdb_comp = mds->openvdb_comp;
    tmds->clipping = mds->clipping;
    tmds->data_depth = mds->data_depth;
  }
  else if (tmmd->flow) {
    MantaFlowSettings *tmfs = tmmd->flow;
    MantaFlowSettings *mfs = mmd->flow;

    tmfs->psys = mfs->psys;
    tmfs->noise_texture = mfs->noise_texture;

    /* initial velocity */
    tmfs->vel_multi = mfs->vel_multi;
    tmfs->vel_normal = mfs->vel_normal;
    tmfs->vel_random = mfs->vel_random;
    tmfs->vel_coord[0] = mfs->vel_coord[0];
    tmfs->vel_coord[1] = mfs->vel_coord[1];
    tmfs->vel_coord[2] = mfs->vel_coord[2];

    /* emission */
    tmfs->density = mfs->density;
    copy_v3_v3(tmfs->color, mfs->color);
    tmfs->fuel_amount = mfs->fuel_amount;
    tmfs->temp = mfs->temp;
    tmfs->volume_density = mfs->volume_density;
    tmfs->surface_distance = mfs->surface_distance;
    tmfs->particle_size = mfs->particle_size;
    tmfs->subframes = mfs->subframes;

    /* texture control */
    tmfs->texture_size = mfs->texture_size;
    tmfs->texture_offset = mfs->texture_offset;
    BLI_strncpy(tmfs->uvlayer_name, mfs->uvlayer_name, sizeof(tmfs->uvlayer_name));
    tmfs->vgroup_density = mfs->vgroup_density;

    tmfs->type = mfs->type;
    tmfs->behavior = mfs->behavior;
    tmfs->source = mfs->source;
    tmfs->texture_type = mfs->texture_type;
    tmfs->flags = mfs->flags;
  }
  else if (tmmd->effec) {
    MantaCollSettings *tmcs = tmmd->effec;
    MantaCollSettings *mcs = mmd->effec;

    tmcs->surface_distance = mcs->surface_distance;
    tmcs->type = mcs->type;

    /* guiding options */
    tmcs->guiding_mode = mcs->guiding_mode;
    tmcs->vel_multi = mcs->vel_multi;
  }
}

// forward declaration
static void manta_smoke_calc_transparency(MantaDomainSettings *mds, ViewLayer *view_layer);
static float calc_voxel_transp(
    float *result, float *input, int res[3], int *pixel, float *tRay, float correct);
static void update_mesh_distances(int index,
                                  float *mesh_distances,
                                  BVHTreeFromMesh *treeData,
                                  const float ray_start[3],
                                  float surface_thickness);

static int get_light(ViewLayer *view_layer, float *light)
{
  Base *base_tmp = NULL;
  int found_light = 0;

  // try to find a lamp, preferably local
  for (base_tmp = FIRSTBASE(view_layer); base_tmp; base_tmp = base_tmp->next) {
    if (base_tmp->object->type == OB_LAMP) {
      Light *la = base_tmp->object->data;

      if (la->type == LA_LOCAL) {
        copy_v3_v3(light, base_tmp->object->obmat[3]);
        return 1;
      }
      else if (!found_light) {
        copy_v3_v3(light, base_tmp->object->obmat[3]);
        found_light = 1;
      }
    }
  }

  return found_light;
}

/**********************************************************
 * Obstacles
 **********************************************************/

typedef struct ObstaclesFromDMData {
  MantaDomainSettings *mds;
  MantaCollSettings *mcs;
  const MVert *mvert;
  const MLoop *mloop;
  const MLoopTri *looptri;
  BVHTreeFromMesh *tree;

  bool has_velocity;
  float *vert_vel;
  float *velocityX, *velocityY, *velocityZ;
  int *num_objects;
  float *distances_map;
} ObstaclesFromDMData;

static void obstacles_from_mesh_task_cb(void *__restrict userdata,
                                        const int z,
                                        const TaskParallelTLS *__restrict UNUSED(tls))
{
  ObstaclesFromDMData *data = userdata;
  MantaDomainSettings *mds = data->mds;

  /* slightly rounded-up sqrt(3 * (0.5)^2) == max. distance of cell boundary along the diagonal */
  const float surface_distance = 2.0f;  //0.867f;
  /* Note: Use larger surface distance to cover larger area with obvel. Manta will use these obvels and extrapolate them (inside and outside obstacle) */

  for (int x = mds->res_min[0]; x < mds->res_max[0]; x++) {
    for (int y = mds->res_min[1]; y < mds->res_max[1]; y++) {
      const int index = manta_get_index(
          x - mds->res_min[0], mds->res[0], y - mds->res_min[1], mds->res[1], z - mds->res_min[2]);

      float ray_start[3] = {(float)x + 0.5f, (float)y + 0.5f, (float)z + 0.5f};
      BVHTreeNearest nearest = {0};
      nearest.index = -1;
      nearest.dist_sq = surface_distance *
                        surface_distance; /* find_nearest uses squared distance */
      bool hasIncObj = false;

      /* find the nearest point on the mesh */
      if (BLI_bvhtree_find_nearest(
              data->tree->tree, ray_start, &nearest, data->tree->nearest_callback, data->tree) !=
          -1) {
        const MLoopTri *lt = &data->looptri[nearest.index];
        float weights[3];
        int v1, v2, v3;

        /* calculate barycentric weights for nearest point */
        v1 = data->mloop[lt->tri[0]].v;
        v2 = data->mloop[lt->tri[1]].v;
        v3 = data->mloop[lt->tri[2]].v;
        interp_weights_tri_v3(
            weights, data->mvert[v1].co, data->mvert[v2].co, data->mvert[v3].co, nearest.co);

        if (data->has_velocity) {
          /* increase object count */
          data->num_objects[index]++;
          hasIncObj = true;

          /* apply object velocity */
          float hit_vel[3];
          interp_v3_v3v3v3(hit_vel,
                           &data->vert_vel[v1 * 3],
                           &data->vert_vel[v2 * 3],
                           &data->vert_vel[v3 * 3],
                           weights);

          /* Guiding has additional velocity multiplier */
          if (data->mcs->type == FLUID_EFFECTOR_TYPE_GUIDE) {
            mul_v3_fl(hit_vel, data->mcs->vel_multi);

            switch (data->mcs->guiding_mode) {
              case FLUID_EFFECTOR_GUIDING_AVERAGED:
                data->velocityX[index] = (data->velocityX[index] + hit_vel[0]) * 0.5f;
                data->velocityY[index] = (data->velocityY[index] + hit_vel[1]) * 0.5f;
                data->velocityZ[index] = (data->velocityZ[index] + hit_vel[2]) * 0.5f;
                break;
              case FLUID_EFFECTOR_GUIDING_OVERRIDE:
                data->velocityX[index] = hit_vel[0];
                data->velocityY[index] = hit_vel[1];
                data->velocityZ[index] = hit_vel[2];
                break;
              case FLUID_EFFECTOR_GUIDING_MINIMUM:
                data->velocityX[index] = MIN2(fabsf(hit_vel[0]), fabsf(data->velocityX[index]));
                data->velocityY[index] = MIN2(fabsf(hit_vel[1]), fabsf(data->velocityY[index]));
                data->velocityZ[index] = MIN2(fabsf(hit_vel[2]), fabsf(data->velocityZ[index]));
                break;
              case FLUID_EFFECTOR_GUIDING_MAXIMUM:
              default:
                data->velocityX[index] = MAX2(fabsf(hit_vel[0]), fabsf(data->velocityX[index]));
                data->velocityY[index] = MAX2(fabsf(hit_vel[1]), fabsf(data->velocityY[index]));
                data->velocityZ[index] = MAX2(fabsf(hit_vel[2]), fabsf(data->velocityZ[index]));
                break;
            }
          }
          else {
            /* Apply (i.e. add) effector object velocity */
            data->velocityX[index] += (data->mcs->type == FLUID_EFFECTOR_TYPE_GUIDE) ?
                                          hit_vel[0] * data->mcs->vel_multi :
                                          hit_vel[0];
            data->velocityY[index] += (data->mcs->type == FLUID_EFFECTOR_TYPE_GUIDE) ?
                                          hit_vel[1] * data->mcs->vel_multi :
                                          hit_vel[1];
            data->velocityZ[index] += (data->mcs->type == FLUID_EFFECTOR_TYPE_GUIDE) ?
                                          hit_vel[2] * data->mcs->vel_multi :
                                          hit_vel[2];
            //printf("adding effector object vel: [%f, %f, %f], dx is: %f\n", hit_vel[0], hit_vel[1], hit_vel[2], mds->dx);
          }
        }
      }

      /* Get distance to mesh surface from both within and outside grid (mantaflow phi grid) */
      if (data->distances_map) {
        update_mesh_distances(
            index, data->distances_map, data->tree, ray_start, data->mcs->surface_distance);

        /* Ensure that num objects are also counted inside object. But dont count twice (see object inc for nearest point) */
        if (data->distances_map[index] < 0 && !hasIncObj) {
          data->num_objects[index]++;
        }
      }
    }
  }
}

static void obstacles_from_mesh(Object *coll_ob,
                                MantaDomainSettings *mds,
                                MantaCollSettings *mcs,
                                float *distances_map,
                                float *velocityX,
                                float *velocityY,
                                float *velocityZ,
                                int *num_objects,
                                float dt)
{
  if (!mcs->mesh) {
    return;
  }
  {
    Mesh *me = NULL;
    MVert *mvert = NULL;
    const MLoopTri *looptri;
    const MLoop *mloop;
    BVHTreeFromMesh treeData = {NULL};
    int numverts, i;

    float *vert_vel = NULL;
    bool has_velocity = false;

    me = BKE_mesh_copy_for_eval(mcs->mesh, true);

    /* Duplicate vertices to modify. */
    if (me->mvert) {
      me->mvert = MEM_dupallocN(me->mvert);
      CustomData_set_layer(&me->vdata, CD_MVERT, me->mvert);
    }

    BKE_mesh_ensure_normals(me);
    mvert = me->mvert;
    mloop = me->mloop;
    looptri = BKE_mesh_runtime_looptri_ensure(me);
    numverts = me->totvert;

    /* TODO (sebbas):
     * Make vert_vel init optional?
     * code is in trouble if the object moves but is declared as "does not move" */
    {
      vert_vel = MEM_callocN(sizeof(float) * numverts * 3, "manta_obs_velocity");

      if (mcs->numverts != numverts || !mcs->verts_old) {
        if (mcs->verts_old) {
          MEM_freeN(mcs->verts_old);
        }

        mcs->verts_old = MEM_callocN(sizeof(float) * numverts * 3, "manta_obs_verts_old");
        mcs->numverts = numverts;
      }
      else {
        has_velocity = true;
      }
    }

    /*  Transform collider vertices to
     *   domain grid space for fast lookups */
    for (i = 0; i < numverts; i++) {
      float n[3];
      float co[3];

      /* vert pos */
      mul_m4_v3(coll_ob->obmat, mvert[i].co);
      manta_pos_to_cell(mds, mvert[i].co);

      /* vert normal */
      normal_short_to_float_v3(n, mvert[i].no);
      mul_mat3_m4_v3(coll_ob->obmat, n);
      mul_mat3_m4_v3(mds->imat, n);
      normalize_v3(n);
      normal_float_to_short_v3(mvert[i].no, n);

      /* vert velocity */
      add_v3fl_v3fl_v3i(co, mvert[i].co, mds->shift);
      if (has_velocity) {
        sub_v3_v3v3(&vert_vel[i * 3], co, &mcs->verts_old[i * 3]);
        mul_v3_fl(&vert_vel[i * 3], mds->dx / dt);
      }
      copy_v3_v3(&mcs->verts_old[i * 3], co);
    }

    if (BKE_bvhtree_from_mesh_get(&treeData, me, BVHTREE_FROM_LOOPTRI, 4)) {
      ObstaclesFromDMData data = {.mds = mds,
                                  .mcs = mcs,
                                  .mvert = mvert,
                                  .mloop = mloop,
                                  .looptri = looptri,
                                  .tree = &treeData,
                                  .has_velocity = has_velocity,
                                  .vert_vel = vert_vel,
                                  .velocityX = velocityX,
                                  .velocityY = velocityY,
                                  .velocityZ = velocityZ,
                                  .num_objects = num_objects,
                                  .distances_map = distances_map};
      TaskParallelSettings settings;
      BLI_parallel_range_settings_defaults(&settings);
      settings.scheduling_mode = TASK_SCHEDULING_DYNAMIC;
      BLI_task_parallel_range(
          mds->res_min[2], mds->res_max[2], &data, obstacles_from_mesh_task_cb, &settings);
    }
    /* free bvh tree */
    free_bvhtree_from_mesh(&treeData);
    BKE_id_free(NULL, me);

    if (vert_vel) {
      MEM_freeN(vert_vel);
    }
    if (me->mvert) {
      MEM_freeN(me->mvert);
    }
  }
}

static void update_obstacleflags(MantaDomainSettings *mds, Object **collobjs, int numcollobj)
{
  int active_fields = mds->active_fields;
  unsigned int collIndex;

  /* Monitor active fields based on flow settings */
  for (collIndex = 0; collIndex < numcollobj; collIndex++) {
    Object *collob = collobjs[collIndex];
    MantaModifierData *mmd2 = (MantaModifierData *)modifiers_findByType(collob,
                                                                        eModifierType_Manta);

    if ((mmd2->type & MOD_MANTA_TYPE_EFFEC) && mmd2->effec) {
      MantaCollSettings *mcs = mmd2->effec;
      if (!mcs) {
        break;
      }
      if (mcs->type == FLUID_EFFECTOR_TYPE_COLLISION) {
        active_fields |= FLUID_DOMAIN_ACTIVE_OBSTACLE;
      }
      if (mcs->type == FLUID_EFFECTOR_TYPE_GUIDE) {
        active_fields |= FLUID_DOMAIN_ACTIVE_GUIDING;
      }
    }
  }
  /* Finally, initialize new data fields if any */
  if (active_fields & FLUID_DOMAIN_ACTIVE_OBSTACLE) {
    manta_ensure_obstacle(mds->fluid, mds->mmd);
  }
  if (active_fields & FLUID_DOMAIN_ACTIVE_GUIDING) {
    manta_ensure_guiding(mds->fluid, mds->mmd);
  }
  mds->active_fields = active_fields;
}

/* Animated obstacles: dx_step = ((x_new - x_old) / totalsteps) * substep */
static void update_obstacles(Depsgraph *depsgraph,
                             Scene *scene,
                             Object *ob,
                             MantaDomainSettings *mds,
                             float time_per_frame,
                             float frame_length,
                             int frame,
                             float dt)
{
  Object **collobjs = NULL;
  unsigned int numcollobj = 0, collIndex = 0;

  collobjs = BKE_collision_objects_create(
      depsgraph, ob, mds->coll_group, &numcollobj, eModifierType_Manta);

  /* Update all flow related flags and ensure that corresponding grids get initialized */
  update_obstacleflags(mds, collobjs, numcollobj);

  float *velx = manta_get_ob_velocity_x(mds->fluid);
  float *vely = manta_get_ob_velocity_y(mds->fluid);
  float *velz = manta_get_ob_velocity_z(mds->fluid);
  float *velxGuide = manta_get_guide_velocity_x(mds->fluid);
  float *velyGuide = manta_get_guide_velocity_y(mds->fluid);
  float *velzGuide = manta_get_guide_velocity_z(mds->fluid);
  float *velxOrig = manta_get_velocity_x(mds->fluid);
  float *velyOrig = manta_get_velocity_y(mds->fluid);
  float *velzOrig = manta_get_velocity_z(mds->fluid);
  float *density = manta_smoke_get_density(mds->fluid);
  float *fuel = manta_smoke_get_fuel(mds->fluid);
  float *flame = manta_smoke_get_flame(mds->fluid);
  float *r = manta_smoke_get_color_r(mds->fluid);
  float *g = manta_smoke_get_color_g(mds->fluid);
  float *b = manta_smoke_get_color_b(mds->fluid);
  float *phiObsIn = manta_get_phiobs_in(mds->fluid);
  float *phiGuideIn = manta_get_phiguide_in(mds->fluid);
  int *obstacles = manta_smoke_get_obstacle(mds->fluid);
  int *num_obstacles = manta_get_num_obstacle(mds->fluid);
  int *num_guides = manta_get_num_guide(mds->fluid);
  unsigned int z;

  /* Grid reset before writing again */
  for (z = 0; z < mds->res[0] * mds->res[1] * mds->res[2]; z++) {
    if (phiObsIn) {
      phiObsIn[z] = 9999;
    }
    if (phiGuideIn) {
      phiGuideIn[z] = 9999;
    }
    if (num_obstacles) {
      num_obstacles[z] = 0;
    }
    if (num_guides) {
      num_guides[z] = 0;
    }

    if (velx && vely && velz) {
      velx[z] = 0.0f;
      vely[z] = 0.0f;
      velz[z] = 0.0f;
    }
    if (velxGuide && velyGuide && velzGuide) {
      velxGuide[z] = 0.0f;
      velyGuide[z] = 0.0f;
      velzGuide[z] = 0.0f;
    }
  }

  /* Prepare grids from effector objects */
  for (collIndex = 0; collIndex < numcollobj; collIndex++) {
    Object *collob = collobjs[collIndex];
    MantaModifierData *mmd2 = (MantaModifierData *)modifiers_findByType(collob,
                                                                        eModifierType_Manta);

    // DG TODO: check if modifier is active?
    if ((mmd2->type & MOD_MANTA_TYPE_EFFEC) && mmd2->effec) {
      MantaCollSettings *mcs = mmd2->effec;

      /* Length of one frame. If using adaptive stepping, length is smaller than actual frame length */
      float adaptframe_length = time_per_frame / frame_length;

      /* Handle adaptive subframe (ie has subframe fraction). Need to set according scene subframe parameter */
      if (time_per_frame < frame_length) {
        scene->r.subframe = adaptframe_length;
        scene->r.cfra = frame - 1;
      }
      /* Handle absolute endframe (ie no subframe fraction). Need to set the scene subframe parameter to 0 and advance current scene frame */
      else {
        scene->r.subframe = 0.0f;
        scene->r.cfra = frame;
      }
      //printf("effector: frame: %d // scene current frame: %d // scene current subframe: %f\n", frame, scene->r.cfra, scene->r.subframe);

      /* TODO (sebbas): Using BKE_scene_frame_get(scene) instead of new DEG_get_ctime(depsgraph) as subframes dont work with the latter yet */
      BKE_object_modifier_update_subframe(
          depsgraph, scene, collob, true, 5, BKE_scene_frame_get(scene), eModifierType_Manta);

      if (mcs && (mcs->type == FLUID_EFFECTOR_TYPE_COLLISION)) {
        obstacles_from_mesh(collob, mds, mcs, phiObsIn, velx, vely, velz, num_obstacles, dt);
      }
      if (mcs && (mcs->type == FLUID_EFFECTOR_TYPE_GUIDE)) {
        obstacles_from_mesh(
            collob, mds, mcs, phiGuideIn, velxGuide, velyGuide, velzGuide, num_guides, dt);
      }
    }
  }

  BKE_collision_objects_free(collobjs);

  /* obstacle cells should not contain any velocity from the smoke simulation */
  for (z = 0; z < mds->res[0] * mds->res[1] * mds->res[2]; z++) {
    if (obstacles[z] & 2)  // mantaflow convention: FlagObstacle
    {
      if (velxOrig && velyOrig && velzOrig) {
        velxOrig[z] = 0;
        velyOrig[z] = 0;
        velzOrig[z] = 0;
      }
      if (density) {
        density[z] = 0;
      }
      if (fuel) {
        fuel[z] = 0;
        flame[z] = 0;
      }
      if (r) {
        r[z] = 0;
        g[z] = 0;
        b[z] = 0;
      }
    }
    /* average velocities from multiple obstacles in one cell */
    if (num_obstacles && num_obstacles[z]) {
      velx[z] /= num_obstacles[z];
      vely[z] /= num_obstacles[z];
      velz[z] /= num_obstacles[z];
    }
    /* average velocities from multiple guides in one cell */
    if (num_guides && num_guides[z]) {
      velxGuide[z] /= num_guides[z];
      velyGuide[z] /= num_guides[z];
      velzGuide[z] /= num_guides[z];
    }
  }
}

/**********************************************************
 * Flow emission code
 **********************************************************/

typedef struct EmissionMap {
  float *influence;
  float *influence_high;
  float *velocity;
  float *distances;
  float *distances_high;
  int min[3], max[3], res[3];
  int hmin[3], hmax[3], hres[3];
  int total_cells, valid;
} EmissionMap;

static void em_boundInsert(EmissionMap *em, float point[3])
{
  int i = 0;
  if (!em->valid) {
    for (; i < 3; i++) {
      em->min[i] = (int)floor(point[i]);
      em->max[i] = (int)ceil(point[i]);
    }
    em->valid = 1;
  }
  else {
    for (; i < 3; i++) {
      if (point[i] < em->min[i]) {
        em->min[i] = (int)floor(point[i]);
      }
      if (point[i] > em->max[i]) {
        em->max[i] = (int)ceil(point[i]);
      }
    }
  }
}

static void clampBoundsInDomain(MantaDomainSettings *mds,
                                int min[3],
                                int max[3],
                                float *min_vel,
                                float *max_vel,
                                int margin,
                                float dt)
{
  int i;
  for (i = 0; i < 3; i++) {
    int adapt = (mds->flags & FLUID_DOMAIN_USE_ADAPTIVE_DOMAIN) ? mds->adapt_res : 0;
    /* add margin */
    min[i] -= margin;
    max[i] += margin;

    /* adapt to velocity */
    if (min_vel && min_vel[i] < 0.0f) {
      min[i] += (int)floor(min_vel[i] * dt);
    }
    if (max_vel && max_vel[i] > 0.0f) {
      max[i] += (int)ceil(max_vel[i] * dt);
    }

    /* clamp within domain max size */
    CLAMP(min[i], -adapt, mds->base_res[i] + adapt);
    CLAMP(max[i], -adapt, mds->base_res[i] + adapt);
  }
}

static void em_allocateData(EmissionMap *em, bool use_velocity, int hires_mul)
{
  int i, res[3];

  for (i = 0; i < 3; i++) {
    res[i] = em->max[i] - em->min[i];
    if (res[i] <= 0) {
      return;
    }
  }
  em->total_cells = res[0] * res[1] * res[2];
  copy_v3_v3_int(em->res, res);

  em->influence = MEM_callocN(sizeof(float) * em->total_cells, "manta_flow_influence");
  if (use_velocity) {
    em->velocity = MEM_callocN(sizeof(float) * em->total_cells * 3, "manta_flow_velocity");
  }

  em->distances = MEM_callocN(sizeof(float) * em->total_cells, "fluid_flow_distances");
  memset(em->distances, 0x7f7f7f7f, sizeof(float) * em->total_cells);  // init to inf

  /* allocate high resolution map if required */
  if (hires_mul > 1) {
    int total_cells_high = em->total_cells * (hires_mul * hires_mul * hires_mul);

    for (i = 0; i < 3; i++) {
      em->hmin[i] = em->min[i] * hires_mul;
      em->hmax[i] = em->max[i] * hires_mul;
      em->hres[i] = em->res[i] * hires_mul;
    }

    em->influence_high = MEM_callocN(sizeof(float) * total_cells_high,
                                     "manta_flow_influence_high");
    em->distances_high = MEM_callocN(sizeof(float) * total_cells_high,
                                     "manta_flow_distances_high");
    memset(em->distances_high, 0x7f7f7f7f, sizeof(float) * total_cells_high);  // init to inf
  }
  em->valid = 1;
}

static void em_freeData(EmissionMap *em)
{
  if (em->influence) {
    MEM_freeN(em->influence);
  }
  if (em->influence_high) {
    MEM_freeN(em->influence_high);
  }
  if (em->velocity) {
    MEM_freeN(em->velocity);
  }
  if (em->distances) {
    MEM_freeN(em->distances);
  }
  if (em->distances_high) {
    MEM_freeN(em->distances_high);
  }
}

static void em_combineMaps(
    EmissionMap *output, EmissionMap *em2, int hires_multiplier, int additive, float sample_size)
{
  int i, x, y, z;

  /* copyfill input 1 struct and clear output for new allocation */
  EmissionMap em1;
  memcpy(&em1, output, sizeof(EmissionMap));
  memset(output, 0, sizeof(EmissionMap));

  for (i = 0; i < 3; i++) {
    if (em1.valid) {
      output->min[i] = MIN2(em1.min[i], em2->min[i]);
      output->max[i] = MAX2(em1.max[i], em2->max[i]);
    }
    else {
      output->min[i] = em2->min[i];
      output->max[i] = em2->max[i];
    }
  }
  /* allocate output map */
  em_allocateData(output, (em1.velocity || em2->velocity), hires_multiplier);

  /* base resolution inputs */
  for (x = output->min[0]; x < output->max[0]; x++) {
    for (y = output->min[1]; y < output->max[1]; y++) {
      for (z = output->min[2]; z < output->max[2]; z++) {
        int index_out = manta_get_index(x - output->min[0],
                                        output->res[0],
                                        y - output->min[1],
                                        output->res[1],
                                        z - output->min[2]);

        /* initialize with first input if in range */
        if (x >= em1.min[0] && x < em1.max[0] && y >= em1.min[1] && y < em1.max[1] &&
            z >= em1.min[2] && z < em1.max[2]) {
          int index_in = manta_get_index(
              x - em1.min[0], em1.res[0], y - em1.min[1], em1.res[1], z - em1.min[2]);

          /* values */
          output->influence[index_out] = em1.influence[index_in];
          output->distances[index_out] = em1.distances[index_in];
          if (output->velocity && em1.velocity) {
            copy_v3_v3(&output->velocity[index_out * 3], &em1.velocity[index_in * 3]);
          }
        }

        /* apply second input if in range */
        if (x >= em2->min[0] && x < em2->max[0] && y >= em2->min[1] && y < em2->max[1] &&
            z >= em2->min[2] && z < em2->max[2]) {
          int index_in = manta_get_index(
              x - em2->min[0], em2->res[0], y - em2->min[1], em2->res[1], z - em2->min[2]);

          /* values */
          if (additive) {
            output->influence[index_out] += em2->influence[index_in] * sample_size;
          }
          else {
            output->influence[index_out] = MAX2(em2->influence[index_in],
                                                output->influence[index_out]);
          }
          output->distances[index_out] = MIN2(em2->distances[index_in],
                                              output->distances[index_out]);
          if (output->velocity && em2->velocity) {
            /* last sample replaces the velocity */
            output->velocity[index_out * 3] = ADD_IF_LOWER(output->velocity[index_out * 3],
                                                           em2->velocity[index_in * 3]);
            output->velocity[index_out * 3 + 1] = ADD_IF_LOWER(output->velocity[index_out * 3 + 1],
                                                               em2->velocity[index_in * 3 + 1]);
            output->velocity[index_out * 3 + 2] = ADD_IF_LOWER(output->velocity[index_out * 3 + 2],
                                                               em2->velocity[index_in * 3 + 2]);
          }
        }
      }  // low res loop
    }
  }

  /* initialize high resolution input if available */
  if (output->influence_high) {
    for (x = output->hmin[0]; x < output->hmax[0]; x++) {
      for (y = output->hmin[1]; y < output->hmax[1]; y++) {
        for (z = output->hmin[2]; z < output->hmax[2]; z++) {
          int index_out = manta_get_index(x - output->hmin[0],
                                          output->hres[0],
                                          y - output->hmin[1],
                                          output->hres[1],
                                          z - output->hmin[2]);

          /* initialize with first input if in range */
          if (x >= em1.hmin[0] && x < em1.hmax[0] && y >= em1.hmin[1] && y < em1.hmax[1] &&
              z >= em1.hmin[2] && z < em1.hmax[2]) {
            int index_in = manta_get_index(
                x - em1.hmin[0], em1.hres[0], y - em1.hmin[1], em1.hres[1], z - em1.hmin[2]);
            /* values */
            output->influence_high[index_out] = em1.influence_high[index_in];
          }

          /* apply second input if in range */
          if (x >= em2->hmin[0] && x < em2->hmax[0] && y >= em2->hmin[1] && y < em2->hmax[1] &&
              z >= em2->hmin[2] && z < em2->hmax[2]) {
            int index_in = manta_get_index(
                x - em2->hmin[0], em2->hres[0], y - em2->hmin[1], em2->hres[1], z - em2->hmin[2]);

            /* values */
            if (additive) {
              output->influence_high[index_out] += em2->distances_high[index_in] * sample_size;
            }
            else {
              output->distances_high[index_out] = MAX2(em2->distances_high[index_in],
                                                       output->distances_high[index_out]);
            }
            output->distances_high[index_out] = MIN2(em2->distances_high[index_in],
                                                     output->distances_high[index_out]);
          }
        }  // high res loop
      }
    }
  }

  /* free original data */
  em_freeData(&em1);
}

typedef struct EmitFromParticlesData {
  MantaFlowSettings *mfs;
  KDTree_3d *tree;
  int hires_multiplier;

  EmissionMap *em;
  float *particle_vel;
  float hr;

  int *min, *max, *res;

  float solid;
  float smooth;
  float hr_smooth;
} EmitFromParticlesData;

static void emit_from_particles_task_cb(void *__restrict userdata,
                                        const int z,
                                        const TaskParallelTLS *__restrict UNUSED(tls))
{
  EmitFromParticlesData *data = userdata;
  MantaFlowSettings *mfs = data->mfs;
  EmissionMap *em = data->em;
  const int hires_multiplier = data->hires_multiplier;

  for (int x = data->min[0]; x < data->max[0]; x++) {
    for (int y = data->min[1]; y < data->max[1]; y++) {
      /* take low res samples where possible */
      if (hires_multiplier <= 1 ||
          !(x % hires_multiplier || y % hires_multiplier || z % hires_multiplier)) {
        /* get low res space coordinates */
        const int lx = x / hires_multiplier;
        const int ly = y / hires_multiplier;
        const int lz = z / hires_multiplier;

        const int index = manta_get_index(
            lx - em->min[0], em->res[0], ly - em->min[1], em->res[1], lz - em->min[2]);
        const float ray_start[3] = {((float)lx) + 0.5f, ((float)ly) + 0.5f, ((float)lz) + 0.5f};

        /* find particle distance from the kdtree */
        KDTreeNearest_3d nearest;
        const float range = data->solid + data->smooth;
        BLI_kdtree_3d_find_nearest(data->tree, ray_start, &nearest);

        if (nearest.dist < range) {
          em->influence[index] = (nearest.dist < data->solid) ?
                                     1.0f :
                                     (1.0f - (nearest.dist - data->solid) / data->smooth);
          /* Uses particle velocity as initial velocity for smoke */
          if (mfs->flags & FLUID_FLOW_INITVELOCITY &&
              (mfs->psys->part->phystype != PART_PHYS_NO)) {
            madd_v3_v3fl(
                &em->velocity[index * 3], &data->particle_vel[nearest.index * 3], mfs->vel_multi);
          }
        }
      }

      /* take high res samples if required */
      if (hires_multiplier > 1) {
        /* get low res space coordinates */
        const float lx = ((float)x) * data->hr;
        const float ly = ((float)y) * data->hr;
        const float lz = ((float)z) * data->hr;

        const int index = manta_get_index(
            x - data->min[0], data->res[0], y - data->min[1], data->res[1], z - data->min[2]);
        const float ray_start[3] = {
            lx + 0.5f * data->hr, ly + 0.5f * data->hr, lz + 0.5f * data->hr};

        /* find particle distance from the kdtree */
        KDTreeNearest_3d nearest;
        const float range = data->solid + data->hr_smooth;
        BLI_kdtree_3d_find_nearest(data->tree, ray_start, &nearest);

        if (nearest.dist < range) {
          em->influence_high[index] = (nearest.dist < data->solid) ?
                                          1.0f :
                                          (1.0f - (nearest.dist - data->solid) / data->smooth);
        }
      }
    }
  }
}

static void emit_from_particles(Object *flow_ob,
                                MantaDomainSettings *mds,
                                MantaFlowSettings *mfs,
                                EmissionMap *em,
                                Depsgraph *depsgraph,
                                Scene *scene,
                                float dt)
{
  if (mfs && mfs->psys && mfs->psys->part &&
      ELEM(mfs->psys->part->type, PART_EMITTER, PART_FLUID))  // is particle system selected
  {
    ParticleSimulationData sim;
    ParticleSystem *psys = mfs->psys;
    float *particle_pos;
    float *particle_vel;
    int totpart = psys->totpart, totchild;
    int p = 0;
    int valid_particles = 0;
    int bounds_margin = 1;

    /* radius based flow */
    const float solid = mfs->particle_size * 0.5f;
    const float smooth = 0.5f; /* add 0.5 cells of linear falloff to reduce aliasing */
    int hires_multiplier = 1;
    KDTree_3d *tree = NULL;

    sim.depsgraph = depsgraph;
    sim.scene = scene;
    sim.ob = flow_ob;
    sim.psys = psys;
    sim.psys->lattice_deform_data = psys_create_lattice_deform_data(&sim);

    /* prepare curvemapping tables */
    if ((psys->part->child_flag & PART_CHILD_USE_CLUMP_CURVE) && psys->part->clumpcurve) {
      BKE_curvemapping_changed_all(psys->part->clumpcurve);
    }
    if ((psys->part->child_flag & PART_CHILD_USE_ROUGH_CURVE) && psys->part->roughcurve) {
      BKE_curvemapping_changed_all(psys->part->roughcurve);
    }
    if ((psys->part->child_flag & PART_CHILD_USE_TWIST_CURVE) && psys->part->twistcurve) {
      BKE_curvemapping_changed_all(psys->part->twistcurve);
    }

    /* initialize particle cache */
    if (psys->part->type == PART_HAIR) {
      // TODO: PART_HAIR not supported whatsoever
      totchild = 0;
    }
    else {
      totchild = psys->totchild * psys->part->disp / 100;
    }

    particle_pos = MEM_callocN(sizeof(float) * (totpart + totchild) * 3, "manta_flow_particles_pos");
    particle_vel = MEM_callocN(sizeof(float) * (totpart + totchild) * 3, "manta_flow_particles_vel");

    /* setup particle radius emission if enabled */
    if (mfs->flags & FLUID_FLOW_USE_PART_SIZE) {
      tree = BLI_kdtree_3d_new(psys->totpart + psys->totchild);

      /* check need for high resolution map */
      if ((mds->flags & FLUID_DOMAIN_USE_NOISE) && (mds->highres_sampling == SM_HRES_FULLSAMPLE)) {
        hires_multiplier = mds->noise_scale;
      }

      bounds_margin = (int)ceil(solid + smooth);
    }

    /* calculate local position for each particle */
    for (p = 0; p < totpart + totchild; p++) {
      ParticleKey state;
      float *pos, *vel;
      if (p < totpart) {
        if (psys->particles[p].flag & (PARS_NO_DISP | PARS_UNEXIST)) {
          continue;
        }
      }
      else {
        /* handle child particle */
        ChildParticle *cpa = &psys->child[p - totpart];
        if (psys->particles[cpa->parent].flag & (PARS_NO_DISP | PARS_UNEXIST)) {
          continue;
        }
      }

      state.time = BKE_scene_frame_get(scene); /* DEG_get_ctime(depsgraph) does not give subframe time */
      if (psys_get_particle_state(&sim, p, &state, 0) == 0) {
        continue;
      }

      /* location */
      pos = &particle_pos[valid_particles * 3];
      copy_v3_v3(pos, state.co);
      manta_pos_to_cell(mds, pos);

      /* velocity */
      vel = &particle_vel[valid_particles * 3];
      copy_v3_v3(vel, state.vel);
      mul_mat3_m4_v3(mds->imat, &particle_vel[valid_particles * 3]);

      if (mfs->flags & FLUID_FLOW_USE_PART_SIZE) {
        BLI_kdtree_3d_insert(tree, valid_particles, pos);
      }

      /* calculate emission map bounds */
      em_boundInsert(em, pos);
      valid_particles++;
    }

    /* set emission map */
    clampBoundsInDomain(mds, em->min, em->max, NULL, NULL, bounds_margin, dt);
    em_allocateData(em, mfs->flags & FLUID_FLOW_INITVELOCITY, hires_multiplier);

    if (!(mfs->flags & FLUID_FLOW_USE_PART_SIZE)) {
      for (p = 0; p < valid_particles; p++) {
        int cell[3];
        size_t i = 0;
        size_t index = 0;
        int badcell = 0;

        /* 1. get corresponding cell */
        cell[0] = floor(particle_pos[p * 3]) - em->min[0];
        cell[1] = floor(particle_pos[p * 3 + 1]) - em->min[1];
        cell[2] = floor(particle_pos[p * 3 + 2]) - em->min[2];
        /* check if cell is valid (in the domain boundary) */
        for (i = 0; i < 3; i++) {
          if ((cell[i] > em->res[i] - 1) || (cell[i] < 0)) {
            badcell = 1;
            break;
          }
        }
        if (badcell) {
          continue;
        }
        /* get cell index */
        index = manta_get_index(cell[0], em->res[0], cell[1], em->res[1], cell[2]);
        /* Add influence to emission map */
        em->influence[index] = 1.0f;
        /* Uses particle velocity as initial velocity for smoke */
        if (mfs->flags & FLUID_FLOW_INITVELOCITY && (psys->part->phystype != PART_PHYS_NO)) {
          madd_v3_v3fl(&em->velocity[index * 3], &particle_vel[p * 3], mfs->vel_multi);
        }
      }  // particles loop
    }
    else if (valid_particles > 0) {  // FLUID_FLOW_USE_PART_SIZE
      int min[3], max[3], res[3];
      const float hr = 1.0f / ((float)hires_multiplier);
      /* slightly adjust high res antialias smoothness based on number of divisions
       * to allow smaller details but yet not differing too much from the low res size */
      const float hr_smooth = smooth * powf(hr, 1.0f / 3.0f);

      /* setup loop bounds */
      for (int i = 0; i < 3; i++) {
        min[i] = em->min[i] * hires_multiplier;
        max[i] = em->max[i] * hires_multiplier;
        res[i] = em->res[i] * hires_multiplier;
      }

      BLI_kdtree_3d_balance(tree);

      EmitFromParticlesData data = {
          .mfs = mfs,
          .tree = tree,
          .hires_multiplier = hires_multiplier,
          .hr = hr,
          .em = em,
          .particle_vel = particle_vel,
          .min = min,
          .max = max,
          .res = res,
          .solid = solid,
          .smooth = smooth,
          .hr_smooth = hr_smooth,
      };

      TaskParallelSettings settings;
      BLI_parallel_range_settings_defaults(&settings);
      settings.scheduling_mode = TASK_SCHEDULING_DYNAMIC;
      BLI_task_parallel_range(min[2], max[2], &data, emit_from_particles_task_cb, &settings);
    }

    if (mfs->flags & FLUID_FLOW_USE_PART_SIZE) {
      BLI_kdtree_3d_free(tree);
    }

    /* free data */
    if (particle_pos) {
      MEM_freeN(particle_pos);
    }
    if (particle_vel) {
      MEM_freeN(particle_vel);
    }
  }
}

/* Calculate map of (minimum) distances to flow/obstacle surface. Distances outside mesh are positive, inside negative */
static void update_mesh_distances(int index,
                                  float *mesh_distances,
                                  BVHTreeFromMesh *treeData,
                                  const float ray_start[3],
                                  float surface_thickness)
{

  /* First pass: Raycasts in 26 directions (6 main axis + 12 quadrant diagonals (2D) + 8 octant diagonals (3D)) */
  float ray_dirs[26][3] = {
      {1.0f, 0.0f, 0.0f},   {0.0f, 1.0f, 0.0f},   {0.0f, 0.0f, 1.0f},  {-1.0f, 0.0f, 0.0f},
      {0.0f, -1.0f, 0.0f},  {0.0f, 0.0f, -1.0f},  {1.0f, 1.0f, 0.0f},  {1.0f, -1.0f, 0.0f},
      {-1.0f, 1.0f, 0.0f},  {-1.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 1.0f},  {1.0f, 0.0f, -1.0f},
      {-1.0f, 0.0f, 1.0f},  {-1.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 1.0f},  {0.0f, 1.0f, -1.0f},
      {0.0f, -1.0f, 1.0f},  {0.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f},  {1.0f, -1.0f, 1.0f},
      {-1.0f, 1.0f, 1.0f},  {-1.0f, -1.0f, 1.0f}, {1.0f, 1.0f, -1.0f}, {1.0f, -1.0f, -1.0f},
      {-1.0f, 1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f}};
  size_t ray_cnt = sizeof ray_dirs / sizeof ray_dirs[0];

  /* Count for ray misses (no face hit) and cases where ray direction matches face normal direction. */
  int miss_cnt = 0;
  int dir_cnt = 0;

  for (int i = 0; i < ray_cnt; i++) {
    BVHTreeRayHit hit_tree = {0};
    hit_tree.index = -1;
    hit_tree.dist = 9999;

    normalize_v3(ray_dirs[i]);
    BLI_bvhtree_ray_cast(treeData->tree,
                         ray_start,
                         ray_dirs[i],
                         0.0f,
                         &hit_tree,
                         treeData->raycast_callback,
                         treeData);

    /* Ray did not hit mesh. Current point definitely not inside mesh. Inside mesh all rays have to hit. */
    if (hit_tree.index == -1) {
      miss_cnt++;
      continue;
    }

    /* Ray and normal are in pointing opposite directions. */
    if (dot_v3v3(ray_dirs[i], hit_tree.no) <= 0) {
      dir_cnt++;
    }
  }

  /* Initialize grid points to -0.5 inside and 0.5 outside mesh.
   * Inside mesh: All rays have to hit (no misses) or all face normals have to match ray direction */
  if (mesh_distances[index] != -0.5f) {
    mesh_distances[index] = (miss_cnt > 0 || dir_cnt == ray_cnt) ? 0.5f : -0.5f;
  }

  /* Second pass: Ensure that single planes get initialized. */
  BVHTreeNearest nearest = {0};
  nearest.index = -1;
  nearest.dist_sq = surface_thickness * surface_thickness; /* find_nearest uses squared distance */

  if (BLI_bvhtree_find_nearest(
          treeData->tree, ray_start, &nearest, treeData->nearest_callback, treeData) != -1) {
    if (mesh_distances[index] != -0.5f) {
      mesh_distances[index] = -0.5f;
    }
  }
}

static void sample_mesh(MantaFlowSettings *mfs,
                        const MVert *mvert,
                        const MLoop *mloop,
                        const MLoopTri *mlooptri,
                        const MLoopUV *mloopuv,
                        float *influence_map,
                        float *velocity_map,
                        int index,
                        const int base_res[3],
                        float flow_center[3],
                        BVHTreeFromMesh *treeData,
                        const float ray_start[3],
                        const float *vert_vel,
                        bool has_velocity,
                        int defgrp_index,
                        MDeformVert *dvert,
                        float x,
                        float y,
                        float z)
{
  float ray_dir[3] = {1.0f, 0.0f, 0.0f};
  BVHTreeRayHit hit = {0};
  BVHTreeNearest nearest = {0};

  float volume_factor = 0.0f;
  float sample_str = 0.0f;

  hit.index = -1;
  hit.dist = 9999;
  nearest.index = -1;
  nearest.dist_sq = mfs->surface_distance *
                    mfs->surface_distance; /* find_nearest uses squared distance */

  /* Check volume collision */
  if (mfs->volume_density) {
    if (BLI_bvhtree_ray_cast(treeData->tree,
                             ray_start,
                             ray_dir,
                             0.0f,
                             &hit,
                             treeData->raycast_callback,
                             treeData) != -1) {
      float dot = ray_dir[0] * hit.no[0] + ray_dir[1] * hit.no[1] + ray_dir[2] * hit.no[2];
      /* If ray and hit face normal are facing same direction
       * hit point is inside a closed mesh. */
      if (dot >= 0) {
        /* Also cast a ray in opposite direction to make sure
         * point is at least surrounded by two faces */
        negate_v3(ray_dir);
        hit.index = -1;
        hit.dist = 9999;

        BLI_bvhtree_ray_cast(
            treeData->tree, ray_start, ray_dir, 0.0f, &hit, treeData->raycast_callback, treeData);
        if (hit.index != -1) {
          volume_factor = mfs->volume_density;
        }
      }
    }
  }

  /* find the nearest point on the mesh */
  if (BLI_bvhtree_find_nearest(
          treeData->tree, ray_start, &nearest, treeData->nearest_callback, treeData) != -1) {
    float weights[3];
    int v1, v2, v3, f_index = nearest.index;
    float n1[3], n2[3], n3[3], hit_normal[3];

    /* emit from surface based on distance */
    if (mfs->surface_distance) {
      sample_str = sqrtf(nearest.dist_sq) / mfs->surface_distance;
      CLAMP(sample_str, 0.0f, 1.0f);
      sample_str = pow(1.0f - sample_str, 0.5f);
    }
    else {
      sample_str = 0.0f;
    }

    /* calculate barycentric weights for nearest point */
    v1 = mloop[mlooptri[f_index].tri[0]].v;
    v2 = mloop[mlooptri[f_index].tri[1]].v;
    v3 = mloop[mlooptri[f_index].tri[2]].v;
    interp_weights_tri_v3(weights, mvert[v1].co, mvert[v2].co, mvert[v3].co, nearest.co);

    if (mfs->flags & FLUID_FLOW_INITVELOCITY && velocity_map) {
      /* apply normal directional velocity */
      if (mfs->vel_normal) {
        /* interpolate vertex normal vectors to get nearest point normal */
        normal_short_to_float_v3(n1, mvert[v1].no);
        normal_short_to_float_v3(n2, mvert[v2].no);
        normal_short_to_float_v3(n3, mvert[v3].no);
        interp_v3_v3v3v3(hit_normal, n1, n2, n3, weights);
        normalize_v3(hit_normal);
        /* apply normal directional and random velocity
         * - TODO: random disabled for now since it doesn't really work well
         *   as pressure calc smoothens it out. */
        velocity_map[index * 3] += hit_normal[0] * mfs->vel_normal * 0.25f;
        velocity_map[index * 3 + 1] += hit_normal[1] * mfs->vel_normal * 0.25f;
        velocity_map[index * 3 + 2] += hit_normal[2] * mfs->vel_normal * 0.25f;
        /* TODO: for fire emitted from mesh surface we can use
         * Vf = Vs + (Ps/Pf - 1)*S to model gaseous expansion from solid to fuel */
      }
      /* apply object velocity */
      if (has_velocity && mfs->vel_multi) {
        float hit_vel[3];
        interp_v3_v3v3v3(
            hit_vel, &vert_vel[v1 * 3], &vert_vel[v2 * 3], &vert_vel[v3 * 3], weights);
        velocity_map[index * 3] += hit_vel[0] * mfs->vel_multi;
        velocity_map[index * 3 + 1] += hit_vel[1] * mfs->vel_multi;
        velocity_map[index * 3 + 2] += hit_vel[2] * mfs->vel_multi;
        //printf("adding flow object vel: [%f, %f, %f]\n", hit_vel[0], hit_vel[1], hit_vel[2]);
      }
      velocity_map[index * 3] += mfs->vel_coord[0];
      velocity_map[index * 3 + 1] += mfs->vel_coord[1];
      velocity_map[index * 3 + 2] += mfs->vel_coord[2];
    }

    /* apply vertex group influence if used */
    if (defgrp_index != -1 && dvert) {
      float weight_mask = defvert_find_weight(&dvert[v1], defgrp_index) * weights[0] +
                          defvert_find_weight(&dvert[v2], defgrp_index) * weights[1] +
                          defvert_find_weight(&dvert[v3], defgrp_index) * weights[2];
      sample_str *= weight_mask;
    }

    /* apply emission texture */
    if ((mfs->flags & FLUID_FLOW_TEXTUREEMIT) && mfs->noise_texture) {
      float tex_co[3] = {0};
      TexResult texres;

      if (mfs->texture_type == FLUID_FLOW_TEXTURE_MAP_AUTO) {
        tex_co[0] = ((x - flow_center[0]) / base_res[0]) / mfs->texture_size;
        tex_co[1] = ((y - flow_center[1]) / base_res[1]) / mfs->texture_size;
        tex_co[2] = ((z - flow_center[2]) / base_res[2] - mfs->texture_offset) / mfs->texture_size;
      }
      else if (mloopuv) {
        const float *uv[3];
        uv[0] = mloopuv[mlooptri[f_index].tri[0]].uv;
        uv[1] = mloopuv[mlooptri[f_index].tri[1]].uv;
        uv[2] = mloopuv[mlooptri[f_index].tri[2]].uv;

        interp_v2_v2v2v2(tex_co, UNPACK3(uv), weights);

        /* map between -1.0f and 1.0f */
        tex_co[0] = tex_co[0] * 2.0f - 1.0f;
        tex_co[1] = tex_co[1] * 2.0f - 1.0f;
        tex_co[2] = mfs->texture_offset;
      }
      texres.nor = NULL;
      BKE_texture_get_value(NULL, mfs->noise_texture, tex_co, &texres, false);
      sample_str *= texres.tin;
    }
  }

  /* multiply initial velocity by emitter influence */
  if (mfs->flags & FLUID_FLOW_INITVELOCITY && velocity_map) {
    mul_v3_fl(&velocity_map[index * 3], sample_str);
  }

  /* apply final influence based on volume factor */
  influence_map[index] = MAX2(volume_factor, sample_str);
}

typedef struct EmitFromDMData {
  MantaDomainSettings *mds;
  MantaFlowSettings *mfs;
  const MVert *mvert;
  const MLoop *mloop;
  const MLoopTri *mlooptri;
  const MLoopUV *mloopuv;
  MDeformVert *dvert;
  int defgrp_index;

  BVHTreeFromMesh *tree;
  int hires_multiplier;
  float hr;

  EmissionMap *em;
  bool has_velocity;
  float *vert_vel;

  float *flow_center;
  int *min, *max, *res;
} EmitFromDMData;

static void emit_from_mesh_task_cb(void *__restrict userdata,
                                   const int z,
                                   const TaskParallelTLS *__restrict UNUSED(tls))
{
  EmitFromDMData *data = userdata;
  EmissionMap *em = data->em;
  const int hires_multiplier = data->hires_multiplier;

  for (int x = data->min[0]; x < data->max[0]; x++) {
    for (int y = data->min[1]; y < data->max[1]; y++) {
      /* take low res samples where possible */
      if (hires_multiplier <= 1 ||
          !(x % hires_multiplier || y % hires_multiplier || z % hires_multiplier)) {
        /* get low res space coordinates */
        const int lx = x / hires_multiplier;
        const int ly = y / hires_multiplier;
        const int lz = z / hires_multiplier;

        const int index = manta_get_index(
            lx - em->min[0], em->res[0], ly - em->min[1], em->res[1], lz - em->min[2]);
        const float ray_start[3] = {((float)lx) + 0.5f, ((float)ly) + 0.5f, ((float)lz) + 0.5f};

        /* Emission for smoke and fire. Result in em->influence. Also, calculate invels */
        sample_mesh(data->mfs,
                    data->mvert,
                    data->mloop,
                    data->mlooptri,
                    data->mloopuv,
                    em->influence,
                    em->velocity,
                    index,
                    data->mds->base_res,
                    data->flow_center,
                    data->tree,
                    ray_start,
                    data->vert_vel,
                    data->has_velocity,
                    data->defgrp_index,
                    data->dvert,
                    (float)lx,
                    (float)ly,
                    (float)lz);

        /* Calculate levelset from meshes. Result in em->distances */
        update_mesh_distances(
            index, em->distances, data->tree, ray_start, data->mfs->surface_distance);
      }

      /* take high res samples if required */
      if (hires_multiplier > 1) {
        /* get low res space coordinates */
        const float lx = ((float)x) * data->hr;
        const float ly = ((float)y) * data->hr;
        const float lz = ((float)z) * data->hr;

        const int index = manta_get_index(
            x - data->min[0], data->res[0], y - data->min[1], data->res[1], z - data->min[2]);
        const float ray_start[3] = {
            lx + 0.5f * data->hr,
            ly + 0.5f * data->hr,
            lz + 0.5f * data->hr,
        };

        /* Emission for smoke and fire high. Result in em->influence_high */
        if (data->mfs->type == FLUID_FLOW_TYPE_SMOKE || data->mfs->type == FLUID_FLOW_TYPE_FIRE ||
            data->mfs->type == FLUID_FLOW_TYPE_SMOKEFIRE) {
          sample_mesh(data->mfs,
                      data->mvert,
                      data->mloop,
                      data->mlooptri,
                      data->mloopuv,
                      em->influence_high,
                      NULL,
                      index,
                      data->mds->base_res,
                      data->flow_center,
                      data->tree,
                      ray_start,
                      data->vert_vel,
                      data->has_velocity,
                      data->defgrp_index,
                      data->dvert,
                      /* x,y,z needs to be always lowres */
                      lx,
                      ly,
                      lz);
        }
      }
    }
  }
}

static void emit_from_mesh(
    Object *flow_ob, MantaDomainSettings *mds, MantaFlowSettings *mfs, EmissionMap *em, float dt)
{
  if (mfs->mesh) {
    Mesh *me = NULL;
    MVert *mvert = NULL;
    const MLoopTri *mlooptri = NULL;
    const MLoop *mloop = NULL;
    const MLoopUV *mloopuv = NULL;
    MDeformVert *dvert = NULL;
    BVHTreeFromMesh treeData = {NULL};
    int numverts, i;

    float *vert_vel = NULL;
    bool has_velocity = false;

    int defgrp_index = mfs->vgroup_density - 1;
    float flow_center[3] = {0};
    int min[3], max[3], res[3];
    int hires_multiplier = 1;

    /* copy mesh for thread safety because we modify it,
     * main issue is its VertArray being modified, then replaced and freed
     */
    me = BKE_mesh_copy_for_eval(mfs->mesh, true);

    /* Duplicate vertices to modify. */
    if (me->mvert) {
      me->mvert = MEM_dupallocN(me->mvert);
      CustomData_set_layer(&me->vdata, CD_MVERT, me->mvert);
    }

    BKE_mesh_ensure_normals(me);
    mvert = me->mvert;
    mloop = me->mloop;
    mlooptri = BKE_mesh_runtime_looptri_ensure(me);
    numverts = me->totvert;
    dvert = CustomData_get_layer(&me->vdata, CD_MDEFORMVERT);
    mloopuv = CustomData_get_layer_named(&me->ldata, CD_MLOOPUV, mfs->uvlayer_name);

    if (mfs->flags & FLUID_FLOW_INITVELOCITY) {
      vert_vel = MEM_callocN(sizeof(float) * numverts * 3, "manta_flow_velocity");

      if (mfs->numverts != numverts || !mfs->verts_old) {
        if (mfs->verts_old) {
          MEM_freeN(mfs->verts_old);
        }
        mfs->verts_old = MEM_callocN(sizeof(float) * numverts * 3, "manta_flow_verts_old");
        mfs->numverts = numverts;
      }
      else {
        has_velocity = true;
      }
    }

    /*  Transform mesh vertices to
     *   domain grid space for fast lookups */
    for (i = 0; i < numverts; i++) {
      float n[3];

      /* vert pos */
      mul_m4_v3(flow_ob->obmat, mvert[i].co);
      manta_pos_to_cell(mds, mvert[i].co);

      /* vert normal */
      normal_short_to_float_v3(n, mvert[i].no);
      mul_mat3_m4_v3(flow_ob->obmat, n);
      mul_mat3_m4_v3(mds->imat, n);
      normalize_v3(n);
      normal_float_to_short_v3(mvert[i].no, n);

      /* vert velocity */
      if (mfs->flags & FLUID_FLOW_INITVELOCITY) {
        float co[3];
        add_v3fl_v3fl_v3i(co, mvert[i].co, mds->shift);
        if (has_velocity) {
          sub_v3_v3v3(&vert_vel[i * 3], co, &mfs->verts_old[i * 3]);
          mul_v3_fl(&vert_vel[i * 3], mds->dx / dt);
        }
        copy_v3_v3(&mfs->verts_old[i * 3], co);
      }

      /* calculate emission map bounds */
      em_boundInsert(em, mvert[i].co);
    }
    mul_m4_v3(flow_ob->obmat, flow_center);
    manta_pos_to_cell(mds, flow_center);

    /* check need for high resolution map */
    if ((mds->flags & FLUID_DOMAIN_USE_NOISE) && (mds->highres_sampling == SM_HRES_FULLSAMPLE)) {
      hires_multiplier = mds->noise_scale;
    }

    /* set emission map */
    clampBoundsInDomain(mds, em->min, em->max, NULL, NULL, (int)ceil(mfs->surface_distance), dt);
    em_allocateData(em, mfs->flags & FLUID_FLOW_INITVELOCITY, hires_multiplier);

    /* setup loop bounds */
    for (i = 0; i < 3; i++) {
      min[i] = em->min[i] * hires_multiplier;
      max[i] = em->max[i] * hires_multiplier;
      res[i] = em->res[i] * hires_multiplier;
    }

    if (BKE_bvhtree_from_mesh_get(&treeData, me, BVHTREE_FROM_LOOPTRI, 4)) {
      const float hr = 1.0f / ((float)hires_multiplier);

      EmitFromDMData data = {
          .mds = mds,
          .mfs = mfs,
          .mvert = mvert,
          .mloop = mloop,
          .mlooptri = mlooptri,
          .mloopuv = mloopuv,
          .dvert = dvert,
          .defgrp_index = defgrp_index,
          .tree = &treeData,
          .hires_multiplier = hires_multiplier,
          .hr = hr,
          .em = em,
          .has_velocity = has_velocity,
          .vert_vel = vert_vel,
          .flow_center = flow_center,
          .min = min,
          .max = max,
          .res = res,
      };

      TaskParallelSettings settings;
      BLI_parallel_range_settings_defaults(&settings);
      settings.scheduling_mode = TASK_SCHEDULING_DYNAMIC;
      BLI_task_parallel_range(min[2], max[2], &data, emit_from_mesh_task_cb, &settings);
    }
    /* free bvh tree */
    free_bvhtree_from_mesh(&treeData);

    if (vert_vel) {
      MEM_freeN(vert_vel);
    }

    if (me->mvert) {
      MEM_freeN(me->mvert);
    }
    BKE_id_free(NULL, me);
  }
}

/**********************************************************
 *  Smoke step
 **********************************************************/

static void adaptiveDomainAdjust(MantaDomainSettings *mds,
                                 Object *ob,
                                 EmissionMap *emaps,
                                 unsigned int numflowobj,
                                 float dt)
{
  /* calculate domain shift for current frame */
  int new_shift[3] = {0};
  int total_shift[3];
  float frame_shift_f[3];
  float ob_loc[3] = {0};

  mul_m4_v3(ob->obmat, ob_loc);

  sub_v3_v3v3(frame_shift_f, ob_loc, mds->prev_loc);
  copy_v3_v3(mds->prev_loc, ob_loc);
  /* convert global space shift to local "cell" space */
  mul_mat3_m4_v3(mds->imat, frame_shift_f);
  frame_shift_f[0] = frame_shift_f[0] / mds->cell_size[0];
  frame_shift_f[1] = frame_shift_f[1] / mds->cell_size[1];
  frame_shift_f[2] = frame_shift_f[2] / mds->cell_size[2];
  /* add to total shift */
  add_v3_v3(mds->shift_f, frame_shift_f);
  /* convert to integer */
  total_shift[0] = (int)(floorf(mds->shift_f[0]));
  total_shift[1] = (int)(floorf(mds->shift_f[1]));
  total_shift[2] = (int)(floorf(mds->shift_f[2]));
  int tmpShift[3];
  copy_v3_v3_int(tmpShift, mds->shift);
  sub_v3_v3v3_int(new_shift, total_shift, mds->shift);
  copy_v3_v3_int(mds->shift, total_shift);

  /* calculate new domain boundary points so that smoke doesn't slide on sub-cell movement */
  mds->p0[0] = mds->dp0[0] - mds->cell_size[0] * (mds->shift_f[0] - total_shift[0] - 0.5f);
  mds->p0[1] = mds->dp0[1] - mds->cell_size[1] * (mds->shift_f[1] - total_shift[1] - 0.5f);
  mds->p0[2] = mds->dp0[2] - mds->cell_size[2] * (mds->shift_f[2] - total_shift[2] - 0.5f);
  mds->p1[0] = mds->p0[0] + mds->cell_size[0] * mds->base_res[0];
  mds->p1[1] = mds->p0[1] + mds->cell_size[1] * mds->base_res[1];
  mds->p1[2] = mds->p0[2] + mds->cell_size[2] * mds->base_res[2];

  /* adjust domain resolution */
  const int block_size = mds->noise_scale;
  int min[3] = {32767, 32767, 32767}, max[3] = {-32767, -32767, -32767}, res[3];
  int total_cells = 1, res_changed = 0, shift_changed = 0;
  float min_vel[3], max_vel[3];
  int x, y, z;
  float *density = manta_smoke_get_density(mds->fluid);
  float *fuel = manta_smoke_get_fuel(mds->fluid);
  float *bigdensity = manta_smoke_turbulence_get_density(mds->fluid);
  float *bigfuel = manta_smoke_turbulence_get_fuel(mds->fluid);
  float *vx = manta_get_velocity_x(mds->fluid);
  float *vy = manta_get_velocity_y(mds->fluid);
  float *vz = manta_get_velocity_z(mds->fluid);
  int wt_res[3];

  if (mds->flags & FLUID_DOMAIN_USE_NOISE && mds->fluid) {
    manta_smoke_turbulence_get_res(mds->fluid, wt_res);
  }

  INIT_MINMAX(min_vel, max_vel);

  /* Calculate bounds for current domain content */
  for (x = mds->res_min[0]; x < mds->res_max[0]; x++) {
    for (y = mds->res_min[1]; y < mds->res_max[1]; y++) {
      for (z = mds->res_min[2]; z < mds->res_max[2]; z++) {
        int xn = x - new_shift[0];
        int yn = y - new_shift[1];
        int zn = z - new_shift[2];
        int index;
        float max_den;

        /* skip if cell already belongs to new area */
        if (xn >= min[0] && xn <= max[0] && yn >= min[1] && yn <= max[1] && zn >= min[2] &&
            zn <= max[2]) {
          continue;
        }

        index = manta_get_index(x - mds->res_min[0],
                                mds->res[0],
                                y - mds->res_min[1],
                                mds->res[1],
                                z - mds->res_min[2]);
        max_den = (fuel) ? MAX2(density[index], fuel[index]) : density[index];

        /* check high resolution bounds if max density isnt already high enough */
        if (max_den < mds->adapt_threshold && mds->flags & FLUID_DOMAIN_USE_NOISE && mds->fluid) {
          int i, j, k;
          /* high res grid index */
          int xx = (x - mds->res_min[0]) * block_size;
          int yy = (y - mds->res_min[1]) * block_size;
          int zz = (z - mds->res_min[2]) * block_size;

          for (i = 0; i < block_size; i++) {
            for (j = 0; j < block_size; j++) {
              for (k = 0; k < block_size; k++) {
                int big_index = manta_get_index(xx + i, wt_res[0], yy + j, wt_res[1], zz + k);
                float den = (bigfuel) ? MAX2(bigdensity[big_index], bigfuel[big_index]) :
                                        bigdensity[big_index];
                if (den > max_den) {
                  max_den = den;
                }
              }
            }
          }
        }

        /* content bounds (use shifted coordinates) */
        if (max_den >= mds->adapt_threshold) {
          if (min[0] > xn) {
            min[0] = xn;
          }
          if (min[1] > yn) {
            min[1] = yn;
          }
          if (min[2] > zn) {
            min[2] = zn;
          }
          if (max[0] < xn) {
            max[0] = xn;
          }
          if (max[1] < yn) {
            max[1] = yn;
          }
          if (max[2] < zn) {
            max[2] = zn;
          }
        }

        /* velocity bounds */
        if (min_vel[0] > vx[index]) {
          min_vel[0] = vx[index];
        }
        if (min_vel[1] > vy[index]) {
          min_vel[1] = vy[index];
        }
        if (min_vel[2] > vz[index]) {
          min_vel[2] = vz[index];
        }
        if (max_vel[0] < vx[index]) {
          max_vel[0] = vx[index];
        }
        if (max_vel[1] < vy[index]) {
          max_vel[1] = vy[index];
        }
        if (max_vel[2] < vz[index]) {
          max_vel[2] = vz[index];
        }
      }
    }
  }

  /* also apply emission maps */
  for (int i = 0; i < numflowobj; i++) {
    EmissionMap *em = &emaps[i];

    for (x = em->min[0]; x < em->max[0]; x++) {
      for (y = em->min[1]; y < em->max[1]; y++) {
        for (z = em->min[2]; z < em->max[2]; z++) {
          int index = manta_get_index(
              x - em->min[0], em->res[0], y - em->min[1], em->res[1], z - em->min[2]);
          float max_den = em->influence[index];

          /* density bounds */
          if (max_den >= mds->adapt_threshold) {
            if (min[0] > x) {
              min[0] = x;
            }
            if (min[1] > y) {
              min[1] = y;
            }
            if (min[2] > z) {
              min[2] = z;
            }
            if (max[0] < x) {
              max[0] = x;
            }
            if (max[1] < y) {
              max[1] = y;
            }
            if (max[2] < z) {
              max[2] = z;
            }
          }
        }
      }
    }
  }

  /* calculate new bounds based on these values */
  clampBoundsInDomain(mds, min, max, min_vel, max_vel, mds->adapt_margin + 1, dt);

  for (int i = 0; i < 3; i++) {
    /* calculate new resolution */
    res[i] = max[i] - min[i];
    total_cells *= res[i];

    if (new_shift[i]) {
      shift_changed = 1;
    }

    /* if no content set minimum dimensions */
    if (res[i] <= 0) {
      int j;
      for (j = 0; j < 3; j++) {
        min[j] = 0;
        max[j] = 1;
        res[j] = 1;
      }
      res_changed = 1;
      total_cells = 1;
      break;
    }
    if (min[i] != mds->res_min[i] || max[i] != mds->res_max[i]) {
      res_changed = 1;
    }
  }

  if (res_changed || shift_changed) {
    BKE_manta_reallocate_copy_fluid(mds, mds->res, res, mds->res_min, min, mds->res_max, tmpShift, total_shift);

    /* set new domain dimensions */
    copy_v3_v3_int(mds->res_min, min);
    copy_v3_v3_int(mds->res_max, max);
    copy_v3_v3_int(mds->res, res);
    mds->total_cells = total_cells;

    /* Redo adapt time step in manta to refresh solver state (ie time variables) */
    manta_adapt_timestep(mds->fluid);
  }

  /* update global size field with new bbox size */
  /* volume bounds */
  float minf[3], maxf[3], size[3];
  madd_v3fl_v3fl_v3fl_v3i(minf, mds->p0, mds->cell_size, mds->res_min);
  madd_v3fl_v3fl_v3fl_v3i(maxf, mds->p0, mds->cell_size, mds->res_max);
  /* calculate domain dimensions */
  sub_v3_v3v3(size, maxf, minf);
  /* apply object scale */
  for (int i = 0; i < 3; i++) {
    size[i] = fabsf(size[i] * ob->scale[i]);
  }
  copy_v3_v3(mds->global_size, size);
}

BLI_INLINE void apply_outflow_fields(int index,
                                     float distance_value,
                                     float *density,
                                     float *heat,
                                     float *fuel,
                                     float *react,
                                     float *color_r,
                                     float *color_g,
                                     float *color_b,
                                     float *phiout)
{
  /* determine outflow cells - phiout used in smoke and liquids */
  if (phiout) {
    phiout[index] = distance_value;
  }

  /* set smoke outflow */
  if (density) {
    density[index] = 0.f;
  }
  if (heat) {
    heat[index] = 0.f;
  }
  if (fuel) {
    fuel[index] = 0.f;
    react[index] = 0.f;
  }
  if (color_r) {
    color_r[index] = 0.f;
    color_g[index] = 0.f;
    color_b[index] = 0.f;
  }
}

BLI_INLINE void apply_inflow_fields(MantaFlowSettings *mfs,
                                    float emission_value,
                                    float distance_value,
                                    int index,
                                    float *density,
                                    float *heat,
                                    float *fuel,
                                    float *react,
                                    float *color_r,
                                    float *color_g,
                                    float *color_b,
                                    float *phi,
                                    float *emission)
{
  /* add inflow */
  if (phi) {
    phi[index] = distance_value;
  }

  /* save emission value for manta inflow */
  if (emission) {
    emission[index] = emission_value;
  }

  /* add smoke inflow */
  int absolute_flow = (mfs->flags & FLUID_FLOW_ABSOLUTE);
  float dens_old = (density) ? density[index] : 0.0;
  // float fuel_old = (fuel) ? fuel[index] : 0.0f;  /* UNUSED */
  float dens_flow = (mfs->type == FLUID_FLOW_TYPE_FIRE) ? 0.0f : emission_value * mfs->density;
  float fuel_flow = (fuel) ? emission_value * mfs->fuel_amount : 0.0f;
  /* add heat */
  if (heat && emission_value > 0.0f) {
    heat[index] = ADD_IF_LOWER(heat[index], mfs->temp);
  }

  /* set density and fuel - absolute mode */
  if (absolute_flow) {
    if (density && mfs->type != FLUID_FLOW_TYPE_FIRE) {
      if (dens_flow > density[index]) {
        density[index] = dens_flow;
      }
    }
    if (fuel && mfs->type != FLUID_FLOW_TYPE_SMOKE && fuel_flow) {
      if (fuel_flow > fuel[index]) {
        fuel[index] = fuel_flow;
      }
    }
  }
  /* set density and fuel - additive mode */
  else {
    if (density && mfs->type != FLUID_FLOW_TYPE_FIRE) {
      density[index] += dens_flow;
      CLAMP(density[index], 0.0f, 1.0f);
    }
    if (fuel && mfs->type != FLUID_FLOW_TYPE_SMOKE && mfs->fuel_amount) {
      fuel[index] += fuel_flow;
      CLAMP(fuel[index], 0.0f, 10.0f);
    }
  }

  /* set color */
  if (color_r && dens_flow) {
    float total_dens = density[index] / (dens_old + dens_flow);
    color_r[index] = (color_r[index] + mfs->color[0] * dens_flow) * total_dens;
    color_g[index] = (color_g[index] + mfs->color[1] * dens_flow) * total_dens;
    color_b[index] = (color_b[index] + mfs->color[2] * dens_flow) * total_dens;
  }

  /* set fire reaction coordinate */
  if (fuel && fuel[index] > FLT_EPSILON) {
    /* instead of using 1.0 for all new fuel add slight falloff
     * to reduce flow blockiness */
    float value = 1.0f - pow2f(1.0f - emission_value);

    if (value > react[index]) {
      float f = fuel_flow / fuel[index];
      react[index] = value * f + (1.0f - f) * react[index];
      CLAMP(react[index], 0.0f, value);
    }
  }
}

static void update_flowsflags(MantaDomainSettings *mds, Object **flowobjs, int numflowobj)
{
  int active_fields = mds->active_fields;
  unsigned int flowIndex;

  /* Monitor active fields based on flow settings */
  for (flowIndex = 0; flowIndex < numflowobj; flowIndex++) {
    Object *collob = flowobjs[flowIndex];
    MantaModifierData *mmd2 = (MantaModifierData *)modifiers_findByType(collob,
                                                                        eModifierType_Manta);

    // Sanity check
    if (!mmd2) {
      continue;
    }

    if ((mmd2->type & MOD_MANTA_TYPE_FLOW) && mmd2->flow) {
      MantaFlowSettings *mfs = mmd2->flow;
      if (!mfs) {
        break;
      }
      if (mfs->flags & FLUID_FLOW_INITVELOCITY) {
        active_fields |= FLUID_DOMAIN_ACTIVE_INVEL;
      }
      if (mfs->behavior == FLUID_FLOW_BEHAVIOR_OUTFLOW) {
        active_fields |= FLUID_DOMAIN_ACTIVE_OUTFLOW;
      }
      /* liquids done from here */
      if (mds->type == FLUID_DOMAIN_TYPE_LIQUID) {
        continue;
      }

      /* activate heat field if flow produces any heat */
      if (mfs->temp) {
        active_fields |= FLUID_DOMAIN_ACTIVE_HEAT;
      }
      /* activate fuel field if flow adds any fuel */
      if (mfs->fuel_amount &&
          (mfs->type == FLUID_FLOW_TYPE_FIRE || mfs->type == FLUID_FLOW_TYPE_SMOKEFIRE)) {
        active_fields |= FLUID_DOMAIN_ACTIVE_FIRE;
      }
      /* activate color field if flows add smoke with varying colors */
      if (mfs->density &&
          (mfs->type == FLUID_FLOW_TYPE_SMOKE || mfs->type == FLUID_FLOW_TYPE_SMOKEFIRE)) {
        if (!(active_fields & FLUID_DOMAIN_ACTIVE_COLOR_SET)) {
          copy_v3_v3(mds->active_color, mfs->color);
          active_fields |= FLUID_DOMAIN_ACTIVE_COLOR_SET;
        }
        else if (!equals_v3v3(mds->active_color, mfs->color)) {
          copy_v3_v3(mds->active_color, mfs->color);
          active_fields |= FLUID_DOMAIN_ACTIVE_COLORS;
        }
      }
    }
  }
  /* Monitor active fields based on domain settings */
  if (mds->type == FLUID_DOMAIN_TYPE_GAS && active_fields & FLUID_DOMAIN_ACTIVE_FIRE) {
    /* heat is always needed for fire */
    active_fields |= FLUID_DOMAIN_ACTIVE_HEAT;
    /* also activate colors if domain smoke color differs from active color */
    if (!(active_fields & FLUID_DOMAIN_ACTIVE_COLOR_SET)) {
      copy_v3_v3(mds->active_color, mds->flame_smoke_color);
      active_fields |= FLUID_DOMAIN_ACTIVE_COLOR_SET;
    }
    else if (!equals_v3v3(mds->active_color, mds->flame_smoke_color)) {
      copy_v3_v3(mds->active_color, mds->flame_smoke_color);
      active_fields |= FLUID_DOMAIN_ACTIVE_COLORS;
    }
  }
  /* Finally, initialize new data fields if any */
  if (active_fields & FLUID_DOMAIN_ACTIVE_INVEL) {
    manta_ensure_invelocity(mds->fluid, mds->mmd);
  }
  if (active_fields & FLUID_DOMAIN_ACTIVE_OUTFLOW) {
    manta_ensure_outflow(mds->fluid, mds->mmd);
  }
  if (active_fields & FLUID_DOMAIN_ACTIVE_HEAT) {
    manta_smoke_ensure_heat(mds->fluid, mds->mmd);
  }
  if (active_fields & FLUID_DOMAIN_ACTIVE_FIRE) {
    manta_smoke_ensure_fire(mds->fluid, mds->mmd);
  }
  if (active_fields & FLUID_DOMAIN_ACTIVE_COLORS) {
    /* initialize all smoke with "active_color" */
    manta_smoke_ensure_colors(mds->fluid, mds->mmd);
  }
  if (mds->type == FLUID_DOMAIN_TYPE_LIQUID &&
      (mds->particle_type & FLUID_DOMAIN_PARTICLE_SPRAY ||
       mds->particle_type & FLUID_DOMAIN_PARTICLE_FOAM ||
       mds->particle_type & FLUID_DOMAIN_PARTICLE_TRACER)) {
    manta_liquid_ensure_sndparts(mds->fluid, mds->mmd);
  }
  mds->active_fields = active_fields;
}

static void update_flowsfluids(struct Depsgraph *depsgraph,
                               Scene *scene,
                               Object *ob,
                               MantaDomainSettings *mds,
                               float time_per_frame,
                               float frame_length,
                               int frame,
                               float dt)
{
  EmissionMap *emaps = NULL;
  Object **flowobjs = NULL;
  unsigned int numflowobj = 0, flowIndex = 0;
  bool is_first_frame = (frame == mds->cache_frame_start);

  flowobjs = BKE_collision_objects_create(
      depsgraph, ob, mds->fluid_group, &numflowobj, eModifierType_Manta);

  /* Update all flow related flags and ensure that corresponding grids get initialized */
  update_flowsflags(mds, flowobjs, numflowobj);

  /* init emission maps for each flow */
  emaps = MEM_callocN(sizeof(struct EmissionMap) * numflowobj, "manta_flow_maps");

  /* Prepare flow emission maps */
  for (flowIndex = 0; flowIndex < numflowobj; flowIndex++) {
    Object *flowobj = flowobjs[flowIndex];
    MantaModifierData *mmd2 = (MantaModifierData *)modifiers_findByType(flowobj,
                                                                        eModifierType_Manta);

    /* Check for initialized smoke object */
    if ((mmd2->type & MOD_MANTA_TYPE_FLOW) && mmd2->flow) {
      MantaFlowSettings *mfs = mmd2->flow;
      int subframes = mfs->subframes;
      EmissionMap *em = &emaps[flowIndex];

      /* Length of one adaptive frame. If using adaptive stepping, length is smaller than actual frame length */
      float adaptframe_length = time_per_frame / frame_length;
      /* Adaptive frame length as percentage */
      CLAMP(adaptframe_length, 0.0f, 1.0f);

      /* Further splitting because of emission subframe: If no subframes present, sample_size is 1 */
      float sample_size = 1.0f / (float)(subframes + 1);
      int hires_multiplier = 1;

      /* First frame cannot have any subframes because there is (obviously) no previous frame from where subframes could come from */
      if (is_first_frame) {
        subframes = 0;
      }

      int subframe;
      float subframe_dt = dt * sample_size;

      /* Emission loop. When not using subframes this will loop only once. */
      for (subframe = subframes; subframe >= 0; subframe--) {

        /* Temporary emission map used when subframes are enabled, i.e. at least one subframe */
        EmissionMap em_temp = {NULL};

        /* Set scene time */
        /* Handle emission subframe */
        if (subframe > 0 && !is_first_frame) {
          scene->r.subframe = adaptframe_length - sample_size * (float)(subframe) * (dt / frame_length);
          scene->r.cfra = frame - 1;
        }
        /* Last frame in this loop (subframe == suframes). Can be real end frame or in between frames (adaptive frame) */
        else {
          /* Handle adaptive subframe (ie has subframe fraction). Need to set according scene subframe parameter */
          if (time_per_frame < frame_length) {
            scene->r.subframe = adaptframe_length;
            scene->r.cfra = frame - 1;
          }
          /* Handle absolute endframe (ie no subframe fraction). Need to set the scene subframe parameter to 0 and advance current scene frame */
          else {
            scene->r.subframe = 0.0f;
            scene->r.cfra = frame;
          }
        }
        /* Sanity check: subframe portion must be between 0 and 1 */
        CLAMP(scene->r.subframe, 0.0f, 1.0f);
        //printf("flow: frame (is first: %d): %d // scene current frame: %d // scene current subframe: %f\n", is_first_frame, frame, scene->r.cfra, scene->r.subframe);

        /* Update frame time, this is considering current subframe fraction
         * BLI_mutex_lock() called in manta_step(), so safe to update subframe here
         * TODO (sebbas): Using BKE_scene_frame_get(scene) instead of new DEG_get_ctime(depsgraph) as subframes dont work with the latter yet */
        BKE_object_modifier_update_subframe(
            depsgraph, scene, flowobj, true, 5, BKE_scene_frame_get(scene), eModifierType_Manta);

        /* Emission from particles */
        if (mfs->source == FLUID_FLOW_SOURCE_PARTICLES) {
          if (subframes) {
            emit_from_particles(flowobj, mds, mfs, &em_temp, depsgraph, scene, subframe_dt);
          }
          else {
            emit_from_particles(flowobj, mds, mfs, em, depsgraph, scene, subframe_dt);
          }

          if (!(mfs->flags & FLUID_FLOW_USE_PART_SIZE)) {
            hires_multiplier = 1;
          }
        }
        /* Emission from mesh */
        else if (mfs->source == FLUID_FLOW_SOURCE_MESH) {
          if (subframes) {
            emit_from_mesh(flowobj, mds, mfs, &em_temp, subframe_dt);
          }
          else {
            emit_from_mesh(flowobj, mds, mfs, em, subframe_dt);
          }
        }
        else {
          printf("Error: unknown flow emission source\n");
        }

        /* If this we emitted with temp emission map in this loop (subframe emission), we combine the temp map with the original emission map */
        if (subframes) {
          /* Combine emission maps */
          em_combineMaps(
              em, &em_temp, hires_multiplier, !(mfs->flags & FLUID_FLOW_ABSOLUTE), sample_size);
          em_freeData(&em_temp);
        }
      }
    }
  }

//  printf("flow: frame: %d // time per frame: %f // frame length: %f // dt: %f\n", frame, time_per_frame, frame_length, dt);

  /* Adjust domain size if needed. Only do this once for every frame */
  if (mds->type == FLUID_DOMAIN_TYPE_GAS &&
      mds->flags & FLUID_DOMAIN_USE_ADAPTIVE_DOMAIN)
  {
    adaptiveDomainAdjust(mds, ob, emaps, numflowobj, dt);
  }

  float *phi_in = manta_get_phi_in(mds->fluid);
  float *phiout_in = manta_get_phiout_in(mds->fluid);
  float *density = manta_smoke_get_density(mds->fluid);
  float *color_r = manta_smoke_get_color_r(mds->fluid);
  float *color_g = manta_smoke_get_color_g(mds->fluid);
  float *color_b = manta_smoke_get_color_b(mds->fluid);
  float *fuel = manta_smoke_get_fuel(mds->fluid);
  float *heat = manta_smoke_get_heat(mds->fluid);
  float *react = manta_smoke_get_react(mds->fluid);

  float *density_in = manta_smoke_get_density_in(mds->fluid);
  float *heat_in = manta_smoke_get_heat_in(mds->fluid);
  float *color_r_in = manta_smoke_get_color_r_in(mds->fluid);
  float *color_g_in = manta_smoke_get_color_g_in(mds->fluid);
  float *color_b_in = manta_smoke_get_color_b_in(mds->fluid);
  float *fuel_in = manta_smoke_get_fuel_in(mds->fluid);
  float *react_in = manta_smoke_get_react_in(mds->fluid);
  float *emission_in = manta_smoke_get_emission_in(mds->fluid);

  float *velx_initial = manta_get_in_velocity_x(mds->fluid);
  float *vely_initial = manta_get_in_velocity_y(mds->fluid);
  float *velz_initial = manta_get_in_velocity_z(mds->fluid);
  unsigned int z;

  /* Grid reset before writing again */
  for (z = 0; z < mds->res[0] * mds->res[1] * mds->res[2]; z++) {
    if (phi_in) {
      phi_in[z] = 9999.0f;
    }
    if (phiout_in) {
      phiout_in[z] = 9999.0f;
    }
    if (density_in) {
      density_in[z] = 0.0f;
    }
    if (heat_in) {
      heat_in[z] = 0.0f;
    }
    if (color_r_in) {
      color_r_in[z] = 0.0f;
      color_g_in[z] = 0.0f;
      color_b_in[z] = 0.0f;
    }
    if (fuel_in) {
      fuel_in[z] = 0.0f;
      react_in[z] = 0.0f;
    }
    if (emission_in) {
      emission_in[z] = 0.0f;
    }
    if (velx_initial) {
      velx_initial[z] = 0.0f;
      vely_initial[z] = 0.0f;
      velz_initial[z] = 0.0f;
    }
  }

  /* Apply emission data */
  for (flowIndex = 0; flowIndex < numflowobj; flowIndex++) {
    Object *flowobj = flowobjs[flowIndex];
    MantaModifierData *mmd2 = (MantaModifierData *)modifiers_findByType(flowobj,
                                                                        eModifierType_Manta);

    // check for initialized flow object
    if ((mmd2->type & MOD_MANTA_TYPE_FLOW) && mmd2->flow) {
      MantaFlowSettings *mfs = mmd2->flow;
      EmissionMap *em = &emaps[flowIndex];
      float *velocity_map = em->velocity;
      float *emission_map = em->influence;
      float *distance_map = em->distances;

      int gx, gy, gz, ex, ey, ez, dx, dy, dz;
      size_t e_index, d_index;

      // loop through every emission map cell
      for (gx = em->min[0]; gx < em->max[0]; gx++) {
        for (gy = em->min[1]; gy < em->max[1]; gy++) {
          for (gz = em->min[2]; gz < em->max[2]; gz++) {
            /* get emission map index */
            ex = gx - em->min[0];
            ey = gy - em->min[1];
            ez = gz - em->min[2];
            e_index = manta_get_index(ex, em->res[0], ey, em->res[1], ez);

            /* get domain index */
            dx = gx - mds->res_min[0];
            dy = gy - mds->res_min[1];
            dz = gz - mds->res_min[2];
            d_index = manta_get_index(dx, mds->res[0], dy, mds->res[1], dz);
            /* make sure emission cell is inside the new domain boundary */
            if (dx < 0 || dy < 0 || dz < 0 || dx >= mds->res[0] || dy >= mds->res[1] ||
                dz >= mds->res[2]) {
              continue;
            }

            /* sync inflow grids with actual simulation grids, inflow computation needs information from actual simulation */
            if (emission_map[e_index] && density) {
              density_in[d_index] = density[d_index];
            }
            if (emission_map[e_index] && heat) {
              heat_in[d_index] = heat[d_index];
            }
            if (emission_map[e_index] && color_r) {
              color_r_in[d_index] = color_r[d_index];
              color_g_in[d_index] = color_g[d_index];
              color_b_in[d_index] = color_b[d_index];
            }
            if (emission_map[e_index] && fuel) {
              fuel_in[d_index] = fuel[d_index];
              react_in[d_index] = react[d_index];
            }

            if (mfs->behavior == FLUID_FLOW_BEHAVIOR_OUTFLOW) {  // outflow
              apply_outflow_fields(d_index,
                                   distance_map[e_index],
                                   density_in,
                                   heat_in,
                                   fuel_in,
                                   react_in,
                                   color_r_in,
                                   color_g_in,
                                   color_b_in,
                                   phiout_in);
            }
            else if (mfs->behavior == FLUID_FLOW_BEHAVIOR_GEOMETRY && mmd2->time > 2) {
              apply_inflow_fields(mfs,
                                  0.0f,
                                  9999.0f,
                                  d_index,
                                  density_in,
                                  heat_in,
                                  fuel_in,
                                  react_in,
                                  color_r_in,
                                  color_g_in,
                                  color_b_in,
                                  phi_in,
                                  emission_in);
            }
            else if (mfs->behavior == FLUID_FLOW_BEHAVIOR_INFLOW ||
                     mfs->behavior == FLUID_FLOW_BEHAVIOR_GEOMETRY) {  // inflow
              /* only apply inflow if enabled */
              if (mfs->flags & FLUID_FLOW_USE_INFLOW) {
                apply_inflow_fields(mfs,
                                    emission_map[e_index],
                                    distance_map[e_index],
                                    d_index,
                                    density_in,
                                    heat_in,
                                    fuel_in,
                                    react_in,
                                    color_r_in,
                                    color_g_in,
                                    color_b_in,
                                    phi_in,
                                    emission_in);
                /* initial velocity */
                if (mfs->flags & FLUID_FLOW_INITVELOCITY) {
                  velx_initial[d_index] = velocity_map[e_index * 3];
                  vely_initial[d_index] = velocity_map[e_index * 3 + 1];
                  velz_initial[d_index] = velocity_map[e_index * 3 + 2];
                }
              }
            }
          }  // low res loop
        }
      }

      // free emission maps
      em_freeData(em);

    }  // end emission
  }

  BKE_collision_objects_free(flowobjs);
  if (emaps) {
    MEM_freeN(emaps);
  }
}

typedef struct UpdateEffectorsData {
  Scene *scene;
  MantaDomainSettings *mds;
  ListBase *effectors;

  float *density;
  float *fuel;
  float *force_x;
  float *force_y;
  float *force_z;
  float *velocity_x;
  float *velocity_y;
  float *velocity_z;
  int *flags;
  float *phiObsIn;
} UpdateEffectorsData;

static void update_effectors_task_cb(void *__restrict userdata,
                                     const int x,
                                     const TaskParallelTLS *__restrict UNUSED(tls))
{
  UpdateEffectorsData *data = userdata;
  MantaDomainSettings *mds = data->mds;

  for (int y = 0; y < mds->res[1]; y++) {
    for (int z = 0; z < mds->res[2]; z++) {
      EffectedPoint epoint;
      float mag;
      float voxelCenter[3] = {0, 0, 0}, vel[3] = {0, 0, 0}, retvel[3] = {0, 0, 0};
      const unsigned int index = manta_get_index(x, mds->res[0], y, mds->res[1], z);

      if ((data->fuel && MAX2(data->density[index], data->fuel[index]) < FLT_EPSILON) ||
          (data->density && data->density[index] < FLT_EPSILON) ||
          (data->phiObsIn && data->phiObsIn[index] < 0.0f) ||
          data->flags[index] & 2)  // mantaflow convention: 2 == FlagObstacle
      {
        continue;
      }

      /* get velocities from manta grid space and convert to blender units */
      vel[0] = data->velocity_x[index];
      vel[1] = data->velocity_y[index];
      vel[2] = data->velocity_z[index];
      mul_v3_fl(vel, mds->dx);

      /* convert vel to global space */
      mag = len_v3(vel);
      mul_mat3_m4_v3(mds->obmat, vel);
      normalize_v3(vel);
      mul_v3_fl(vel, mag);

      voxelCenter[0] = mds->p0[0] + mds->cell_size[0] * ((float)(x + mds->res_min[0]) + 0.5f);
      voxelCenter[1] = mds->p0[1] + mds->cell_size[1] * ((float)(y + mds->res_min[1]) + 0.5f);
      voxelCenter[2] = mds->p0[2] + mds->cell_size[2] * ((float)(z + mds->res_min[2]) + 0.5f);
      mul_m4_v3(mds->obmat, voxelCenter);

      /* do effectors */
      pd_point_from_loc(data->scene, voxelCenter, vel, index, &epoint);
      BKE_effectors_apply(data->effectors, NULL, mds->effector_weights, &epoint, retvel, NULL);

      /* convert retvel to local space */
      mag = len_v3(retvel);
      mul_mat3_m4_v3(mds->imat, retvel);
      normalize_v3(retvel);
      mul_v3_fl(retvel, mag);

      /* constrain forces to interval -1 to 1 */
      data->force_x[index] = min_ff(max_ff(-1.0f, retvel[0] * 0.2f), 1.0f);
      data->force_y[index] = min_ff(max_ff(-1.0f, retvel[1] * 0.2f), 1.0f);
      data->force_z[index] = min_ff(max_ff(-1.0f, retvel[2] * 0.2f), 1.0f);
    }
  }
}

static void update_effectors(
    Depsgraph *depsgraph, Scene *scene, Object *ob, MantaDomainSettings *mds, float UNUSED(dt))
{
  ListBase *effectors;
  /* make sure smoke flow influence is 0.0f */
  mds->effector_weights->weight[PFIELD_SMOKEFLOW] = 0.0f;
  effectors = BKE_effectors_create(depsgraph, ob, NULL, mds->effector_weights);

  if (effectors) {
    // precalculate wind forces
    UpdateEffectorsData data;
    data.scene = scene;
    data.mds = mds;
    data.effectors = effectors;
    data.density = manta_smoke_get_density(mds->fluid);
    data.fuel = manta_smoke_get_fuel(mds->fluid);
    data.force_x = manta_get_force_x(mds->fluid);
    data.force_y = manta_get_force_y(mds->fluid);
    data.force_z = manta_get_force_z(mds->fluid);
    data.velocity_x = manta_get_velocity_x(mds->fluid);
    data.velocity_y = manta_get_velocity_y(mds->fluid);
    data.velocity_z = manta_get_velocity_z(mds->fluid);
    data.flags = manta_smoke_get_obstacle(mds->fluid);
    data.phiObsIn = manta_get_phiobs_in(mds->fluid);

    TaskParallelSettings settings;
    BLI_parallel_range_settings_defaults(&settings);
    settings.scheduling_mode = TASK_SCHEDULING_DYNAMIC;
    BLI_task_parallel_range(0, mds->res[0], &data, update_effectors_task_cb, &settings);
  }

  BKE_effectors_free(effectors);
}

static Mesh *createLiquidGeometry(MantaDomainSettings *mds, Mesh *orgmesh, Object *ob)
{
  Mesh *me;
  MVert *mverts;
  MPoly *mpolys;
  MLoop *mloops;
  short *normals, *no_s;
  float no[3];
  float min[3];
  float max[3];
  float size[3];
  float cell_size_scaled[3];

  /* assign material + flags to new dm
   * if there's no faces in original dm, keep materials and flags unchanged */
  MPoly *mpoly;
  MPoly mp_example = {0};
  mpoly = orgmesh->mpoly;
  if (mpoly) {
    mp_example = *mpoly;
  }
  /* else leave NULL'd */

  const short mp_mat_nr = mp_example.mat_nr;
  const char mp_flag = mp_example.flag;

  int i;
  int num_verts, num_normals, num_faces;

  if (!mds->fluid) {
    return NULL;
  }

  num_verts = manta_liquid_get_num_verts(mds->fluid);
  num_normals = manta_liquid_get_num_normals(mds->fluid);
  num_faces = manta_liquid_get_num_triangles(mds->fluid);

  //printf("num_verts: %d, num_normals: %d, num_faces: %d\n", num_verts, num_normals, num_faces);

  if (!num_verts || !num_faces) {
    return NULL;
  }

  me = BKE_mesh_new_nomain(num_verts, 0, 0, num_faces * 3, num_faces);
  mverts = me->mvert;
  mpolys = me->mpoly;
  mloops = me->mloop;
  if (!me) {
    return NULL;
  }

  // Get size (dimension) but considering scaling scaling
  copy_v3_v3(cell_size_scaled, mds->cell_size);
  mul_v3_v3(cell_size_scaled, ob->scale);
  madd_v3fl_v3fl_v3fl_v3i(min, mds->p0, cell_size_scaled, mds->res_min);
  madd_v3fl_v3fl_v3fl_v3i(max, mds->p0, cell_size_scaled, mds->res_max);
  sub_v3_v3v3(size, max, min);

  // Biggest dimension will be used for upscaling
  float max_size = MAX3(size[0], size[1], size[2]);

  // Vertices
  for (i = 0; i < num_verts; i++, mverts++) {
    // read raw data. is normalized cube around domain origin
    mverts->co[0] = manta_liquid_get_vertex_x_at(mds->fluid, i);
    mverts->co[1] = manta_liquid_get_vertex_y_at(mds->fluid, i);
    mverts->co[2] = manta_liquid_get_vertex_z_at(mds->fluid, i);

    // if reading raw data directly from manta, normalize now, otherwise omit this, ie when reading from files
    {
      // normalize to unit cube around 0
      mverts->co[0] -= ((float)mds->res[0] * mds->mesh_scale) * 0.5f;
      mverts->co[1] -= ((float)mds->res[1] * mds->mesh_scale) * 0.5f;
      mverts->co[2] -= ((float)mds->res[2] * mds->mesh_scale) * 0.5f;
      mverts->co[0] *= mds->dx / mds->mesh_scale;
      mverts->co[1] *= mds->dx / mds->mesh_scale;
      mverts->co[2] *= mds->dx / mds->mesh_scale;
    }

    mverts->co[0] *= max_size / fabsf(ob->scale[0]);
    mverts->co[1] *= max_size / fabsf(ob->scale[1]);
    mverts->co[2] *= max_size / fabsf(ob->scale[2]);

    //printf("mverts->co[0]: %f, mverts->co[1]: %f, mverts->co[2]: %f\n", mverts->co[0], mverts->co[1], mverts->co[2]);
  }

  // Normals
  normals = MEM_callocN(sizeof(short) * num_normals * 3, "Fluidmesh_tmp_normals");

  for (i = 0, no_s = normals; i < num_normals; no_s += 3, i++) {
    no[0] = manta_liquid_get_normal_x_at(mds->fluid, i);
    no[1] = manta_liquid_get_normal_y_at(mds->fluid, i);
    no[2] = manta_liquid_get_normal_z_at(mds->fluid, i);

    normal_float_to_short_v3(no_s, no);

    //printf("no_s[0]: %d, no_s[1]: %d, no_s[2]: %d\n", no_s[0], no_s[1], no_s[2]);
  }

  // Triangles
  for (i = 0; i < num_faces; i++, mpolys++, mloops += 3) {
    /* initialize from existing face */
    mpolys->mat_nr = mp_mat_nr;
    mpolys->flag = mp_flag;

    mpolys->loopstart = i * 3;
    mpolys->totloop = 3;

    mloops[0].v = manta_liquid_get_triangle_x_at(mds->fluid, i);
    mloops[1].v = manta_liquid_get_triangle_y_at(mds->fluid, i);
    mloops[2].v = manta_liquid_get_triangle_z_at(mds->fluid, i);

    //printf("mloops[0].v: %d, mloops[1].v: %d, mloops[2].v: %d\n", mloops[0].v, mloops[1].v, mloops[2].v);
  }

  BKE_mesh_ensure_normals(me);
  BKE_mesh_calc_edges(me, false, false);
  BKE_mesh_vert_normals_apply(me, (short(*)[3])normals);

  MEM_freeN(normals);

  /* return early if no mesh vert velocities required */
  if ((mds->flags & FLUID_DOMAIN_USE_SPEED_VECTORS) == 0) {
    return me;
  }

  if (mds->mesh_velocities) {
    MEM_freeN(mds->mesh_velocities);
  }

  mds->mesh_velocities = MEM_calloc_arrayN(
      num_verts, sizeof(MantaVertexVelocity), "Fluidmesh_vertvelocities");
  mds->totvert = num_verts;

  MantaVertexVelocity *velarray = NULL;
  velarray = mds->mesh_velocities;

  float time_mult = 25.f * DT_DEFAULT;

  for (i = 0; i < num_verts; i++, mverts++) {
    velarray[i].vel[0] = manta_liquid_get_vertvel_x_at(mds->fluid, i) * (mds->dx / time_mult);
    velarray[i].vel[1] = manta_liquid_get_vertvel_y_at(mds->fluid, i) * (mds->dx / time_mult);
    velarray[i].vel[2] = manta_liquid_get_vertvel_z_at(mds->fluid, i) * (mds->dx / time_mult);

    //printf("velarray[%d].vel[0]: %f, velarray[%d].vel[1]: %f, velarray[%d].vel[2]: %f\n", i, velarray[i].vel[0], i, velarray[i].vel[1], i, velarray[i].vel[2]);
  }

  return me;
}

static Mesh *createSmokeGeometry(MantaDomainSettings *mds, Mesh *orgmesh, Object *ob)
{
  Mesh *result;
  MVert *mverts;
  MPoly *mpolys;
  MLoop *mloops;
  float min[3];
  float max[3];
  float *co;
  MPoly *mp;
  MLoop *ml;

  int num_verts = 8;
  int num_faces = 6;
  int i;
  float ob_loc[3] = {0};
  float ob_cache_loc[3] = {0};

  /* just copy existing mesh if there is no content or if the adaptive domain is not being used */
  if (mds->total_cells <= 1 || (mds-> flags & FLUID_DOMAIN_USE_ADAPTIVE_DOMAIN) == 0) {
    return BKE_mesh_copy_for_eval(orgmesh, false);
  }

  result = BKE_mesh_new_nomain(num_verts, 0, 0, num_faces * 4, num_faces);
  mverts = result->mvert;
  mpolys = result->mpoly;
  mloops = result->mloop;

  if (num_verts) {
    /* volume bounds */
    madd_v3fl_v3fl_v3fl_v3i(min, mds->p0, mds->cell_size, mds->res_min);
    madd_v3fl_v3fl_v3fl_v3i(max, mds->p0, mds->cell_size, mds->res_max);

    /* set vertices */
    /* top slab */
    co = mverts[0].co;
    co[0] = min[0];
    co[1] = min[1];
    co[2] = max[2];
    co = mverts[1].co;
    co[0] = max[0];
    co[1] = min[1];
    co[2] = max[2];
    co = mverts[2].co;
    co[0] = max[0];
    co[1] = max[1];
    co[2] = max[2];
    co = mverts[3].co;
    co[0] = min[0];
    co[1] = max[1];
    co[2] = max[2];
    /* bottom slab */
    co = mverts[4].co;
    co[0] = min[0];
    co[1] = min[1];
    co[2] = min[2];
    co = mverts[5].co;
    co[0] = max[0];
    co[1] = min[1];
    co[2] = min[2];
    co = mverts[6].co;
    co[0] = max[0];
    co[1] = max[1];
    co[2] = min[2];
    co = mverts[7].co;
    co[0] = min[0];
    co[1] = max[1];
    co[2] = min[2];

    /* create faces */
    /* top */
    mp = &mpolys[0];
    ml = &mloops[0 * 4];
    mp->loopstart = 0 * 4;
    mp->totloop = 4;
    ml[0].v = 0;
    ml[1].v = 1;
    ml[2].v = 2;
    ml[3].v = 3;
    /* right */
    mp = &mpolys[1];
    ml = &mloops[1 * 4];
    mp->loopstart = 1 * 4;
    mp->totloop = 4;
    ml[0].v = 2;
    ml[1].v = 1;
    ml[2].v = 5;
    ml[3].v = 6;
    /* bottom */
    mp = &mpolys[2];
    ml = &mloops[2 * 4];
    mp->loopstart = 2 * 4;
    mp->totloop = 4;
    ml[0].v = 7;
    ml[1].v = 6;
    ml[2].v = 5;
    ml[3].v = 4;
    /* left */
    mp = &mpolys[3];
    ml = &mloops[3 * 4];
    mp->loopstart = 3 * 4;
    mp->totloop = 4;
    ml[0].v = 0;
    ml[1].v = 3;
    ml[2].v = 7;
    ml[3].v = 4;
    /* front */
    mp = &mpolys[4];
    ml = &mloops[4 * 4];
    mp->loopstart = 4 * 4;
    mp->totloop = 4;
    ml[0].v = 3;
    ml[1].v = 2;
    ml[2].v = 6;
    ml[3].v = 7;
    /* back */
    mp = &mpolys[5];
    ml = &mloops[5 * 4];
    mp->loopstart = 5 * 4;
    mp->totloop = 4;
    ml[0].v = 1;
    ml[1].v = 0;
    ml[2].v = 4;
    ml[3].v = 5;

    /* calculate required shift to match domain's global position
     * it was originally simulated at (if object moves without manta step) */
    invert_m4_m4(ob->imat, ob->obmat);
    mul_m4_v3(ob->obmat, ob_loc);
    mul_m4_v3(mds->obmat, ob_cache_loc);
    sub_v3_v3v3(mds->obj_shift_f, ob_cache_loc, ob_loc);
    /* convert shift to local space and apply to vertices */
    mul_mat3_m4_v3(ob->imat, mds->obj_shift_f);
    /* apply */
    for (i = 0; i < num_verts; i++) {
      add_v3_v3(mverts[i].co, mds->obj_shift_f);
    }
  }

  BKE_mesh_calc_edges(result, false, false);
  result->runtime.cd_dirty_vert |= CD_MASK_NORMAL;
  return result;
}

static void manta_step(Depsgraph *depsgraph,
                       Scene *scene,
                       Object *ob,
                       Mesh *me,
                       MantaModifierData *mmd,
                       int frame)
{
  MantaDomainSettings *mds = mmd->domain;
  float dt, frame_length, time_total;
  float time_per_frame;
  bool init_resolution = true;

  /* update object state */
  invert_m4_m4(mds->imat, ob->obmat);
  copy_m4_m4(mds->obmat, ob->obmat);

  /* gas domain might use adaptive domain */
  if (mds->type == FLUID_DOMAIN_TYPE_GAS) {
    init_resolution = (mds->flags & FLUID_DOMAIN_USE_ADAPTIVE_DOMAIN) != 0;
  }
  manta_set_domain_from_mesh(mds, ob, me, init_resolution);

  /* use local variables for adaptive loop, dt can change */
  frame_length = mds->frame_length;
  dt = mds->dt;
  time_per_frame = 0;
  time_total = mds->time_total;

  BLI_mutex_lock(&object_update_lock);

  /* loop as long as time_per_frame (sum of sub dt's) does not exceed actual framelength */
  while (time_per_frame < frame_length) {
    manta_adapt_timestep(mds->fluid);
    dt = manta_get_timestep(mds->fluid);

    /* save adapted dt so that MANTA object can access it (important when adaptive domain creates new MANTA object) */
    mds->dt = dt;

    /* count for how long this while loop is running */
    time_per_frame += dt;
    time_total += dt;

    /* Calculate inflow geometry */
    update_flowsfluids(depsgraph, scene, ob, mds, time_per_frame, frame_length, frame, dt);


    manta_update_variables(mds->fluid, mmd);

    /* Calculate obstacle geometry */
    update_obstacles(depsgraph, scene, ob, mds, time_per_frame, frame_length, frame, dt);

    if (mds->total_cells > 1) {
      update_effectors(
          depsgraph,
          scene,
          ob,
          mds,
          dt);
      manta_bake_data(mds->fluid, mmd, frame);

      mds->time_per_frame = time_per_frame;
      mds->time_total = time_total;
    }
  }
  if (mds->type == FLUID_DOMAIN_TYPE_GAS) {
    manta_smoke_calc_transparency(mds, DEG_get_evaluated_view_layer(depsgraph));
  }
  BLI_mutex_unlock(&object_update_lock);
}

static void manta_guiding(
    Depsgraph *depsgraph, Scene *scene, Object *ob, MantaModifierData *mmd, int frame)
{
  MantaDomainSettings *mds = mmd->domain;
  float fps = scene->r.frs_sec / scene->r.frs_sec_base;
  float dt = DT_DEFAULT * (25.0f / fps) * mds->time_scale;;

  BLI_mutex_lock(&object_update_lock);

  update_obstacles(depsgraph, scene, ob, mds, dt, dt, frame, dt);
  manta_bake_guiding(mds->fluid, mmd, frame);

  BLI_mutex_unlock(&object_update_lock);
}

static void mantaModifier_process(
    MantaModifierData *mmd, Depsgraph *depsgraph, Scene *scene, Object *ob, Mesh *me)
{
  const int scene_framenr = (int)DEG_get_ctime(depsgraph);

  if ((mmd->type & MOD_MANTA_TYPE_FLOW)) {
    if (scene_framenr >= mmd->time) {
      mantaModifier_init(mmd, depsgraph, ob, scene, me);
    }

    if (mmd->flow) {
      if (mmd->flow->mesh) {
        BKE_id_free(NULL, mmd->flow->mesh);
      }
      mmd->flow->mesh = BKE_mesh_copy_for_eval(me, false);
    }

    if (scene_framenr > mmd->time) {
      mmd->time = scene_framenr;
    }
    else if (scene_framenr < mmd->time) {
      mmd->time = scene_framenr;
      mantaModifier_reset_ex(mmd, false);
    }
  }
  else if (mmd->type & MOD_MANTA_TYPE_EFFEC) {
    if (scene_framenr >= mmd->time) {
      mantaModifier_init(mmd, depsgraph, ob, scene, me);
    }

    if (mmd->effec) {
      if (mmd->effec->mesh) {
        BKE_id_free(NULL, mmd->effec->mesh);
      }
      mmd->effec->mesh = BKE_mesh_copy_for_eval(me, false);
    }

    if (scene_framenr > mmd->time) {
      mmd->time = scene_framenr;
    }
    else if (scene_framenr < mmd->time) {
      mmd->time = scene_framenr;
      mantaModifier_reset_ex(mmd, false);
    }
  }
  else if (mmd->type & MOD_MANTA_TYPE_DOMAIN) {
    MantaDomainSettings *mds = mmd->domain;
    Object *guiding_parent = NULL;
    Object **objs = NULL;
    unsigned int numobj = 0;
    MantaModifierData *mmd_parent = NULL;

#if 0
    bool is_baking = (mds->cache_flag & (FLUID_DOMAIN_BAKING_DATA | FLUID_DOMAIN_BAKING_NOISE |
                                         FLUID_DOMAIN_BAKING_MESH | FLUID_DOMAIN_BAKING_PARTICLES |
                                         FLUID_DOMAIN_BAKING_GUIDING));
    bool is_baked = (mds->cache_flag & (FLUID_DOMAIN_BAKED_DATA | FLUID_DOMAIN_BAKED_NOISE |
                                        FLUID_DOMAIN_BAKED_MESH | FLUID_DOMAIN_BAKED_PARTICLES |
                                        FLUID_DOMAIN_BAKED_GUIDING));

    /* Reset fluid if no fluid present (obviously)
     * or if timeline gets reset to startframe when no (!) baking is running
     * or if no baking is running and also there is no baked data present */
    if (!mds->fluid || (scene_framenr == startframe && !is_baking) || (!is_baking && !is_baked)) {
      mantaModifier_reset_ex(mmd, false);
    }
#endif

    bool is_startframe;
    is_startframe = (scene_framenr == mds->cache_frame_start);

    /* Reset fluid if no fluid present (obviously)
     * or if timeline gets reset to startframe */
     if (!mds->fluid || is_startframe) {
       mantaModifier_reset_ex(mmd, false);
     }

    mantaModifier_init(mmd, depsgraph, ob, scene, me);

    /* ensure that time parameters are initialized correctly before every step */
    float fps = scene->r.frs_sec / scene->r.frs_sec_base;
    mds->frame_length = DT_DEFAULT * (25.0f / fps) * mds->time_scale;
    mds->dt = mds->frame_length;
    mds->time_per_frame = 0;
    mds->time_total = (scene_framenr-1) * mds->frame_length;

    /* Guiding parent res pointer needs initialization */
    guiding_parent = mds->guiding_parent;
    if (guiding_parent) {
      mmd_parent = (MantaModifierData *)modifiers_findByType(guiding_parent, eModifierType_Manta);
      if (mmd_parent->domain) {
        mds->guide_res = mmd_parent->domain->res;
      }
    }

    /* Cache does not keep track of active fields yet. So refresh them here */
    objs = BKE_collision_objects_create(
        depsgraph, ob, mds->fluid_group, &numobj, eModifierType_Manta);
    update_flowsflags(mds, objs, numobj);
    if (objs) {
      MEM_freeN(objs);
    }

    objs = BKE_collision_objects_create(
        depsgraph, ob, mds->coll_group, &numobj, eModifierType_Manta);
    update_obstacleflags(mds, objs, numobj);
    if (objs) {
      MEM_freeN(objs);
    }

    /* Ensure cache directory is not relative */
    const char *relbase = modifier_path_relbase_from_global(ob);
    BLI_path_abs(mds->cache_directory, relbase);

    int data_frame = scene_framenr, noise_frame = scene_framenr;
    int mesh_frame = scene_framenr, particles_frame = scene_framenr, guiding_frame = scene_framenr;

    bool with_smoke, with_liquid;
    with_smoke  = mds->type == FLUID_DOMAIN_TYPE_GAS;
    with_liquid = mds->type == FLUID_DOMAIN_TYPE_LIQUID;

    bool drops, bubble, floater;
    drops   = mds->particle_type & FLUID_DOMAIN_PARTICLE_SPRAY;
    bubble  = mds->particle_type & FLUID_DOMAIN_PARTICLE_BUBBLE;
    floater = mds->particle_type & FLUID_DOMAIN_PARTICLE_FOAM;

    bool with_script, with_adaptive, with_noise, with_mesh, with_particles, with_guiding;
    with_script    = mds->flags & FLUID_DOMAIN_EXPORT_MANTA_SCRIPT;
    with_adaptive  = mds->flags & FLUID_DOMAIN_USE_ADAPTIVE_DOMAIN;
    with_noise     = mds->flags & FLUID_DOMAIN_USE_NOISE;
    with_mesh      = mds->flags & FLUID_DOMAIN_USE_MESH;
    with_guiding   = mds->flags & FLUID_DOMAIN_USE_GUIDING;
    with_particles = drops || bubble || floater;

    bool has_config, has_data, has_noise, has_mesh, has_particles, has_guiding;
    has_config = has_data = has_noise = has_mesh = has_particles = has_guiding = false;

    bool baking_data, baking_noise, baking_mesh, baking_particles, baking_guiding, bake_outdated;
    baking_data      = mds->cache_flag & FLUID_DOMAIN_BAKING_DATA;
    baking_noise     = mds->cache_flag & FLUID_DOMAIN_BAKING_NOISE;
    baking_mesh      = mds->cache_flag & FLUID_DOMAIN_BAKING_MESH;
    baking_particles = mds->cache_flag & FLUID_DOMAIN_BAKING_PARTICLES;
    baking_guiding   = mds->cache_flag & FLUID_DOMAIN_BAKING_GUIDING;
    bake_outdated    = mds->cache_flag & FLUID_DOMAIN_CACHE_OUTDATED;

    bool resume_data, resume_noise, resume_mesh, resume_particles, resume_guiding;
    resume_data      = (!is_startframe) && (mds->cache_frame_pause_data == scene_framenr);
    resume_noise     = (!is_startframe) && (mds->cache_frame_pause_noise == scene_framenr);
    resume_mesh      = (!is_startframe) && (mds->cache_frame_pause_mesh == scene_framenr);
    resume_particles = (!is_startframe) && (mds->cache_frame_pause_particles == scene_framenr);
    resume_guiding   = (!is_startframe) && (mds->cache_frame_pause_guiding == scene_framenr);

    bool read_cache, bake_cache;
    read_cache = false, bake_cache = baking_data || baking_noise || baking_mesh || baking_particles;

    bool with_gdomain;
    with_gdomain = (mds->guiding_source == FLUID_DOMAIN_GUIDING_SRC_DOMAIN);

    int mode = mds->cache_type;
    int prev_frame = scene_framenr - 1;
    int o_res[3], o_min[3], o_max[3], o_shift[3];

    /* Cache mode specific settings */
    switch (mode)
    {
      case FLUID_DOMAIN_CACHE_FINAL:
        /* Just load the data that has already been baked */
        if (!baking_data && !baking_noise && !baking_mesh && !baking_particles) {
          read_cache = true;
          bake_cache = false;
          break;
        }
      case FLUID_DOMAIN_CACHE_MODULAR:
      {
        /* Just load the data that has already been baked */
        if (!baking_data && !baking_noise && !baking_mesh && !baking_particles) {
          read_cache = true;
          bake_cache = false;
          break;
        }

        /* Set to previous frame if the bake was resumed
         * ie don't read all of the already baked frames, just the one before bake resumes */
        if (baking_data && resume_data) {
          data_frame = prev_frame;
        }
        if (baking_noise && resume_noise) {
          noise_frame = prev_frame;
        }
        if (baking_mesh && resume_mesh) {
          mesh_frame = prev_frame;
        }
        if (baking_particles && resume_particles) {
          particles_frame = prev_frame;
        }
        if (baking_guiding && resume_guiding) {
          guiding_frame = prev_frame;
        }

        /* Force to read cache as we're resuming the bake */
        read_cache = true;
        break;
      }
      case FLUID_DOMAIN_CACHE_REPLAY:
      default:
        /* Always trying to read the cache in replay mode */
        read_cache = true;
        break;
    }

    /* Cache outdated? If so, don't read, just bake */
    if (bake_outdated) {
        read_cache = false;
        bake_cache = true;
    }

    /* Try to read from cache and keep track of read success */
    if (read_cache)
    {
      /* Read noise cache */
      if (with_smoke && with_noise) {
        /* Read config cache */
        copy_v3_v3_int(o_res, mds->res);
        copy_v3_v3_int(o_min, mds->res_min);
        copy_v3_v3_int(o_max, mds->res_max);
        copy_v3_v3_int(o_shift, mds->shift);
        if (manta_read_config(mds->fluid, mmd, noise_frame) && manta_needs_realloc(mds->fluid, mmd)) {
          BKE_manta_reallocate_copy_fluid(mds, o_res, mds->res, o_min, mds->res_min, o_max, o_shift, mds->shift);
        }
        has_noise = manta_read_noise(mds->fluid, mmd, noise_frame);
      }

      /* Read mesh cache */
      if (with_liquid && with_mesh) {
        has_mesh = manta_read_mesh(mds->fluid, mmd, mesh_frame);
      }

      /* Read particles cache */
      if (with_liquid && with_particles) {
        has_particles = manta_read_particles(mds->fluid, mmd, particles_frame);
      }

      /* Read guiding cache */
      if (with_guiding) {
        MantaModifierData* mmd2 = (with_gdomain) ? mmd_parent : mmd;
        has_guiding = manta_read_guiding(mds->fluid, mmd2, scene_framenr, with_gdomain);
      }

      /* Read config cache */
      if (manta_read_config(mds->fluid, mmd, data_frame) && manta_needs_realloc(mds->fluid, mmd)) {
          BKE_manta_reallocate_fluid(mds, mds->res, 1);
      }

      /* Read data cache */
      has_data = manta_read_data(mds->fluid, mmd, data_frame);
    }

    /* Cache mode specific settings */
    switch (mode)
    {
      case FLUID_DOMAIN_CACHE_FINAL:
      case FLUID_DOMAIN_CACHE_MODULAR:
        break;
      case FLUID_DOMAIN_CACHE_REPLAY:
      default:
        baking_data = !has_data;
        if (with_smoke && with_noise) {
          baking_noise = !has_noise;
        }
        if (with_liquid && with_mesh) {
          baking_mesh = !has_mesh;
        }
        if (with_liquid && with_particles) {
          baking_particles = !has_particles;
        }

        bake_cache = baking_data || baking_noise || baking_mesh || baking_particles;
        break;
    }

    /* Trigger bake calls individually */
    if (bake_cache)
    {
      /* Ensure fresh variables at every animation step */
      manta_update_variables(mds->fluid, mmd);

      /* Export mantaflow python script on first frame (once only) and for any bake type */
      if (with_script && is_startframe) {
        if (with_smoke) {
          manta_smoke_export_script(mmd->domain->fluid, mmd);
        }
        if (with_liquid) {
          manta_liquid_export_script(mmd->domain->fluid, mmd);
        }
      }

      if (baking_guiding) {
        manta_guiding(depsgraph, scene, ob, mmd, scene_framenr);
      }
      if (baking_data) {
        manta_step(depsgraph, scene, ob, me, mmd, scene_framenr);
        manta_write_config(mds->fluid, mmd, scene_framenr);
        manta_write_data(mds->fluid, mmd, scene_framenr);
      }
      if (has_data || baking_data) {
        if (baking_noise) {
          manta_bake_noise(mds->fluid, mmd, scene_framenr);
        }
        if (baking_mesh) {
          manta_bake_mesh(mds->fluid, mmd, scene_framenr);
        }
        if (baking_particles) {
          manta_bake_particles(mds->fluid, mmd, scene_framenr);
        }
      }
      mds->cache_flag &= ~FLUID_DOMAIN_CACHE_OUTDATED;
    }

#if 0
    /* Read cache. For liquids update data directly (i.e. not via python) */
    if (!is_baking) {
      if (mds->cache_flag & FLUID_DOMAIN_BAKED_DATA)
      {
        if (mds->type == FLUID_DOMAIN_TYPE_GAS) {
          if (manta_read_config(mds->fluid, mmd, scene_framenr)) {
            /* Adaptive domain might have changed resolution */
            if (manta_needs_realloc(mds->fluid, mmd)) {
              BKE_manta_reallocate_fluid(mds, mds->res, 1);
            }
            manta_read_data(mds->fluid, mmd, scene_framenr);
          }
        }
        if (mds->type == FLUID_DOMAIN_TYPE_LIQUID) {
          manta_update_liquid_structures(mds->fluid, mmd, scene_framenr);
        }
      }
      if (mds->cache_flag & FLUID_DOMAIN_BAKED_NOISE)
      {
        if (mds->type == FLUID_DOMAIN_TYPE_GAS) {
          if (manta_read_config(mds->fluid, mmd, scene_framenr)) {
            if (manta_needs_realloc(mds->fluid, mmd)) {
              BKE_manta_reallocate_fluid(mds, mds->res, 1);
            }
            manta_read_noise(mds->fluid, mmd, scene_framenr);
          }
        }
      }
      if (mds->cache_flag & FLUID_DOMAIN_BAKED_MESH)
      {
        //if (mds->type == FLUID_DOMAIN_TYPE_GAS)
        // TODO (sebbas): smoke as mesh
        if (mds->type == FLUID_DOMAIN_TYPE_LIQUID) {
          manta_update_mesh_structures(mds->fluid, mmd, scene_framenr);
        }
      }
      if (mds->cache_flag & FLUID_DOMAIN_BAKED_PARTICLES)
      {
        //if (mds->type == FLUID_DOMAIN_TYPE_GAS)
        // TODO (sebbas): fire particles
        if (mds->type == FLUID_DOMAIN_TYPE_LIQUID) {
          manta_update_particle_structures(mds->fluid, mmd, scene_framenr);
        }
      }
    }

    /* Simulate step and write cache. Optionally also read py objects once from previous frame (bake started from resume operator) */
    if (is_baking) {
      /* Ensure fresh variables at every animation step */
      manta_update_variables(mds->fluid, mmd);

      /* Export mantaflow python script on first frame (once only) and for any bake type */
      if ((mds->flags & FLUID_DOMAIN_EXPORT_MANTA_SCRIPT) &&
          scene_framenr == mds->cache_frame_start)
      {
        if (mmd->domain && mmd->domain->type == FLUID_DOMAIN_TYPE_GAS) {
          manta_smoke_export_script(mmd->domain->fluid, mmd);
        }
        if (mmd->domain && mmd->domain->type == FLUID_DOMAIN_TYPE_LIQUID) {
          manta_liquid_export_script(mmd->domain->fluid, mmd);
        }
      }

      if (mds->cache_flag & FLUID_DOMAIN_BAKING_DATA)
      {
        if (mds->flags & FLUID_DOMAIN_USE_GUIDING) {
          /* Load guiding vel from flow object (only if baked) or else from domain object */
          if (mds->guiding_source == FLUID_DOMAIN_GUIDING_SRC_EFFECTOR &&
              mds->cache_flag & FLUID_DOMAIN_BAKED_GUIDING) {
            manta_read_guiding(mds->fluid, mmd, scene_framenr, false);
          }
          else if (mds->guiding_source == FLUID_DOMAIN_GUIDING_SRC_DOMAIN && mmd_parent) {
            manta_read_guiding(mds->fluid, mmd_parent, scene_framenr, true);
          }
        }

        /* Refresh all objects if we start baking from a resumed frame */
        if (mds->cache_frame_start != scene_framenr &&
            mds->cache_frame_pause_data == scene_framenr) {
          /* Adaptive domain might have changed resolution */
          manta_read_config(mds->fluid, mmd, scene_framenr - 1);
          if (manta_needs_realloc(mds->fluid, mmd)) {
            BKE_manta_reallocate_fluid(mds, mds->res, 1);
          }
          manta_read_data(mds->fluid, mmd, scene_framenr - 1);
        }

        /* Base step needs separated bake and write calls as transparency calculation happens after fluid step */
        manta_step(depsgraph, scene, ob, me, mmd, scene_framenr);
        manta_write_config(mds->fluid, mmd, scene_framenr);
        manta_write_data(mds->fluid, mmd, scene_framenr);
      }
      if (mds->cache_flag & FLUID_DOMAIN_BAKING_NOISE)
      {
        if (mds->type == FLUID_DOMAIN_TYPE_GAS) {
          /* Refresh all objects if we start baking from a resumed frame */
          if (mds->cache_frame_start != scene_framenr &&
              mds->cache_frame_pause_noise == scene_framenr) {
            /* Adaptive domain might have changed resolution */
            manta_read_config(mds->fluid, mmd, scene_framenr - 1);
            if (manta_needs_realloc(mds->fluid, mmd)) {
              BKE_manta_reallocate_fluid(mds, mds->res, 1);
            }
            manta_read_data(mds->fluid, mmd, scene_framenr - 1);
            manta_read_noise(mds->fluid, mmd, scene_framenr - 1);
          }
          if (mds->flags & FLUID_DOMAIN_USE_ADAPTIVE_DOMAIN) {
            int o_res[3], o_min[3], o_max[3], o_shift[3];
            copy_v3_v3_int(o_res, mds->res);
            copy_v3_v3_int(o_min, mds->res_min);
            copy_v3_v3_int(o_max, mds->res_max);
            copy_v3_v3_int(o_shift, mds->shift);
            manta_read_config(mds->fluid, mmd, scene_framenr);
            if (manta_needs_realloc(mds->fluid, mmd)) {
              /* Copy function also handles realloc of MANTA object */
              BKE_manta_reallocate_copy_fluid(mds, o_res, mds->res, o_min, mds->res_min, o_max, o_shift, mds->shift);
            }
          }
          manta_bake_noise(mds->fluid, mmd, scene_framenr);
        }
      }
      if (mds->cache_flag & FLUID_DOMAIN_BAKING_MESH)
      {
        if (mds->type == FLUID_DOMAIN_TYPE_LIQUID) {
          /* Note: Mesh bake does not need object refresh from cache */
          manta_bake_mesh(mds->fluid, mmd, scene_framenr);
        }
      }
      if (mds->cache_flag & FLUID_DOMAIN_BAKING_PARTICLES)
      {
        if (mds->type == FLUID_DOMAIN_TYPE_LIQUID) {
          /* Refresh all objects if we start baking from a resumed frame */
          if (mds->cache_frame_start != scene_framenr &&
              mds->cache_frame_pause_particles == scene_framenr) {
            manta_read_particles(mds->fluid, mmd, scene_framenr - 1);
          }
          manta_bake_particles(mds->fluid, mmd, scene_framenr);
        }
      }
      if (mds->cache_flag & FLUID_DOMAIN_BAKING_GUIDING)
      {
        manta_guiding(depsgraph, scene, ob, mmd, scene_framenr);
      }
    }
#endif

    mmd->time = scene_framenr;
  }
}

struct Mesh *mantaModifier_do(
    MantaModifierData *mmd, Depsgraph *depsgraph, Scene *scene, Object *ob, Mesh *me)
{
  /* lock so preview render does not read smoke data while it gets modified */
  if ((mmd->type & MOD_MANTA_TYPE_DOMAIN) && mmd->domain) {
    BLI_rw_mutex_lock(mmd->domain->fluid_mutex, THREAD_LOCK_WRITE);
  }

  mantaModifier_process(mmd, depsgraph, scene, ob, me);

  if ((mmd->type & MOD_MANTA_TYPE_DOMAIN) && mmd->domain) {
    BLI_rw_mutex_unlock(mmd->domain->fluid_mutex);
  }

  /* return generated geometry for adaptive domain */
  Mesh *result = NULL;
  if (mmd->type & MOD_MANTA_TYPE_DOMAIN && mmd->domain)
  {
    if (mmd->domain->type == FLUID_DOMAIN_TYPE_LIQUID) {
      result = createLiquidGeometry(mmd->domain, me, ob);
    }
    if (mmd->domain->type == FLUID_DOMAIN_TYPE_GAS) {
      result = createSmokeGeometry(mmd->domain, me, ob);
    }
  }
  if (!result) {
    result = BKE_mesh_copy_for_eval(me, false);
  }
  else {
    BKE_mesh_copy_settings(result, me);
  }

  /* Liquid simulation has a texture space that based on the bounds of the fluid mesh.
   * This does not seem particularly useful, but it's backwards compatible.
   *
   * Smoke simulation needs a texture space relative to the adaptive domain bounds, not the
   * original mesh. So recompute it at this point in the modifier stack. See T58492. */
  BKE_mesh_texspace_calc(result);

  return result;
}

static float calc_voxel_transp(
    float *result, float *input, int res[3], int *pixel, float *tRay, float correct)
{
  const size_t index = manta_get_index(pixel[0], res[0], pixel[1], res[1], pixel[2]);

  // T_ray *= T_vox
  *tRay *= expf(input[index] * correct);

  if (result[index] < 0.0f) {
    result[index] = *tRay;
  }

  return *tRay;
}

static void bresenham_linie_3D(int x1,
                               int y1,
                               int z1,
                               int x2,
                               int y2,
                               int z2,
                               float *tRay,
                               bresenham_callback cb,
                               float *result,
                               float *input,
                               int res[3],
                               float correct)
{
  int dx, dy, dz, i, l, m, n, x_inc, y_inc, z_inc, err_1, err_2, dx2, dy2, dz2;
  int pixel[3];

  pixel[0] = x1;
  pixel[1] = y1;
  pixel[2] = z1;

  dx = x2 - x1;
  dy = y2 - y1;
  dz = z2 - z1;

  x_inc = (dx < 0) ? -1 : 1;
  l = abs(dx);
  y_inc = (dy < 0) ? -1 : 1;
  m = abs(dy);
  z_inc = (dz < 0) ? -1 : 1;
  n = abs(dz);
  dx2 = l << 1;
  dy2 = m << 1;
  dz2 = n << 1;

  if ((l >= m) && (l >= n)) {
    err_1 = dy2 - l;
    err_2 = dz2 - l;
    for (i = 0; i < l; i++) {
      if (cb(result, input, res, pixel, tRay, correct) <= FLT_EPSILON) {
        break;
      }
      if (err_1 > 0) {
        pixel[1] += y_inc;
        err_1 -= dx2;
      }
      if (err_2 > 0) {
        pixel[2] += z_inc;
        err_2 -= dx2;
      }
      err_1 += dy2;
      err_2 += dz2;
      pixel[0] += x_inc;
    }
  }
  else if ((m >= l) && (m >= n)) {
    err_1 = dx2 - m;
    err_2 = dz2 - m;
    for (i = 0; i < m; i++) {
      if (cb(result, input, res, pixel, tRay, correct) <= FLT_EPSILON) {
        break;
      }
      if (err_1 > 0) {
        pixel[0] += x_inc;
        err_1 -= dy2;
      }
      if (err_2 > 0) {
        pixel[2] += z_inc;
        err_2 -= dy2;
      }
      err_1 += dx2;
      err_2 += dz2;
      pixel[1] += y_inc;
    }
  }
  else {
    err_1 = dy2 - n;
    err_2 = dx2 - n;
    for (i = 0; i < n; i++) {
      if (cb(result, input, res, pixel, tRay, correct) <= FLT_EPSILON) {
        break;
      }
      if (err_1 > 0) {
        pixel[1] += y_inc;
        err_1 -= dz2;
      }
      if (err_2 > 0) {
        pixel[0] += x_inc;
        err_2 -= dz2;
      }
      err_1 += dy2;
      err_2 += dx2;
      pixel[2] += z_inc;
    }
  }
  cb(result, input, res, pixel, tRay, correct);
}

static void manta_smoke_calc_transparency(MantaDomainSettings *mds, ViewLayer *view_layer)
{
  float bv[6] = {0};
  float light[3];
  int a, z, slabsize = mds->res[0] * mds->res[1], size = mds->res[0] * mds->res[1] * mds->res[2];
  float *density = manta_smoke_get_density(mds->fluid);
  float *shadow = manta_smoke_get_shadow(mds->fluid);
  float correct = -7.0f * mds->dx;

  if (!get_light(view_layer, light)) {
    return;
  }

  /* convert light pos to sim cell space */
  mul_m4_v3(mds->imat, light);
  light[0] = (light[0] - mds->p0[0]) / mds->cell_size[0] - 0.5f - (float)mds->res_min[0];
  light[1] = (light[1] - mds->p0[1]) / mds->cell_size[1] - 0.5f - (float)mds->res_min[1];
  light[2] = (light[2] - mds->p0[2]) / mds->cell_size[2] - 0.5f - (float)mds->res_min[2];

  for (a = 0; a < size; a++) {
    shadow[a] = -1.0f;
  }

  /* calculate domain bounds in sim cell space */
  // 0,2,4 = 0.0f
  bv[1] = (float)mds->res[0];  // x
  bv[3] = (float)mds->res[1];  // y
  bv[5] = (float)mds->res[2];  // z

  for (z = 0; z < mds->res[2]; z++) {
    size_t index = z * slabsize;
    int x, y;

    for (y = 0; y < mds->res[1]; y++) {
      for (x = 0; x < mds->res[0]; x++, index++) {
        float voxelCenter[3];
        float pos[3];
        int cell[3];
        float tRay = 1.0;

        if (shadow[index] >= 0.0f) {
          continue;
        }
        voxelCenter[0] = (float)x;
        voxelCenter[1] = (float)y;
        voxelCenter[2] = (float)z;

        // get starting cell (light pos)
        if (BLI_bvhtree_bb_raycast(bv, light, voxelCenter, pos) > FLT_EPSILON) {
          // we're ouside -> use point on side of domain
          cell[0] = (int)floor(pos[0]);
          cell[1] = (int)floor(pos[1]);
          cell[2] = (int)floor(pos[2]);
        }
        else {
          // we're inside -> use light itself
          cell[0] = (int)floor(light[0]);
          cell[1] = (int)floor(light[1]);
          cell[2] = (int)floor(light[2]);
        }
        /* clamp within grid bounds */
        CLAMP(cell[0], 0, mds->res[0] - 1);
        CLAMP(cell[1], 0, mds->res[1] - 1);
        CLAMP(cell[2], 0, mds->res[2] - 1);

        bresenham_linie_3D(cell[0],
                           cell[1],
                           cell[2],
                           x,
                           y,
                           z,
                           &tRay,
                           calc_voxel_transp,
                           shadow,
                           density,
                           mds->res,
                           correct);

        // convention -> from a RGBA float array, use G value for tRay
        shadow[index] = tRay;
      }
    }
  }
}

/* get smoke velocity and density at given coordinates
 * returns fluid density or -1.0f if outside domain. */
float BKE_manta_get_velocity_at(struct Object *ob, float position[3], float velocity[3])
{
  MantaModifierData *mmd = (MantaModifierData *)modifiers_findByType(ob, eModifierType_Manta);
  zero_v3(velocity);

  if (mmd && (mmd->type & MOD_MANTA_TYPE_DOMAIN) && mmd->domain && mmd->domain->fluid) {
    MantaDomainSettings *mds = mmd->domain;
    float time_mult = 25.f * DT_DEFAULT;
    float vel_mag;
    float *velX = manta_get_velocity_x(mds->fluid);
    float *velY = manta_get_velocity_y(mds->fluid);
    float *velZ = manta_get_velocity_z(mds->fluid);
    float density = 0.0f, fuel = 0.0f;
    float pos[3];
    copy_v3_v3(pos, position);
    manta_pos_to_cell(mds, pos);

    /* check if point is outside domain max bounds */
    if (pos[0] < mds->res_min[0] || pos[1] < mds->res_min[1] || pos[2] < mds->res_min[2]) {
      return -1.0f;
    }
    if (pos[0] > mds->res_max[0] || pos[1] > mds->res_max[1] || pos[2] > mds->res_max[2]) {
      return -1.0f;
    }

    /* map pos between 0.0 - 1.0 */
    pos[0] = (pos[0] - mds->res_min[0]) / ((float)mds->res[0]);
    pos[1] = (pos[1] - mds->res_min[1]) / ((float)mds->res[1]);
    pos[2] = (pos[2] - mds->res_min[2]) / ((float)mds->res[2]);

    /* check if point is outside active area */
    if (mmd->domain->type == FLUID_DOMAIN_TYPE_GAS &&
        mmd->domain->flags & FLUID_DOMAIN_USE_ADAPTIVE_DOMAIN)
    {
      if (pos[0] < 0.0f || pos[1] < 0.0f || pos[2] < 0.0f) {
        return 0.0f;
      }
      if (pos[0] > 1.0f || pos[1] > 1.0f || pos[2] > 1.0f) {
        return 0.0f;
      }
    }

    /* get interpolated velocity */
    velocity[0] = BLI_voxel_sample_trilinear(velX, mds->res, pos) * mds->global_size[0] *
                  time_mult;
    velocity[1] = BLI_voxel_sample_trilinear(velY, mds->res, pos) * mds->global_size[1] *
                  time_mult;
    velocity[2] = BLI_voxel_sample_trilinear(velZ, mds->res, pos) * mds->global_size[2] *
                  time_mult;

    /* convert velocity direction to global space */
    vel_mag = len_v3(velocity);
    mul_mat3_m4_v3(mds->obmat, velocity);
    normalize_v3(velocity);
    mul_v3_fl(velocity, vel_mag);

    /* use max value of fuel or smoke density */
    density = BLI_voxel_sample_trilinear(manta_smoke_get_density(mds->fluid), mds->res, pos);
    if (manta_smoke_has_fuel(mds->fluid)) {
      fuel = BLI_voxel_sample_trilinear(manta_smoke_get_fuel(mds->fluid), mds->res, pos);
    }
    return MAX2(density, fuel);
  }
  return -1.0f;
}

int BKE_manta_get_data_flags(MantaDomainSettings *mds)
{
  int flags = 0;

  if (mds->fluid) {
    if (manta_smoke_has_heat(mds->fluid)) {
      flags |= FLUID_DOMAIN_ACTIVE_HEAT;
    }
    if (manta_smoke_has_fuel(mds->fluid)) {
      flags |= FLUID_DOMAIN_ACTIVE_FIRE;
    }
    if (manta_smoke_has_colors(mds->fluid)) {
      flags |= FLUID_DOMAIN_ACTIVE_COLORS;
    }
  }

  return flags;
}


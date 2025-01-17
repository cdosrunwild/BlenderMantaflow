/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
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
 * The Original Code is Copyright (C) 2016 Blender Foundation.
 * All rights reserved.
 *
 * Contributor(s): Sebastian Barschkis (sebbas)
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file mantaflow/extern/manta_smoke_API.h
 *  \ingroup mantaflow
 */

#ifndef MANTA_API_H
#define MANTA_API_H

#ifdef __cplusplus
extern "C" {
#endif

struct MANTA;

/* Fluid functions */
struct MANTA *manta_init(int *res, struct MantaModifierData *mmd);
void manta_free(struct MANTA *fluid);
void manta_ensure_obstacle(struct MANTA *fluid, struct MantaModifierData *mmd);
void manta_ensure_guiding(struct MANTA *fluid, struct MantaModifierData *mmd);
void manta_ensure_invelocity(struct MANTA *fluid, struct MantaModifierData *mmd);
void manta_ensure_outflow(struct MANTA *fluid, struct MantaModifierData *mmd);
int manta_write_config(struct MANTA *fluid, struct MantaModifierData *mmd, int framenr);
int manta_write_data(struct MANTA *fluid, struct MantaModifierData *mmd, int framenr);
int manta_read_config(struct MANTA *fluid, struct MantaModifierData *mmd, int framenr);
int manta_read_data(struct MANTA *fluid, struct MantaModifierData *mmd, int framenr);
int manta_read_noise(struct MANTA *fluid, struct MantaModifierData *mmd, int framenr);
int manta_read_mesh(struct MANTA *fluid, struct MantaModifierData *mmd, int framenr);
int manta_read_particles(struct MANTA *fluid, struct MantaModifierData *mmd, int framenr);
int manta_read_guiding(struct MANTA *fluid,
                       struct MantaModifierData *mmd,
                       int framenr,
                       bool sourceDomain);
int manta_update_liquid_structures(struct MANTA *fluid,
                                   struct MantaModifierData *mmd,
                                   int framenr);
int manta_update_mesh_structures(struct MANTA *fluid, struct MantaModifierData *mmd, int framenr);
int manta_update_particle_structures(struct MANTA *fluid,
                                     struct MantaModifierData *mmd,
                                     int framenr);
int manta_bake_data(struct MANTA *fluid, struct MantaModifierData *mmd, int framenr);
int manta_bake_noise(struct MANTA *fluid, struct MantaModifierData *mmd, int framenr);
int manta_bake_mesh(struct MANTA *fluid, struct MantaModifierData *mmd, int framenr);
int manta_bake_particles(struct MANTA *fluid, struct MantaModifierData *mmd, int framenr);
int manta_bake_guiding(struct MANTA *fluid, struct MantaModifierData *mmd, int framenr);
void manta_update_variables(struct MANTA *fluid, struct MantaModifierData *mmd);
int manta_get_frame(struct MANTA *fluid);
float manta_get_timestep(struct MANTA *fluid);
void manta_adapt_timestep(struct MANTA *fluid);
bool manta_needs_realloc(struct MANTA *fluid, struct MantaModifierData *mmd);

/* Fluid accessors */
size_t manta_get_index(int x, int max_x, int y, int max_y, int z /*, int max_z */);
size_t manta_get_index2d(int x, int max_x, int y /*, int max_y, int z, int max_z */);
float *manta_get_velocity_x(struct MANTA *fluid);
float *manta_get_velocity_y(struct MANTA *fluid);
float *manta_get_velocity_z(struct MANTA *fluid);
float *manta_get_ob_velocity_x(struct MANTA *fluid);
float *manta_get_ob_velocity_y(struct MANTA *fluid);
float *manta_get_ob_velocity_z(struct MANTA *fluid);
float *manta_get_guide_velocity_x(struct MANTA *fluid);
float *manta_get_guide_velocity_y(struct MANTA *fluid);
float *manta_get_guide_velocity_z(struct MANTA *fluid);
float *manta_get_in_velocity_x(struct MANTA *fluid);
float *manta_get_in_velocity_y(struct MANTA *fluid);
float *manta_get_in_velocity_z(struct MANTA *fluid);
float *manta_get_force_x(struct MANTA *fluid);
float *manta_get_force_y(struct MANTA *fluid);
float *manta_get_force_z(struct MANTA *fluid);
float *manta_get_phiguide_in(struct MANTA *fluid);
int *manta_get_num_obstacle(struct MANTA *fluid);
int *manta_get_num_guide(struct MANTA *fluid);
int manta_get_res_x(struct MANTA *fluid);
int manta_get_res_y(struct MANTA *fluid);
int manta_get_res_z(struct MANTA *fluid);
float *manta_get_phi_in(struct MANTA *fluid);
float *manta_get_phiobs_in(struct MANTA *fluid);
float *manta_get_phiout_in(struct MANTA *fluid);

/* Smoke functions */
void manta_smoke_export_script(struct MANTA *smoke, struct MantaModifierData *mmd);
void manta_smoke_export(struct MANTA *smoke,
                  float *dt,
                  float *dx,
                  float **dens,
                  float **react,
                  float **flame,
                  float **fuel,
                  float **heat,
                  float **vx,
                  float **vy,
                  float **vz,
                  float **r,
                  float **g,
                  float **b,
                  int **obstacles,
                  float **shadow);
void manta_smoke_turbulence_export(struct MANTA *smoke,
                             float **dens,
                             float **react,
                             float **flame,
                             float **fuel,
                             float **r,
                             float **g,
                             float **b,
                             float **tcu,
                             float **tcv,
                             float **tcw,
                             float **tcu2,
                             float **tcv2,
                             float **tcw2);
void manta_smoke_get_rgba(struct MANTA *smoke, float *data, int sequential);
void manta_smoke_turbulence_get_rgba(struct MANTA *smoke, float *data, int sequential);
void manta_smoke_get_rgba_from_density(struct MANTA *smoke, float color[3], float *data, int sequential);
void manta_smoke_turbulence_get_rgba_from_density(struct MANTA *smoke,
                                            float color[3],
                                            float *data,
                                            int sequential);
void manta_smoke_ensure_heat(struct MANTA *smoke, struct MantaModifierData *mmd);
void manta_smoke_ensure_fire(struct MANTA *smoke, struct MantaModifierData *mmd);
void manta_smoke_ensure_colors(struct MANTA *smoke, struct MantaModifierData *mmd);

/* Smoke accessors */
float *manta_smoke_get_density(struct MANTA *smoke);
float *manta_smoke_get_fuel(struct MANTA *smoke);
float *manta_smoke_get_react(struct MANTA *smoke);
float *manta_smoke_get_heat(struct MANTA *smoke);
float *manta_smoke_get_flame(struct MANTA *smoke);
float *manta_smoke_get_shadow(struct MANTA *fluid);
float *manta_smoke_get_color_r(struct MANTA *smoke);
float *manta_smoke_get_color_g(struct MANTA *smoke);
float *manta_smoke_get_color_b(struct MANTA *smoke);
int *manta_smoke_get_obstacle(struct MANTA *smoke);
float *manta_smoke_get_density_in(struct MANTA *smoke);
float *manta_smoke_get_heat_in(struct MANTA *smoke);
float *manta_smoke_get_color_r_in(struct MANTA *smoke);
float *manta_smoke_get_color_g_in(struct MANTA *smoke);
float *manta_smoke_get_color_b_in(struct MANTA *smoke);
float *manta_smoke_get_fuel_in(struct MANTA *smoke);
float *manta_smoke_get_react_in(struct MANTA *smoke);
float *manta_smoke_get_emission_in(struct MANTA *smoke);
int manta_smoke_has_heat(struct MANTA *smoke);
int manta_smoke_has_fuel(struct MANTA *smoke);
int manta_smoke_has_colors(struct MANTA *smoke);
float *manta_smoke_turbulence_get_density(struct MANTA *smoke);
float *manta_smoke_turbulence_get_fuel(struct MANTA *smoke);
float *manta_smoke_turbulence_get_react(struct MANTA *smoke);
float *manta_smoke_turbulence_get_color_r(struct MANTA *smoke);
float *manta_smoke_turbulence_get_color_g(struct MANTA *smoke);
float *manta_smoke_turbulence_get_color_b(struct MANTA *smoke);
float *manta_smoke_turbulence_get_flame(struct MANTA *smoke);
int manta_smoke_turbulence_has_fuel(struct MANTA *smoke);
int manta_smoke_turbulence_has_colors(struct MANTA *smoke);
void manta_smoke_turbulence_get_res(struct MANTA *smoke, int *res);
int manta_smoke_turbulence_get_cells(struct MANTA *smoke);

/* Liquid functions */
void manta_liquid_export_script(struct MANTA *smoke, struct MantaModifierData *mmd);
void manta_liquid_ensure_sndparts(struct MANTA *fluid, struct MantaModifierData *mmd);

/* Liquid accessors */
int manta_liquid_get_particle_res_x(struct MANTA *liquid);
int manta_liquid_get_particle_res_y(struct MANTA *liquid);
int manta_liquid_get_particle_res_z(struct MANTA *liquid);
int manta_liquid_get_mesh_res_x(struct MANTA *liquid);
int manta_liquid_get_mesh_res_y(struct MANTA *liquid);
int manta_liquid_get_mesh_res_z(struct MANTA *liquid);
int manta_liquid_get_particle_upres(struct MANTA *liquid);
int manta_liquid_get_mesh_upres(struct MANTA *liquid);
int manta_liquid_get_num_verts(struct MANTA *liquid);
int manta_liquid_get_num_normals(struct MANTA *liquid);
int manta_liquid_get_num_triangles(struct MANTA *liquid);
float manta_liquid_get_vertex_x_at(struct MANTA *liquid, int i);
float manta_liquid_get_vertex_y_at(struct MANTA *liquid, int i);
float manta_liquid_get_vertex_z_at(struct MANTA *liquid, int i);
float manta_liquid_get_normal_x_at(struct MANTA *liquid, int i);
float manta_liquid_get_normal_y_at(struct MANTA *liquid, int i);
float manta_liquid_get_normal_z_at(struct MANTA *liquid, int i);
int manta_liquid_get_triangle_x_at(struct MANTA *liquid, int i);
int manta_liquid_get_triangle_y_at(struct MANTA *liquid, int i);
int manta_liquid_get_triangle_z_at(struct MANTA *liquid, int i);
float manta_liquid_get_vertvel_x_at(struct MANTA *liquid, int i);
float manta_liquid_get_vertvel_y_at(struct MANTA *liquid, int i);
float manta_liquid_get_vertvel_z_at(struct MANTA *liquid, int i);
int manta_liquid_get_num_flip_particles(struct MANTA *liquid);
int manta_liquid_get_num_snd_particles(struct MANTA *liquid);
int manta_liquid_get_flip_particle_flag_at(struct MANTA *liquid, int i);
int manta_liquid_get_snd_particle_flag_at(struct MANTA *liquid, int i);
float manta_liquid_get_flip_particle_position_x_at(struct MANTA *liquid, int i);
float manta_liquid_get_flip_particle_position_y_at(struct MANTA *liquid, int i);
float manta_liquid_get_flip_particle_position_z_at(struct MANTA *liquid, int i);
float manta_liquid_get_flip_particle_velocity_x_at(struct MANTA *liquid, int i);
float manta_liquid_get_flip_particle_velocity_y_at(struct MANTA *liquid, int i);
float manta_liquid_get_flip_particle_velocity_z_at(struct MANTA *liquid, int i);
float manta_liquid_get_snd_particle_position_x_at(struct MANTA *liquid, int i);
float manta_liquid_get_snd_particle_position_y_at(struct MANTA *liquid, int i);
float manta_liquid_get_snd_particle_position_z_at(struct MANTA *liquid, int i);
float manta_liquid_get_snd_particle_velocity_x_at(struct MANTA *liquid, int i);
float manta_liquid_get_snd_particle_velocity_y_at(struct MANTA *liquid, int i);
float manta_liquid_get_snd_particle_velocity_z_at(struct MANTA *liquid, int i);

#ifdef __cplusplus
}
#endif

#endif /* MANTA_API_H_ */

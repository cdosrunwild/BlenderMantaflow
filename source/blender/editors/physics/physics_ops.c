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
 * The Original Code is Copyright (C) 2009 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup edphys
 */

#include <stdlib.h>

#include "RNA_access.h"

#include "WM_api.h"
#include "WM_types.h"

#include "ED_select_utils.h"
#include "ED_physics.h"
#include "ED_object.h"

#include "physics_intern.h"  // own include

/***************************** particles ***********************************/

static void operatortypes_particle(void)
{
  WM_operatortype_append(PARTICLE_OT_select_all);
  WM_operatortype_append(PARTICLE_OT_select_roots);
  WM_operatortype_append(PARTICLE_OT_select_tips);
  WM_operatortype_append(PARTICLE_OT_select_random);
  WM_operatortype_append(PARTICLE_OT_select_linked);
  WM_operatortype_append(PARTICLE_OT_select_less);
  WM_operatortype_append(PARTICLE_OT_select_more);

  WM_operatortype_append(PARTICLE_OT_hide);
  WM_operatortype_append(PARTICLE_OT_reveal);

  WM_operatortype_append(PARTICLE_OT_rekey);
  WM_operatortype_append(PARTICLE_OT_subdivide);
  WM_operatortype_append(PARTICLE_OT_remove_doubles);
  WM_operatortype_append(PARTICLE_OT_weight_set);
  WM_operatortype_append(PARTICLE_OT_delete);
  WM_operatortype_append(PARTICLE_OT_mirror);

  WM_operatortype_append(PARTICLE_OT_brush_edit);

  WM_operatortype_append(PARTICLE_OT_shape_cut);

  WM_operatortype_append(PARTICLE_OT_particle_edit_toggle);
  WM_operatortype_append(PARTICLE_OT_edited_clear);

  WM_operatortype_append(PARTICLE_OT_unify_length);

  WM_operatortype_append(OBJECT_OT_particle_system_add);
  WM_operatortype_append(OBJECT_OT_particle_system_remove);

  WM_operatortype_append(PARTICLE_OT_new);
  WM_operatortype_append(PARTICLE_OT_new_target);
  WM_operatortype_append(PARTICLE_OT_target_remove);
  WM_operatortype_append(PARTICLE_OT_target_move_up);
  WM_operatortype_append(PARTICLE_OT_target_move_down);
  WM_operatortype_append(PARTICLE_OT_connect_hair);
  WM_operatortype_append(PARTICLE_OT_disconnect_hair);
  WM_operatortype_append(PARTICLE_OT_copy_particle_systems);
  WM_operatortype_append(PARTICLE_OT_duplicate_particle_system);

  WM_operatortype_append(PARTICLE_OT_dupliob_refresh);
  WM_operatortype_append(PARTICLE_OT_dupliob_copy);
  WM_operatortype_append(PARTICLE_OT_dupliob_remove);
  WM_operatortype_append(PARTICLE_OT_dupliob_move_up);
  WM_operatortype_append(PARTICLE_OT_dupliob_move_down);

  WM_operatortype_append(RIGIDBODY_OT_object_add);
  WM_operatortype_append(RIGIDBODY_OT_object_remove);

  WM_operatortype_append(RIGIDBODY_OT_objects_add);
  WM_operatortype_append(RIGIDBODY_OT_objects_remove);

  WM_operatortype_append(RIGIDBODY_OT_shape_change);
  WM_operatortype_append(RIGIDBODY_OT_mass_calculate);

  WM_operatortype_append(RIGIDBODY_OT_constraint_add);
  WM_operatortype_append(RIGIDBODY_OT_constraint_remove);

  WM_operatortype_append(RIGIDBODY_OT_world_add);
  WM_operatortype_append(RIGIDBODY_OT_world_remove);
  //  WM_operatortype_append(RIGIDBODY_OT_world_export);
}

static void keymap_particle(wmKeyConfig *keyconf)
{
  wmKeyMap *keymap = WM_keymap_ensure(keyconf, "Particle", 0, 0);
  keymap->poll = PE_poll;
}

/******************************* boids *************************************/

static void operatortypes_boids(void)
{
  WM_operatortype_append(BOID_OT_rule_add);
  WM_operatortype_append(BOID_OT_rule_del);
  WM_operatortype_append(BOID_OT_rule_move_up);
  WM_operatortype_append(BOID_OT_rule_move_down);

  WM_operatortype_append(BOID_OT_state_add);
  WM_operatortype_append(BOID_OT_state_del);
  WM_operatortype_append(BOID_OT_state_move_up);
  WM_operatortype_append(BOID_OT_state_move_down);
}

/********************************* mantaflow ***********************************/

static void operatortypes_manta(void)
{
  WM_operatortype_append(MANTA_OT_bake_data);
  WM_operatortype_append(MANTA_OT_free_data);
  WM_operatortype_append(MANTA_OT_bake_noise);
  WM_operatortype_append(MANTA_OT_free_noise);
  WM_operatortype_append(MANTA_OT_bake_mesh);
  WM_operatortype_append(MANTA_OT_free_mesh);
  WM_operatortype_append(MANTA_OT_bake_particles);
  WM_operatortype_append(MANTA_OT_free_particles);
  WM_operatortype_append(MANTA_OT_bake_guiding);
  WM_operatortype_append(MANTA_OT_free_guiding);
  WM_operatortype_append(MANTA_OT_pause_bake);
}

/**************************** point cache **********************************/

static void operatortypes_pointcache(void)
{
  WM_operatortype_append(PTCACHE_OT_bake_all);
  WM_operatortype_append(PTCACHE_OT_free_bake_all);
  WM_operatortype_append(PTCACHE_OT_bake);
  WM_operatortype_append(PTCACHE_OT_free_bake);
  WM_operatortype_append(PTCACHE_OT_bake_from_cache);
  WM_operatortype_append(PTCACHE_OT_add);
  WM_operatortype_append(PTCACHE_OT_remove);
}

/********************************* dynamic paint ***********************************/

static void operatortypes_dynamicpaint(void)
{
  WM_operatortype_append(DPAINT_OT_bake);
  WM_operatortype_append(DPAINT_OT_surface_slot_add);
  WM_operatortype_append(DPAINT_OT_surface_slot_remove);
  WM_operatortype_append(DPAINT_OT_type_toggle);
  WM_operatortype_append(DPAINT_OT_output_toggle);
}

/****************************** general ************************************/

void ED_operatortypes_physics(void)
{
  operatortypes_particle();
  operatortypes_boids();
  operatortypes_manta();
  operatortypes_pointcache();
  operatortypes_dynamicpaint();
}

void ED_keymap_physics(wmKeyConfig *keyconf)
{
  keymap_particle(keyconf);
}

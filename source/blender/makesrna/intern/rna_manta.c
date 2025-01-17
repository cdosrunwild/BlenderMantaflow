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
 */

/** \file
 * \ingroup RNA
 */

#include <stdlib.h>
#include <limits.h>

#include "BLI_utildefines.h"
#include "BLI_path_util.h"
#include "BLI_sys_types.h"

#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "rna_internal.h"

#include "BKE_modifier.h"
#include "BKE_manta.h"
#include "BKE_pointcache.h"

#include "DNA_modifier_types.h"
#include "DNA_object_force_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_manta_types.h"
#include "DNA_particle_types.h"

#include "WM_types.h"
#include "WM_api.h"

#ifdef RNA_RUNTIME

#  include "BLI_threads.h"

#  include "BKE_colorband.h"
#  include "BKE_context.h"
#  include "BKE_particle.h"

#  include "DEG_depsgraph.h"
#  include "DEG_depsgraph_build.h"

#  include "manta_fluid_API.h"

static void rna_Manta_update(Main *UNUSED(bmain), Scene *UNUSED(scene), PointerRNA *ptr)
{
  DEG_id_tag_update(ptr->owner_id, ID_RECALC_GEOMETRY);

  // Needed for liquid domain objects
  Object *ob = (Object *)ptr->owner_id;
  WM_main_add_notifier(NC_OBJECT | ND_DRAW, ob);
}

static void rna_Manta_dependency_update(Main *bmain, Scene *scene, PointerRNA *ptr)
{
  rna_Manta_update(bmain, scene, ptr);
  DEG_relations_tag_update(bmain);
}

static void rna_Manta_resetCache(Main *UNUSED(bmain), Scene *UNUSED(scene), PointerRNA *ptr)
{
  MantaDomainSettings *settings = (MantaDomainSettings *)ptr->data;
  if (settings->mmd && settings->mmd->domain)
    settings->cache_flag |= FLUID_DOMAIN_CACHE_OUTDATED;
  DEG_id_tag_update(ptr->owner_id, ID_RECALC_GEOMETRY);
}
static void rna_Manta_reset(Main *bmain, Scene *scene, PointerRNA *ptr)
{
  MantaDomainSettings *settings = (MantaDomainSettings *)ptr->data;

  mantaModifier_reset(settings->mmd);
  rna_Manta_resetCache(bmain, scene, ptr);

  rna_Manta_update(bmain, scene, ptr);
}

static void rna_Manta_reset_dependency(Main *bmain, Scene *scene, PointerRNA *ptr)
{
  MantaDomainSettings *settings = (MantaDomainSettings *)ptr->data;

  mantaModifier_reset(settings->mmd);

  if (settings->mmd && settings->mmd->domain)
    settings->mmd->domain->point_cache[0]->flag |= PTCACHE_OUTDATED;

  rna_Manta_dependency_update(bmain, scene, ptr);
}

static void rna_Manta_parts_create(Main *bmain,
                                   PointerRNA *ptr,
                                   char *pset_name,
                                   char *parts_name,
                                   char *psys_name,
                                   int psys_type)
{
  Object *ob = (Object *)ptr->owner_id;
  ParticleSystemModifierData *pmmd;
  ParticleSystem *psys;
  ParticleSettings *part;

  /* add particle system */
  part = BKE_particlesettings_add(bmain, pset_name);
  psys = MEM_callocN(sizeof(ParticleSystem), "particle_system");

  part->type = psys_type;
  part->totpart = 0;
  part->draw_size = 0.05f;  // make fluid particles more subtle in viewport
  part->draw_col = PART_DRAW_COL_VEL;
  psys->part = part;
  psys->pointcache = BKE_ptcache_add(&psys->ptcaches);
  BLI_strncpy(psys->name, parts_name, sizeof(psys->name));
  BLI_addtail(&ob->particlesystem, psys);

  /* add modifier */
  pmmd = (ParticleSystemModifierData *)modifier_new(eModifierType_ParticleSystem);
  BLI_strncpy(pmmd->modifier.name, psys_name, sizeof(pmmd->modifier.name));
  pmmd->psys = psys;
  BLI_addtail(&ob->modifiers, pmmd);
  modifier_unique_name(&ob->modifiers, (ModifierData *)pmmd);
}

static void rna_Manta_parts_delete(PointerRNA *ptr, int ptype)
{
  Object *ob = (Object *)ptr->owner_id;
  ParticleSystemModifierData *pmmd;
  ParticleSystem *psys, *next_psys;

  for (psys = ob->particlesystem.first; psys; psys = next_psys) {
    next_psys = psys->next;
    if (psys->part->type == ptype) {
      /* clear modifier */
      pmmd = psys_get_modifier(ob, psys);
      BLI_remlink(&ob->modifiers, pmmd);
      modifier_free((ModifierData *)pmmd);

      /* clear particle system */
      BLI_remlink(&ob->particlesystem, psys);
      psys_free(ob, psys);
    }
  }
}

static bool rna_Manta_parts_exists(PointerRNA *ptr, int ptype)
{
  Object *ob = (Object *)ptr->owner_id;
  ParticleSystem *psys;

  for (psys = ob->particlesystem.first; psys; psys = psys->next) {
    if (psys->part->type == ptype)
      return true;
  }
  return false;
}

static void rna_Manta_draw_type_update(Main *UNUSED(bmain),
                                       Scene *UNUSED(scene),
                                       struct PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  MantaDomainSettings *settings = (MantaDomainSettings *)ptr->data;

  /* Wireframe mode more convenient when particles present */
  if (settings->particle_type == 0) {
    ob->dt = OB_SOLID;
  }
  else {
    ob->dt = OB_WIRE;
  }
}

static void rna_Manta_flip_parts_update(Main *bmain, Scene *UNUSED(scene), PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  MantaModifierData *mmd;
  mmd = (MantaModifierData *)modifiers_findByType(ob, eModifierType_Manta);
  bool exists = rna_Manta_parts_exists(ptr, PART_MANTA_FLIP);

  if (ob->type == OB_MESH && !exists) {
    rna_Manta_parts_create(bmain,
                           ptr,
                           "FlipParticleSettings",
                           "FLIP Particles",
                           "FLIP Particle System",
                           PART_MANTA_FLIP);
    mmd->domain->particle_type |= FLUID_DOMAIN_PARTICLE_FLIP;
  }
  else {
    rna_Manta_parts_delete(ptr, PART_MANTA_FLIP);
    rna_Manta_resetCache(NULL, NULL, ptr);

    mmd->domain->particle_type &= ~FLUID_DOMAIN_PARTICLE_FLIP;
  }
  rna_Manta_draw_type_update(NULL, NULL, ptr);
  rna_Manta_reset(NULL, NULL, ptr);
}

static void rna_Manta_spray_parts_update(Main *bmain, Scene *UNUSED(scene), PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  MantaModifierData *mmd;
  mmd = (MantaModifierData *)modifiers_findByType(ob, eModifierType_Manta);
  bool exists = rna_Manta_parts_exists(ptr, PART_MANTA_SPRAY);

  if (ob->type == OB_MESH && !exists) {
    rna_Manta_parts_create(bmain,
                           ptr,
                           "SprayParticleSettings",
                           "Spray Particles",
                           "Spray Particle System",
                           PART_MANTA_SPRAY);
    mmd->domain->particle_type |= FLUID_DOMAIN_PARTICLE_SPRAY;
  }
  else {
    rna_Manta_parts_delete(ptr, PART_MANTA_SPRAY);
    rna_Manta_resetCache(NULL, NULL, ptr);

    mmd->domain->particle_type &= ~FLUID_DOMAIN_PARTICLE_SPRAY;
  }
  rna_Manta_draw_type_update(NULL, NULL, ptr);
  rna_Manta_reset(NULL, NULL, ptr);
}

static void rna_Manta_bubble_parts_update(Main *bmain, Scene *UNUSED(scene), PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  MantaModifierData *mmd;
  mmd = (MantaModifierData *)modifiers_findByType(ob, eModifierType_Manta);
  bool exists = rna_Manta_parts_exists(ptr, PART_MANTA_BUBBLE);

  if (ob->type == OB_MESH && !exists) {
    rna_Manta_parts_create(bmain,
                           ptr,
                           "BubbleParticleSettings",
                           "Bubble Particles",
                           "Bubble Particle System",
                           PART_MANTA_BUBBLE);
    mmd->domain->particle_type |= FLUID_DOMAIN_PARTICLE_BUBBLE;
  }
  else {
    rna_Manta_parts_delete(ptr, PART_MANTA_BUBBLE);
    rna_Manta_resetCache(NULL, NULL, ptr);

    mmd->domain->particle_type &= ~FLUID_DOMAIN_PARTICLE_BUBBLE;
  }
  rna_Manta_draw_type_update(NULL, NULL, ptr);
  rna_Manta_reset(NULL, NULL, ptr);
}

static void rna_Manta_foam_parts_update(Main *bmain, Scene *UNUSED(scene), PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  MantaModifierData *mmd;
  mmd = (MantaModifierData *)modifiers_findByType(ob, eModifierType_Manta);
  bool exists = rna_Manta_parts_exists(ptr, PART_MANTA_FOAM);

  if (ob->type == OB_MESH && !exists) {
    rna_Manta_parts_create(bmain,
                           ptr,
                           "FoamParticleSettings",
                           "Foam Particles",
                           "Foam Particle System",
                           PART_MANTA_FOAM);
    mmd->domain->particle_type |= FLUID_DOMAIN_PARTICLE_FOAM;
  }
  else {
    rna_Manta_parts_delete(ptr, PART_MANTA_FOAM);
    rna_Manta_resetCache(NULL, NULL, ptr);

    mmd->domain->particle_type &= ~FLUID_DOMAIN_PARTICLE_FOAM;
  }
  rna_Manta_draw_type_update(NULL, NULL, ptr);
  rna_Manta_reset(NULL, NULL, ptr);
}

static void rna_Manta_tracer_parts_update(Main *bmain, Scene *UNUSED(scene), PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  MantaModifierData *mmd;
  mmd = (MantaModifierData *)modifiers_findByType(ob, eModifierType_Manta);
  bool exists = rna_Manta_parts_exists(ptr, PART_MANTA_TRACER);

  if (ob->type == OB_MESH && !exists) {
    rna_Manta_parts_create(bmain,
                           ptr,
                           "TracerParticleSettings",
                           "Tracer Particles",
                           "Tracer Particle System",
                           PART_MANTA_TRACER);
    mmd->domain->particle_type |= FLUID_DOMAIN_PARTICLE_TRACER;
  }
  else {
    rna_Manta_parts_delete(ptr, PART_MANTA_TRACER);
    rna_Manta_resetCache(NULL, NULL, ptr);

    mmd->domain->particle_type &= ~FLUID_DOMAIN_PARTICLE_TRACER;
  }
  rna_Manta_draw_type_update(NULL, NULL, ptr);
  rna_Manta_reset(NULL, NULL, ptr);
}

static void rna_Manta_combined_export_update(Main *bmain, Scene *scene, PointerRNA *ptr)
{
  Object *ob = (Object *)ptr->owner_id;
  MantaModifierData *mmd;
  mmd = (MantaModifierData *)modifiers_findByType(ob, eModifierType_Manta);

  if (mmd->domain->sndparticle_combined_export == SNDPARTICLE_COMBINED_EXPORT_OFF) {
    rna_Manta_parts_delete(ptr, (PART_MANTA_SPRAY | PART_MANTA_FOAM));
    rna_Manta_parts_delete(ptr, (PART_MANTA_SPRAY | PART_MANTA_BUBBLE));
    rna_Manta_parts_delete(ptr, (PART_MANTA_FOAM | PART_MANTA_BUBBLE));
    rna_Manta_parts_delete(ptr, (PART_MANTA_SPRAY | PART_MANTA_FOAM | PART_MANTA_BUBBLE));

    // re-add each particle type if enabled
    if ((mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_SPRAY) != 0) {
      rna_Manta_spray_parts_update(bmain, scene, ptr);
    }
    if ((mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_FOAM) != 0) {
      rna_Manta_foam_parts_update(bmain, scene, ptr);
    }
    if ((mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_BUBBLE) != 0) {
      rna_Manta_bubble_parts_update(bmain, scene, ptr);
    }
  }
  else if (mmd->domain->sndparticle_combined_export == SNDPARTICLE_COMBINED_EXPORT_SPRAY_FOAM) {
    if (ob->type == OB_MESH && !rna_Manta_parts_exists(ptr, (PART_MANTA_SPRAY | PART_MANTA_FOAM)))
      rna_Manta_parts_create(bmain,
                             ptr,
                             "SprayFoamParticleSettings",
                             "Spray + Foam Particles",
                             "Spray + Foam Particle System",
                             (PART_MANTA_SPRAY | PART_MANTA_FOAM));
    rna_Manta_parts_delete(ptr, PART_MANTA_SPRAY);
    rna_Manta_parts_delete(ptr, PART_MANTA_FOAM);
    rna_Manta_parts_delete(ptr, (PART_MANTA_SPRAY | PART_MANTA_BUBBLE));
    rna_Manta_parts_delete(ptr, (PART_MANTA_FOAM | PART_MANTA_BUBBLE));
    rna_Manta_parts_delete(ptr, (PART_MANTA_SPRAY | PART_MANTA_FOAM | PART_MANTA_BUBBLE));

    mmd->domain->particle_type |= FLUID_DOMAIN_PARTICLE_SPRAY;
    mmd->domain->particle_type |= FLUID_DOMAIN_PARTICLE_FOAM;

    // re-add bubbles if enabled
    if ((mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_BUBBLE) != 0) {
      rna_Manta_bubble_parts_update(bmain, scene, ptr);
    }
  }
  else if (mmd->domain->sndparticle_combined_export == SNDPARTICLE_COMBINED_EXPORT_SPRAY_BUBBLE) {
    if (ob->type == OB_MESH &&
        !rna_Manta_parts_exists(ptr, (PART_MANTA_SPRAY | PART_MANTA_BUBBLE)))
      rna_Manta_parts_create(bmain,
                             ptr,
                             "SprayBubbleParticleSettings",
                             "Spray + Bubble Particles",
                             "Spray + Bubble Particle System",
                             (PART_MANTA_SPRAY | PART_MANTA_BUBBLE));
    rna_Manta_parts_delete(ptr, PART_MANTA_SPRAY);
    rna_Manta_parts_delete(ptr, PART_MANTA_BUBBLE);
    rna_Manta_parts_delete(ptr, (PART_MANTA_SPRAY | PART_MANTA_FOAM));
    rna_Manta_parts_delete(ptr, (PART_MANTA_FOAM | PART_MANTA_BUBBLE));
    rna_Manta_parts_delete(ptr, (PART_MANTA_SPRAY | PART_MANTA_FOAM | PART_MANTA_BUBBLE));

    mmd->domain->particle_type |= FLUID_DOMAIN_PARTICLE_SPRAY;
    mmd->domain->particle_type |= FLUID_DOMAIN_PARTICLE_BUBBLE;

    // re-add foam if enabled
    if ((mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_FOAM) != 0) {
      rna_Manta_foam_parts_update(bmain, scene, ptr);
    }
  }
  else if (mmd->domain->sndparticle_combined_export == SNDPARTICLE_COMBINED_EXPORT_FOAM_BUBBLE) {
    if (ob->type == OB_MESH && !rna_Manta_parts_exists(ptr, (PART_MANTA_FOAM | PART_MANTA_BUBBLE)))
      rna_Manta_parts_create(bmain,
                             ptr,
                             "FoamBubbleParticleSettings",
                             "Foam + Bubble Particles",
                             "Foam + Bubble Particle System",
                             (PART_MANTA_FOAM | PART_MANTA_BUBBLE));
    rna_Manta_parts_delete(ptr, PART_MANTA_FOAM);
    rna_Manta_parts_delete(ptr, PART_MANTA_BUBBLE);
    rna_Manta_parts_delete(ptr, (PART_MANTA_SPRAY | PART_MANTA_FOAM));
    rna_Manta_parts_delete(ptr, (PART_MANTA_SPRAY | PART_MANTA_BUBBLE));
    rna_Manta_parts_delete(ptr, (PART_MANTA_SPRAY | PART_MANTA_FOAM | PART_MANTA_BUBBLE));

    mmd->domain->particle_type |= FLUID_DOMAIN_PARTICLE_FOAM;
    mmd->domain->particle_type |= FLUID_DOMAIN_PARTICLE_BUBBLE;

    // re-add spray if enabled
    if ((mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_SPRAY) != 0) {
      rna_Manta_spray_parts_update(bmain, scene, ptr);
    }
  }
  else if (mmd->domain->sndparticle_combined_export ==
           SNDPARTICLE_COMBINED_EXPORT_SPRAY_FOAM_BUBBLE) {
    if (ob->type == OB_MESH &&
        !rna_Manta_parts_exists(ptr, (PART_MANTA_SPRAY | PART_MANTA_FOAM | PART_MANTA_BUBBLE)))
      rna_Manta_parts_create(bmain,
                             ptr,
                             "SprayFoamBubbleParticleSettings",
                             "Spray + Foam + Bubble Particles",
                             "Spray + Foam + Bubble Particle System",
                             (PART_MANTA_SPRAY | PART_MANTA_FOAM | PART_MANTA_BUBBLE));
    rna_Manta_parts_delete(ptr, PART_MANTA_SPRAY);
    rna_Manta_parts_delete(ptr, PART_MANTA_FOAM);
    rna_Manta_parts_delete(ptr, PART_MANTA_BUBBLE);
    rna_Manta_parts_delete(ptr, (PART_MANTA_SPRAY | PART_MANTA_FOAM));
    rna_Manta_parts_delete(ptr, (PART_MANTA_SPRAY | PART_MANTA_BUBBLE));
    rna_Manta_parts_delete(ptr, (PART_MANTA_FOAM | PART_MANTA_BUBBLE));

    mmd->domain->particle_type |= FLUID_DOMAIN_PARTICLE_SPRAY;
    mmd->domain->particle_type |= FLUID_DOMAIN_PARTICLE_FOAM;
    mmd->domain->particle_type |= FLUID_DOMAIN_PARTICLE_BUBBLE;
  }
  else {
    // sanity check, should not occur
    printf("ERROR: Unexpected combined export setting encountered!");
  }
  rna_Manta_resetCache(NULL, NULL, ptr);
  rna_Manta_draw_type_update(NULL, NULL, ptr);
}

static void rna_Manta_cachetype_mesh_set(struct PointerRNA *ptr, int value)
{
  MantaDomainSettings *settings = (MantaDomainSettings *)ptr->data;

  if (value != settings->cache_mesh_format) {
    /* TODO (sebbas): Clear old caches. */
    settings->cache_mesh_format = value;
  }
}

static void rna_Manta_cachetype_data_set(struct PointerRNA *ptr, int value)
{
  MantaDomainSettings *settings = (MantaDomainSettings *)ptr->data;

  if (value != settings->cache_data_format) {
    /* TODO (sebbas): Clear old caches. */
    settings->cache_data_format = value;
  }
}

static void rna_Manta_cachetype_particle_set(struct PointerRNA *ptr, int value)
{
  MantaDomainSettings *settings = (MantaDomainSettings *)ptr->data;

  if (value != settings->cache_particle_format) {
    /* TODO (sebbas): Clear old caches. */
    settings->cache_particle_format = value;
  }
}

static void rna_Manta_cachetype_noise_set(struct PointerRNA *ptr, int value)
{
  MantaDomainSettings *settings = (MantaDomainSettings *)ptr->data;

  if (value != settings->cache_noise_format) {
    /* TODO (sebbas): Clear old caches. */
    settings->cache_noise_format = value;
  }
}

static void rna_Manta_guiding_parent_set(struct PointerRNA *ptr,
                                         struct PointerRNA value,
                                         struct ReportList *UNUSED(reports))
{
  MantaDomainSettings *mds = (MantaDomainSettings *)ptr->data;
  Object *par = (Object *)value.data;

  MantaModifierData *mmd_par = NULL;

  if (par != NULL) {
    mmd_par = (MantaModifierData *)modifiers_findByType(par, eModifierType_Manta);
    if (mmd_par && mmd_par->domain) {
      mds->guiding_parent = value.data;
      mds->guide_res = mmd_par->domain->res;
    }
  }
  else {
    mds->guiding_parent = NULL;
    mds->guide_res = NULL;
  }
}

static const EnumPropertyItem *rna_Manta_cachetype_mesh_itemf(bContext *UNUSED(C),
                                                              PointerRNA *UNUSED(ptr),
                                                              PropertyRNA *UNUSED(prop),
                                                              bool *r_free)
{
  EnumPropertyItem *item = NULL;
  EnumPropertyItem tmp = {0, "", 0, "", ""};
  int totitem = 0;

  tmp.value = FLUID_DOMAIN_FILE_BIN_OBJECT;
  tmp.identifier = "BOBJECT";
  tmp.name = "Binary Object files";
  tmp.description = "Binary object file format";
  RNA_enum_item_add(&item, &totitem, &tmp);

  tmp.value = FLUID_DOMAIN_FILE_OBJECT;
  tmp.identifier = "OBJECT";
  tmp.name = "Object files";
  tmp.description = "Object file format";
  RNA_enum_item_add(&item, &totitem, &tmp);

  RNA_enum_item_end(&item, &totitem);
  *r_free = true;

  return item;
}

static const EnumPropertyItem *rna_Manta_cachetype_volume_itemf(bContext *UNUSED(C),
                                                                PointerRNA *UNUSED(ptr),
                                                                PropertyRNA *UNUSED(prop),
                                                                bool *r_free)
{
  EnumPropertyItem *item = NULL;
  EnumPropertyItem tmp = {0, "", 0, "", ""};
  int totitem = 0;

  tmp.value = FLUID_DOMAIN_FILE_UNI;
  tmp.identifier = "UNI";
  tmp.name = "Uni Cache";
  tmp.description = "Uni file format";
  RNA_enum_item_add(&item, &totitem, &tmp);

#  ifdef WITH_OPENVDB
  tmp.value = FLUID_DOMAIN_FILE_OPENVDB;
  tmp.identifier = "OPENVDB";
  tmp.name = "OpenVDB";
  tmp.description = "OpenVDB file format";
  RNA_enum_item_add(&item, &totitem, &tmp);
#  endif

  tmp.value = FLUID_DOMAIN_FILE_RAW;
  tmp.identifier = "RAW";
  tmp.name = "Raw Cache";
  tmp.description = "Raw file format";
  RNA_enum_item_add(&item, &totitem, &tmp);

  RNA_enum_item_end(&item, &totitem);
  *r_free = true;

  return item;
}

static const EnumPropertyItem *rna_Manta_cachetype_particle_itemf(bContext *UNUSED(C),
                                                                  PointerRNA *UNUSED(ptr),
                                                                  PropertyRNA *UNUSED(prop),
                                                                  bool *r_free)
{
  EnumPropertyItem *item = NULL;
  EnumPropertyItem tmp = {0, "", 0, "", ""};
  int totitem = 0;

  tmp.value = FLUID_DOMAIN_FILE_UNI;
  tmp.identifier = "UNI";
  tmp.name = "Uni Cache";
  tmp.description = "Uni file format";
  RNA_enum_item_add(&item, &totitem, &tmp);

  RNA_enum_item_end(&item, &totitem);
  *r_free = true;

  return item;
}

static void rna_Manta_collisionextents_set(struct PointerRNA *ptr, int value, bool clear)
{
  MantaDomainSettings *settings = (MantaDomainSettings *)ptr->data;
  if (clear) {
    settings->border_collisions &= value;
  }
  else {
    settings->border_collisions |= value;
  }
}

static void rna_Manta_cache_directory_set(struct PointerRNA *ptr, const char *value)
{
  MantaDomainSettings *settings = (MantaDomainSettings *)ptr->data;

  if (STREQ(settings->cache_directory, value)) {
    return;
  }

  BLI_strncpy(settings->cache_directory, value, sizeof(settings->cache_directory));

  /* TODO (sebbas): Read cache state in order to set cache bake flags and cache pause frames correctly */
  //settings->cache_flag = 0;
}

static void rna_Manta_domaintype_set(struct PointerRNA *ptr, int value)
{
  MantaDomainSettings *settings = (MantaDomainSettings *)ptr->data;
  Object *ob = (Object *)ptr->owner_id;

  if (value != settings->type) {
    /* Set common values for liquid/smoke domain: cache type, border collision and viewport drawtype. */
    if (value == FLUID_DOMAIN_TYPE_GAS) {
      rna_Manta_cachetype_mesh_set(ptr, FLUID_DOMAIN_FILE_BIN_OBJECT);
      rna_Manta_cachetype_data_set(ptr, FLUID_DOMAIN_FILE_UNI);
      rna_Manta_cachetype_particle_set(ptr, FLUID_DOMAIN_FILE_UNI);
      rna_Manta_cachetype_noise_set(ptr, FLUID_DOMAIN_FILE_UNI);
      rna_Manta_collisionextents_set(ptr, FLUID_DOMAIN_BORDER_FRONT, 1);
      rna_Manta_collisionextents_set(ptr, FLUID_DOMAIN_BORDER_BACK, 1);
      rna_Manta_collisionextents_set(ptr, FLUID_DOMAIN_BORDER_RIGHT, 1);
      rna_Manta_collisionextents_set(ptr, FLUID_DOMAIN_BORDER_LEFT, 1);
      rna_Manta_collisionextents_set(ptr, FLUID_DOMAIN_BORDER_TOP, 1);
      rna_Manta_collisionextents_set(ptr, FLUID_DOMAIN_BORDER_BOTTOM, 1);
      ob->dt = OB_WIRE;
    }
    else if (value == FLUID_DOMAIN_TYPE_LIQUID) {
      rna_Manta_cachetype_mesh_set(ptr, FLUID_DOMAIN_FILE_BIN_OBJECT);
      rna_Manta_cachetype_data_set(ptr, FLUID_DOMAIN_FILE_UNI);
      rna_Manta_cachetype_particle_set(ptr, FLUID_DOMAIN_FILE_UNI);
      rna_Manta_cachetype_noise_set(ptr, FLUID_DOMAIN_FILE_UNI);
      rna_Manta_collisionextents_set(ptr, FLUID_DOMAIN_BORDER_FRONT, 0);
      rna_Manta_collisionextents_set(ptr, FLUID_DOMAIN_BORDER_BACK, 0);
      rna_Manta_collisionextents_set(ptr, FLUID_DOMAIN_BORDER_RIGHT, 0);
      rna_Manta_collisionextents_set(ptr, FLUID_DOMAIN_BORDER_LEFT, 0);
      rna_Manta_collisionextents_set(ptr, FLUID_DOMAIN_BORDER_TOP, 0);
      rna_Manta_collisionextents_set(ptr, FLUID_DOMAIN_BORDER_BOTTOM, 0);
      ob->dt = OB_SOLID;
    }

    /* Set actual domain type */
    settings->type = value;
  }
}

static char *rna_MantaDomainSettings_path(PointerRNA *ptr)
{
  MantaDomainSettings *settings = (MantaDomainSettings *)ptr->data;
  ModifierData *md = (ModifierData *)settings->mmd;
  char name_esc[sizeof(md->name) * 2];

  BLI_strescape(name_esc, md->name, sizeof(name_esc));
  return BLI_sprintfN("modifiers[\"%s\"].domain_settings", name_esc);
}

static char *rna_MantaFlowSettings_path(PointerRNA *ptr)
{
  MantaFlowSettings *settings = (MantaFlowSettings *)ptr->data;
  ModifierData *md = (ModifierData *)settings->mmd;
  char name_esc[sizeof(md->name) * 2];

  BLI_strescape(name_esc, md->name, sizeof(name_esc));
  return BLI_sprintfN("modifiers[\"%s\"].flow_settings", name_esc);
}

static char *rna_MantaCollSettings_path(PointerRNA *ptr)
{
  MantaCollSettings *settings = (MantaCollSettings *)ptr->data;
  ModifierData *md = (ModifierData *)settings->mmd;
  char name_esc[sizeof(md->name) * 2];

  BLI_strescape(name_esc, md->name, sizeof(name_esc));
  return BLI_sprintfN("modifiers[\"%s\"].effec_settings", name_esc);
}

static int rna_MantaModifier_grid_get_length(PointerRNA *ptr, int length[RNA_MAX_ARRAY_DIMENSION])
{
  MantaDomainSettings *mds = (MantaDomainSettings *)ptr->data;
  float *density = NULL;
  int size = 0;

  if (mds->flags & FLUID_DOMAIN_USE_NOISE && mds->fluid) {
    /* high resolution smoke */
    int res[3];

    manta_smoke_turbulence_get_res(mds->fluid, res);
    size = res[0] * res[1] * res[2];

    density = manta_smoke_turbulence_get_density(mds->fluid);
  }
  else if (mds->fluid) {
    /* regular resolution */
    size = mds->res[0] * mds->res[1] * mds->res[2];
    density = manta_smoke_get_density(mds->fluid);
  }

  length[0] = (density) ? size : 0;
  return length[0];
}

static int rna_MantaModifier_color_grid_get_length(PointerRNA *ptr,
                                                   int length[RNA_MAX_ARRAY_DIMENSION])
{
  rna_MantaModifier_grid_get_length(ptr, length);

  length[0] *= 4;
  return length[0];
}

static int rna_MantaModifier_velocity_grid_get_length(PointerRNA *ptr,
                                                      int length[RNA_MAX_ARRAY_DIMENSION])
{
  MantaDomainSettings *mds = (MantaDomainSettings *)ptr->data;
  float *vx = NULL;
  float *vy = NULL;
  float *vz = NULL;
  int size = 0;

  /* Velocity data is always low-resolution. */
  if (mds->fluid) {
    size = 3 * mds->res[0] * mds->res[1] * mds->res[2];
    vx = manta_get_velocity_x(mds->fluid);
    vy = manta_get_velocity_y(mds->fluid);
    vz = manta_get_velocity_z(mds->fluid);
  }

  length[0] = (vx && vy && vz) ? size : 0;
  return length[0];
}

static int rna_MantaModifier_heat_grid_get_length(PointerRNA *ptr,
                                                  int length[RNA_MAX_ARRAY_DIMENSION])
{
  MantaDomainSettings *mds = (MantaDomainSettings *)ptr->data;
  float *heat = NULL;
  int size = 0;

  /* Heat data is always low-resolution. */
  if (mds->fluid) {
    size = mds->res[0] * mds->res[1] * mds->res[2];
    heat = manta_smoke_get_heat(mds->fluid);
  }

  length[0] = (heat) ? size : 0;
  return length[0];
}

static void rna_MantaModifier_density_grid_get(PointerRNA *ptr, float *values)
{
  MantaDomainSettings *mds = (MantaDomainSettings *)ptr->data;
  int length[RNA_MAX_ARRAY_DIMENSION];
  int size = rna_MantaModifier_grid_get_length(ptr, length);
  float *density;

  BLI_rw_mutex_lock(mds->fluid_mutex, THREAD_LOCK_READ);

  if (mds->flags & FLUID_DOMAIN_USE_NOISE && mds->fluid)
    density = manta_smoke_turbulence_get_density(mds->fluid);
  else
    density = manta_smoke_get_density(mds->fluid);

  memcpy(values, density, size * sizeof(float));

  BLI_rw_mutex_unlock(mds->fluid_mutex);
}

static void rna_MantaModifier_velocity_grid_get(PointerRNA *ptr, float *values)
{
  MantaDomainSettings *mds = (MantaDomainSettings *)ptr->data;
  int length[RNA_MAX_ARRAY_DIMENSION];
  int size = rna_MantaModifier_velocity_grid_get_length(ptr, length);
  float *vx, *vy, *vz;
  int i;

  BLI_rw_mutex_lock(mds->fluid_mutex, THREAD_LOCK_READ);

  vx = manta_get_velocity_x(mds->fluid);
  vy = manta_get_velocity_y(mds->fluid);
  vz = manta_get_velocity_z(mds->fluid);

  for (i = 0; i < size; i += 3) {
    *(values++) = *(vx++);
    *(values++) = *(vy++);
    *(values++) = *(vz++);
  }

  BLI_rw_mutex_unlock(mds->fluid_mutex);
}

static void rna_MantaModifier_color_grid_get(PointerRNA *ptr, float *values)
{
  MantaDomainSettings *mds = (MantaDomainSettings *)ptr->data;
  int length[RNA_MAX_ARRAY_DIMENSION];
  int size = rna_MantaModifier_grid_get_length(ptr, length);

  BLI_rw_mutex_lock(mds->fluid_mutex, THREAD_LOCK_READ);

  if (!mds->fluid) {
    memset(values, 0, size * sizeof(float));
  }
  else {
    if (mds->flags & FLUID_DOMAIN_USE_NOISE) {
      if (manta_smoke_turbulence_has_colors(mds->fluid))
        manta_smoke_turbulence_get_rgba(mds->fluid, values, 0);
      else
        manta_smoke_turbulence_get_rgba_from_density(mds->fluid, mds->active_color, values, 0);
    }
    else {
      if (manta_smoke_has_colors(mds->fluid))
        manta_smoke_get_rgba(mds->fluid, values, 0);
      else
        manta_smoke_get_rgba_from_density(mds->fluid, mds->active_color, values, 0);
    }
  }

  BLI_rw_mutex_unlock(mds->fluid_mutex);
}

static void rna_MantaModifier_flame_grid_get(PointerRNA *ptr, float *values)
{
  MantaDomainSettings *mds = (MantaDomainSettings *)ptr->data;
  int length[RNA_MAX_ARRAY_DIMENSION];
  int size = rna_MantaModifier_grid_get_length(ptr, length);
  float *flame;

  BLI_rw_mutex_lock(mds->fluid_mutex, THREAD_LOCK_READ);

  if (mds->flags & FLUID_DOMAIN_USE_NOISE && mds->fluid)
    flame = manta_smoke_turbulence_get_flame(mds->fluid);
  else
    flame = manta_smoke_get_flame(mds->fluid);

  if (flame)
    memcpy(values, flame, size * sizeof(float));
  else
    memset(values, 0, size * sizeof(float));

  BLI_rw_mutex_unlock(mds->fluid_mutex);
}

static void rna_MantaModifier_heat_grid_get(PointerRNA *ptr, float *values)
{
  MantaDomainSettings *mds = (MantaDomainSettings *)ptr->data;
  int length[RNA_MAX_ARRAY_DIMENSION];
  int size = rna_MantaModifier_heat_grid_get_length(ptr, length);
  float *heat;

  BLI_rw_mutex_lock(mds->fluid_mutex, THREAD_LOCK_READ);

  heat = manta_smoke_get_heat(mds->fluid);

  if (heat != NULL) {
    /* scale heat values from -2.0-2.0 to -1.0-1.0. */
    for (int i = 0; i < size; i++) {
      values[i] = heat[i] * 0.5f;
    }
  }
  else {
    memset(values, 0, size * sizeof(float));
  }

  BLI_rw_mutex_unlock(mds->fluid_mutex);
}

static void rna_MantaModifier_temperature_grid_get(PointerRNA *ptr, float *values)
{
  MantaDomainSettings *mds = (MantaDomainSettings *)ptr->data;
  int length[RNA_MAX_ARRAY_DIMENSION];
  int size = rna_MantaModifier_grid_get_length(ptr, length);
  float *flame;

  BLI_rw_mutex_lock(mds->fluid_mutex, THREAD_LOCK_READ);

  if (mds->flags & FLUID_DOMAIN_USE_NOISE && mds->fluid) {
    flame = manta_smoke_turbulence_get_flame(mds->fluid);
  }
  else {
    flame = manta_smoke_get_flame(mds->fluid);
  }

  if (flame) {
    /* Output is such that 0..1 maps to 0..1000K */
    float offset = mds->flame_ignition;
    float scale = mds->flame_max_temp - mds->flame_ignition;

    for (int i = 0; i < size; i++) {
      values[i] = (flame[i] > 0.01f) ? offset + flame[i] * scale : 0.0f;
    }
  }
  else {
    memset(values, 0, size * sizeof(float));
  }

  BLI_rw_mutex_unlock(mds->fluid_mutex);
}

static void rna_MantaFlow_density_vgroup_get(PointerRNA *ptr, char *value)
{
  MantaFlowSettings *flow = (MantaFlowSettings *)ptr->data;
  rna_object_vgroup_name_index_get(ptr, value, flow->vgroup_density);
}

static int rna_MantaFlow_density_vgroup_length(PointerRNA *ptr)
{
  MantaFlowSettings *flow = (MantaFlowSettings *)ptr->data;
  return rna_object_vgroup_name_index_length(ptr, flow->vgroup_density);
}

static void rna_MantaFlow_density_vgroup_set(struct PointerRNA *ptr, const char *value)
{
  MantaFlowSettings *flow = (MantaFlowSettings *)ptr->data;
  rna_object_vgroup_name_index_set(ptr, value, &flow->vgroup_density);
}

static void rna_MantaFlow_uvlayer_set(struct PointerRNA *ptr, const char *value)
{
  MantaFlowSettings *flow = (MantaFlowSettings *)ptr->data;
  rna_object_uvlayer_name_set(ptr, value, flow->uvlayer_name, sizeof(flow->uvlayer_name));
}

static void rna_Manta_use_color_ramp_set(struct PointerRNA *ptr, bool value)
{
  MantaDomainSettings *mds = (MantaDomainSettings *)ptr->data;

  mds->use_coba = value;

  if (value && mds->coba == NULL) {
    mds->coba = BKE_colorband_add(false);
  }
}

static void rna_Manta_flowsource_set(struct PointerRNA *ptr, int value)
{
  MantaFlowSettings *settings = (MantaFlowSettings *)ptr->data;

  if (value != settings->source) {
    settings->source = value;
  }
}

static const EnumPropertyItem *rna_Manta_flowsource_itemf(bContext *UNUSED(C),
                                                          PointerRNA *ptr,
                                                          PropertyRNA *UNUSED(prop),
                                                          bool *r_free)
{
  MantaFlowSettings *settings = (MantaFlowSettings *)ptr->data;

  EnumPropertyItem *item = NULL;
  EnumPropertyItem tmp = {0, "", 0, "", ""};
  int totitem = 0;

  tmp.value = FLUID_FLOW_SOURCE_MESH;
  tmp.identifier = "MESH";
  tmp.icon = ICON_META_CUBE;
  tmp.name = "Mesh";
  tmp.description = "Emit fluid from mesh surface or volume";
  RNA_enum_item_add(&item, &totitem, &tmp);

  if (settings->type != FLUID_FLOW_TYPE_LIQUID) {
    tmp.value = FLUID_FLOW_SOURCE_PARTICLES;
    tmp.identifier = "PARTICLES";
    tmp.icon = ICON_PARTICLES;
    tmp.name = "Particle System";
    tmp.description = "Emit smoke from particles";
    RNA_enum_item_add(&item, &totitem, &tmp);
  }

  RNA_enum_item_end(&item, &totitem);
  *r_free = true;

  return item;
}

static void rna_Manta_flowtype_set(struct PointerRNA *ptr, int value)
{
  MantaFlowSettings *settings = (MantaFlowSettings *)ptr->data;

  if (value != settings->type) {
    settings->type = value;

    /* Force flow source to mesh */
    if (value == FLUID_FLOW_TYPE_LIQUID) {
      rna_Manta_flowsource_set(ptr, FLUID_FLOW_SOURCE_MESH);
      settings->surface_distance = 0.5f;
    }
    else {
      settings->surface_distance = 1.5f;
    }
  }
}

#else

static void rna_def_manta_mesh_vertices(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "MantaVertexVelocity", NULL);
  RNA_def_struct_ui_text(srna, "Fluid Mesh Velocity", "Velocity of a simulated fluid mesh");
  RNA_def_struct_ui_icon(srna, ICON_VERTEXSEL);

  prop = RNA_def_property(srna, "velocity", PROP_FLOAT, PROP_VELOCITY);
  RNA_def_property_array(prop, 3);
  RNA_def_property_float_sdna(prop, NULL, "vel");
  RNA_def_property_ui_text(prop, "Velocity", "");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
}

static void rna_def_manta_domain_settings(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  static EnumPropertyItem domain_types[] = {
      {FLUID_DOMAIN_TYPE_GAS, "GAS", 0, "Gas", "Create domain for gases"},
      {FLUID_DOMAIN_TYPE_LIQUID, "LIQUID", 0, "Liquid", "Create domain for liquids"},
      {0, NULL, 0, NULL, NULL}};

  static const EnumPropertyItem prop_noise_type_items[] = {
      {FLUID_NOISE_TYPE_WAVELET, "NOISEWAVE", 0, "Wavelet", ""},
      {0, NULL, 0, NULL, NULL},
  };

  static const EnumPropertyItem prop_compression_items[] = {
      {VDB_COMPRESSION_ZIP, "ZIP", 0, "Zip", "Effective but slow compression"},
#  ifdef WITH_OPENVDB_BLOSC
      {VDB_COMPRESSION_BLOSC,
       "BLOSC",
       0,
       "Blosc",
       "Multithreaded compression, similar in size and quality as 'Zip'"},
#  endif
      {VDB_COMPRESSION_NONE, "NONE", 0, "None", "Do not use any compression"},
      {0, NULL, 0, NULL, NULL}};

  static const EnumPropertyItem cache_comp_items[] = {
      {SM_CACHE_LIGHT, "CACHELIGHT", 0, "Lite", "Fast but not so effective compression"},
      {SM_CACHE_HEAVY, "CACHEHEAVY", 0, "Heavy", "Effective but slow compression"},
      {0, NULL, 0, NULL, NULL},
  };

  static const EnumPropertyItem smoke_highres_sampling_items[] = {
      {SM_HRES_FULLSAMPLE, "FULLSAMPLE", 0, "Full Sample", ""},
      {SM_HRES_LINEAR, "LINEAR", 0, "Linear", ""},
      {SM_HRES_NEAREST, "NEAREST", 0, "Nearest", ""},
      {0, NULL, 0, NULL, NULL},
  };

  static EnumPropertyItem cache_types[] = {
      {FLUID_DOMAIN_CACHE_REPLAY, "REPLAY", 0, "Replay", "Use the timeline to bake the scene. Pausing and resuming possible."},
      {FLUID_DOMAIN_CACHE_MODULAR, "MODULAR", 0, "Modular", "Bake every stage of the simulation on its own. Can pause and resume bake jobs."},
      /*{FLUID_DOMAIN_CACHE_FINAL, "FINAL", 0, "Final", "Only bakes cache files that are essential for the final render. Cannot resume bake jobs."},*/
      {0, NULL, 0, NULL, NULL}
  };

  static const EnumPropertyItem smoke_data_depth_items[] = {
      {16, "16", 0, "Float (Half)", "Half float (16 bit data)"},
      {0, "32", 0, "Float (Full)", "Full float (32 bit data)"}, /* default */
      {0, NULL, 0, NULL, NULL},
  };

  static EnumPropertyItem fluid_mesh_quality_items[] = {
      {FLUID_DOMAIN_MESH_IMPROVED,
       "IMPROVED",
       0,
       "Final",
       "Use improved particle levelset (slower but more precise and with mesh smoothening "
       "options)"},
      {FLUID_DOMAIN_MESH_UNION,
       "UNION",
       0,
       "Preview",
       "Use union particle levelset (faster but lower quality)"},
      {0, NULL, 0, NULL, NULL},
  };

  static EnumPropertyItem fluid_guiding_source_items[] = {
      {FLUID_DOMAIN_GUIDING_SRC_DOMAIN,
       "DOMAIN",
       0,
       "Domain",
       "Use a fluid domain for guiding (domain needs to be baked already so that velocities can "
       "be extracted but can be of any type)"},
      {FLUID_DOMAIN_GUIDING_SRC_EFFECTOR,
       "EFFECTOR",
       0,
       "Effector",
       "Use guiding (effector) objects to create fluid guiding (guiding objects should be "
       "animated and baked once set up completely)"},
      {0, NULL, 0, NULL, NULL},
  };

  /*  Cache type - generated dynamically based on domain type */
  static EnumPropertyItem cache_file_type_items[] = {
      {0, "NONE", 0, "", ""},
      {0, NULL, 0, NULL, NULL},
  };

  static const EnumPropertyItem view_items[] = {
      {FLUID_DOMAIN_SLICE_VIEW_ALIGNED,
       "VIEW_ALIGNED",
       0,
       "View",
       "Slice volume parallel to the view plane"},
      {FLUID_DOMAIN_SLICE_AXIS_ALIGNED,
       "AXIS_ALIGNED",
       0,
       "Axis",
       "Slice volume parallel to the major axis"},
      {0, NULL, 0, NULL, NULL},
  };

  static const EnumPropertyItem axis_slice_method_items[] = {
      {AXIS_SLICE_FULL, "FULL", 0, "Full", "Slice the whole domain object"},
      {AXIS_SLICE_SINGLE, "SINGLE", 0, "Single", "Perform a single slice of the domain object"},
      {0, NULL, 0, NULL, NULL},
  };

  static const EnumPropertyItem interp_method_item[] = {
      {VOLUME_INTERP_LINEAR, "LINEAR", 0, "Linear", "Good smoothness and speed"},
      {VOLUME_INTERP_CUBIC,
       "CUBIC",
       0,
       "Cubic",
       "Smoothed high quality interpolation, but slower"},
      {0, NULL, 0, NULL, NULL},
  };

  static const EnumPropertyItem axis_slice_position_items[] = {
      {SLICE_AXIS_AUTO,
       "AUTO",
       0,
       "Auto",
       "Adjust slice direction according to the view direction"},
      {SLICE_AXIS_X, "X", 0, "X", "Slice along the X axis"},
      {SLICE_AXIS_Y, "Y", 0, "Y", "Slice along the Y axis"},
      {SLICE_AXIS_Z, "Z", 0, "Z", "Slice along the Z axis"},
      {0, NULL, 0, NULL, NULL},
  };

  static const EnumPropertyItem vector_draw_items[] = {
      {VECTOR_DRAW_NEEDLE, "NEEDLE", 0, "Needle", "Display vectors as needles"},
      {VECTOR_DRAW_STREAMLINE, "STREAMLINE", 0, "Streamlines", "Display vectors as streamlines"},
      {0, NULL, 0, NULL, NULL},
  };

  static const EnumPropertyItem sndparticle_boundary_items[] = {
      {SNDPARTICLE_BOUNDARY_DELETE,
       "DELETE",
       0,
       "Delete",
       "Delete secondary particles that are inside obstacles or left the domain"},
      {SNDPARTICLE_BOUNDARY_PUSHOUT,
       "PUSHOUT",
       0,
       "Push Out",
       "Push secondary particles that left the domain back into the domain"},
      {0, NULL, 0, NULL, NULL}};

  static const EnumPropertyItem sndparticle_combined_export_items[] = {
      {SNDPARTICLE_COMBINED_EXPORT_OFF,
       "OFF",
       0,
       "Off",
       "Create a seperate particle system for every secondary particle type"},
      {SNDPARTICLE_COMBINED_EXPORT_SPRAY_FOAM,
       "SPRAY_FOAM",
       0,
       "Spray + Foam",
       "Spray and foam particles are saved in the same particle system"},
      {SNDPARTICLE_COMBINED_EXPORT_SPRAY_BUBBLE,
       "SPRAY_BUBBLES",
       0,
       "Spray + Bubbles",
       "Spray and bubble particles are saved in the same particle system"},
      {SNDPARTICLE_COMBINED_EXPORT_FOAM_BUBBLE,
       "FOAM_BUBBLES",
       0,
       "Foam + Bubbles",
       "Foam and bubbles particles are saved in the same particle system"},
      {SNDPARTICLE_COMBINED_EXPORT_SPRAY_FOAM_BUBBLE,
       "SPRAY_FOAM_BUBBLES",
       0,
       "Spray + Foam + Bubbles",
       "Create one particle system that contains all three secondary particle types"},
      {0, NULL, 0, NULL, NULL}};

  srna = RNA_def_struct(brna, "MantaDomainSettings", NULL);
  RNA_def_struct_ui_text(srna, "Domain Settings", "Fluid domain settings");
  RNA_def_struct_sdna(srna, "MantaDomainSettings");
  RNA_def_struct_path_func(srna, "rna_MantaDomainSettings_path");

  prop = RNA_def_property(srna, "effector_weights", PROP_POINTER, PROP_NONE);
  RNA_def_property_struct_type(prop, "EffectorWeights");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Effector Weights", "");

  /* object collections */

  prop = RNA_def_property(srna, "collision_group", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, NULL, "coll_group");
  RNA_def_property_struct_type(prop, "Collection");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Collision Collection", "Limit collisions to this collection");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset_dependency");

  prop = RNA_def_property(srna, "fluid_group", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, NULL, "fluid_group");
  RNA_def_property_struct_type(prop, "Collection");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Fluid Collection", "Limit fluid objects to this collection");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset_dependency");

  prop = RNA_def_property(srna, "effector_collection", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, NULL, "eff_group");
  RNA_def_property_struct_type(prop, "Collection");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Effector Collection", "Limit effectors to this collection");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset_dependency");

  /* grid access */

  prop = RNA_def_property(srna, "density_grid", PROP_FLOAT, PROP_NONE);
  RNA_def_property_array(prop, 32);
  RNA_def_property_flag(prop, PROP_DYNAMIC);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_dynamic_array_funcs(prop, "rna_MantaModifier_grid_get_length");
  RNA_def_property_float_funcs(prop, "rna_MantaModifier_density_grid_get", NULL, NULL);
  RNA_def_property_ui_text(prop, "Density Grid", "Smoke density grid");

  prop = RNA_def_property(srna, "velocity_grid", PROP_FLOAT, PROP_NONE);
  RNA_def_property_array(prop, 32);
  RNA_def_property_flag(prop, PROP_DYNAMIC);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_dynamic_array_funcs(prop, "rna_MantaModifier_velocity_grid_get_length");
  RNA_def_property_float_funcs(prop, "rna_MantaModifier_velocity_grid_get", NULL, NULL);
  RNA_def_property_ui_text(prop, "Velocity Grid", "Smoke velocity grid");

  prop = RNA_def_property(srna, "flame_grid", PROP_FLOAT, PROP_NONE);
  RNA_def_property_array(prop, 32);
  RNA_def_property_flag(prop, PROP_DYNAMIC);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_dynamic_array_funcs(prop, "rna_MantaModifier_grid_get_length");
  RNA_def_property_float_funcs(prop, "rna_MantaModifier_flame_grid_get", NULL, NULL);
  RNA_def_property_ui_text(prop, "Flame Grid", "Smoke flame grid");

  prop = RNA_def_property(srna, "color_grid", PROP_FLOAT, PROP_NONE);
  RNA_def_property_array(prop, 32);
  RNA_def_property_flag(prop, PROP_DYNAMIC);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_dynamic_array_funcs(prop, "rna_MantaModifier_color_grid_get_length");
  RNA_def_property_float_funcs(prop, "rna_MantaModifier_color_grid_get", NULL, NULL);
  RNA_def_property_ui_text(prop, "Color Grid", "Smoke color grid");

  prop = RNA_def_property(srna, "heat_grid", PROP_FLOAT, PROP_NONE);
  RNA_def_property_array(prop, 32);
  RNA_def_property_flag(prop, PROP_DYNAMIC);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_dynamic_array_funcs(prop, "rna_MantaModifier_heat_grid_get_length");
  RNA_def_property_float_funcs(prop, "rna_MantaModifier_heat_grid_get", NULL, NULL);
  RNA_def_property_ui_text(prop, "Heat Grid", "Smoke heat grid");

  prop = RNA_def_property(srna, "temperature_grid", PROP_FLOAT, PROP_NONE);
  RNA_def_property_array(prop, 32);
  RNA_def_property_flag(prop, PROP_DYNAMIC);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_dynamic_array_funcs(prop, "rna_MantaModifier_grid_get_length");
  RNA_def_property_float_funcs(prop, "rna_MantaModifier_temperature_grid_get", NULL, NULL);
  RNA_def_property_ui_text(
      prop, "Temperature Grid", "Smoke temperature grid, range 0..1 represents 0..1000K");

  /* domain object data */

  prop = RNA_def_property(srna,
                          "start_point",
                          PROP_FLOAT,
                          PROP_XYZ); /* can change each frame when using adaptive domain */
  RNA_def_property_float_sdna(prop, NULL, "p0");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "p0", "Start point");

  prop = RNA_def_property(srna,
                          "cell_size",
                          PROP_FLOAT,
                          PROP_XYZ); /* can change each frame when using adaptive domain */
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "cell_size", "Cell Size");

  prop = RNA_def_property(srna,
                          "domain_resolution",
                          PROP_INT,
                          PROP_XYZ); /* can change each frame when using adaptive domain */
  RNA_def_property_int_sdna(prop, NULL, "res");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "res", "Smoke Grid Resolution");

  /* adaptive domain options */

  prop = RNA_def_property(srna, "additional_res", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "adapt_res");
  RNA_def_property_range(prop, 0, 512);
  RNA_def_property_ui_text(prop, "Additional", "Maximum number of additional cells");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "adapt_margin", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "adapt_margin");
  RNA_def_property_range(prop, 2, 24);
  RNA_def_property_ui_text(
      prop, "Margin", "Margin added around fluid to minimize boundary interference");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "adapt_threshold", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.01, 0.5);
  RNA_def_property_ui_text(
      prop, "Threshold", "Maximum amount of fluid cell can contain before it is considered empty");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "use_adaptive_domain", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flags", FLUID_DOMAIN_USE_ADAPTIVE_DOMAIN);
  RNA_def_property_ui_text(
      prop, "Adaptive Domain", "Adapt simulation resolution and size to fluid");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  /* fluid domain options */

  prop = RNA_def_property(srna, "resolution_max", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "maxres");
  RNA_def_property_range(prop, 6, 10000);
  RNA_def_property_ui_range(prop, 24, 10000, 2, -1);
  RNA_def_property_ui_text(prop, "Max Res", "Resolution used for the fluid domain");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "use_collision_border_front", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "border_collisions", FLUID_DOMAIN_BORDER_FRONT);
  RNA_def_property_ui_text(prop, "Front", "Enable collisons with front domain border");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "use_collision_border_back", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "border_collisions", FLUID_DOMAIN_BORDER_BACK);
  RNA_def_property_ui_text(prop, "Back", "Enable collisons with back domain border");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "use_collision_border_right", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "border_collisions", FLUID_DOMAIN_BORDER_RIGHT);
  RNA_def_property_ui_text(prop, "Right", "Enable collisons with right domain border");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "use_collision_border_left", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "border_collisions", FLUID_DOMAIN_BORDER_LEFT);
  RNA_def_property_ui_text(prop, "Left", "Enable collisons with left domain border");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "use_collision_border_top", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "border_collisions", FLUID_DOMAIN_BORDER_TOP);
  RNA_def_property_ui_text(prop, "Top", "Enable collisons with top domain border");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "use_collision_border_bottom", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "border_collisions", FLUID_DOMAIN_BORDER_BOTTOM);
  RNA_def_property_ui_text(prop, "Bottom", "Enable collisons with bottom domain border");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "gravity", PROP_FLOAT, PROP_ACCELERATION);
  RNA_def_property_float_sdna(prop, NULL, "gravity");
  RNA_def_property_array(prop, 3);
  RNA_def_property_range(prop, -1000.1, 1000.1);
  RNA_def_property_ui_text(prop, "Gravity", "Gravity in X, Y and Z direction");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "domain_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "type");
  RNA_def_property_enum_items(prop, domain_types);
  RNA_def_property_enum_funcs(prop, NULL, "rna_Manta_domaintype_set", NULL);
  RNA_def_property_ui_text(prop, "Domain Type", "Change domain type of the simulation");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Manta_reset");

  /* smoke domain options */

  prop = RNA_def_property(srna, "alpha", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "alpha");
  RNA_def_property_range(prop, -5.0, 5.0);
  RNA_def_property_ui_range(prop, -5.0, 5.0, 0.02, 5);
  RNA_def_property_ui_text(
      prop,
      "Density",
      "How much density affects smoke motion (higher value results in faster rising smoke)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "beta", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "beta");
  RNA_def_property_range(prop, -5.0, 5.0);
  RNA_def_property_ui_range(prop, -5.0, 5.0, 0.02, 5);
  RNA_def_property_ui_text(
      prop,
      "Heat",
      "How much heat affects smoke motion (higher value results in faster rising smoke)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "dissolve_speed", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "diss_speed");
  RNA_def_property_range(prop, 1.0, 10000.0);
  RNA_def_property_ui_range(prop, 1.0, 10000.0, 1, -1);
  RNA_def_property_ui_text(prop, "Dissolve Speed", "Dissolve Speed");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "vorticity", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "vorticity");
  RNA_def_property_range(prop, 0.0, 4.0);
  RNA_def_property_ui_text(prop, "Vorticity", "Amount of turbulence/rotation in fluid");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "highres_sampling", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, smoke_highres_sampling_items);
  RNA_def_property_ui_text(prop, "Emitter", "Method for sampling the high resolution flow");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "use_dissolve_smoke", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flags", FLUID_DOMAIN_USE_DISSOLVE);
  RNA_def_property_ui_text(prop, "Dissolve Smoke", "Enable smoke to disappear over time");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "use_dissolve_smoke_log", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flags", FLUID_DOMAIN_USE_DISSOLVE_LOG);
  RNA_def_property_ui_text(prop, "Logarithmic dissolve", "Using 1/x ");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  /* flame options */

  prop = RNA_def_property(srna, "burning_rate", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.01, 4.0);
  RNA_def_property_ui_range(prop, 0.01, 2.0, 1.0, 5);
  RNA_def_property_ui_text(
      prop, "Speed", "Speed of the burning reaction (use larger values for smaller flame)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "flame_smoke", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 8.0);
  RNA_def_property_ui_range(prop, 0.0, 4.0, 1.0, 5);
  RNA_def_property_ui_text(prop, "Smoke", "Amount of smoke created by burning fuel");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "flame_vorticity", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_range(prop, 0.0, 2.0);
  RNA_def_property_ui_range(prop, 0.0, 1.0, 1.0, 5);
  RNA_def_property_ui_text(prop, "Vorticity", "Additional vorticity for the flames");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "flame_ignition", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.5, 5.0);
  RNA_def_property_ui_range(prop, 0.5, 2.5, 1.0, 5);
  RNA_def_property_ui_text(prop, "Ignition", "Minimum temperature of flames");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "flame_max_temp", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 1.0, 10.0);
  RNA_def_property_ui_range(prop, 1.0, 5.0, 1.0, 5);
  RNA_def_property_ui_text(prop, "Maximum", "Maximum temperature of flames");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "flame_smoke_color", PROP_FLOAT, PROP_COLOR_GAMMA);
  RNA_def_property_array(prop, 3);
  RNA_def_property_ui_text(prop, "Smoke Color", "Color of smoke emitted from burning fuel");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  /* noise options */

  prop = RNA_def_property(srna, "noise_strength", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "noise_strength");
  RNA_def_property_range(prop, 0.0, 10.0);
  RNA_def_property_ui_range(prop, 0.0, 10.0, 1, 2);
  RNA_def_property_ui_text(prop, "Strength", "Strength of noise");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "noise_pos_scale", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "noise_pos_scale");
  RNA_def_property_range(prop, 0.0001, 10.0);
  RNA_def_property_ui_text(
      prop, "Scale", "Scale of noise (higher value results in larger vortices)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "noise_time_anim", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "noise_time_anim");
  RNA_def_property_range(prop, 0.0001, 10.0);
  RNA_def_property_ui_text(prop, "Time", "Animation time of noise");

  prop = RNA_def_property(srna, "noise_scale", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "noise_scale");
  RNA_def_property_range(prop, 1, 100);
  RNA_def_property_ui_range(prop, 1, 10, 1, -1);
  RNA_def_property_ui_text(prop,
                           "Noise scale",
                           "Scale underlying noise grids by this factor. Noise grids have size "
                           "factor times base resolution");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "noise_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "noise_type");
  RNA_def_property_enum_items(prop, prop_noise_type_items);
  RNA_def_property_ui_text(
      prop, "Noise Method", "Noise method which is used for creating the high resolution");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "use_noise", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flags", FLUID_DOMAIN_USE_NOISE);
  RNA_def_property_ui_text(prop, "Use Noise", "Enable fluid noise (using amplification)");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  /* liquid domain options */

  prop = RNA_def_property(srna, "particle_randomness", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 10.0);
  RNA_def_property_ui_text(prop, "Randomness", "Randomness factor for particle sampling");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "particle_number", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 1, 5);
  RNA_def_property_ui_text(
      prop, "Number", "Particle number factor (higher value results in more particles)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "particle_minimum", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 0, 1000);
  RNA_def_property_ui_text(prop,
                           "Minimum",
                           "Minimum number of particles per cell (ensures that each cell has at "
                           "least this amount of particles)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "particle_maximum", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 0, 1000);
  RNA_def_property_ui_text(prop,
                           "Maximum",
                           "Maximum number of particles per cell (ensures that each cell has at "
                           "most this amount of particles)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "particle_radius", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 10.0);
  RNA_def_property_ui_text(
      prop, "Radius", "Particle radius factor (higher value results in larger particles)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "particle_band_width", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 1000.0);
  RNA_def_property_ui_text(
      prop,
      "Width",
      "Particle (narrow) band width (higher value results in thicker band and more particles)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "use_flip_particles", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "particle_type", FLUID_DOMAIN_PARTICLE_FLIP);
  RNA_def_property_ui_text(prop, "FLIP", "Create FLIP particle system");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Manta_flip_parts_update");

  /*  diffusion options */

  prop = RNA_def_property(srna, "surface_tension", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 100.0);
  RNA_def_property_ui_text(
      prop,
      "Tension",
      "Surface tension of liquid (higher value results in greater hydrophobic behaviour)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "viscosity_base", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "viscosity_base");
  RNA_def_property_range(prop, 0, 10);
  RNA_def_property_ui_text(
      prop,
      "Viscosity Base",
      "Viscosity setting: value that is multiplied by 10 to the power of (exponent*-1)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "viscosity_exponent", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "viscosity_exponent");
  RNA_def_property_range(prop, 0, 10);
  RNA_def_property_ui_text(
      prop,
      "Viscosity Exponent",
      "Negative exponent for the viscosity value (to simplify entering small values "
      "e.g. 5*10^-6)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "domain_size", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.001, 10000.0);
  RNA_def_property_ui_text(prop, "Meters", "Domain size in meters (longest domain side)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  /*  mesh options options */

  prop = RNA_def_property(srna, "mesh_concave_upper", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 10.0);
  RNA_def_property_ui_text(
      prop,
      "Upper Concavity",
      "Upper mesh concavity bound (high values tend to smoothen and fill out concave regions)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "mesh_concave_lower", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 10.0);
  RNA_def_property_ui_text(
      prop,
      "Lower Concavity",
      "Lower mesh concavity bound (high values tend to smoothen and fill out concave regions)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "mesh_smoothen_pos", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 0, 100);
  RNA_def_property_ui_text(prop, "Smoothen Pos", "Positive mesh smoothening");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "mesh_smoothen_neg", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 0, 100);
  RNA_def_property_ui_text(prop, "Smoothen Neg", "Negative mesh smoothening");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "mesh_scale", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "mesh_scale");
  RNA_def_property_range(prop, 1, 100);
  RNA_def_property_ui_range(prop, 1, 10, 1, -1);
  RNA_def_property_ui_text(prop,
                           "Mesh scale",
                           "Scale underlying mesh grids by this factor. Mesh grids have size "
                           "factor times base resolution");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "mesh_generator", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "mesh_generator");
  RNA_def_property_enum_items(prop, fluid_mesh_quality_items);
  RNA_def_property_ui_text(prop, "Mesh generator", "Which particle levelset generator to use");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Manta_update");

  prop = RNA_def_property(srna, "mesh_vertices", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, NULL, "mesh_velocities", "totvert");
  RNA_def_property_struct_type(prop, "MantaVertexVelocity");
  RNA_def_property_ui_text(
      prop, "Fluid Mesh Vertices", "Vertices of the fluid mesh generated by simulation");

  rna_def_manta_mesh_vertices(brna);

  prop = RNA_def_property(srna, "use_mesh", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flags", FLUID_DOMAIN_USE_MESH);
  RNA_def_property_ui_text(prop, "Use Mesh", "Enable fluid mesh (using amplification)");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "use_speed_vectors", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flags", FLUID_DOMAIN_USE_SPEED_VECTORS);
  RNA_def_property_ui_text(
      prop,
      "Speed Vectors",
      "Generate speed vectors (will be loaded automatically during render for motion blur)");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  /*  secondary particles options */

  prop = RNA_def_property(srna, "sndparticle_tau_min_wc", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 1000.0);
  RNA_def_property_ui_range(prop, 0.0, 1000.0, 100.0, 3);
  RNA_def_property_ui_text(prop,
                           "tauMin_wc",
                           "Lower clamping threshold for marking fluid cells as wave crests "
                           "(lower values result in more marked cells)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "sndparticle_tau_max_wc", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 1000.0);
  RNA_def_property_ui_range(prop, 0.0, 1000.0, 100.0, 3);
  RNA_def_property_ui_text(prop,
                           "tauMax_wc",
                           "Upper clamping threshold for marking fluid cells as wave crests "
                           "(higher values result in less marked cells)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "sndparticle_tau_min_ta", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 1000.0);
  RNA_def_property_ui_range(prop, 0.0, 10000.0, 100.0, 3);
  RNA_def_property_ui_text(prop,
                           "tauMin_ta",
                           "Lower clamping threshold for marking fluid cells where air is trapped "
                           "(lower values result in more marked cells)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "sndparticle_tau_max_ta", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 1000.0);
  RNA_def_property_ui_range(prop, 0.0, 1000.0, 100.0, 3);
  RNA_def_property_ui_text(prop,
                           "tauMax_ta",
                           "Upper clamping threshold for marking fluid cells where air is trapped "
                           "(higher values result in less marked cells)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "sndparticle_tau_min_k", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 1000.0);
  RNA_def_property_ui_range(prop, 0.0, 1000.0, 100.0, 3);
  RNA_def_property_ui_text(
      prop,
      "tauMin_k",
      "Lower clamping threshold that indicates the fluid speed where cells start to emit "
      "particles (lower values result in generally more particles)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "sndparticle_tau_max_k", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 1000.0);
  RNA_def_property_ui_range(prop, 0.0, 1000.0, 100.0, 3);
  RNA_def_property_ui_text(
      prop,
      "tauMax_k",
      "Upper clamping threshold that indicates the fluid speed where cells no longer emit more "
      "particles (higher values result in generally less particles)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "sndparticle_k_wc", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 0, 10000);
  RNA_def_property_ui_range(prop, 0, 10000, 1.0, -1);
  RNA_def_property_ui_text(prop,
                           "Wave Crest Sampling",
                           "Maximum number of particles generated per wave crest cell per frame");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "sndparticle_k_ta", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 0, 10000);
  RNA_def_property_ui_range(prop, 0, 10000, 1.0, -1);
  RNA_def_property_ui_text(prop,
                           "Trapped Air Sampling",
                           "Maximum number of particles generated per trapped air cell per frame");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "sndparticle_k_b", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 100.0);
  RNA_def_property_ui_range(prop, 0.0, 100.0, 10.0, 2);
  RNA_def_property_ui_text(prop,
                           "Buoyancy",
                           "Amount of buoyancy force that rises bubbles (high values result in "
                           "bubble movement mainly upwards)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "sndparticle_k_d", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 100.0);
  RNA_def_property_ui_range(prop, 0.0, 100.0, 10.0, 2);
  RNA_def_property_ui_text(prop,
                           "Drag",
                           "Amount of drag force that moves bubbles along with the fluid (high "
                           "values result in bubble movement mainly along with the fluid)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "sndparticle_l_min", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 10000.0);
  RNA_def_property_ui_range(prop, 0.0, 10000.0, 100.0, 1);
  RNA_def_property_ui_text(prop, "Lifetime(min)", "Lowest possible particle lifetime");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "sndparticle_l_max", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 10000.0);
  RNA_def_property_ui_range(prop, 0.0, 10000.0, 100.0, 1);
  RNA_def_property_ui_text(prop, "Lifetime(max)", "Highest possible particle lifetime");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "sndparticle_boundary", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "sndparticle_boundary");
  RNA_def_property_enum_items(prop, sndparticle_boundary_items);
  RNA_def_property_ui_text(
      prop, "Particles in Boundary", "How particles that left the domain are treated");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "sndparticle_combined_export", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "sndparticle_combined_export");
  RNA_def_property_enum_items(prop, sndparticle_combined_export_items);
  RNA_def_property_ui_text(
      prop,
      "Combined Export",
      "Determines which particle systems are created from secondary particles");
  RNA_def_property_update(prop, 0, "rna_Manta_combined_export_update");

  prop = RNA_def_property(srna, "sndparticle_potential_radius", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "sndparticle_potential_radius");
  RNA_def_property_range(prop, 1, 4);
  RNA_def_property_ui_range(prop, 1, 4, 1, -1);
  RNA_def_property_ui_text(prop,
                           "Potential Radius",
                           "Radius to compute potential for each cell (higher values are slower "
                           "but create smoother potential grids)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "sndparticle_update_radius", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "sndparticle_update_radius");
  RNA_def_property_range(prop, 1, 4);
  RNA_def_property_ui_range(prop, 1, 4, 1, -1);
  RNA_def_property_ui_text(prop,
                           "Update Radius",
                           "Radius to compute position update for each particle (higher values "
                           "are slower but particles move less chaotic)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "particle_scale", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "particle_scale");
  RNA_def_property_range(prop, 1, 100);
  RNA_def_property_ui_range(prop, 1, 10, 1, -1);
  RNA_def_property_ui_text(prop,
                           "Mesh scale",
                           "Scale underlying particle grids by this factor. Particle grids have "
                           "size factor times base resolution");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "use_spray_particles", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "particle_type", FLUID_DOMAIN_PARTICLE_SPRAY);
  RNA_def_property_ui_text(prop, "Spray", "Create spray particle system");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Manta_spray_parts_update");

  prop = RNA_def_property(srna, "use_bubble_particles", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "particle_type", FLUID_DOMAIN_PARTICLE_BUBBLE);
  RNA_def_property_ui_text(prop, "Bubble", "Create bubble particle system");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Manta_bubble_parts_update");

  prop = RNA_def_property(srna, "use_foam_particles", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "particle_type", FLUID_DOMAIN_PARTICLE_FOAM);
  RNA_def_property_ui_text(prop, "Foam", "Create foam particle system");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Manta_foam_parts_update");

  prop = RNA_def_property(srna, "use_tracer_particles", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "particle_type", FLUID_DOMAIN_PARTICLE_TRACER);
  RNA_def_property_ui_text(prop, "Tracer", "Create tracer particle system");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, 0, "rna_Manta_tracer_parts_update");

  /* fluid guiding options */

  prop = RNA_def_property(srna, "guiding_alpha", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "guiding_alpha");
  RNA_def_property_range(prop, 1.0, 100.0);
  RNA_def_property_ui_text(prop, "Weight", "Guiding weight (higher value results in greater lag)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "guiding_beta", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "guiding_beta");
  RNA_def_property_range(prop, 1, 50);
  RNA_def_property_ui_text(prop, "Size", "Guiding size (higher value results in larger vortices)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "guiding_vel_factor", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "guiding_vel_factor");
  RNA_def_property_range(prop, 0.0, 100.0);
  RNA_def_property_ui_text(
      prop,
      "Weight",
      "Guiding velocity factor (higher value results in bigger guiding velocities)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "guiding_source", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "guiding_source");
  RNA_def_property_enum_items(prop, fluid_guiding_source_items);
  RNA_def_property_ui_text(prop, "Guiding source", "Choose where to get guiding velocities from");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Manta_update");

  prop = RNA_def_property(srna, "guiding_parent", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, NULL, "guiding_parent");
  RNA_def_property_struct_type(prop, "Object");
  RNA_def_property_pointer_funcs(prop, NULL, "rna_Manta_guiding_parent_set", NULL, NULL);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop,
                           "",
                           "Use velocities from this object for the guiding effect (object needs "
                           "to have fluid modifier and be of type domain))");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Manta_update");

  prop = RNA_def_property(srna, "use_guiding", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flags", FLUID_DOMAIN_USE_GUIDING);
  RNA_def_property_ui_text(prop, "Use Guiding", "Enable fluid guiding");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  /*  cache options */

  prop = RNA_def_property(srna, "cache_frame_start", PROP_INT, PROP_TIME);
  RNA_def_property_int_sdna(prop, NULL, "cache_frame_start");
  RNA_def_property_range(prop, -MAXFRAME, MAXFRAME);
  RNA_def_property_ui_range(prop, 1, MAXFRAME, 1, 1);
  RNA_def_property_ui_text(prop, "Start", "Frame on which the simulation starts");

  prop = RNA_def_property(srna, "cache_frame_end", PROP_INT, PROP_TIME);
  RNA_def_property_int_sdna(prop, NULL, "cache_frame_end");
  RNA_def_property_range(prop, 1, MAXFRAME);
  RNA_def_property_ui_text(prop, "End", "Frame on which the simulation stops");

  prop = RNA_def_property(srna, "cache_frame_pause_data", PROP_INT, PROP_TIME);
  RNA_def_property_int_sdna(prop, NULL, "cache_frame_pause_data");

  prop = RNA_def_property(srna, "cache_frame_pause_noise", PROP_INT, PROP_TIME);
  RNA_def_property_int_sdna(prop, NULL, "cache_frame_pause_noise");

  prop = RNA_def_property(srna, "cache_frame_pause_mesh", PROP_INT, PROP_TIME);
  RNA_def_property_int_sdna(prop, NULL, "cache_frame_pause_mesh");

  prop = RNA_def_property(srna, "cache_frame_pause_particles", PROP_INT, PROP_TIME);
  RNA_def_property_int_sdna(prop, NULL, "cache_frame_pause_particles");

  prop = RNA_def_property(srna, "cache_frame_pause_guiding", PROP_INT, PROP_TIME);
  RNA_def_property_int_sdna(prop, NULL, "cache_frame_pause_guiding");

  prop = RNA_def_property(srna, "cache_mesh_format", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "cache_mesh_format");
  RNA_def_property_enum_items(prop, cache_file_type_items);
  RNA_def_property_enum_funcs(
      prop, NULL, "rna_Manta_cachetype_mesh_set", "rna_Manta_cachetype_mesh_itemf");
  RNA_def_property_ui_text(
      prop, "File Format", "Select the file format to be used for caching surface data");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "cache_data_format", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "cache_data_format");
  RNA_def_property_enum_items(prop, cache_file_type_items);
  RNA_def_property_enum_funcs(
      prop, NULL, "rna_Manta_cachetype_data_set", "rna_Manta_cachetype_volume_itemf");
  RNA_def_property_ui_text(
      prop, "File Format", "Select the file format to be used for caching volumetric data");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "cache_particle_format", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "cache_particle_format");
  RNA_def_property_enum_items(prop, cache_file_type_items);
  RNA_def_property_enum_funcs(
      prop, NULL, "rna_Manta_cachetype_particle_set", "rna_Manta_cachetype_particle_itemf");
  RNA_def_property_ui_text(
      prop, "File Format", "Select the file format to be used for caching particle data");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "cache_noise_format", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "cache_noise_format");
  RNA_def_property_enum_items(prop, cache_file_type_items);
  RNA_def_property_enum_funcs(
      prop, NULL, "rna_Manta_cachetype_noise_set", "rna_Manta_cachetype_volume_itemf");
  RNA_def_property_ui_text(
      prop, "File Format", "Select the file format to be used for caching noise data");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "cache_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "cache_type");
  RNA_def_property_enum_items(prop, cache_types);
  RNA_def_property_ui_text(prop, "Type", "Change the cache type of the simulation");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Manta_reset");

  prop = RNA_def_property(srna, "cache_directory", PROP_STRING, PROP_DIRPATH);
  RNA_def_property_string_maxlength(prop, FILE_MAX);
  RNA_def_property_string_funcs(prop, NULL, NULL, "rna_Manta_cache_directory_set");
  RNA_def_property_string_sdna(prop, NULL, "cache_directory");
  RNA_def_property_ui_text(prop, "Cache directory", "Directory that contains fluid cache files");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, NULL);

  prop = RNA_def_property(srna, "cache_baking_data", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "cache_flag", FLUID_DOMAIN_BAKING_DATA);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, NULL);

  prop = RNA_def_property(srna, "cache_baked_data", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "cache_flag", FLUID_DOMAIN_BAKED_DATA);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, NULL);

  prop = RNA_def_property(srna, "cache_baking_noise", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "cache_flag", FLUID_DOMAIN_BAKING_NOISE);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, NULL);

  prop = RNA_def_property(srna, "cache_baked_noise", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "cache_flag", FLUID_DOMAIN_BAKED_NOISE);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, NULL);

  prop = RNA_def_property(srna, "cache_baking_mesh", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "cache_flag", FLUID_DOMAIN_BAKING_MESH);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, NULL);

  prop = RNA_def_property(srna, "cache_baked_mesh", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "cache_flag", FLUID_DOMAIN_BAKED_MESH);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, NULL);

  prop = RNA_def_property(srna, "cache_baking_particles", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "cache_flag", FLUID_DOMAIN_BAKING_PARTICLES);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, NULL);

  prop = RNA_def_property(srna, "cache_baked_particles", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "cache_flag", FLUID_DOMAIN_BAKED_PARTICLES);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, NULL);

  prop = RNA_def_property(srna, "cache_baking_guiding", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "cache_flag", FLUID_DOMAIN_BAKING_GUIDING);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, NULL);

  prop = RNA_def_property(srna, "cache_baked_guiding", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "cache_flag", FLUID_DOMAIN_BAKED_GUIDING);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, NULL);

  prop = RNA_def_property(srna, "export_manta_script", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flags", FLUID_DOMAIN_EXPORT_MANTA_SCRIPT);
  RNA_def_property_ui_text(
      prop,
      "Export Mantaflow Script",
      "Generate and export Mantaflow script from current domain settings during bake");
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  /* time options */

  prop = RNA_def_property(srna, "time_scale", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "time_scale");
  RNA_def_property_range(prop, 0.0001, 10.0);
  RNA_def_property_ui_text(prop, "Time Scale", "Adjust simulation speed");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "cfl_condition", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "cfl_condition");
  RNA_def_property_range(prop, 0.0, 10.0);
  RNA_def_property_ui_text(
      prop, "CFL", "Maximal velocity per cell (higher value results in larger timesteps)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  prop = RNA_def_property(srna, "use_adaptive_stepping", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flags", FLUID_DOMAIN_USE_ADAPTIVE_TIME);
  RNA_def_property_ui_text(prop, "Adaptive stepping", "Enable adaptive time-stepping");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_resetCache");

  /* display settings */

  prop = RNA_def_property(srna, "slice_method", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "slice_method");
  RNA_def_property_enum_items(prop, view_items);
  RNA_def_property_ui_text(prop, "View Method", "How to slice the volume for viewport rendering");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "axis_slice_method", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "axis_slice_method");
  RNA_def_property_enum_items(prop, axis_slice_method_items);
  RNA_def_property_ui_text(prop, "Method", "");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "slice_axis", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "slice_axis");
  RNA_def_property_enum_items(prop, axis_slice_position_items);
  RNA_def_property_ui_text(prop, "Axis", "");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "slice_per_voxel", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "slice_per_voxel");
  RNA_def_property_range(prop, 0.0, 100.0);
  RNA_def_property_ui_range(prop, 0.0, 5.0, 0.1, 1);
  RNA_def_property_ui_text(
      prop, "Slice Per Voxel", "How many slices per voxel should be generated");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "slice_depth", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, NULL, "slice_depth");
  RNA_def_property_range(prop, 0.0, 1.0);
  RNA_def_property_ui_range(prop, 0.0, 1.0, 0.1, 3);
  RNA_def_property_ui_text(prop, "Position", "Position of the slice");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "display_thickness", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "display_thickness");
  RNA_def_property_range(prop, 0.001, 1000.0);
  RNA_def_property_ui_range(prop, 0.1, 100.0, 0.1, 3);
  RNA_def_property_ui_text(prop, "Thickness", "Thickness of smoke drawing in the viewport");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, NULL);

  prop = RNA_def_property(srna, "display_interpolation", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "interp_method");
  RNA_def_property_enum_items(prop, interp_method_item);
  RNA_def_property_ui_text(
      prop, "Interpolation", "Interpolation method to use for smoke/fire volumes in solid mode");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "show_velocity", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "draw_velocity", 0);
  RNA_def_property_ui_text(
      prop, "Display Velocity", "Toggle visualization of the velocity field as needles");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "vector_display_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "vector_draw_type");
  RNA_def_property_enum_items(prop, vector_draw_items);
  RNA_def_property_ui_text(prop, "Display Type", "");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "vector_scale", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "vector_scale");
  RNA_def_property_range(prop, 0.0, 1000.0);
  RNA_def_property_ui_range(prop, 0.0, 100.0, 0.1, 3);
  RNA_def_property_ui_text(prop, "Scale", "Multiplier for scaling the vectors");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  /* --------- Color mapping. --------- */

  prop = RNA_def_property(srna, "use_color_ramp", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "use_coba", 0);
  RNA_def_property_boolean_funcs(prop, NULL, "rna_Manta_use_color_ramp_set");
  RNA_def_property_ui_text(
      prop,
      "Use Color Ramp",
      "Render a simulation field while mapping its voxels values to the colors of a ramp");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  static const EnumPropertyItem coba_field_items[] = {
      {FLUID_DOMAIN_FIELD_COLOR_R, "COLOR_R", 0, "Red", "Red component of the color field"},
      {FLUID_DOMAIN_FIELD_COLOR_G, "COLOR_G", 0, "Green", "Green component of the color field"},
      {FLUID_DOMAIN_FIELD_COLOR_B, "COLOR_B", 0, "Blue", "Blue component of the color field"},
      {FLUID_DOMAIN_FIELD_DENSITY, "DENSITY", 0, "Density", "Quantity of soot in the fluid"},
      {FLUID_DOMAIN_FIELD_FLAME, "FLAME", 0, "Flame", "Flame field"},
      {FLUID_DOMAIN_FIELD_FUEL, "FUEL", 0, "Fuel", "Fuel field"},
      {FLUID_DOMAIN_FIELD_HEAT, "HEAT", 0, "Heat", "Temperature of the fluid"},
      {FLUID_DOMAIN_FIELD_VELOCITY_X,
       "VELOCITY_X",
       0,
       "X Velocity",
       "X component of the velocity field"},
      {FLUID_DOMAIN_FIELD_VELOCITY_Y,
       "VELOCITY_Y",
       0,
       "Y Velocity",
       "Y component of the velocity field"},
      {FLUID_DOMAIN_FIELD_VELOCITY_Z,
       "VELOCITY_Z",
       0,
       "Z Velocity",
       "Z component of the velocity field"},
      {0, NULL, 0, NULL, NULL},
  };

  prop = RNA_def_property(srna, "coba_field", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "coba_field");
  RNA_def_property_enum_items(prop, coba_field_items);
  RNA_def_property_ui_text(prop, "Field", "Simulation field to color map");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "color_ramp", PROP_POINTER, PROP_NEVER_NULL);
  RNA_def_property_pointer_sdna(prop, NULL, "coba");
  RNA_def_property_struct_type(prop, "ColorRamp");
  RNA_def_property_ui_text(prop, "Color Ramp", "");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, NULL);

  prop = RNA_def_property(srna, "clipping", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "clipping");
  RNA_def_property_range(prop, 0.0, 1.0);
  RNA_def_property_ui_range(prop, 0.0, 1.0, 0.1, 3);
  RNA_def_property_ui_text(
      prop,
      "Clipping",
      "Value under which voxels are considered empty space to optimize caching and rendering");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, NULL);

  /* -- Deprecated / unsed options (below)-- */

  /* pointcache options */

  prop = RNA_def_property(srna, "point_cache", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NEVER_NULL);
  RNA_def_property_pointer_sdna(prop, NULL, "point_cache[0]");
  RNA_def_property_struct_type(prop, "PointCache");
  RNA_def_property_ui_text(prop, "Point Cache", "");

  prop = RNA_def_property(srna, "point_cache_compress_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "cache_comp");
  RNA_def_property_enum_items(prop, cache_comp_items);
  RNA_def_property_ui_text(prop, "Cache Compression", "Compression method to be used");

  /* OpenVDB options */

  prop = RNA_def_property(srna, "openvdb_cache_compress_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "openvdb_comp");
  RNA_def_property_enum_items(prop, prop_compression_items);
  RNA_def_property_ui_text(prop, "Compression", "Compression method to be used");

  prop = RNA_def_property(srna, "data_depth", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_bitflag_sdna(prop, NULL, "data_depth");
  RNA_def_property_enum_items(prop, smoke_data_depth_items);
  RNA_def_property_ui_text(prop,
                           "Data Depth",
                           "Bit depth for writing all scalar (including vector) "
                           "lower values reduce file size");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, NULL);
}

static void rna_def_manta_flow_settings(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  static const EnumPropertyItem flow_types[] = {
      {FLUID_FLOW_TYPE_SMOKE, "SMOKE", 0, "Smoke", "Add smoke"},
      {FLUID_FLOW_TYPE_SMOKEFIRE, "BOTH", 0, "Fire + Smoke", "Add fire and smoke"},
      {FLUID_FLOW_TYPE_FIRE, "FIRE", 0, "Fire", "Add fire"},
      {FLUID_FLOW_TYPE_LIQUID, "LIQUID", 0, "Liquid", "Add liquid"},
      {0, NULL, 0, NULL, NULL},
  };

  static EnumPropertyItem flow_behaviors[] = {
      {FLUID_FLOW_BEHAVIOR_INFLOW, "INFLOW", 0, "Inflow", "Add fluid to simulation"},
      {FLUID_FLOW_BEHAVIOR_OUTFLOW, "OUTFLOW", 0, "Outflow", "Delete fluid from simulation"},
      {FLUID_FLOW_BEHAVIOR_GEOMETRY,
       "GEOMETRY",
       0,
       "Geometry",
       "Only use given geometry for fluid"},
      {0, NULL, 0, NULL, NULL},
  };

  /*  Flow source - generated dynamically based on flow type */
  static EnumPropertyItem flow_sources[] = {
      {0, "NONE", 0, "", ""},
      {0, NULL, 0, NULL, NULL},
  };

  static const EnumPropertyItem flow_texture_types[] = {
      {FLUID_FLOW_TEXTURE_MAP_AUTO,
       "AUTO",
       0,
       "Generated",
       "Generated coordinates centered to flow object"},
      {FLUID_FLOW_TEXTURE_MAP_UV, "UV", 0, "UV", "Use UV layer for texture coordinates"},
      {0, NULL, 0, NULL, NULL},
  };

  srna = RNA_def_struct(brna, "MantaFlowSettings", NULL);
  RNA_def_struct_ui_text(srna, "Flow Settings", "Fluid flow settings");
  RNA_def_struct_sdna(srna, "MantaFlowSettings");
  RNA_def_struct_path_func(srna, "rna_MantaFlowSettings_path");

  prop = RNA_def_property(srna, "density", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, NULL, "density");
  RNA_def_property_range(prop, 0.0, 1);
  RNA_def_property_ui_range(prop, 0.0, 1.0, 1.0, 4);
  RNA_def_property_ui_text(prop, "Density", "");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "smoke_color", PROP_FLOAT, PROP_COLOR_GAMMA);
  RNA_def_property_float_sdna(prop, NULL, "color");
  RNA_def_property_array(prop, 3);
  RNA_def_property_ui_text(prop, "Smoke Color", "Color of smoke");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "fuel_amount", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 10);
  RNA_def_property_ui_range(prop, 0.0, 5.0, 1.0, 4);
  RNA_def_property_ui_text(prop, "Flame Rate", "");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "temperature", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "temp");
  RNA_def_property_range(prop, -10, 10);
  RNA_def_property_ui_range(prop, -10, 10, 1, 1);
  RNA_def_property_ui_text(prop, "Temp. Diff.", "Temperature difference to ambient temperature");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "particle_system", PROP_POINTER, PROP_NONE);
  RNA_def_property_pointer_sdna(prop, NULL, "psys");
  RNA_def_property_struct_type(prop, "ParticleSystem");
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Particle Systems", "Particle systems emitted from the object");
  RNA_def_property_update(prop, 0, "rna_Manta_reset_dependency");

  prop = RNA_def_property(srna, "flow_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "type");
  RNA_def_property_enum_items(prop, flow_types);
  RNA_def_property_enum_funcs(prop, NULL, "rna_Manta_flowtype_set", NULL);
  RNA_def_property_ui_text(prop, "Flow Type", "Change type of fluid in the simulation");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "flow_behavior", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "behavior");
  RNA_def_property_enum_items(prop, flow_behaviors);
  RNA_def_property_ui_text(prop, "Flow Behavior", "Change flow behavior in the simulation");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "flow_source", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "source");
  RNA_def_property_enum_items(prop, flow_sources);
  RNA_def_property_enum_funcs(
      prop, NULL, "rna_Manta_flowsource_set", "rna_Manta_flowsource_itemf");
  RNA_def_property_ui_text(prop, "Source", "Change how fluid is emitted");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "use_absolute", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flags", FLUID_FLOW_ABSOLUTE);
  RNA_def_property_ui_text(prop,
                           "Absolute Density",
                           "Only allow given density value in emitter area and will not add up");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "use_initial_velocity", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flags", FLUID_FLOW_INITVELOCITY);
  RNA_def_property_ui_text(
      prop, "Initial Velocity", "Fluid has some initial velocity when it is emitted");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "velocity_factor", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "vel_multi");
  RNA_def_property_range(prop, -100.0, 100.0);
  RNA_def_property_ui_range(prop, -2.0, 2.0, 0.05, 5);
  RNA_def_property_ui_text(prop,
                           "Source",
                           "Multiplier of source velocity passed to fluid (source velocity is "
                           "non-zero only if object is moving)");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "velocity_normal", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "vel_normal");
  RNA_def_property_range(prop, -100.0, 100.0);
  RNA_def_property_ui_range(prop, -2.0, 2.0, 0.05, 5);
  RNA_def_property_ui_text(prop, "Normal", "Amount of normal directional velocity");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "velocity_random", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "vel_random");
  RNA_def_property_range(prop, 0.0, 10.0);
  RNA_def_property_ui_range(prop, 0.0, 2.0, 0.05, 5);
  RNA_def_property_ui_text(prop, "Random", "Amount of random velocity");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "velocity_coord", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_float_sdna(prop, NULL, "vel_coord");
  RNA_def_property_array(prop, 3);
  RNA_def_property_range(prop, -1000.1, 1000.1);
  RNA_def_property_ui_text(prop, "Initial", "Initial velocity in X, Y and Z direction");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "volume_density", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 1.0);
  RNA_def_property_ui_range(prop, 0.0, 1.0, 0.05, 5);
  RNA_def_property_ui_text(prop, "Volume", "Factor for smoke emitted from inside the mesh volume");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "surface_distance", PROP_FLOAT, PROP_DISTANCE);
  RNA_def_property_range(prop, 0.0, 10.0);
  RNA_def_property_ui_range(prop, 0.5, 5.0, 0.05, 5);
  RNA_def_property_ui_text(prop, "Surface", "Maximum distance from mesh surface to emit fluid");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "particle_size", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.1, 20.0);
  RNA_def_property_ui_range(prop, 0.5, 5.0, 0.05, 5);
  RNA_def_property_ui_text(prop, "Size", "Particle size in simulation cells");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "use_particle_size", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flags", FLUID_FLOW_USE_PART_SIZE);
  RNA_def_property_ui_text(
      prop, "Set Size", "Set particle size in simulation cells or use nearest cell");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "use_inflow", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flags", FLUID_FLOW_USE_INFLOW);
  RNA_def_property_ui_text(prop, "Enabled", "Control when to apply inflow");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "subframes", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 0, 50);
  RNA_def_property_ui_range(prop, 0, 10, 1, -1);
  RNA_def_property_ui_text(prop,
                           "Subframes",
                           "Number of additional samples to take between frames to improve "
                           "quality of fast moving flows");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "density_vertex_group", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_MantaFlow_density_vgroup_get",
                                "rna_MantaFlow_density_vgroup_length",
                                "rna_MantaFlow_density_vgroup_set");
  RNA_def_property_ui_text(
      prop, "Vertex Group", "Name of vertex group which determines surface emission rate");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "use_texture", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flags", FLUID_FLOW_TEXTUREEMIT);
  RNA_def_property_ui_text(prop, "Use Texture", "Use a texture to control emission strength");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "texture_map_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "texture_type");
  RNA_def_property_enum_items(prop, flow_texture_types);
  RNA_def_property_ui_text(prop, "Mapping", "Texture mapping type");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "uv_layer", PROP_STRING, PROP_NONE);
  RNA_def_property_string_sdna(prop, NULL, "uvlayer_name");
  RNA_def_property_ui_text(prop, "UV Map", "UV map name");
  RNA_def_property_string_funcs(prop, NULL, NULL, "rna_MantaFlow_uvlayer_set");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "noise_texture", PROP_POINTER, PROP_NONE);
  RNA_def_property_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Texture", "Texture that controls emission strength");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "texture_size", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.01, 10.0);
  RNA_def_property_ui_range(prop, 0.1, 5.0, 0.05, 5);
  RNA_def_property_ui_text(prop, "Size", "Size of texture mapping");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "texture_offset", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 200.0);
  RNA_def_property_ui_range(prop, 0.0, 100.0, 0.05, 5);
  RNA_def_property_ui_text(prop, "Offset", "Z-offset of texture mapping");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");
}

static void rna_def_manta_effec_settings(BlenderRNA *brna)
{
  static EnumPropertyItem effec_type_items[] = {
      {FLUID_EFFECTOR_TYPE_COLLISION, "COLLISION", 0, "Collision", "Create collision object"},
      {FLUID_EFFECTOR_TYPE_GUIDE, "GUIDE", 0, "Guide", "Create guiding object"},
      {0, NULL, 0, NULL, NULL},
  };

  static EnumPropertyItem fluid_guiding_mode_items[] = {
      {FLUID_EFFECTOR_GUIDING_MAXIMUM,
       "MAXIMUM",
       0,
       "Maximize",
       "Compare velocities from previous frame with new velocities from current frame and keep "
       "the maximum"},
      {FLUID_EFFECTOR_GUIDING_MINIMUM,
       "MINIMUM",
       0,
       "Minimize",
       "Compare velocities from previous frame with new velocities from current frame and keep "
       "the minimum"},
      {FLUID_EFFECTOR_GUIDING_OVERRIDE,
       "OVERRIDE",
       0,
       "Override",
       "Always write new guiding velocities for every frame (each frame only contains current "
       "velocities from guiding objects)"},
      {FLUID_EFFECTOR_GUIDING_AVERAGED,
       "AVERAGED",
       0,
       "Averaged",
       "Take average of velocities from previous frame and new velocities from current frame"},
      {0, NULL, 0, NULL, NULL},
  };

  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "MantaCollSettings", NULL);
  RNA_def_struct_ui_text(srna, "Collision Settings", "Smoke collision settings");
  RNA_def_struct_sdna(srna, "MantaCollSettings");
  RNA_def_struct_path_func(srna, "rna_MantaCollSettings_path");

  prop = RNA_def_property(srna, "effec_type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "type");
  RNA_def_property_enum_items(prop, effec_type_items);
  RNA_def_property_ui_text(prop, "Effector Type", "Change type of effector in the simulation");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "surface_distance", PROP_FLOAT, PROP_NONE);
  RNA_def_property_range(prop, 0.0, 10.0);
  RNA_def_property_ui_text(
      prop, "Distance", "Distance around mesh surface to consider as effector");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "velocity_factor", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "vel_multi");
  RNA_def_property_range(prop, -100.0, 100.0);
  RNA_def_property_ui_text(prop, "Source", "Multiplier of obstacle velocity");
  RNA_def_property_update(prop, NC_OBJECT | ND_MODIFIER, "rna_Manta_reset");

  prop = RNA_def_property(srna, "guiding_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "guiding_mode");
  RNA_def_property_enum_items(prop, fluid_guiding_mode_items);
  RNA_def_property_ui_text(prop, "Guiding mode", "How to create guiding velocities");
  RNA_def_property_update(prop, NC_OBJECT | ND_DRAW, "rna_Manta_update");
}

void RNA_def_manta(BlenderRNA *brna)
{
  rna_def_manta_domain_settings(brna);
  rna_def_manta_flow_settings(brna);
  rna_def_manta_effec_settings(brna);
}

#endif

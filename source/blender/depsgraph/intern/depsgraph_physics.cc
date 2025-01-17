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
 * The Original Code is Copyright (C) 2018 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup depsgraph
 *
 * Physics utilities for effectors and collision.
 */

#include "intern/depsgraph_physics.h"

#include "MEM_guardedalloc.h"

#include "BLI_compiler_compat.h"
#include "BLI_ghash.h"
#include "BLI_listbase.h"

extern "C" {
#include "BKE_collision.h"
#include "BKE_effect.h"
#include "BKE_modifier.h"
} /* extern "C" */

#include "DNA_collection_types.h"
#include "DNA_object_types.h"
#include "DNA_object_force_types.h"

#include "DEG_depsgraph_build.h"
#include "DEG_depsgraph_physics.h"
#include "DEG_depsgraph_query.h"

#include "depsgraph.h"

/*************************** Evaluation Query API *****************************/

static ePhysicsRelationType modifier_to_relation_type(unsigned int modifier_type)
{
  switch (modifier_type) {
    case eModifierType_Collision:
      return DEG_PHYSICS_COLLISION;
    case eModifierType_Manta:
      return DEG_PHYSICS_SMOKE_COLLISION;
    case eModifierType_DynamicPaint:
      return DEG_PHYSICS_DYNAMIC_BRUSH;
  }

  BLI_assert(!"Unknown collision modifier type");
  return DEG_PHYSICS_RELATIONS_NUM;
}

ListBase *DEG_get_effector_relations(const Depsgraph *graph, Collection *collection)
{
  const DEG::Depsgraph *deg_graph = reinterpret_cast<const DEG::Depsgraph *>(graph);
  if (deg_graph->physics_relations[DEG_PHYSICS_EFFECTOR] == NULL) {
    return NULL;
  }

  ID *collection_orig = DEG_get_original_id(&collection->id);
  return (ListBase *)BLI_ghash_lookup(deg_graph->physics_relations[DEG_PHYSICS_EFFECTOR],
                                      collection_orig);
}

ListBase *DEG_get_collision_relations(const Depsgraph *graph,
                                      Collection *collection,
                                      unsigned int modifier_type)
{
  const DEG::Depsgraph *deg_graph = reinterpret_cast<const DEG::Depsgraph *>(graph);
  const ePhysicsRelationType type = modifier_to_relation_type(modifier_type);
  if (deg_graph->physics_relations[type] == NULL) {
    return NULL;
  }
  ID *collection_orig = DEG_get_original_id(&collection->id);
  return (ListBase *)BLI_ghash_lookup(deg_graph->physics_relations[type], collection_orig);
}

/********************** Depsgraph Building API ************************/

void DEG_add_collision_relations(DepsNodeHandle *handle,
                                 Object *object,
                                 Collection *collection,
                                 unsigned int modifier_type,
                                 DEG_CollobjFilterFunction filter_function,
                                 const char *name)
{
  Depsgraph *depsgraph = DEG_get_graph_from_handle(handle);
  DEG::Depsgraph *deg_graph = (DEG::Depsgraph *)depsgraph;
  ListBase *relations = build_collision_relations(deg_graph, collection, modifier_type);
  LISTBASE_FOREACH (CollisionRelation *, relation, relations) {
    Object *ob1 = relation->ob;
    if (ob1 == object) {
      continue;
    }
    if (filter_function == NULL ||
        filter_function(ob1, modifiers_findByType(ob1, (ModifierType)modifier_type))) {
      DEG_add_object_pointcache_relation(handle, ob1, DEG_OB_COMP_TRANSFORM, name);
      DEG_add_object_pointcache_relation(handle, ob1, DEG_OB_COMP_GEOMETRY, name);
    }
  }
}

void DEG_add_forcefield_relations(DepsNodeHandle *handle,
                                  Object *object,
                                  EffectorWeights *effector_weights,
                                  bool add_absorption,
                                  int skip_forcefield,
                                  const char *name)
{
  Depsgraph *depsgraph = DEG_get_graph_from_handle(handle);
  DEG::Depsgraph *deg_graph = (DEG::Depsgraph *)depsgraph;
  ListBase *relations = build_effector_relations(deg_graph, effector_weights->group);
  LISTBASE_FOREACH (EffectorRelation *, relation, relations) {
    if (relation->ob == object) {
      continue;
    }
    if (relation->pd->forcefield == skip_forcefield) {
      continue;
    }

    /* Relation to forcefield object, optionally including geometry.
     * Use special point cache relations for automatic cache clearing. */
    DEG_add_object_pointcache_relation(handle, relation->ob, DEG_OB_COMP_TRANSFORM, name);

    if (relation->psys || ELEM(relation->pd->shape, PFIELD_SHAPE_SURFACE, PFIELD_SHAPE_POINTS) ||
        relation->pd->forcefield == PFIELD_GUIDE) {
      /* TODO(sergey): Consider going more granular with more dedicated
       * particle system operation. */
      DEG_add_object_pointcache_relation(handle, relation->ob, DEG_OB_COMP_GEOMETRY, name);
    }

    /* Smoke flow relations. */
    if (relation->pd->forcefield == PFIELD_SMOKEFLOW && relation->pd->f_source != NULL) {
      DEG_add_object_pointcache_relation(
          handle, relation->pd->f_source, DEG_OB_COMP_TRANSFORM, "Smoke Force Domain");
      DEG_add_object_pointcache_relation(
          handle, relation->pd->f_source, DEG_OB_COMP_GEOMETRY, "Smoke Force Domain");
    }

    /* Absorption forces need collision relation. */
    if (add_absorption && (relation->pd->flag & PFIELD_VISIBILITY)) {
      DEG_add_collision_relations(
          handle, object, NULL, eModifierType_Collision, NULL, "Force Absorption");
    }
  }
}

/******************************** Internal API ********************************/

namespace DEG {

ListBase *build_effector_relations(Depsgraph *graph, Collection *collection)
{
  GHash *hash = graph->physics_relations[DEG_PHYSICS_EFFECTOR];
  if (hash == NULL) {
    graph->physics_relations[DEG_PHYSICS_EFFECTOR] = BLI_ghash_ptr_new(
        "Depsgraph physics relations hash");
    hash = graph->physics_relations[DEG_PHYSICS_EFFECTOR];
  }
  ListBase *relations = reinterpret_cast<ListBase *>(BLI_ghash_lookup(hash, collection));
  if (relations == NULL) {
    ::Depsgraph *depsgraph = reinterpret_cast<::Depsgraph *>(graph);
    relations = BKE_effector_relations_create(depsgraph, graph->view_layer, collection);
    BLI_ghash_insert(hash, &collection->id, relations);
  }
  return relations;
}

ListBase *build_collision_relations(Depsgraph *graph,
                                    Collection *collection,
                                    unsigned int modifier_type)
{
  const ePhysicsRelationType type = modifier_to_relation_type(modifier_type);
  GHash *hash = graph->physics_relations[type];
  if (hash == NULL) {
    graph->physics_relations[type] = BLI_ghash_ptr_new("Depsgraph physics relations hash");
    hash = graph->physics_relations[type];
  }
  ListBase *relations = reinterpret_cast<ListBase *>(BLI_ghash_lookup(hash, collection));
  if (relations == NULL) {
    ::Depsgraph *depsgraph = reinterpret_cast<::Depsgraph *>(graph);
    relations = BKE_collision_relations_create(depsgraph, collection, modifier_type);
    BLI_ghash_insert(hash, &collection->id, relations);
  }
  return relations;
}

namespace {

void free_effector_relations(void *value)
{
  BKE_effector_relations_free(reinterpret_cast<ListBase *>(value));
}

void free_collision_relations(void *value)
{
  BKE_collision_relations_free(reinterpret_cast<ListBase *>(value));
}

}  // namespace

void clear_physics_relations(Depsgraph *graph)
{
  for (int i = 0; i < DEG_PHYSICS_RELATIONS_NUM; i++) {
    if (graph->physics_relations[i]) {
      const ePhysicsRelationType type = (ePhysicsRelationType)i;

      switch (type) {
        case DEG_PHYSICS_EFFECTOR:
          BLI_ghash_free(graph->physics_relations[i], NULL, free_effector_relations);
          break;
        case DEG_PHYSICS_COLLISION:
        case DEG_PHYSICS_SMOKE_COLLISION:
        case DEG_PHYSICS_DYNAMIC_BRUSH:
          BLI_ghash_free(graph->physics_relations[i], NULL, free_collision_relations);
          break;
        case DEG_PHYSICS_RELATIONS_NUM:
          break;
      }
      graph->physics_relations[i] = NULL;
    }
  }
}

}  // namespace DEG

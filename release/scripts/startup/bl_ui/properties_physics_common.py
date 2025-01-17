# ##### BEGIN GPL LICENSE BLOCK #####
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software Foundation,
#  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# ##### END GPL LICENSE BLOCK #####

# <pep8 compliant>

import bpy
from bpy.types import (
    Panel,
)
from bpy.app.translations import contexts as i18n_contexts


class PhysicButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "physics"

    @classmethod
    def poll(cls, context):
        return (context.object) and context.engine in cls.COMPAT_ENGINES


def physics_add(layout, md, name, type, typeicon, toggles):
    row = layout.row(align=True)
    if md:
        row.context_pointer_set("modifier", md)
        row.operator(
            "object.modifier_remove",
            text=name,
            text_ctxt=i18n_contexts.default,
            icon='X',
        )
        if toggles:
            row.prop(md, "show_render", text="")
            row.prop(md, "show_viewport", text="")
    else:
        row.operator(
            "object.modifier_add",
            text=name,
            text_ctxt=i18n_contexts.default,
            icon=typeicon,
        ).type = type


def physics_add_special(layout, data, name, addop, removeop, typeicon):
    row = layout.row(align=True)
    if data:
        row.operator(removeop, text=name, text_ctxt=i18n_contexts.default, icon='X')
    else:
        row.operator(addop, text=name, text_ctxt=i18n_contexts.default, icon=typeicon)


class PHYSICS_PT_add(PhysicButtonsPanel, Panel):
    bl_label = ""
    bl_options = {'HIDE_HEADER'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}

    def draw(self, context):
        layout = self.layout

        row = layout.row(align=True)
        row.alignment = 'LEFT'
        row.label(text="Enable physics for:")

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=True)

        obj = context.object

        col = flow.column()

        if obj.field.type == 'NONE':
            col.operator("object.forcefield_toggle", text="Force Field", icon='FORCE_FORCE')
        else:
            col.operator("object.forcefield_toggle", text="Force Field", icon='X')

        if obj.type == 'MESH':
            physics_add(col, context.collision, "Collision", 'COLLISION', 'MOD_PHYSICS', False)
            physics_add(col, context.cloth, "Cloth", 'CLOTH', 'MOD_CLOTH', True)
            physics_add(col, context.dynamic_paint, "Dynamic Paint", 'DYNAMIC_PAINT', 'MOD_DYNAMICPAINT', True)

        col = flow.column()

        if obj.type in {'MESH', 'LATTICE', 'CURVE', 'SURFACE', 'FONT'}:
            physics_add(col, context.soft_body, "Soft Body", 'SOFT_BODY', 'MOD_SOFT', True)

        if obj.type == 'MESH':
            physics_add(col, context.manta, "Fluid", 'MANTA', 'MOD_MANTA', True)

            physics_add_special(
                col, obj.rigid_body, "Rigid Body",
                "rigidbody.object_add",
                "rigidbody.object_remove",
                'RIGID_BODY'
            )

        # all types of objects can have rigid body constraint.
        physics_add_special(
            col, obj.rigid_body_constraint, "Rigid Body Constraint",
            "rigidbody.constraint_add",
            "rigidbody.constraint_remove",
            'RIGID_BODY_CONSTRAINT'
        )


# cache-type can be 'PSYS' 'HAIR' 'MANTA' etc.

def point_cache_ui(self, cache, enabled, cachetype):
    layout = self.layout
    layout.use_property_split = True

    layout.context_pointer_set("point_cache", cache)

    is_saved = bpy.data.is_saved

    # NOTE: TODO temporarily used until the animate properties are properly skipped.
    layout.use_property_decorate = False  # No animation (remove this later on).

    if not cachetype == 'RIGID_BODY':
        row = layout.row()
        row.template_list(
            "UI_UL_list", "point_caches", cache, "point_caches",
            cache.point_caches, "active_index", rows=1,
        )
        col = row.column(align=True)
        col.operator("ptcache.add", icon='ADD', text="")
        col.operator("ptcache.remove", icon='REMOVE', text="")

    if cachetype in {'PSYS', 'HAIR', 'MANTA'}:
        col = layout.column()

        if cachetype == 'MANTA':
            col.prop(cache, "use_library_path", text="Use Library Path")

        col.prop(cache, "use_external")

    if cache.use_external:
        col = layout.column()
        col.prop(cache, "index", text="Index")
        col.prop(cache, "filepath", text="Path")

        cache_info = cache.info
        if cache_info:
            col = layout.column()
            col.alignment = 'RIGHT'
            col.label(text=cache_info)
    else:
        if cachetype in {'MANTA', 'DYNAMIC_PAINT'}:
            if not is_saved:
                col = layout.column(align=True)
                col.alignment = 'RIGHT'
                col.label(text="Cache is disabled until the file is saved")
                layout.enabled = False

    if not cache.use_external or cachetype == 'MANTA':
        col = layout.column(align=True)

        if cachetype not in {'PSYS', 'DYNAMIC_PAINT'}:
            col.enabled = enabled
            col.prop(cache, "frame_start", text="Simulation Start")
            col.prop(cache, "frame_end")

        if cachetype not in {'MANTA', 'CLOTH', 'DYNAMIC_PAINT', 'RIGID_BODY'}:
            col.prop(cache, "frame_step")

        cache_info = cache.info
        if cachetype != 'MANTA' and cache_info:  # avoid empty space.
            col = layout.column(align=True)
            col.alignment = 'RIGHT'
            col.label(text=cache_info)

        can_bake = True

        if cachetype not in {'MANTA', 'DYNAMIC_PAINT', 'RIGID_BODY'}:
            if not is_saved:
                col = layout.column(align=True)
                col.alignment = 'RIGHT'
                col.label(text="Options are disabled until the file is saved")

            flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=True)
            flow.enabled = enabled and is_saved

            col = flow.column(align=True)
            col.prop(cache, "use_disk_cache")

            subcol = col.column()
            subcol.active = cache.use_disk_cache
            subcol.prop(cache, "use_library_path", text="Use Library Path")

            col = flow.column()
            col.active = cache.use_disk_cache
            col.prop(cache, "compression", text="Compression")

            if cache.id_data.library and not cache.use_disk_cache:
                can_bake = False

                col = layout.column(align=True)
                col.alignment = 'RIGHT'

                col.separator()

                col.label(text="Linked object baking requires Disk Cache to be enabled")
        else:
            layout.separator()

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)
        col = flow.column()
        col.active = can_bake

        if cache.is_baked is True:
            col.operator("ptcache.free_bake", text="Delete Bake")
        else:
            col.operator("ptcache.bake", text="Bake").bake = True

        sub = col.row()
        sub.enabled = (cache.is_frame_skip or cache.is_outdated) and enabled
        sub.operator("ptcache.bake", text="Calculate To Frame").bake = False

        sub = col.column()
        sub.enabled = enabled
        sub.operator("ptcache.bake_from_cache", text="Current Cache to Bake")

        col = flow.column()
        col.operator("ptcache.bake_all", text="Bake All Dynamics").bake = True
        col.operator("ptcache.free_bake_all", text="Delete All Bakes")
        col.operator("ptcache.bake_all", text="Update All To Frame").bake = False


def effector_weights_ui(self, weights, weight_type):
    layout = self.layout
    layout.use_property_split = True

    # NOTE: TODO temporarily used until the animate properties are properly skipped.
    layout.use_property_decorate = False  # No animation (remove this later on).

    layout.prop(weights, "collection")

    flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=True)

    col = flow.column()
    col.prop(weights, "gravity", slider=True)
    col.prop(weights, "all", slider=True)
    col.prop(weights, "force", slider=True)
    col.prop(weights, "vortex", slider=True)

    col = flow.column()
    col.prop(weights, "magnetic", slider=True)
    col.prop(weights, "harmonic", slider=True)
    col.prop(weights, "charge", slider=True)
    col.prop(weights, "lennardjones", slider=True)

    col = flow.column()
    col.prop(weights, "wind", slider=True)
    col.prop(weights, "curve_guide", slider=True)
    col.prop(weights, "texture", slider=True)

    if weight_type != 'MANTA':
        col.prop(weights, "smokeflow", slider=True)

    col = flow.column()
    col.prop(weights, "turbulence", slider=True)
    col.prop(weights, "drag", slider=True)
    col.prop(weights, "boid", slider=True)


def basic_force_field_settings_ui(self, field):
    layout = self.layout
    layout.use_property_split = True

    if not field or field.type == 'NONE':
        return

    flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=True)

    col = flow.column()

    if field.type == 'DRAG':
        col.prop(field, "linear_drag", text="Linear")
    else:
        col.prop(field, "strength")

    if field.type == 'TURBULENCE':
        col.prop(field, "size")
        col.prop(field, "flow")

    elif field.type == 'HARMONIC':
        col.prop(field, "harmonic_damping", text="Damping")
        col.prop(field, "rest_length")

    elif field.type == 'VORTEX' and field.shape != 'POINT':
        col.prop(field, "inflow")

    elif field.type == 'DRAG':
        col.prop(field, "quadratic_drag", text="Quadratic")

    else:
        col.prop(field, "flow")

    col.prop(field, "apply_to_location", text="Affect Location")
    col.prop(field, "apply_to_rotation", text="Affect Rotation")

    col = flow.column()
    sub = col.column(align=True)
    sub.prop(field, "noise", text="Noise Amount")
    sub.prop(field, "seed", text="Seed")

    if field.type == 'TURBULENCE':
        col.prop(field, "use_global_coords", text="Global")

    elif field.type == 'HARMONIC':
        col.prop(field, "use_multiple_springs")

    if field.type == 'FORCE':
        col.prop(field, "use_gravity_falloff", text="Gravitation")

    col.prop(field, "use_absorption")


def basic_force_field_falloff_ui(self, field):
    layout = self.layout

    if not field or field.type == 'NONE':
        return

    flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=True)

    col = flow.column()
    col.prop(field, "z_direction")
    col.prop(field, "falloff_power", text="Power")

    col = flow.column()
    col.prop(field, "use_min_distance", text="Use Minimum")

    sub = col.column(align=True)
    sub.active = field.use_min_distance
    sub.prop(field, "distance_min", text="Min Distance")

    col = flow.column()
    col.prop(field, "use_max_distance", text="Use Maximum")

    sub = col.column(align=True)
    sub.active = field.use_max_distance
    sub.prop(field, "distance_max", text="Max Distance")


classes = (
    PHYSICS_PT_add,
)


if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)

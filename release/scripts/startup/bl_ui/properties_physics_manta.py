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
    Menu,
)
from .properties_physics_common import (
    effector_weights_ui,
)

class MANTA_MT_presets(Menu):
    bl_label = "Fluid Presets"
    preset_subdir = "mantaflow"
    preset_operator = "script.execute_preset"
    draw = Menu.draw_preset

class PhysicButtonsPanel:
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "physics"

    @staticmethod
    def poll_fluid(context):
        ob = context.object
        if not ((ob and ob.type == 'MESH') and (context.manta)):
            return False

        md = context.manta
        return md and (context.manta.manta_type != 'NONE') and (bpy.app.build_options.manta)

    @staticmethod
    def poll_fluid_domain(context):
        if not PhysicButtonsPanel.poll_fluid(context):
            return False

        md = context.manta
        return md and (md.manta_type == 'DOMAIN')

    @staticmethod
    def poll_gas_domain(context):
        if not PhysicButtonsPanel.poll_fluid(context):
            return False

        md = context.manta
        if md and (md.manta_type == 'DOMAIN'):
            domain = md.domain_settings
            return domain.domain_type in {'GAS'}
        return False

    @staticmethod
    def poll_liquid_domain(context):
        if not PhysicButtonsPanel.poll_fluid(context):
            return False

        md = context.manta
        if md and (md.manta_type == 'DOMAIN'):
            domain = md.domain_settings
            return domain.domain_type in {'LIQUID'}
        return False

    @staticmethod
    def poll_fluid_flow(context):
        if not PhysicButtonsPanel.poll_fluid(context):
            return False

        md = context.manta
        return md and (md.manta_type == 'FLOW')


class PHYSICS_PT_manta(PhysicButtonsPanel, Panel):
    bl_label = "Fluid"
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        ob = context.object
        return (ob and ob.type == 'MESH') and (context.engine in cls.COMPAT_ENGINES) and (context.manta)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        if not bpy.app.build_options.manta:
            col = layout.column(align=True)
            col.alignment = 'RIGHT'
            col.label(text="Built without Fluid modifier")
            return

        md = context.manta

        layout.prop(md, "manta_type")


class PHYSICS_PT_manta_fluid(PhysicButtonsPanel, Panel):
    bl_label = "Settings"
    bl_parent_id = 'PHYSICS_PT_manta'
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_fluid(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        md = context.manta
        ob = context.object
        scene = context.scene

        if md.manta_type == 'DOMAIN':
            domain = md.domain_settings

            # Deactivate UI if guiding is enabled but not baked yet
            layout.active = not (domain.use_guiding and not domain.cache_baked_guiding and (domain.guiding_source == "EFFECTOR" or (domain.guiding_source == "DOMAIN" and not domain.guiding_parent)))

            baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
            baked_any = domain.cache_baked_data or domain.cache_baked_mesh or domain.cache_baked_particles or domain.cache_baked_noise or domain.cache_baked_guiding
            baked_data = domain.cache_baked_data

            row = layout.row()
            row.enabled = not baking_any and not baked_data
            row.prop(domain, "domain_type", expand=False)

            flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)
            flow.enabled = not baking_any and not baked_data

            col = flow.column()
            col.prop(domain, "resolution_max", text="Resolution Divisions")
            col.prop(domain, "use_adaptive_stepping", text="Use Adaptive Stepping")
            col.prop(domain, "time_scale", text="Time Scale")
            col.prop(domain, "cfl_condition", text="CFL Number")

            col.separator()

            col = flow.column()
            if scene.use_gravity:
                sub = col.column()
                sub.enabled = False
                sub.prop(domain, "gravity", text="Using Scene Gravity", icon='SCENE_DATA')
            else:
                col.prop(domain, "gravity", text="Gravity")
            # TODO (sebas): Clipping var useful for manta openvdb caching?
            # col.prop(domain, "clipping", text="Empty Space")

            if domain.cache_type == "MODULAR":
                col.separator()
                split = layout.split()

                bake_incomplete = (domain.cache_frame_pause_data < domain.cache_frame_end)
                if domain.cache_baked_data and not domain.cache_baking_data and bake_incomplete:
                    col = split.column()
                    col.operator("manta.bake_data", text="Resume")
                    col = split.column()
                    col.operator("manta.free_data", text="Free")
                elif domain.cache_baking_data and not domain.cache_baked_data:
                    split.enabled = False
                    split.operator("manta.pause_bake", text="Baking Data - ESC to pause")
                elif not domain.cache_baked_data and not domain.cache_baking_data:
                    split.operator("manta.bake_data", text="Bake Data")
                else:
                    split.operator("manta.free_data", text="Free Data")

        elif md.manta_type == 'FLOW':
            flow = md.flow_settings

            row = layout.row()
            row.prop(flow, "flow_type", expand=False)

            grid = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)

            col = grid.column()
            col.prop(flow, "flow_behavior", expand=False)
            if flow.flow_behavior in {'INFLOW'}:
                col.prop(flow, "use_inflow", text="Use Inflow")

            col.prop(flow, "subframes", text="Sampling Substeps")

            if not flow.flow_behavior == 'OUTFLOW' and flow.flow_type in {'SMOKE', 'BOTH', 'FIRE'}:

                if flow.flow_type in {'SMOKE', 'BOTH'}:
                    col.prop(flow, "smoke_color", text="Smoke Color")

                col = grid.column(align=True)
                col.prop(flow, "use_absolute", text="Absolute Density")

                if flow.flow_type in {'SMOKE', 'BOTH'}:
                    col.prop(flow, "temperature", text="Initial Temperature")
                    col.prop(flow, "density", text="Density")

                if flow.flow_type in {'FIRE', 'BOTH'}:
                    col.prop(flow, "fuel_amount", text="Fuel")

                col.separator()
                col.prop_search(flow, "density_vertex_group", ob, "vertex_groups", text="Vertex Group")

        elif md.manta_type == 'EFFECTOR':
            effec = md.effec_settings

            row = layout.row()
            row.prop(effec, "effec_type")

            flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)

            col = flow.column()

            col.prop(effec, "surface_distance", text="Surface Thickness")

            if effec.effec_type == "GUIDE":
                col.prop(effec, "velocity_factor", text="Velocity Factor")
                col = flow.column()
                col.prop(effec, "guiding_mode", text="Guiding Mode")

class PHYSICS_PT_manta_borders(PhysicButtonsPanel, Panel):
    bl_label = "Border Collisions"
    bl_parent_id = 'PHYSICS_PT_manta_fluid'
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_fluid_domain(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        md = context.manta
        domain = md.domain_settings

        baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
        baked_any = domain.cache_baked_data or domain.cache_baked_mesh or domain.cache_baked_particles or domain.cache_baked_noise or domain.cache_baked_guiding
        baked_data = domain.cache_baked_data

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)
        flow.enabled = not baking_any and not baked_data

        col = flow.column()
        col.prop(domain, "use_collision_border_front", text="Front")
        col = flow.column()
        col.prop(domain, "use_collision_border_back", text="Back")
        col = flow.column()
        col.prop(domain, "use_collision_border_right", text="Right")
        col = flow.column()
        col.prop(domain, "use_collision_border_left", text="Left")
        col = flow.column()
        col.prop(domain, "use_collision_border_top", text="Top")
        col = flow.column()
        col.prop(domain, "use_collision_border_bottom", text="Bottom")

class PHYSICS_PT_manta_smoke(PhysicButtonsPanel, Panel):
    bl_label = "Smoke"
    bl_parent_id = 'PHYSICS_PT_manta_fluid'
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_gas_domain(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        md = context.manta
        domain = md.domain_settings

        baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
        baked_any = domain.cache_baked_data or domain.cache_baked_mesh or domain.cache_baked_particles or domain.cache_baked_noise or domain.cache_baked_guiding
        baked_data = domain.cache_baked_data

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)
        flow.enabled = not baking_any and not baked_data

        col = flow.column()
        col.prop(domain, "alpha")
        col.prop(domain, "beta", text="Temperature Diff.")
        col = flow.column()
        col.prop(domain, "vorticity")


class PHYSICS_PT_manta_smoke_dissolve(PhysicButtonsPanel, Panel):
    bl_label = "Dissolve"
    bl_parent_id = 'PHYSICS_PT_manta_smoke'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_gas_domain(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        md = context.manta
        domain = md.domain_settings

        self.layout.prop(domain, "use_dissolve_smoke", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        md = context.manta
        domain = md.domain_settings

        baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
        baked_any = domain.cache_baked_data or domain.cache_baked_mesh or domain.cache_baked_particles or domain.cache_baked_noise or domain.cache_baked_guiding
        baked_data = domain.cache_baked_data

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)
        flow.enabled = not baking_any and not baked_data

        layout.active = domain.use_dissolve_smoke

        col = flow.column()
        col.prop(domain, "dissolve_speed", text="Time")

        col = flow.column()
        col.prop(domain, "use_dissolve_smoke_log", text="Slow")


class PHYSICS_PT_manta_fire(PhysicButtonsPanel, Panel):
    bl_label = "Fire"
    bl_parent_id = 'PHYSICS_PT_manta_fluid'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_gas_domain(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        md = context.manta
        domain = md.domain_settings

        baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
        baked_any = domain.cache_baked_data or domain.cache_baked_mesh or domain.cache_baked_particles or domain.cache_baked_noise or domain.cache_baked_guiding
        baked_data = domain.cache_baked_data

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)
        flow.enabled = not baking_any and not baked_data

        col = flow.column()
        col.prop(domain, "burning_rate", text="Reaction Speed")
        col = flow.column()
        col.prop(domain, "flame_smoke", text="Flame Smoke")
        col = flow.column()
        col.prop(domain, "flame_vorticity", text="Flame Vorticity")
        col = flow.column()
        col.prop(domain, "flame_ignition", text="Temperature Ignition")
        col = flow.column()
        col.prop(domain, "flame_max_temp", text="Maximum Temperature")
        col = flow.column()
        col.prop(domain, "flame_smoke_color", text="Flame Color")

class PHYSICS_PT_manta_liquid(PhysicButtonsPanel, Panel):
    bl_label = "Liquid"
    bl_parent_id = 'PHYSICS_PT_manta_fluid'
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_OPENGL'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_liquid_domain(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        md = context.manta
        domain = md.domain_settings

        baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
        baked_any = domain.cache_baked_data or domain.cache_baked_mesh or domain.cache_baked_particles or domain.cache_baked_noise or domain.cache_baked_guiding
        baked_data = domain.cache_baked_data

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)

        col = flow.column()
        col1 = col.column(align=True)
        col1.enabled = not baking_any and not baked_data
        col1.prop(domain, "particle_maximum", text="Particles Maximum")
        col1.prop(domain, "particle_minimum", text="Minimum")

        col1 = flow.column()
        col1.enabled = not baking_any and not baked_data
        col1.prop(domain, "particle_number", text="Particle Sampling Number")
        col1.prop(domain, "particle_band_width", text="Narrow Band Width")
        col1.prop(domain, "particle_randomness", text="Particle Randomness")
        col2 = col.column()
        col2.enabled = not baking_any
        col2.prop(domain, "use_flip_particles", text="Show FLIP")

class PHYSICS_PT_manta_flow_source(PhysicButtonsPanel, Panel):
    bl_label = "Flow Source"
    bl_parent_id = 'PHYSICS_PT_manta_fluid'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_fluid_flow(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        ob = context.object
        flow = context.manta.flow_settings

        col = layout.column()
        col.prop(flow, "flow_source", expand=False, text="Flow Source")
        if flow.flow_source == 'PARTICLES':
            col.prop_search(flow, "particle_system", ob, "particle_systems", text="Particle System")

        grid = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)

        col = grid.column()
        if flow.flow_source == 'MESH':
            col.prop(flow, "surface_distance", text="Surface Thickness")
            if flow.flow_type in {'SMOKE', 'BOTH', 'FIRE'}:
                col = grid.column()
                col.prop(flow, "volume_density", text="Volume Density")

        if flow.flow_source == 'PARTICLES':
            col.prop(flow, "use_particle_size", text="Set Size")
            sub = col.column()
            sub.active = flow.use_particle_size
            sub.prop(flow, "particle_size")

class PHYSICS_PT_manta_flow_initial_velocity(PhysicButtonsPanel, Panel):
    bl_label = "Initial Velocity"
    bl_parent_id = 'PHYSICS_PT_manta_fluid'
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_OPENGL'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_fluid_flow(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        md = context.manta
        flow_smoke = md.flow_settings

        self.layout.prop(flow_smoke, "use_initial_velocity", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        flow = layout.grid_flow(row_major=False, columns=0, even_columns=True, even_rows=False, align=True)

        md = context.manta
        flow_smoke = md.flow_settings

        flow.active = flow_smoke.use_initial_velocity

        col = flow.column()
        col.prop(flow_smoke, "velocity_factor")

        if flow_smoke.flow_source == 'MESH':
            col.prop(flow_smoke, "velocity_normal")
            # col.prop(flow_smoke, "velocity_random")
            col = flow.column()
            col.prop(flow_smoke, "velocity_coord")


class PHYSICS_PT_manta_flow_texture(PhysicButtonsPanel, Panel):
    bl_label = "Texture"
    bl_parent_id = 'PHYSICS_PT_manta_fluid'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_fluid_flow(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        md = context.manta
        flow_smoke = md.flow_settings

        self.layout.prop(flow_smoke, "use_texture", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)

        ob = context.object
        flow_smoke = context.manta.flow_settings

        sub = flow.column()
        sub.active = flow_smoke.use_texture
        sub.prop(flow_smoke, "noise_texture")
        sub.prop(flow_smoke, "texture_map_type", text="Mapping")

        col = flow.column()
        sub = col.column()
        sub.active = flow_smoke.use_texture

        if flow_smoke.texture_map_type == 'UV':
            sub.prop_search(flow_smoke, "uv_layer", ob.data, "uv_layers")

        if flow_smoke.texture_map_type == 'AUTO':
            sub.prop(flow_smoke, "texture_size")

        sub.prop(flow_smoke, "texture_offset")

class PHYSICS_PT_manta_adaptive_domain(PhysicButtonsPanel, Panel):
    bl_label = "Adaptive Domain"
    bl_parent_id = 'PHYSICS_PT_manta'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_OPENGL'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_gas_domain(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        md = context.manta.domain_settings
        domain = context.manta.domain_settings
        baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
        baked_any = domain.cache_baked_data or domain.cache_baked_mesh or domain.cache_baked_particles or domain.cache_baked_noise or domain.cache_baked_guiding
        self.layout.enabled = not baking_any and not baked_any
        self.layout.prop(md, "use_adaptive_domain", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        domain = context.manta.domain_settings
        layout.active = domain.use_adaptive_domain

        baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
        baked_any = domain.cache_baked_data or domain.cache_baked_mesh or domain.cache_baked_particles or domain.cache_baked_noise or domain.cache_baked_guiding

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=True)
        flow.enabled = not baking_any and not baked_any

        col = flow.column()
        col.prop(domain, "additional_res", text="Add Resolution")
        col.prop(domain, "adapt_margin")

        col.separator()

        col = flow.column()
        col.prop(domain, "adapt_threshold", text="Threshold")

class PHYSICS_PT_manta_noise(PhysicButtonsPanel, Panel):
    bl_label = "Noise"
    bl_parent_id = 'PHYSICS_PT_manta'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_gas_domain(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        md = context.manta.domain_settings
        domain = context.manta.domain_settings
        baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
        self.layout.enabled = not baking_any
        self.layout.prop(md, "use_noise", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        domain = context.manta.domain_settings

        # Deactivate UI if guiding is enabled but not baked yet
        layout.active = domain.use_noise and not (domain.use_guiding and not domain.cache_baked_guiding and (domain.guiding_source == "EFFECTOR" or (domain.guiding_source == "DOMAIN" and not domain.guiding_parent)))

        baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
        baked_noise = domain.cache_baked_noise

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)
        flow.enabled = not baking_any and not baked_noise

        col = flow.column()
        col.prop(domain, "noise_scale", text="Upres Factor")
        # TODO (sebbas): Mantaflow only supports wavelet noise. Maybe get rid of noise type field.
        col.prop(domain, "noise_type", text="Noise Method")

        col = flow.column()
        col.prop(domain, "noise_strength", text="Strength")
        col.prop(domain, "noise_pos_scale", text="Scale")
        col.prop(domain, "noise_time_anim", text="Time")

        if domain.cache_type == "MODULAR":
            col.separator()

            split = layout.split()
            split.enabled = domain.cache_baked_data

            bake_incomplete = (domain.cache_frame_pause_noise < domain.cache_frame_end)
            if domain.cache_baked_noise and not domain.cache_baking_noise and bake_incomplete:
                col = split.column()
                col.operator("manta.bake_noise", text="Resume")
                col = split.column()
                col.operator("manta.free_noise", text="Free")
            elif not domain.cache_baked_noise and domain.cache_baking_noise:
                split.enabled = False
                split.operator("manta.pause_bake", text="Baking Noise - ESC to pause")
            elif not domain.cache_baked_noise and not domain.cache_baking_noise:
                split.operator("manta.bake_noise", text="Bake Noise")
            else:
                split.operator("manta.free_noise", text="Free Noise")

class PHYSICS_PT_manta_mesh(PhysicButtonsPanel, Panel):
    bl_label = "Mesh"
    bl_parent_id = 'PHYSICS_PT_manta'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_liquid_domain(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        md = context.manta.domain_settings
        domain = context.manta.domain_settings
        baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
        self.layout.enabled = not baking_any
        self.layout.prop(md, "use_mesh", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        domain = context.manta.domain_settings

        # Deactivate UI if guiding is enabled but not baked yet
        layout.active = domain.use_mesh and not (domain.use_guiding and not domain.cache_baked_guiding and (domain.guiding_source == "EFFECTOR" or (domain.guiding_source == "DOMAIN" and not domain.guiding_parent)))

        baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
        baked_mesh = domain.cache_baked_mesh

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)
        flow.enabled = not baking_any and not baked_mesh

        col = flow.column()

        col.prop(domain, "mesh_scale", text="Upres Factor")
        col.prop(domain, "particle_radius", text="Particle Radius")

        col = flow.column()
        col.prop(domain, "use_speed_vectors", text="Use Speed vectors")

        col.separator()
        col.prop(domain, "mesh_generator", text="Mesh Generator")

        if domain.mesh_generator in {'IMPROVED'}:
            col = flow.column(align=True)
            col.prop(domain, "mesh_smoothen_pos", text="Smoothing Positive")
            col.prop(domain, "mesh_smoothen_neg", text="Negative")

            col = flow.column(align=True)
            col.prop(domain, "mesh_concave_upper", text="Concavity Upper")
            col.prop(domain, "mesh_concave_lower", text="Lower")

        # TODO (sebbas): for now just interpolate any upres grids, ie not sampling highres grids
        #col.prop(domain, "highres_sampling", text="Flow Sampling:")

        if domain.cache_type == "MODULAR":
            col.separator()

            split = layout.split()
            split.enabled = domain.cache_baked_data

            bake_incomplete = (domain.cache_frame_pause_mesh < domain.cache_frame_end)
            if domain.cache_baked_mesh and not domain.cache_baking_mesh and bake_incomplete:
                col = split.column()
                col.operator("manta.bake_mesh", text="Resume")
                col = split.column()
                col.operator("manta.free_mesh", text="Free")
            elif not domain.cache_baked_mesh and domain.cache_baking_mesh:
                split.enabled = False
                split.operator("manta.pause_bake", text="Baking Mesh - ESC to pause")
            elif not domain.cache_baked_mesh and not domain.cache_baking_mesh:
                split.operator("manta.bake_mesh", text="Bake Mesh")
            else:
                split.operator("manta.free_mesh", text="Free Mesh")

class PHYSICS_PT_manta_particles(PhysicButtonsPanel, Panel):
    bl_label = "Particles"
    bl_parent_id = 'PHYSICS_PT_manta'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_OPENGL'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_liquid_domain(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        domain = context.manta.domain_settings

        # Deactivate UI if guiding is enabled but not baked yet
        layout.active = not (domain.use_guiding and not domain.cache_baked_guiding and (domain.guiding_source == "EFFECTOR" or (domain.guiding_source == "DOMAIN" and not domain.guiding_parent)))

        baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
        baked_particles = domain.cache_baked_particles
        using_particles = domain.use_spray_particles or domain.use_foam_particles or domain.use_bubble_particles

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)
        flow.enabled = not baking_any

        subSpray = flow.column()
        subSpray.enabled = (domain.sndparticle_combined_export == 'OFF') or (domain.sndparticle_combined_export == 'FOAM + BUBBLES')
        subSpray.prop(domain, "use_spray_particles", text="Spray")
        subFoam = flow.column()
        subFoam.enabled = (domain.sndparticle_combined_export == 'OFF') or (domain.sndparticle_combined_export == 'SPRAY + BUBBLES')
        subFoam.prop(domain, "use_foam_particles", text="Foam")
        subBubbles = flow.column()
        subBubbles.enabled = (domain.sndparticle_combined_export == 'OFF') or (domain.sndparticle_combined_export == 'SPRAY + FOAM')
        subBubbles.prop(domain, "use_bubble_particles", text="Bubbles")

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)
        flow.enabled = not baking_any and not baked_particles and using_particles

        col = flow.column()
        col.prop(domain, "sndparticle_combined_export")
        col.prop(domain, "particle_scale", text="Upres Factor")
        col.separator()

        col = flow.column(align=True)
        col.prop(domain, "sndparticle_tau_max_wc", text="Wave Crest Potential Maximum")
        col.prop(domain, "sndparticle_tau_min_wc", text="Minimum")
        col.separator()

        col = flow.column(align=True)
        col.prop(domain, "sndparticle_tau_max_ta", text="Trapped Air Potential Maximum")
        col.prop(domain, "sndparticle_tau_min_ta", text="Minimum")
        col.separator()

        col = flow.column(align=True)
        col.prop(domain, "sndparticle_tau_max_k", text="Kinetic Energy Potential Maximum")
        col.prop(domain, "sndparticle_tau_min_k", text="Minimum")
        col.separator()

        col = flow.column(align=True)
        col.prop(domain, "sndparticle_potential_radius", text="Potential Radius")
        col.prop(domain, "sndparticle_update_radius", text="Particle Update Radius")
        col.separator()

        col = flow.column(align=True)
        col.prop(domain, "sndparticle_k_wc", text="Wave Crest Particle Sampling")
        col.prop(domain, "sndparticle_k_ta", text="Trapped Air Particle Sampling")
        col.separator()

        col = flow.column(align=True)
        col.prop(domain, "sndparticle_l_max", text="Particle Life Maximum")
        col.prop(domain, "sndparticle_l_min", text="Minimum")
        col.separator()

        col = flow.column(align=True)
        col.prop(domain, "sndparticle_k_b", text="Bubble Buoyancy")
        col.prop(domain, "sndparticle_k_d", text="Bubble Drag")
        col.separator()

        col = flow.column()
        col.prop(domain, "sndparticle_boundary", text="Particles in Boundary:")

        if domain.cache_type == "MODULAR":
            col.separator()

            split = layout.split()
            split.enabled = domain.cache_baked_data and (domain.use_spray_particles or domain.use_bubble_particles or domain.use_foam_particles or domain.use_tracer_particles)

            bake_incomplete = (domain.cache_frame_pause_particles < domain.cache_frame_end)
            if domain.cache_baked_particles and not domain.cache_baking_particles and bake_incomplete:
                col = split.column()
                col.operator("manta.bake_particles", text="Resume")
                col = split.column()
                col.operator("manta.free_particles", text="Free")
            elif not domain.cache_baked_particles and domain.cache_baking_particles:
                split.enabled = False
                split.operator("manta.pause_bake", text="Baking Particles - ESC to pause")
            elif not domain.cache_baked_particles and not domain.cache_baking_particles:
                split.operator("manta.bake_particles", text="Bake Particles")
            else:
                split.operator("manta.free_particles", text="Free Particles")

class PHYSICS_PT_manta_diffusion(PhysicButtonsPanel, Panel):
    bl_label = "Diffusion"
    bl_parent_id = 'PHYSICS_PT_manta'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        # Fluid diffusion only enabled for liquids (surface tension and viscosity not relevant for smoke)
        if not PhysicButtonsPanel.poll_liquid_domain(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        domain = context.manta.domain_settings

        # Deactivate UI if guiding is enabled but not baked yet
        layout.active = not (domain.use_guiding and not domain.cache_baked_guiding and (domain.guiding_source == "EFFECTOR" or (domain.guiding_source == "DOMAIN" and not domain.guiding_parent)))

        split = layout.split()
        baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
        baked_any = domain.cache_baked_data or domain.cache_baked_mesh or domain.cache_baked_particles or domain.cache_baked_noise or domain.cache_baked_guiding
        baked_data = domain.cache_baked_data

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)
        flow.enabled = not baking_any and not baked_any and not baked_data

        row = flow.row()

        col = row.column()
        col.label(text="Viscosity Presets:")
        col.menu("MANTA_MT_presets", text=bpy.types.MANTA_MT_presets.bl_label)

        col = row.column(align=True)
        col.operator("manta.preset_add", text="", icon='ADD')
        col.operator("manta.preset_add", text="", icon='REMOVE').remove_active = True

        col = flow.column(align=True)
        col.prop(domain, "viscosity_base", text="Base")
        col.prop(domain, "viscosity_exponent", text="Exponent", slider=True)

        col = flow.column()
        col.prop(domain, "domain_size", text="Real World Size")
        col.prop(domain, "surface_tension", text="Surface tension")

class PHYSICS_PT_manta_guiding(PhysicButtonsPanel, Panel):
    bl_label = "Guiding"
    bl_parent_id = 'PHYSICS_PT_manta'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_OPENGL'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_fluid_domain(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw_header(self, context):
        md = context.manta.domain_settings
        domain = context.manta.domain_settings
        baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
        self.layout.enabled = not baking_any
        self.layout.prop(md, "use_guiding", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        domain = context.manta.domain_settings

        layout.active = domain.use_guiding

        baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
        baked_data = domain.cache_baked_data

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)
        flow.enabled = not baking_any and not baked_data

        col = flow.column()
        col.prop(domain, "guiding_alpha", text="Weight")
        col.prop(domain, "guiding_beta", text="Size")
        col.prop(domain, "guiding_vel_factor", text="Velocity Factor")

        col = flow.column()
        col.prop(domain, "guiding_source", text="Velocity Source")
        if domain.guiding_source == "DOMAIN":
            col.prop(domain, "guiding_parent", text="Guiding parent")

        if domain.cache_type == "MODULAR":
            col.separator()

            if domain.guiding_source == "EFFECTOR":
                split = layout.split()
                bake_incomplete = (domain.cache_frame_pause_guiding < domain.cache_frame_end)
                if domain.cache_baked_guiding and not domain.cache_baking_guiding and bake_incomplete:
                    col = split.column()
                    col.operator("manta.bake_guiding", text="Resume")
                    col = split.column()
                    col.operator("manta.free_guiding", text="Free")
                elif not domain.cache_baked_guiding and domain.cache_baking_guiding:
                    split.operator("manta.pause_bake", text="Pause Guiding")
                elif not domain.cache_baked_guiding and not domain.cache_baking_guiding:
                    split.operator("manta.bake_guiding", text="Bake Guiding")
                else:
                    split.operator("manta.free_guiding", text="Free Guiding")

class PHYSICS_PT_manta_collections(PhysicButtonsPanel, Panel):
    bl_label = "Collections"
    bl_parent_id = 'PHYSICS_PT_manta'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_fluid_domain(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        domain = context.manta.domain_settings

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)

        col = flow.column()
        col.prop(domain, "fluid_group", text="Flow")

        # col.prop(domain, "effector_group", text="Forces")
        col.prop(domain, "collision_group", text="Effector")

class PHYSICS_PT_manta_cache(PhysicButtonsPanel, Panel):
    bl_label = "Cache"
    bl_parent_id = 'PHYSICS_PT_manta'
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_fluid_domain(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        layout = self.layout

        md = context.manta
        domain = context.manta.domain_settings

        baking_any = domain.cache_baking_data or domain.cache_baking_mesh or domain.cache_baking_particles or domain.cache_baking_noise or domain.cache_baking_guiding
        baked_any = domain.cache_baked_data or domain.cache_baked_mesh or domain.cache_baked_particles or domain.cache_baked_noise or domain.cache_baked_guiding

        col = layout.column()
        col.prop(domain, "cache_directory", text="")
        col.enabled = not baking_any

        layout.use_property_split = True

        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=False)

        col = flow.column()
        col.prop(domain, "cache_type", expand=False)

        col = flow.column(align=True)
        col.separator()

        col.prop(domain, "cache_frame_start", text="Frame Start")
        col.prop(domain, "cache_frame_end", text="End")
        col.enabled = not baking_any

        col.separator()

        col = flow.column()
        col.enabled = not baking_any and not baked_any
        col.prop(domain, "cache_data_format", text="Data file format")

        if md.domain_settings.domain_type in {'GAS'}:
            if domain.use_noise:
                col.prop(domain, "cache_noise_format", text="Noise file format")

        if md.domain_settings.domain_type in {'LIQUID'}:
            # File format for all particle systemes (FLIP and secondary)
            col.prop(domain, "cache_particle_format", text="Particle file format")

            if domain.use_mesh:
                col.prop(domain, "cache_mesh_format", text="Mesh file format")

        col.prop(domain, "export_manta_script", text="Export Mantaflow Script")

class PHYSICS_PT_manta_field_weights(PhysicButtonsPanel, Panel):
    bl_label = "Field Weights"
    bl_parent_id = 'PHYSICS_PT_manta'
    bl_options = {'DEFAULT_CLOSED'}
    COMPAT_ENGINES = {'BLENDER_RENDER', 'BLENDER_EEVEE', 'BLENDER_WORKBENCH'}

    @classmethod
    def poll(cls, context):
        if not PhysicButtonsPanel.poll_fluid_domain(context):
            return False

        return (context.engine in cls.COMPAT_ENGINES)

    def draw(self, context):
        domain = context.manta.domain_settings
        effector_weights_ui(self, domain.effector_weights, 'SMOKE')

class PHYSICS_PT_manta_viewport_display(PhysicButtonsPanel, Panel):
    bl_label = "Viewport Display"
    bl_parent_id = 'PHYSICS_PT_manta'
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return (PhysicButtonsPanel.poll_gas_domain(context))

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=True)

        domain = context.manta.domain_settings

        col = flow.column()
        col.prop(domain, "display_thickness")

        col.separator()

        col.prop(domain, "slice_method", text="Slicing")

        slice_method = domain.slice_method
        axis_slice_method = domain.axis_slice_method

        do_axis_slicing = (slice_method == 'AXIS_ALIGNED')
        do_full_slicing = (axis_slice_method == 'FULL')

        col = col.column()
        col.enabled = do_axis_slicing
        col.prop(domain, "axis_slice_method")

        col = flow.column()
        sub = col.column()
        sub.enabled = not do_full_slicing and do_axis_slicing
        sub.prop(domain, "slice_axis")
        sub.prop(domain, "slice_depth")

        row = col.row()
        row.enabled = do_full_slicing or not do_axis_slicing
        row.prop(domain, "slice_per_voxel")

        col.prop(domain, "display_interpolation")


class PHYSICS_PT_manta_viewport_display_color(PhysicButtonsPanel, Panel):
    bl_label = "Color Mapping"
    bl_parent_id = 'PHYSICS_PT_manta_viewport_display'
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return (PhysicButtonsPanel.poll_gas_domain(context))

    def draw_header(self, context):
        md = context.manta.domain_settings

        self.layout.prop(md, "use_color_ramp", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True

        domain = context.manta.domain_settings
        col = layout.column()
        col.enabled = domain.use_color_ramp

        col.prop(domain, "coba_field")

        col.use_property_split = False

        col = col.column()
        col.template_color_ramp(domain, "color_ramp", expand=True)


class PHYSICS_PT_manta_viewport_display_debug(PhysicButtonsPanel, Panel):
    bl_label = "Debug Velocity"
    bl_parent_id = 'PHYSICS_PT_manta_viewport_display'
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        return (PhysicButtonsPanel.poll_gas_domain(context))

    def draw_header(self, context):
        md = context.manta.domain_settings

        self.layout.prop(md, "show_velocity", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        flow = layout.grid_flow(row_major=True, columns=0, even_columns=True, even_rows=False, align=True)

        domain = context.manta.domain_settings

        col = flow.column()
        col.enabled = domain.show_velocity
        col.prop(domain, "vector_display_type", text="Display As")
        col.prop(domain, "vector_scale")


classes = (
    MANTA_MT_presets,
    PHYSICS_PT_manta,
    PHYSICS_PT_manta_fluid,
    PHYSICS_PT_manta_borders,
    PHYSICS_PT_manta_smoke,
    PHYSICS_PT_manta_smoke_dissolve,
    PHYSICS_PT_manta_fire,
    PHYSICS_PT_manta_liquid,
    PHYSICS_PT_manta_flow_source,
    PHYSICS_PT_manta_flow_initial_velocity,
    PHYSICS_PT_manta_flow_texture,
    PHYSICS_PT_manta_adaptive_domain,
    PHYSICS_PT_manta_noise,
    PHYSICS_PT_manta_mesh,
    PHYSICS_PT_manta_particles,
    PHYSICS_PT_manta_diffusion,
    PHYSICS_PT_manta_guiding,
    PHYSICS_PT_manta_collections,
    PHYSICS_PT_manta_cache,
    PHYSICS_PT_manta_field_weights,
    PHYSICS_PT_manta_viewport_display,
    PHYSICS_PT_manta_viewport_display_color,
    PHYSICS_PT_manta_viewport_display_debug,
)


if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)

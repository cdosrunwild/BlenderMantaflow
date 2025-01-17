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

/** \file mantaflow/intern/strings/shared_script.h
 *  \ingroup mantaflow
 */

#include <string>

//////////////////////////////////////////////////////////////////////
// LIBRARIES
//////////////////////////////////////////////////////////////////////

const std::string manta_import =
    "\
from manta import *\n\
import os.path, shutil, math, sys, gc, multiprocessing, platform, time\n\
\n\
withMPBake = False # Bake files asynchronously\n\
withMPSave = True # Save files asynchronously\n\
isWindows = platform.system() != 'Darwin' and platform.system() != 'Linux'\n\
# TODO (sebbas): Use this to simulate Windows multiprocessing (has default mode spawn)\n\
#try:\n\
#    multiprocessing.set_start_method('spawn')\n\
#except:\n\
#    pass\n\
\n\
bpy = sys.modules.get('bpy')\n\
if bpy is not None:\n\
    sys.executable = bpy.app.binary_path_python\n";

//////////////////////////////////////////////////////////////////////
// DEBUG
//////////////////////////////////////////////////////////////////////

const std::string manta_debuglevel =
    "\n\
def set_manta_debuglevel(level):\n\
    setDebugLevel(level=level)\n # level 0 = mute all output from manta\n";

//////////////////////////////////////////////////////////////////////
// SOLVERS
//////////////////////////////////////////////////////////////////////

const std::string fluid_solver =
    "\n\
mantaMsg('Solver base')\n\
s$ID$ = Solver(name='solver_base$ID$', gridSize=gs_s$ID$, dim=dim_s$ID$)\n";

const std::string fluid_solver_noise =
    "\n\
mantaMsg('Solver noise')\n\
sn$ID$ = Solver(name='solver_noise$ID$', gridSize=gs_sn$ID$)\n";

const std::string fluid_solver_mesh =
    "\n\
mantaMsg('Solver mesh')\n\
sm$ID$ = Solver(name='solver_mesh$ID$', gridSize=gs_sm$ID$)\n";

const std::string fluid_solver_particles =
    "\n\
mantaMsg('Solver particles')\n\
sp$ID$ = Solver(name='solver_particles$ID$', gridSize=gs_sp$ID$)\n";

const std::string fluid_solver_guiding =
    "\n\
mantaMsg('Solver guiding')\n\
sg$ID$ = Solver(name='solver_guiding$ID$', gridSize=gs_sg$ID$)\n";

//////////////////////////////////////////////////////////////////////
// VARIABLES
//////////////////////////////////////////////////////////////////////

const std::string fluid_variables =
    "\n\
mantaMsg('Fluid variables')\n\
dim_s$ID$     = $SOLVER_DIM$\n\
res_s$ID$     = $RES$\n\
gravity_s$ID$ = vec3($GRAVITY_X$, $GRAVITY_Y$, $GRAVITY_Z$)\n\
gs_s$ID$      = vec3($RESX$, $RESY$, $RESZ$)\n\
maxVel_s$ID$  = 0\n\
\n\
doOpen_s$ID$          = $DO_OPEN$\n\
boundConditions_s$ID$ = '$BOUND_CONDITIONS$'\n\
boundaryWidth_s$ID$   = $BOUNDARY_WIDTH$\n\
\n\
using_smoke_s$ID$        = $USING_SMOKE$\n\
using_liquid_s$ID$       = $USING_LIQUID$\n\
using_noise_s$ID$        = $USING_NOISE$\n\
using_adaptTime_s$ID$    = $USING_ADAPTIVETIME$\n\
using_obstacle_s$ID$     = $USING_OBSTACLE$\n\
using_guiding_s$ID$      = $USING_GUIDING$\n\
using_invel_s$ID$        = $USING_INVEL$\n\
using_outflow_s$ID$      = $USING_OUTFLOW$\n\
using_sndparts_s$ID$     = $USING_SNDPARTS$\n\
using_speedvectors_s$ID$ = $USING_SPEEDVECTORS$\n\
\n\
# Fluid time params\n\
timeTotal_s$ID$    = $TIME_TOTAL$\n\
timePerFrame_s$ID$ = $TIME_PER_FRAME$\n\
frameLength_s$ID$  = $FRAME_LENGTH$\n\
dt0_s$ID$          = $DT$\n\
cflCond_s$ID$      = $CFL$\n\
\n\
# Fluid diffusion / viscosity\n\
domainSize_s$ID$ = $FLUID_DOMAIN_SIZE$ # longest domain side in meters\n\
viscosity_s$ID$ = $FLUID_VISCOSITY$ / (domainSize_s$ID$*domainSize_s$ID$) # kinematic viscosity in m^2/s\n\
\n\
# Factor to convert blender velocities to manta velocities\n\
toMantaUnitsFac_s$ID$ = (1.0 / (1.0 / res_s$ID$))\n # = dt/dx * 1/dt ";

const std::string fluid_variables_noise =
    "\n\
mantaMsg('Fluid variables noise')\n\
upres_sn$ID$  = $NOISE_SCALE$\n\
gs_sn$ID$     = vec3($NOISE_RESX$, $NOISE_RESY$, $NOISE_RESZ$)\n";

const std::string fluid_variables_mesh =
    "\n\
mantaMsg('Fluid variables mesh')\n\
upres_sm$ID$  = $MESH_SCALE$\n\
gs_sm$ID$     = vec3($MESH_RESX$, $MESH_RESY$, $MESH_RESZ$)\n";

const std::string fluid_variables_particles =
    "\n\
mantaMsg('Fluid variables particles')\n\
upres_sp$ID$  = $PARTICLE_SCALE$\n\
gs_sp$ID$     = vec3($PARTICLE_RESX$, $PARTICLE_RESY$, $PARTICLE_RESZ$)\n";

const std::string fluid_variables_guiding =
    "\n\
mantaMsg('Fluid variables guiding')\n\
gs_sg$ID$   = vec3($GUIDING_RESX$, $GUIDING_RESY$, $GUIDING_RESZ$)\n\
\n\
alpha_sg$ID$ = $GUIDING_ALPHA$\n\
beta_sg$ID$  = $GUIDING_BETA$\n\
gamma_sg$ID$ = $GUIDING_FACTOR$\n\
tau_sg$ID$   = 1.0\n\
sigma_sg$ID$ = 0.99/tau_sg$ID$\n\
theta_sg$ID$ = 1.0\n";

const std::string fluid_with_obstacle =
    "\n\
using_obstacle_s$ID$ = True\n";

const std::string fluid_with_guiding =
    "\n\
using_guiding_s$ID$ = True\n";

const std::string fluid_with_invel =
    "\n\
using_invel_s$ID$ = True\n";

const std::string fluid_with_outflow =
    "\n\
using_outflow_s$ID$ = True\n";

const std::string fluid_with_sndparts =
    "\n\
using_sndparts_s$ID$ = True\n";

//////////////////////////////////////////////////////////////////////
// ADAPTIVE TIME STEPPING
//////////////////////////////////////////////////////////////////////

const std::string fluid_time_stepping =
    "\n\
mantaMsg('Fluid adaptive time stepping')\n\
s$ID$.frameLength  = frameLength_s$ID$\n\
s$ID$.timestepMin  = s$ID$.frameLength / 5.\n\
s$ID$.timestepMax  = s$ID$.frameLength\n\
s$ID$.cfl          = cflCond_s$ID$\n\
s$ID$.timePerFrame = timePerFrame_s$ID$\n\
s$ID$.timestep     = dt0_s$ID$\n\
s$ID$.timeTotal    = timeTotal_s$ID$\n\
#mantaMsg('timestep: ' + str(s$ID$.timestep) + ' // timPerFrame: ' + str(s$ID$.timePerFrame) + ' // frameLength: ' + str(s$ID$.frameLength) + ' // timeTotal: ' + str(s$ID$.timeTotal) )\n";

const std::string fluid_time_stepping_noise =
    "\n\
mantaMsg('Fluid adaptive time stepping noise')\n\
sn$ID$.frameLength = frameLength_s$ID$\n\
sn$ID$.timestepMin = sn$ID$.frameLength / 10.\n\
sn$ID$.timestepMax = sn$ID$.frameLength\n\
sn$ID$.cfl         = cflCond_s$ID$\n\
sn$ID$.timestep    = dt0_s$ID$\n\
sn$ID$.timeTotal   = timeTotal_s$ID$\n\
#mantaMsg('noise timestep: ' + str(sn$ID$.timestep) + ' // timPerFrame: ' + str(sn$ID$.timePerFrame) + ' // frameLength: ' + str(sn$ID$.frameLength) + ' // timeTotal: ' + str(sn$ID$.timeTotal) )\n";

const std::string fluid_adapt_time_step =
    "\n\
def fluid_adapt_time_step_$ID$():\n\
    mantaMsg('Fluid adapt time step')\n\
    \n\
    # time params are animatable\n\
    s$ID$.frameLength = frameLength_s$ID$\n\
    s$ID$.cfl         = cflCond_s$ID$\n\
    \n\
    # ensure that vel grid is full (remember: adaptive domain can reallocate solver)\n\
    copyRealToVec3(sourceX=x_vel_s$ID$, sourceY=y_vel_s$ID$, sourceZ=z_vel_s$ID$, target=vel_s$ID$)\n\
    maxVel_s$ID$ = vel_s$ID$.getMax() if vel_s$ID$ else 0\n\
    if using_adaptTime_s$ID$:\n\
        mantaMsg('Adapt timestep, maxvel: ' + str(maxVel_s$ID$))\n\
        s$ID$.adaptTimestep(maxVel_s$ID$)\n";

//////////////////////////////////////////////////////////////////////
// GRIDS
//////////////////////////////////////////////////////////////////////

const std::string fluid_alloc =
    "\n\
mantaMsg('Fluid alloc data')\n\
flags_s$ID$       = s$ID$.create(FlagGrid)\n\
vel_s$ID$         = s$ID$.create(MACGrid)\n\
velC_s$ID$        = s$ID$.create(MACGrid)\n\
x_vel_s$ID$       = s$ID$.create(RealGrid)\n\
y_vel_s$ID$       = s$ID$.create(RealGrid)\n\
z_vel_s$ID$       = s$ID$.create(RealGrid)\n\
pressure_s$ID$    = s$ID$.create(RealGrid)\n\
phiObs_s$ID$      = s$ID$.create(LevelsetGrid)\n\
phiIn_s$ID$       = s$ID$.create(LevelsetGrid)\n\
phiOut_s$ID$      = s$ID$.create(LevelsetGrid)\n\
forces_s$ID$      = s$ID$.create(Vec3Grid)\n\
x_force_s$ID$     = s$ID$.create(RealGrid)\n\
y_force_s$ID$     = s$ID$.create(RealGrid)\n\
z_force_s$ID$     = s$ID$.create(RealGrid)\n\
obvel_s$ID$       = 0\n\
\n\
# Keep track of important objects in dict to load them later on\n\
fluid_data_dict_s$ID$ = dict(vel=vel_s$ID$, phiObs=phiObs_s$ID$, phiIn=phiIn_s$ID$, phiOut=phiOut_s$ID$, flags=flags_s$ID$)\n";

const std::string fluid_alloc_obstacle =
    "\n\
mantaMsg('Allocating obstacle data')\n\
numObs_s$ID$     = s$ID$.create(IntGrid)\n\
phiObsIn_s$ID$   = s$ID$.create(LevelsetGrid)\n\
obvel_s$ID$      = s$ID$.create(MACGrid)\n\
obvelC_s$ID$     = s$ID$.create(Vec3Grid)\n\
x_obvel_s$ID$    = s$ID$.create(RealGrid)\n\
y_obvel_s$ID$    = s$ID$.create(RealGrid)\n\
z_obvel_s$ID$    = s$ID$.create(RealGrid)\n\
\n\
tmpDict_s$ID$ = dict(phiObsIn=phiObsIn_s$ID$)\n\
fluid_data_dict_s$ID$.update(tmpDict_s$ID$)\n";

const std::string fluid_alloc_guiding =
    "\n\
mantaMsg('Allocating guiding data')\n\
velT_s$ID$        = s$ID$.create(MACGrid)\n\
weightGuide_s$ID$ = s$ID$.create(RealGrid)\n\
numGuides_s$ID$   = s$ID$.create(IntGrid)\n\
phiGuideIn_s$ID$  = s$ID$.create(LevelsetGrid)\n\
guidevelC_s$ID$   = s$ID$.create(Vec3Grid)\n\
x_guidevel_s$ID$  = s$ID$.create(RealGrid)\n\
y_guidevel_s$ID$  = s$ID$.create(RealGrid)\n\
z_guidevel_s$ID$  = s$ID$.create(RealGrid)\n\
\n\
# Final guide vel grid needs to have independent size\n\
guidevel_sg$ID$   = sg$ID$.create(MACGrid)\n\
\n\
# Keep track of important objects in dict to load them later on\n\
fluid_guiding_dict_s$ID$ = dict(guidevel=guidevel_sg$ID$)\n";

const std::string fluid_alloc_invel =
    "\n\
mantaMsg('Allocating initial velocity data')\n\
invelC_s$ID$  = s$ID$.create(VecGrid)\n\
invel_s$ID$   = s$ID$.create(MACGrid)\n\
x_invel_s$ID$ = s$ID$.create(RealGrid)\n\
y_invel_s$ID$ = s$ID$.create(RealGrid)\n\
z_invel_s$ID$ = s$ID$.create(RealGrid)\n";

const std::string fluid_alloc_outflow =
    "\n\
mantaMsg('Allocating outflow data')\n\
phiOutIn_s$ID$ = s$ID$.create(LevelsetGrid)\n";

const std::string fluid_alloc_sndparts =
    "\n\
mantaMsg('Allocating snd parts low')\n\
ppSnd_sp$ID$     = sp$ID$.create(BasicParticleSystem)\n\
pVelSnd_pp$ID$   = ppSnd_sp$ID$.create(PdataVec3)\n\
pForceSnd_pp$ID$ = ppSnd_sp$ID$.create(PdataVec3)\n\
pLifeSnd_pp$ID$  = ppSnd_sp$ID$.create(PdataReal)\n\
vel_sp$ID$       = sp$ID$.create(MACGrid)\n\
flags_sp$ID$     = sp$ID$.create(FlagGrid)\n\
phi_sp$ID$       = sp$ID$.create(LevelsetGrid)\n\
phiIn_sp$ID$     = sp$ID$.create(LevelsetGrid)\n\
phiObs_sp$ID$    = sp$ID$.create(LevelsetGrid)\n\
phiObsIn_sp$ID$  = sp$ID$.create(LevelsetGrid)\n\
\n\
# Keep track of important objects in dict to load them later on\n\
fluid_particles_dict_s$ID$ = dict(ppSnd=ppSnd_sp$ID$, pVelSnd=pVelSnd_pp$ID$, pLifeSnd=pLifeSnd_pp$ID$)\n";

//////////////////////////////////////////////////////////////////////
// PRE / POST STEP
//////////////////////////////////////////////////////////////////////

const std::string fluid_pre_step =
    "\n\
def fluid_pre_step_$ID$():\n\
    mantaMsg('Fluid pre step')\n\
    \n\
    phiObs_s$ID$.setConst(9999)\n\
    phiOut_s$ID$.setConst(9999)\n\
    \n\
    # Main vel grid is copied in adapt time step function\n\
    \n\
    # translate obvels (world space) to grid space\n\
    if using_obstacle_s$ID$:\n\
        x_obvel_s$ID$.multConst(toMantaUnitsFac_s$ID$)\n\
        y_obvel_s$ID$.multConst(toMantaUnitsFac_s$ID$)\n\
        z_obvel_s$ID$.multConst(toMantaUnitsFac_s$ID$)\n\
        copyRealToVec3(sourceX=x_obvel_s$ID$, sourceY=y_obvel_s$ID$, sourceZ=z_obvel_s$ID$, target=obvelC_s$ID$)\n\
    \n\
    # translate invels (world space) to grid space\n\
    if using_invel_s$ID$:\n\
        x_invel_s$ID$.multConst(toMantaUnitsFac_s$ID$)\n\
        y_invel_s$ID$.multConst(toMantaUnitsFac_s$ID$)\n\
        z_invel_s$ID$.multConst(toMantaUnitsFac_s$ID$)\n\
        copyRealToVec3(sourceX=x_invel_s$ID$, sourceY=y_invel_s$ID$, sourceZ=z_invel_s$ID$, target=invelC_s$ID$)\n\
    \n\
    if using_guiding_s$ID$:\n\
        weightGuide_s$ID$.multConst(0)\n\
        weightGuide_s$ID$.addConst(alpha_sg$ID$)\n\
        interpolateMACGrid(source=guidevel_sg$ID$, target=velT_s$ID$)\n\
        velT_s$ID$.multConst(vec3(gamma_sg$ID$))\n\
    \n\
    # translate external forces (world space) to grid space\n\
    x_force_s$ID$.multConst(toMantaUnitsFac_s$ID$)\n\
    y_force_s$ID$.multConst(toMantaUnitsFac_s$ID$)\n\
    z_force_s$ID$.multConst(toMantaUnitsFac_s$ID$)\n\
    copyRealToVec3(sourceX=x_force_s$ID$, sourceY=y_force_s$ID$, sourceZ=z_force_s$ID$, target=forces_s$ID$)\n\
    \n\
    # If obstacle has velocity, i.e. is a moving obstacle, switch to dynamic preconditioner\n\
    if using_smoke_s$ID$ and using_obstacle_s$ID$ and obvelC_s$ID$.getMax() > 0:\n\
        mantaMsg('Using dynamic preconditioner')\n\
        preconditioner_s$ID$ = PcMGDynamic\n\
    else:\n\
        mantaMsg('Using static preconditioner')\n\
        preconditioner_s$ID$ = PcMGStatic\n";

const std::string fluid_post_step =
    "\n\
def fluid_post_step_$ID$():\n\
    mantaMsg('Fluid post step')\n\
    forces_s$ID$.clear()\n\
    x_force_s$ID$.clear()\n\
    y_force_s$ID$.clear()\n\
    z_force_s$ID$.clear()\n\
    \n\
    if using_guiding_s$ID$:\n\
        weightGuide_s$ID$.clear()\n\
    if using_invel_s$ID$:\n\
        x_invel_s$ID$.clear()\n\
        y_invel_s$ID$.clear()\n\
        z_invel_s$ID$.clear()\n\
        invel_s$ID$.clear()\n\
        invelC_s$ID$.clear()\n\
    \n\
    # Copy vel grid to reals grids (which Blender internal will in turn use for vel access)\n\
    copyVec3ToReal(source=vel_s$ID$, targetX=x_vel_s$ID$, targetY=y_vel_s$ID$, targetZ=z_vel_s$ID$)\n";

//////////////////////////////////////////////////////////////////////
// DESTRUCTION
//////////////////////////////////////////////////////////////////////

const std::string fluid_delete_all =
    "\n\
mantaMsg('Deleting fluid')\n\
# Clear all helper dictionaries first\n\
mantaMsg('Clear helper dictionaries')\n\
if 'liquid_data_dict_s$ID$' in globals(): liquid_data_dict_s$ID$.clear()\n\
if 'liquid_flip_dict_s$ID$' in globals(): liquid_flip_dict_s$ID$.clear()\n\
if 'liquid_mesh_dict_s$ID$' in globals(): liquid_mesh_dict_s$ID$.clear()\n\
if 'liquid_meshvel_dict_s$ID$' in globals(): liquid_meshvel_dict_s$ID$.clear()\n\
if 'smoke_data_dict_s$ID$' in globals(): smoke_data_dict_s$ID$.clear()\n\
if 'smoke_noise_dict_s$ID$' in globals(): smoke_noise_dict_s$ID$.clear()\n\
if 'fluid_particles_dict_s$ID$' in globals(): fluid_particles_dict_s$ID$.clear()\n\
if 'fluid_guiding_dict_s$ID$' in globals(): fluid_guiding_dict_s$ID$.clear()\n\
if 'fluid_data_dict_s$ID$' in globals(): fluid_data_dict_s$ID$.clear()\n\
\n\
# Delete all childs from objects (e.g. pdata for particles)\n\
mantaMsg('Release solver childs childs')\n\
for var in list(globals()):\n\
    if var.endswith('_pp$ID$') or var.endswith('_mesh$ID$'):\n\
        del globals()[var]\n\
\n\
# Now delete childs from solver objects\n\
mantaMsg('Release solver childs')\n\
for var in list(globals()):\n\
    if var.endswith('_s$ID$') or var.endswith('_sn$ID$') or var.endswith('_sm$ID$') or var.endswith('_sp$ID$') or var.endswith('_sg$ID$'):\n\
        del globals()[var]\n\
\n\
# Extra cleanup for multigrid and fluid guiding\n\
mantaMsg('Release multigrid')\n\
if 's$ID$' in globals(): releaseMG(s$ID$)\n\
if 'sn$ID$' in globals(): releaseMG(sn$ID$)\n\
mantaMsg('Release fluid guiding')\n\
releaseBlurPrecomp()\n\
\n\
# Release unreferenced memory (if there is some left, can in fact happen)\n\
gc.collect()\n\
\n\
# Now it is safe to delete solver objects (always need to be deleted last)\n\
mantaMsg('Delete base solver')\n\
if 's$ID$' in globals(): del s$ID$\n\
mantaMsg('Delete noise solver')\n\
if 'sn$ID$' in globals(): del sn$ID$\n\
mantaMsg('Delete mesh solver')\n\
if 'sm$ID$' in globals(): del sm$ID$\n\
mantaMsg('Delete particle solver')\n\
if 'sp$ID$' in globals(): del sp$ID$\n\
mantaMsg('Delete guiding solver')\n\
if 'sg$ID$' in globals(): del sg$ID$\n\
\n\
# Release unreferenced memory (if there is some left)\n\
gc.collect()\n";

//////////////////////////////////////////////////////////////////////
// BAKE
//////////////////////////////////////////////////////////////////////

const std::string fluid_cache_helper =
    "\n\
def fluid_cache_get_framenr_formatted_$ID$(framenr):\n\
    return str(framenr).zfill(4) # framenr with leading zeroes\n";

const std::string fluid_bake_multiprocessing =
    "\n\
def fluid_cache_multiprocessing_start_$ID$(function, framenr, format_data=None, format_noise=None, format_mesh=None, format_particles=None, format_guiding=None, path_data=None, path_noise=None, path_mesh=None, path_particles=None, path_guiding=None, dict=None, do_join=True):\n\
    mantaMsg('Multiprocessing cache')\n\
    if __name__ == '__main__':\n\
        args = (framenr,)\n\
        if format_data:\n\
            args += (format_data,)\n\
        if format_noise:\n\
            args += (format_noise,)\n\
        if format_mesh:\n\
            args += (format_mesh,)\n\
        if format_particles:\n\
            args += (format_particles,)\n\
        if format_guiding:\n\
            args += (format_guiding,)\n\
        if path_data:\n\
            args += (path_data,)\n\
        if path_noise:\n\
            args += (path_noise,)\n\
        if path_mesh:\n\
            args += (path_mesh,)\n\
        if path_particles:\n\
            args += (path_particles,)\n\
        if path_guiding:\n\
            args += (path_guiding,)\n\
        if dict:\n\
            args += (dict,)\n\
        p$ID$ = multiprocessing.Process(target=function, args=args)\n\
        p$ID$.start()\n\
        if do_join:\n\
            p$ID$.join()\n";

const std::string fluid_bake_data =
    "\n\
def bake_fluid_process_data_$ID$(framenr, format_data, format_particles, format_guiding, path_data, path_guiding):\n\
    mantaMsg('Bake fluid data')\n\
    \n\
    s$ID$.frame = framenr\n\
    # Must not set 'timeTotal' here. Remember, this function is called from manta.c while-loop\n\
    \n\
    start_time = time.time()\n\
    if using_smoke_s$ID$:\n\
        smoke_adaptive_step_$ID$(framenr)\n\
    if using_liquid_s$ID$:\n\
        liquid_adaptive_step_$ID$(framenr)\n\
    mantaMsg('--- Step: %s seconds ---' % (time.time() - start_time))\n\
\n\
def bake_fluid_data_$ID$(path_data, path_guiding, framenr, format_data, format_particles, format_guiding):\n\
    if not withMPBake or isWindows:\n\
        bake_fluid_process_data_$ID$(framenr, format_data, format_particles, format_guiding, path_data, path_guiding)\n\
    else:\n\
        fluid_cache_multiprocessing_start_$ID$(function=bake_fluid_process_data_$ID$, framenr=framenr, format_data=format_data, format_particles=format_particles, format_guiding=format_guiding, path_data=path_data, path_guiding=path_guiding, do_join=False)\n";

const std::string fluid_bake_noise =
    "\n\
def bake_noise_process_$ID$(framenr, format_data, format_noise, path_data, path_noise):\n\
    mantaMsg('Bake fluid noise')\n\
    \n\
    sn$ID$.frame = framenr\n\
    sn$ID$.timeTotal = (framenr-1) * frameLength_s$ID$\n\
    mantaMsg('sn$ID$.timeTotal: ' + str(sn$ID$.timeTotal))\n\
    \n\
    smoke_step_noise_$ID$(framenr)\n\
    smoke_save_noise_$ID$(path_noise, framenr, format_noise)\n\
\n\
def bake_noise_$ID$(path_data, path_noise, framenr, format_data, format_noise):\n\
    if not withMPBake or isWindows:\n\
        bake_noise_process_$ID$(framenr, format_data, format_noise, path_data, path_noise)\n\
    else:\n\
        fluid_cache_multiprocessing_start_$ID$(function=bake_noise_process_$ID$, framenr=framenr, format_data=format_data, format_noise=format_noise, path_data=path_data, path_noise=path_noise)\n";

const std::string fluid_bake_mesh =
    "\n\
def bake_mesh_process_$ID$(framenr, format_data, format_mesh, format_particles, path_data, path_mesh):\n\
    mantaMsg('Bake fluid mesh')\n\
    \n\
    sm$ID$.frame = framenr\n\
    sm$ID$.timeTotal = (framenr-1) * frameLength_s$ID$\n\
    \n\
    #if using_smoke_s$ID$:\n\
        # TODO (sebbas): Future update could include smoke mesh (vortex sheets)\n\
    if using_liquid_s$ID$:\n\
        liquid_step_mesh_$ID$()\n\
        liquid_save_mesh_$ID$(path_mesh, framenr, format_mesh)\n\
        if using_speedvectors_s$ID$:\n\
            liquid_save_meshvel_$ID$(path_mesh, framenr, format_data)\n\
\n\
def bake_mesh_$ID$(path_data, path_mesh, framenr, format_data, format_mesh, format_particles):\n\
    if not withMPBake or isWindows:\n\
        bake_mesh_process_$ID$(framenr, format_data, format_mesh, format_particles, path_data, path_mesh)\n\
    else:\n\
        fluid_cache_multiprocessing_start_$ID$(function=bake_mesh_process_$ID$, framenr=framenr, format_data=format_data, format_mesh=format_mesh, format_particles=format_particles, path_data=path_data, path_mesh=path_mesh)\n";

const std::string fluid_bake_particles =
    "\n\
def bake_particles_process_$ID$(framenr, format_data, format_particles, path_data, path_particles):\n\
    mantaMsg('Bake secondary particles')\n\
    \n\
    sp$ID$.frame = framenr\n\
    sp$ID$.timeTotal = (framenr-1) * frameLength_s$ID$\n\
    \n\
    fluid_load_data_$ID$(path_data, framenr, format_data)\n\
    #if using_smoke_s$ID$:\n\
        # TODO (sebbas): Future update could include smoke particles (e.g. fire sparks)\n\
    if using_liquid_s$ID$:\n\
        liquid_load_data_$ID$(path_data, framenr, format_data)\n\
        liquid_step_particles_$ID$()\n\
        fluid_save_particles_$ID$(path_particles, framenr, format_particles)\n\
        liquid_save_particles_$ID$(path_particles, framenr, format_particles)\n\
\n\
def bake_particles_$ID$(path_data, path_particles, framenr, format_data, format_particles):\n\
    if not withMPBake or isWindows:\n\
        bake_particles_process_$ID$(framenr, format_data, format_particles, path_data, path_particles)\n\
    else:\n\
        fluid_cache_multiprocessing_start_$ID$(function=bake_particles_process_$ID$, framenr=framenr, format_data=format_data, format_particles=format_particles, path_data=path_data, path_particles=path_particles)\n";

const std::string fluid_bake_guiding =
    "\n\
def bake_guiding_process_$ID$(framenr, format_guiding, path_guiding):\n\
    mantaMsg('Bake fluid guiding')\n\
    \n\
    if framenr>1:\n\
        fluid_load_guiding_$ID$(path_guiding, framenr-1, format_guiding)\n\
    \n\
    x_guidevel_s$ID$.multConst(Real(toMantaUnitsFac_s$ID$))\n\
    y_guidevel_s$ID$.multConst(Real(toMantaUnitsFac_s$ID$))\n\
    z_guidevel_s$ID$.multConst(Real(toMantaUnitsFac_s$ID$))\n\
    copyRealToVec3(sourceX=x_guidevel_s$ID$, sourceY=y_guidevel_s$ID$, sourceZ=z_guidevel_s$ID$, target=guidevelC_s$ID$)\n\
    \n\
    mantaMsg('Extrapolating guiding velocity')\n\
    # ensure velocities inside of guiding object, slightly add guiding vels outside of object too\n\
    extrapolateVec3Simple(vel=guidevelC_s$ID$, phi=phiGuideIn_s$ID$, distance=int(res_s$ID$/2), inside=True)\n\
    extrapolateVec3Simple(vel=guidevelC_s$ID$, phi=phiGuideIn_s$ID$, distance=4, inside=False)\n\
    resampleVec3ToMac(source=guidevelC_s$ID$, target=guidevel_sg$ID$)\n\
    \n\
    fluid_save_guiding_$ID$(path_guiding, framenr, format_guiding)\n\
\n\
def bake_guiding_$ID$(path_guiding, framenr, format_guiding):\n\
    if not withMPBake or isWindows:\n\
        bake_guiding_process_$ID$(framenr, format_guiding, path_guiding)\n\
    else:\n\
        fluid_cache_multiprocessing_start_$ID$(function=bake_guiding_process_$ID$, framenr=framenr, format_guiding=format_guiding, path_guiding=path_guiding)\n";

//////////////////////////////////////////////////////////////////////
// IMPORT
//////////////////////////////////////////////////////////////////////

const std::string fluid_file_import =
    "\n\
def fluid_file_import_s$ID$(dict, path, framenr, file_format):\n\
    try:\n\
        framenr = fluid_cache_get_framenr_formatted_$ID$(framenr)\n\
        for name, object in dict.items():\n\
            file = os.path.join(path, name + '_' + framenr + file_format)\n\
            if os.path.isfile(file):\n\
                object.load(file)\n\
            else:\n\
                mantaMsg('Could not load file ' + str(file))\n\
    except:\n\
        mantaMsg('exception found')\n\
        #mantaMsg(str(e))\n\
        pass # Just skip file load errors for now\n";

const std::string fluid_load_particles =
    "\n\
def fluid_load_particles_$ID$(path, framenr, file_format):\n\
    mantaMsg('Fluid load particles, frame ' + str(framenr))\n\
    fluid_file_import_s$ID$(dict=fluid_particles_dict_s$ID$, path=path, framenr=framenr, file_format=file_format)\n";

const std::string fluid_load_data =
    "\n\
def fluid_load_data_$ID$(path, framenr, file_format):\n\
    mantaMsg('Fluid load data, frame ' + str(framenr))\n\
    fluid_file_import_s$ID$(dict=fluid_data_dict_s$ID$, path=path, framenr=framenr, file_format=file_format)\n\
    \n\
    # When adaptive domain bake is resumed we need correct values in xyz vel grids\n\
    copyVec3ToReal(source=vel_s$ID$, targetX=x_vel_s$ID$, targetY=y_vel_s$ID$, targetZ=z_vel_s$ID$)\n";

const std::string fluid_load_guiding =
    "\n\
def fluid_load_guiding_$ID$(path, framenr, file_format):\n\
    mantaMsg('Fluid load guiding, frame ' + str(framenr))\n\
    fluid_file_import_s$ID$(dict=fluid_guiding_dict_s$ID$, path=path, framenr=framenr, file_format=file_format)\n";

const std::string fluid_load_vel =
    "\n\
def fluid_load_vel_$ID$(path, framenr, file_format):\n\
    mantaMsg('Fluid load vel, frame ' + str(framenr))\n\
    vel_dict = dict(vel=guidevel_sg$ID$)\n\
    fluid_file_import_s$ID$(dict=vel_dict, path=path, framenr=framenr, file_format=file_format)\n";

//////////////////////////////////////////////////////////////////////
// EXPORT
//////////////////////////////////////////////////////////////////////

const std::string fluid_file_export =
    "\n\
def fluid_file_export_s$ID$(framenr, file_format, path, dict, mode_override=True, skip_subframes=True):\n\
    if skip_subframes and ((timePerFrame_s$ID$ + dt0_s$ID$) < frameLength_s$ID$):\n\
        return\n\
    mantaMsg('Fluid file export, frame: ' + str(framenr))\n\
    try:\n\
        framenr = fluid_cache_get_framenr_formatted_$ID$(framenr)\n\
        if not os.path.exists(path):\n\
            os.makedirs(path)\n\
        for name, object in dict.items():\n\
            file = os.path.join(path, name + '_' + framenr + file_format)\n\
            if not os.path.isfile(file) or mode_override: object.save(file)\n\
    except Exception as e:\n\
        mantaMsg(str(e))\n\
        pass # Just skip file save errors for now\n";

const std::string fluid_save_particles =
    "\n\
def fluid_save_particles_$ID$(path, framenr, file_format):\n\
    mantaMsg('Liquid save particles, frame ' + str(framenr))\n\
    if not withMPSave or isWindows:\n\
        fluid_file_export_s$ID$(dict=fluid_particles_dict_s$ID$, framenr=framenr, file_format=file_format, path=path)\n\
    else:\n\
        fluid_cache_multiprocessing_start_$ID$(function=fluid_file_export_s$ID$, framenr=framenr, format_data=file_format, path_data=path, dict=fluid_particles_dict_s$ID$, do_join=False)\n";

const std::string fluid_save_data =
    "\n\
def fluid_save_data_$ID$(path, framenr, file_format):\n\
    mantaMsg('Fluid save data, frame ' + str(framenr))\n\
    start_time = time.time()\n\
    if not withMPSave or isWindows:\n\
        fluid_file_export_s$ID$(framenr=framenr, file_format=file_format, path=path, dict=fluid_data_dict_s$ID$)\n\
    else:\n\
        fluid_cache_multiprocessing_start_$ID$(function=fluid_file_export_s$ID$, framenr=framenr, format_data=file_format, path_data=path, dict=fluid_data_dict_s$ID$, do_join=False)\n\
    mantaMsg('--- Save: %s seconds ---' % (time.time() - start_time))\n";

const std::string fluid_save_guiding =
    "\n\
def fluid_save_guiding_$ID$(path, framenr, file_format):\n\
    mantaMsg('Fluid save guiding, frame ' + str(framenr))\n\
    if not withMPSave or isWindows:\n\
        fluid_file_export_s$ID$(dict=fluid_guiding_dict_s$ID$, framenr=framenr, file_format=file_format, path=path)\n\
    else:\n\
        fluid_cache_multiprocessing_start_$ID$(function=fluid_file_export_s$ID$, framenr=framenr, format_data=file_format, path_data=path, dict=fluid_guiding_dict_s$ID$, do_join=False)\n";

//////////////////////////////////////////////////////////////////////
// STANDALONE MODE
//////////////////////////////////////////////////////////////////////

const std::string fluid_standalone =
    "\n\
gui = None\n\
if (GUI):\n\
    gui=Gui()\n\
    gui.show()\n\
    gui.pause()\n\
\n\
cache_dir = '$CACHE_DIR$'\n\
file_format_data      = '.uni'\n\
file_format_noise     = '.uni'\n\
file_format_particles = '.uni'\n\
file_format_mesh      = '.bobj.gz'\n\
\n\
# Start and stop for simulation\n\
current_frame  = $CURRENT_FRAME$\n\
end_frame      = $END_FRAME$\n\
\n\
# How many frame to load from cache\n\
from_cache_cnt = 100\n\
\n\
loop_cnt = 0\n\
while current_frame <= end_frame:\n\
    \n\
    # Load already simulated data from cache:\n\
    if loop_cnt < from_cache_cnt:\n\
        load(current_frame)\n\
    \n\
    # Otherwise simulate new data\n\
    else:\n\
        while(s$ID$.frame <= current_frame):\n\
            if using_adaptTime_s$ID$:\n\
                fluid_adapt_time_step_$ID$()\n\
            step(current_frame)\n\
    \n\
    current_frame += 1\n\
    loop_cnt += 1\n\
    \n\
    if gui:\n\
        gui.pause()\n";

//////////////////////////////////////////////////////////////////////
// SCRIPT SECTION HEADERS
//////////////////////////////////////////////////////////////////////

const std::string header_libraries =
    "\n\
######################################################################\n\
## LIBRARIES\n\
######################################################################\n";

const std::string header_main =
    "\n\
######################################################################\n\
## MAIN\n\
######################################################################\n";

const std::string header_prepost =
    "\n\
######################################################################\n\
## PRE/POST STEPS\n\
######################################################################\n";

const std::string header_steps =
    "\n\
######################################################################\n\
## STEPS\n\
######################################################################\n";

const std::string header_import =
    "\n\
######################################################################\n\
## IMPORT\n\
######################################################################\n";

const std::string header_grids =
    "\n\
######################################################################\n\
## GRIDS\n\
######################################################################\n";

const std::string header_solvers =
    "\n\
######################################################################\n\
## SOLVERS\n\
######################################################################\n";

const std::string header_variables =
    "\n\
######################################################################\n\
## VARIABLES\n\
######################################################################\n";

const std::string header_time =
    "\n\
######################################################################\n\
## ADAPTIVE TIME\n\
######################################################################\n";

const std::string header_gridinit =
    "\n\
######################################################################\n\
## DOMAIN INIT\n\
######################################################################\n";

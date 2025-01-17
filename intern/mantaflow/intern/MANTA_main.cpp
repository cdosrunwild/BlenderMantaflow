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

/** \file mantaflow/intern/MANTA.cpp
 *  \ingroup mantaflow
 */

#include <sstream>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <zlib.h>

#include "MANTA_main.h"
#include "manta.h"
#include "Python.h"
#include "fluid_script.h"
#include "smoke_script.h"
#include "liquid_script.h"

#include "BLI_path_util.h"
#include "BLI_utildefines.h"
#include "BLI_fileops.h"

#include "DNA_scene_types.h"
#include "DNA_modifier_types.h"
#include "DNA_manta_types.h"

std::atomic<bool> MANTA::mantaInitialized(false);
std::atomic<int> MANTA::solverID(0);
int MANTA::with_debug(0);

MANTA::MANTA(int *res, MantaModifierData *mmd) : mCurrentID(++solverID)
{
  if (with_debug)
    std::cout << "MANTA: " << mCurrentID << " with res(" << res[0] << ", " << res[1] << ", "
              << res[2] << ")" << std::endl;

  mmd->domain->fluid = this;

  mUsingHeat = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_HEAT;
  mUsingFire = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_FIRE;
  mUsingColors = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_COLORS;
  mUsingObstacle = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_OBSTACLE;
  mUsingInvel = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_INVEL;
  mUsingOutflow = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_OUTFLOW;
  mUsingNoise = mmd->domain->flags & FLUID_DOMAIN_USE_NOISE;
  mUsingMesh = mmd->domain->flags & FLUID_DOMAIN_USE_MESH;
  mUsingMVel = mmd->domain->flags & FLUID_DOMAIN_USE_SPEED_VECTORS;
  mUsingGuiding = mmd->domain->flags & FLUID_DOMAIN_USE_GUIDING;
  mUsingLiquid = mmd->domain->type == FLUID_DOMAIN_TYPE_LIQUID;
  mUsingSmoke = mmd->domain->type == FLUID_DOMAIN_TYPE_GAS;
  mUsingDrops = mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_SPRAY;
  mUsingBubbles = mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_BUBBLE;
  mUsingFloats = mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_FOAM;
  mUsingTracers = mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_TRACER;

  // Simulation constants
  mTempAmb = 0;  // TODO: Maybe use this later for buoyancy calculation
  mResX = res[0];
  mResY = res[1];
  mResZ = res[2];
  mMaxRes = MAX3(mResX, mResY, mResZ);
  mConstantScaling = 64.0f / mMaxRes;
  mConstantScaling = (mConstantScaling < 1.0f) ? 1.0f : mConstantScaling;
  mTotalCells = mResX * mResY * mResZ;
  mResGuiding = mmd->domain->res;

  // Smoke low res grids
  mDensity = NULL;
  mShadow = NULL;
  mHeat = NULL;
  mVelocityX = NULL;
  mVelocityY = NULL;
  mVelocityZ = NULL;
  mForceX = NULL;
  mForceY = NULL;
  mForceZ = NULL;
  mFlame = NULL;
  mFuel = NULL;
  mReact = NULL;
  mColorR = NULL;
  mColorG = NULL;
  mColorB = NULL;
  mObstacle = NULL;
  mDensityIn = NULL;
  mHeatIn = NULL;
  mColorRIn = NULL;
  mColorGIn = NULL;
  mColorBIn = NULL;
  mFuelIn = NULL;
  mReactIn = NULL;
  mEmissionIn = NULL;

  // Smoke high res grids
  mDensityHigh = NULL;
  mFlameHigh = NULL;
  mFuelHigh = NULL;
  mReactHigh = NULL;
  mColorRHigh = NULL;
  mColorGHigh = NULL;
  mColorBHigh = NULL;
  mTextureU = NULL;
  mTextureV = NULL;
  mTextureW = NULL;
  mTextureU2 = NULL;
  mTextureV2 = NULL;
  mTextureW2 = NULL;

  // Fluid low res grids
  mPhiIn = NULL;
  mPhiOutIn = NULL;
  mPhi = NULL;

  // Mesh
  mMeshNodes = NULL;
  mMeshTriangles = NULL;
  mMeshVelocities = NULL;

  // Fluid obstacle
  mPhiObsIn = NULL;
  mNumObstacle = NULL;
  mObVelocityX = NULL;
  mObVelocityY = NULL;
  mObVelocityZ = NULL;

  // Fluid guiding
  mPhiGuideIn = NULL;
  mNumGuide = NULL;
  mGuideVelocityX = NULL;
  mGuideVelocityY = NULL;
  mGuideVelocityZ = NULL;

  // Fluid initial velocity
  mInVelocityX = NULL;
  mInVelocityY = NULL;
  mInVelocityZ = NULL;

  // Secondary particles
  mFlipParticleData = NULL;
  mFlipParticleVelocity = NULL;
  mSndParticleData = NULL;
  mSndParticleVelocity = NULL;
  mSndParticleLife = NULL;

  // Only start Mantaflow once. No need to start whenever new FLUID objected is allocated
  if (!mantaInitialized)
    initializeMantaflow();

  // Initialize Mantaflow variables in Python
  // Liquid
  if (mUsingLiquid) {
    initDomain(mmd);
    initLiquid(mmd);
    if (mUsingObstacle)
      initObstacle(mmd);
    if (mUsingInvel)
      initInVelocity(mmd);
    if (mUsingOutflow)
      initOutflow(mmd);

    if (mUsingDrops || mUsingBubbles || mUsingFloats || mUsingTracers) {
      mUpresParticle = mmd->domain->particle_scale;
      mResXParticle = mUpresParticle * mResX;
      mResYParticle = mUpresParticle * mResY;
      mResZParticle = mUpresParticle * mResZ;
      mTotalCellsParticles = mResXParticle * mResYParticle * mResZParticle;

      initSndParts(mmd);
      initLiquidSndParts(mmd);
    }

    if (mUsingMesh) {
      mUpresMesh = mmd->domain->mesh_scale;
      mResXMesh = mUpresMesh * mResX;
      mResYMesh = mUpresMesh * mResY;
      mResZMesh = mUpresMesh * mResZ;
      mTotalCellsMesh = mResXMesh * mResYMesh * mResZMesh;

      // Initialize Mantaflow variables in Python
      initMesh(mmd);
      initLiquidMesh(mmd);
    }

    if (mUsingGuiding) {
      mResGuiding = (mmd->domain->guiding_parent) ? mmd->domain->guide_res : mmd->domain->res;
      initGuiding(mmd);
    }
    updatePointers();

    return;
  }

  // Smoke
  if (mUsingSmoke) {
    initDomain(mmd);
    initSmoke(mmd);
    if (mUsingHeat)
      initHeat(mmd);
    if (mUsingFire)
      initFire(mmd);
    if (mUsingColors)
      initColors(mmd);
    if (mUsingObstacle)
      initObstacle(mmd);
    if (mUsingInvel)
      initInVelocity(mmd);
    if (mUsingOutflow)
      initOutflow(mmd);

    if (mUsingGuiding) {
      mResGuiding = (mmd->domain->guiding_parent) ? mmd->domain->guide_res : mmd->domain->res;
      initGuiding(mmd);
    }
    updatePointers();  // Needs to be after heat, fire, color init

    if (mUsingNoise) {
      int amplify = mmd->domain->noise_scale;
      mResXNoise = amplify * mResX;
      mResYNoise = amplify * mResY;
      mResZNoise = amplify * mResZ;
      mTotalCellsHigh = mResXNoise * mResYNoise * mResZNoise;

      // Initialize Mantaflow variables in Python
      initNoise(mmd);
      initSmokeNoise(mmd);
      if (mUsingFire)
        initFireHigh(mmd);
      if (mUsingColors)
        initColorsHigh(mmd);

      updatePointersNoise();  // Needs to be after fire, color init
    }
  }
}

void MANTA::initDomain(MantaModifierData *mmd)
{
  // Vector will hold all python commands that are to be executed
  std::vector<std::string> pythonCommands;

  // Set manta debug level first
  pythonCommands.push_back(manta_import + manta_debuglevel);

  std::ostringstream ss;
  ss << "set_manta_debuglevel(" << with_debug << ")";
  pythonCommands.push_back(ss.str());

  // Now init basic fluid domain
  std::string tmpString = fluid_variables + fluid_solver + fluid_alloc + fluid_cache_helper +
                          fluid_bake_multiprocessing + fluid_bake_data + fluid_bake_noise +
                          fluid_bake_mesh + fluid_bake_particles + fluid_bake_guiding +
                          fluid_file_import + fluid_file_export + fluid_save_data +
                          fluid_load_data + fluid_pre_step + fluid_post_step +
                          fluid_adapt_time_step + fluid_time_stepping;
  std::string finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);
  runPythonString(pythonCommands);
}

void MANTA::initNoise(MantaModifierData *mmd)
{
  std::vector<std::string> pythonCommands;
  std::string tmpString = fluid_variables_noise + fluid_solver_noise +
                          fluid_time_stepping_noise;
  std::string finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);

  runPythonString(pythonCommands);
}

void MANTA::initSmoke(MantaModifierData *mmd)
{
  std::vector<std::string> pythonCommands;
  std::string tmpString = smoke_variables + smoke_alloc + smoke_adaptive_step + smoke_save_data +
                          smoke_load_data + smoke_step;
  std::string finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);

  runPythonString(pythonCommands);
}

void MANTA::initSmokeNoise(MantaModifierData *mmd)
{
  std::vector<std::string> pythonCommands;
  std::string tmpString = smoke_variables_noise + smoke_alloc_noise + smoke_wavelet_noise +
                          smoke_save_noise + smoke_load_noise + smoke_step_noise;
  std::string finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);

  runPythonString(pythonCommands);
  mUsingNoise = true;
}

void MANTA::initHeat(MantaModifierData *mmd)
{
  if (!mHeat) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = smoke_alloc_heat + smoke_with_heat;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingHeat = true;
  }
}

void MANTA::initFire(MantaModifierData *mmd)
{
  if (!mFuel) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = smoke_alloc_fire + smoke_with_fire;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingFire = true;
  }
}

void MANTA::initFireHigh(MantaModifierData *mmd)
{
  if (!mFuelHigh) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = smoke_alloc_fire_noise + smoke_with_fire;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingFire = true;
  }
}

void MANTA::initColors(MantaModifierData *mmd)
{
  if (!mColorR) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = smoke_alloc_colors + smoke_init_colors + smoke_with_colors;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingColors = true;
  }
}

void MANTA::initColorsHigh(MantaModifierData *mmd)
{
  if (!mColorRHigh) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = smoke_alloc_colors_noise + smoke_init_colors_noise + smoke_with_colors;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingColors = true;
  }
}

void MANTA::initLiquid(MantaModifierData *mmd)
{
  if (!mPhiIn) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = liquid_variables + liquid_alloc + liquid_init_phi + liquid_save_data +
                            liquid_save_flip + liquid_load_data + liquid_load_flip +
                            liquid_adaptive_step + liquid_step;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingLiquid = true;
  }
}

void MANTA::initMesh(MantaModifierData *mmd)
{
  std::vector<std::string> pythonCommands;
  std::string tmpString = fluid_variables_mesh + fluid_solver_mesh +
                          liquid_load_mesh + liquid_load_meshvel;
  std::string finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);

  runPythonString(pythonCommands);
  mUsingMesh = true;
}

void MANTA::initLiquidMesh(MantaModifierData *mmd)
{
  std::vector<std::string> pythonCommands;
  std::string tmpString = liquid_alloc_mesh + liquid_step_mesh + liquid_save_mesh +
                          liquid_save_meshvel;
  std::string finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);

  runPythonString(pythonCommands);
  mUsingMesh = true;
}

void MANTA::initObstacle(MantaModifierData *mmd)
{
  if (!mPhiObsIn) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = fluid_alloc_obstacle + fluid_with_obstacle;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingObstacle = true;
  }
}

void MANTA::initGuiding(MantaModifierData *mmd)
{
  if (!mPhiGuideIn) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = fluid_variables_guiding + fluid_solver_guiding + fluid_alloc_guiding +
                            fluid_save_guiding + fluid_load_vel + fluid_load_guiding;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingGuiding = true;
  }
}

void MANTA::initInVelocity(MantaModifierData *mmd)
{
  if (!mInVelocityX) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = fluid_alloc_invel + fluid_with_invel;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingInvel = true;
  }
}

void MANTA::initOutflow(MantaModifierData *mmd)
{
  if (!mPhiOutIn) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = fluid_alloc_outflow + fluid_with_outflow;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
    mUsingOutflow = true;
  }
}

void MANTA::initSndParts(MantaModifierData *mmd)
{
  std::vector<std::string> pythonCommands;
  std::string tmpString = fluid_variables_particles + fluid_solver_particles +
                          fluid_load_particles + fluid_save_particles;
  std::string finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);

  runPythonString(pythonCommands);
}

void MANTA::initLiquidSndParts(MantaModifierData *mmd)
{
  if (!mSndParticleData) {
    std::vector<std::string> pythonCommands;
    std::string tmpString = fluid_alloc_sndparts + liquid_alloc_particles +
                            liquid_variables_particles + liquid_step_particles +
                            fluid_with_sndparts + liquid_load_particles + liquid_save_particles;
    std::string finalString = parseScript(tmpString, mmd);
    pythonCommands.push_back(finalString);

    runPythonString(pythonCommands);
  }
}

MANTA::~MANTA()
{
  if (with_debug)
    std::cout << "~FLUID: " << mCurrentID << " with res(" << mResX << ", " << mResY << ", "
              << mResZ << ")" << std::endl;

  // Destruction string for Python
  std::string tmpString = "";
  std::vector<std::string> pythonCommands;

  tmpString += manta_import;
  tmpString += fluid_delete_all;

  // Leave out mmd argument in parseScript since only looking up IDs
  std::string finalString = parseScript(tmpString);
  pythonCommands.push_back(finalString);
  runPythonString(pythonCommands);
}

void MANTA::runPythonString(std::vector<std::string> commands)
{
  PyGILState_STATE gilstate = PyGILState_Ensure();
  for (std::vector<std::string>::iterator it = commands.begin(); it != commands.end(); ++it) {
    std::string command = *it;

#ifdef WIN32
    // special treatment for windows when running python code
    size_t cmdLength = command.length();
    char *buffer = new char[cmdLength + 1];
    memcpy(buffer, command.data(), cmdLength);

    buffer[cmdLength] = '\0';
    PyRun_SimpleString(buffer);
    delete[] buffer;
#else
    PyRun_SimpleString(command.c_str());
#endif
  }
  PyGILState_Release(gilstate);
}

void MANTA::initializeMantaflow()
{
  if (with_debug)
    std::cout << "Initializing  Mantaflow" << std::endl;

  std::string filename = "manta_scene_" + std::to_string(mCurrentID) + ".py";
  std::vector<std::string> fill = std::vector<std::string>();

  // Initialize extension classes and wrappers
  srand(0);
  PyGILState_STATE gilstate = PyGILState_Ensure();
  Pb::setup(filename, fill);  // Namespace from Mantaflow (registry)
  PyGILState_Release(gilstate);
  mantaInitialized = true;
}

void MANTA::terminateMantaflow()
{
  if (with_debug)
    std::cout << "Terminating Mantaflow" << std::endl;

  PyGILState_STATE gilstate = PyGILState_Ensure();
  Pb::finalize();  // Namespace from Mantaflow (registry)
  PyGILState_Release(gilstate);
  mantaInitialized = false;
}

std::string MANTA::getRealValue(const std::string &varName, MantaModifierData *mmd)
{
  std::ostringstream ss;
  bool is2D = false;
  int tmpVar;
  float tmpFloat;

  if (varName == "ID") {
    ss << mCurrentID;
    return ss.str();
  }

  if (!mmd) {
    if (with_debug)
      std::cout << "Invalid modifier data in getRealValue()" << std::endl;
    ss << "ERROR - INVALID MODIFIER DATA";
    return ss.str();
  }

  is2D = (mmd->domain->solver_res == 2);

  if (varName == "USING_SMOKE")
    ss << ((mmd->domain->type == FLUID_DOMAIN_TYPE_GAS) ? "True" : "False");
  else if (varName == "USING_LIQUID")
    ss << ((mmd->domain->type == FLUID_DOMAIN_TYPE_LIQUID) ? "True" : "False");
  else if (varName == "USING_COLORS")
    ss << (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_COLORS ? "True" : "False");
  else if (varName == "USING_HEAT")
    ss << (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_HEAT ? "True" : "False");
  else if (varName == "USING_FIRE")
    ss << (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_FIRE ? "True" : "False");
  else if (varName == "USING_NOISE")
    ss << (mmd->domain->flags & FLUID_DOMAIN_USE_NOISE ? "True" : "False");
  else if (varName == "USING_OBSTACLE")
    ss << (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_OBSTACLE ? "True" : "False");
  else if (varName == "USING_GUIDING")
    ss << (mmd->domain->flags & FLUID_DOMAIN_USE_GUIDING ? "True" : "False");
  else if (varName == "USING_INVEL")
    ss << (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_INVEL ? "True" : "False");
  else if (varName == "USING_OUTFLOW")
    ss << (mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_OUTFLOW ? "True" : "False");
  else if (varName == "USING_LOG_DISSOLVE")
    ss << (mmd->domain->flags & FLUID_DOMAIN_USE_DISSOLVE_LOG ? "True" : "False");
  else if (varName == "USING_DISSOLVE")
    ss << (mmd->domain->flags & FLUID_DOMAIN_USE_DISSOLVE ? "True" : "False");
  else if (varName == "SOLVER_DIM")
    ss << mmd->domain->solver_res;
  else if (varName == "DO_OPEN") {
    tmpVar = (FLUID_DOMAIN_BORDER_BACK | FLUID_DOMAIN_BORDER_FRONT | FLUID_DOMAIN_BORDER_LEFT |
              FLUID_DOMAIN_BORDER_RIGHT | FLUID_DOMAIN_BORDER_BOTTOM | FLUID_DOMAIN_BORDER_TOP);
    ss << (((mmd->domain->border_collisions & tmpVar) == tmpVar) ? "False" : "True");
  }
  else if (varName == "BOUND_CONDITIONS") {
    if (mmd->domain->solver_res == 2) {
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_LEFT) == 0)
        ss << "x";
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_RIGHT) == 0)
        ss << "X";
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_FRONT) == 0)
        ss << "y";
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_BACK) == 0)
        ss << "Y";
    }
    if (mmd->domain->solver_res == 3) {
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_LEFT) == 0)
        ss << "x";
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_RIGHT) == 0)
        ss << "X";
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_FRONT) == 0)
        ss << "y";
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_BACK) == 0)
        ss << "Y";
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_BOTTOM) == 0)
        ss << "z";
      if ((mmd->domain->border_collisions & FLUID_DOMAIN_BORDER_TOP) == 0)
        ss << "Z";
    }
  }
  else if (varName == "BOUNDARY_WIDTH")
    ss << mmd->domain->boundary_width;
  else if (varName == "RES")
    ss << mMaxRes;
  else if (varName == "RESX")
    ss << mResX;
  else if (varName == "RESY")
    if (is2D) {
      ss << mResZ;
    }
    else {
      ss << mResY;
    }
  else if (varName == "RESZ") {
    if (is2D) {
      ss << 1;
    }
    else {
      ss << mResZ;
    }
  }
  else if (varName == "FRAME_LENGTH")
    ss << mmd->domain->frame_length;
  else if (varName == "CFL")
    ss << mmd->domain->cfl_condition;
  else if (varName == "DT")
    ss << mmd->domain->dt;
  else if (varName == "TIME_TOTAL")
    ss << mmd->domain->time_total;
  else if (varName == "TIME_PER_FRAME")
    ss << mmd->domain->time_per_frame;
  else if (varName == "VORTICITY")
    ss << mmd->domain->vorticity / mConstantScaling;
  else if (varName == "NOISE_SCALE")
    ss << mmd->domain->noise_scale;
  else if (varName == "MESH_SCALE")
    ss << mmd->domain->mesh_scale;
  else if (varName == "PARTICLE_SCALE")
    ss << mmd->domain->particle_scale;
  else if (varName == "NOISE_RESX")
    ss << mResXNoise;
  else if (varName == "NOISE_RESY") {
    if (is2D) {
      ss << mResZNoise;
    }
    else {
      ss << mResYNoise;
    }
  }
  else if (varName == "NOISE_RESZ") {
    if (is2D) {
      ss << 1;
    }
    else {
      ss << mResZNoise;
    }
  }
  else if (varName == "MESH_RESX")
    ss << mResXMesh;
  else if (varName == "MESH_RESY") {
    if (is2D) {
      ss << mResZMesh;
    }
    else {
      ss << mResYMesh;
    }
  }
  else if (varName == "MESH_RESZ") {
    if (is2D) {
      ss << 1;
    }
    else {
      ss << mResZMesh;
    }
  }
  else if (varName == "PARTICLE_RESX")
    ss << mResXParticle;
  else if (varName == "PARTICLE_RESY") {
    if (is2D) {
      ss << mResZParticle;
    }
    else {
      ss << mResYParticle;
    }
  }
  else if (varName == "PARTICLE_RESZ") {
    if (is2D) {
      ss << 1;
    }
    else {
      ss << mResZParticle;
    }
  }
  else if (varName == "GUIDING_RESX")
    ss << mResGuiding[0];
  else if (varName == "GUIDING_RESY") {
    if (is2D) {
      ss << mResGuiding[2];
    }
    else {
      ss << mResGuiding[1];
    }
  }
  else if (varName == "GUIDING_RESZ") {
    if (is2D) {
      ss << 1;
    }
    else {
      ss << mResGuiding[2];
    }
  }
  else if (varName == "MIN_RESX")
    ss << mmd->domain->res_min[0];
  else if (varName == "MIN_RESY")
    ss << mmd->domain->res_min[1];
  else if (varName == "MIN_RESZ")
    ss << mmd->domain->res_min[2];
  else if (varName == "BASE_RESX")
    ss << mmd->domain->base_res[0];
  else if (varName == "BASE_RESY")
    ss << mmd->domain->base_res[1];
  else if (varName == "BASE_RESZ")
    ss << mmd->domain->base_res[2];
  else if (varName == "WLT_STR")
    ss << mmd->domain->noise_strength;
  else if (varName == "NOISE_POSSCALE")
    ss << mmd->domain->noise_pos_scale;
  else if (varName == "NOISE_TIMEANIM")
    ss << mmd->domain->noise_time_anim;
  else if (varName == "COLOR_R")
    ss << mmd->domain->active_color[0];
  else if (varName == "COLOR_G")
    ss << mmd->domain->active_color[1];
  else if (varName == "COLOR_B")
    ss << mmd->domain->active_color[2];
  else if (varName == "BUOYANCY_ALPHA")
    ss << mmd->domain->alpha;
  else if (varName == "BUOYANCY_BETA")
    ss << mmd->domain->beta;
  else if (varName == "DISSOLVE_SPEED")
    ss << mmd->domain->diss_speed;
  else if (varName == "BURNING_RATE")
    ss << mmd->domain->burning_rate;
  else if (varName == "FLAME_SMOKE")
    ss << mmd->domain->flame_smoke;
  else if (varName == "IGNITION_TEMP")
    ss << mmd->domain->flame_ignition;
  else if (varName == "MAX_TEMP")
    ss << mmd->domain->flame_max_temp;
  else if (varName == "FLAME_SMOKE_COLOR_X")
    ss << mmd->domain->flame_smoke_color[0];
  else if (varName == "FLAME_SMOKE_COLOR_Y")
    ss << mmd->domain->flame_smoke_color[1];
  else if (varName == "FLAME_SMOKE_COLOR_Z")
    ss << mmd->domain->flame_smoke_color[2];
  else if (varName == "CURRENT_FRAME")
    ss << mmd->time;
  else if (varName == "END_FRAME")
    ss << mmd->domain->cache_frame_end;
  else if (varName == "PARTICLE_RANDOMNESS")
    ss << mmd->domain->particle_randomness;
  else if (varName == "PARTICLE_NUMBER")
    ss << mmd->domain->particle_number;
  else if (varName == "PARTICLE_MINIMUM")
    ss << mmd->domain->particle_minimum;
  else if (varName == "PARTICLE_MAXIMUM")
    ss << mmd->domain->particle_maximum;
  else if (varName == "PARTICLE_RADIUS")
    ss << mmd->domain->particle_radius;
  else if (varName == "MESH_CONCAVE_UPPER")
    ss << mmd->domain->mesh_concave_upper;
  else if (varName == "MESH_CONCAVE_LOWER")
    ss << mmd->domain->mesh_concave_lower;
  else if (varName == "MESH_SMOOTHEN_POS")
    ss << mmd->domain->mesh_smoothen_pos;
  else if (varName == "MESH_SMOOTHEN_NEG")
    ss << mmd->domain->mesh_smoothen_neg;
  else if (varName == "USING_MESH")
    ss << (mmd->domain->flags & FLUID_DOMAIN_USE_MESH ? "True" : "False");
  else if (varName == "USING_IMPROVED_MESH")
    ss << (mmd->domain->mesh_generator == FLUID_DOMAIN_MESH_IMPROVED ? "True" : "False");
  else if (varName == "PARTICLE_BAND_WIDTH")
    ss << mmd->domain->particle_band_width;
  else if (varName == "SNDPARTICLE_TAU_MIN_WC")
    ss << mmd->domain->sndparticle_tau_min_wc;
  else if (varName == "SNDPARTICLE_TAU_MAX_WC")
    ss << mmd->domain->sndparticle_tau_max_wc;
  else if (varName == "SNDPARTICLE_TAU_MIN_TA")
    ss << mmd->domain->sndparticle_tau_min_ta;
  else if (varName == "SNDPARTICLE_TAU_MAX_TA")
    ss << mmd->domain->sndparticle_tau_max_ta;
  else if (varName == "SNDPARTICLE_TAU_MIN_K")
    ss << mmd->domain->sndparticle_tau_min_k;
  else if (varName == "SNDPARTICLE_TAU_MAX_K")
    ss << mmd->domain->sndparticle_tau_max_k;
  else if (varName == "SNDPARTICLE_K_WC")
    ss << mmd->domain->sndparticle_k_wc;
  else if (varName == "SNDPARTICLE_K_TA")
    ss << mmd->domain->sndparticle_k_ta;
  else if (varName == "SNDPARTICLE_K_B")
    ss << mmd->domain->sndparticle_k_b;
  else if (varName == "SNDPARTICLE_K_D")
    ss << mmd->domain->sndparticle_k_d;
  else if (varName == "SNDPARTICLE_L_MIN")
    ss << mmd->domain->sndparticle_l_min;
  else if (varName == "SNDPARTICLE_L_MAX")
    ss << mmd->domain->sndparticle_l_max;
  else if (varName == "SNDPARTICLE_BOUNDARY_DELETE")
    ss << (mmd->domain->sndparticle_boundary == SNDPARTICLE_BOUNDARY_DELETE);
  else if (varName == "SNDPARTICLE_BOUNDARY_PUSHOUT")
    ss << (mmd->domain->sndparticle_boundary == SNDPARTICLE_BOUNDARY_PUSHOUT);
  else if (varName == "SNDPARTICLE_POTENTIAL_RADIUS")
    ss << mmd->domain->sndparticle_potential_radius;
  else if (varName == "SNDPARTICLE_UPDATE_RADIUS")
    ss << mmd->domain->sndparticle_update_radius;
  else if (varName == "LIQUID_SURFACE_TENSION")
    ss << mmd->domain->surface_tension;
  else if (varName == "FLUID_VISCOSITY")
    ss << mmd->domain->viscosity_base * pow(10.0f, -mmd->domain->viscosity_exponent);
  else if (varName == "FLUID_DOMAIN_SIZE") {
    tmpFloat = MAX3(mmd->domain->global_size[0], mmd->domain->global_size[1], mmd->domain->global_size[2]);
    ss << tmpFloat;
  } else if (varName == "SNDPARTICLE_TYPES") {
    if (mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_SPRAY) {
      ss << "PtypeSpray";
    }
    if (mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_BUBBLE) {
      if (!ss.str().empty())
        ss << "|";
      ss << "PtypeBubble";
    }
    if (mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_FOAM) {
      if (!ss.str().empty())
        ss << "|";
      ss << "PtypeFoam";
    }
    if (mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_TRACER) {
      if (!ss.str().empty())
        ss << "|";
      ss << "PtypeTracer";
    }
    if (ss.str().empty())
      ss << "0";
  }
  else if (varName == "USING_SNDPARTS") {
    tmpVar = (FLUID_DOMAIN_PARTICLE_SPRAY | FLUID_DOMAIN_PARTICLE_BUBBLE |
              FLUID_DOMAIN_PARTICLE_FOAM | FLUID_DOMAIN_PARTICLE_TRACER);
    ss << (((mmd->domain->particle_type & tmpVar)) ? "True" : "False");
  }
  else if (varName == "GUIDING_ALPHA")
    ss << mmd->domain->guiding_alpha;
  else if (varName == "GUIDING_BETA")
    ss << mmd->domain->guiding_beta;
  else if (varName == "GUIDING_FACTOR")
    ss << mmd->domain->guiding_vel_factor;
  else if (varName == "GRAVITY_X")
    ss << mmd->domain->gravity[0];
  else if (varName == "GRAVITY_Y")
    ss << mmd->domain->gravity[1];
  else if (varName == "GRAVITY_Z")
    ss << mmd->domain->gravity[2];
  else if (varName == "CACHE_DIR")
    ss << mmd->domain->cache_directory;
  else if (varName == "USING_ADAPTIVETIME")
    ss << (mmd->domain->flags & FLUID_DOMAIN_USE_ADAPTIVE_TIME ? "True" : "False");
  else if (varName == "USING_SPEEDVECTORS")
    ss << (mmd->domain->flags & FLUID_DOMAIN_USE_SPEED_VECTORS ? "True" : "False");
  else
    std::cout << "ERROR: Unknown option: " << varName << std::endl;
  return ss.str();
}

std::string MANTA::parseLine(const std::string &line, MantaModifierData *mmd)
{
  if (line.size() == 0)
    return "";
  std::string res = "";
  int currPos = 0, start_del = 0, end_del = -1;
  bool readingVar = false;
  const char delimiter = '$';
  while (currPos < line.size()) {
    if (line[currPos] == delimiter && !readingVar) {
      readingVar = true;
      start_del = currPos + 1;
      res += line.substr(end_del + 1, currPos - end_del - 1);
    }
    else if (line[currPos] == delimiter && readingVar) {
      readingVar = false;
      end_del = currPos;
      res += getRealValue(line.substr(start_del, currPos - start_del), mmd);
    }
    currPos++;
  }
  res += line.substr(end_del + 1, line.size() - end_del);
  return res;
}

std::string MANTA::parseScript(const std::string &setup_string, MantaModifierData *mmd)
{
  std::istringstream f(setup_string);
  std::ostringstream res;
  std::string line = "";
  while (getline(f, line)) {
    res << parseLine(line, mmd) << "\n";
  }
  return res.str();
}

static std::string getCacheFileEnding(char cache_format)
{
  if (MANTA::with_debug)
    std::cout << "MANTA::getCacheFileEnding()" << std::endl;

  switch (cache_format) {
    case FLUID_DOMAIN_FILE_UNI:
      return ".uni";
    case FLUID_DOMAIN_FILE_OPENVDB:
      return ".vdb";
    case FLUID_DOMAIN_FILE_RAW:
      return ".raw";
    case FLUID_DOMAIN_FILE_BIN_OBJECT:
      return ".bobj.gz";
    case FLUID_DOMAIN_FILE_OBJECT:
      return ".obj";
    default:
      if (MANTA::with_debug)
        std::cout << "Error: Could not find file extension" << std::endl;
      return ".uni";
  }
}

int MANTA::updateFlipStructures(MantaModifierData *mmd, int framenr)
{
  if (MANTA::with_debug)
    std::cout << "MANTA::updateFlipStructures()" << std::endl;

  // Ensure empty data structures at start
  if (mFlipParticleData)
    mFlipParticleData->clear();
  if (mFlipParticleVelocity)
    mFlipParticleVelocity->clear();

  if (!mUsingLiquid)
    return 0;
  if (BLI_path_is_rel(mmd->domain->cache_directory))
    return 0;

  std::ostringstream ss;
  char cacheDir[FILE_MAX], targetFile[FILE_MAX];
  cacheDir[0] = '\0';
  targetFile[0] = '\0';

  std::string pformat = getCacheFileEnding(mmd->domain->cache_particle_format);
  BLI_path_join(
      cacheDir, sizeof(cacheDir), mmd->domain->cache_directory, FLUID_DOMAIN_DIR_DATA, NULL);

  // TODO (sebbas): Use pp_xl and pVel_xl when using upres simulation?

  ss << "pp_####" << pformat;
  BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDir, ss.str().c_str());
  BLI_path_frame(targetFile, framenr, 0);

  if (BLI_exists(targetFile)) {
    updateParticlesFromFile(targetFile, false, false);
  }

  ss.str("");
  ss << "pVel_####" << pformat;
  BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDir, ss.str().c_str());
  BLI_path_frame(targetFile, framenr, 0);

  if (BLI_exists(targetFile)) {
    updateParticlesFromFile(targetFile, false, true);
  }
  return 1;
}

int MANTA::updateMeshStructures(MantaModifierData *mmd, int framenr)
{
  if (MANTA::with_debug)
    std::cout << "MANTA::updateMeshStructures()" << std::endl;

  if (!mUsingMesh)
    return 0;
  if (BLI_path_is_rel(mmd->domain->cache_directory))
    return 0;

  // Ensure empty data structures at start
  if (mMeshNodes)
    mMeshNodes->clear();
  if (mMeshTriangles)
    mMeshTriangles->clear();
  if (mMeshVelocities)
    mMeshVelocities->clear();

  std::ostringstream ss;
  char cacheDir[FILE_MAX], targetFile[FILE_MAX];
  cacheDir[0] = '\0';
  targetFile[0] = '\0';

  std::string mformat = getCacheFileEnding(mmd->domain->cache_mesh_format);
  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);
  BLI_path_join(
      cacheDir, sizeof(cacheDir), mmd->domain->cache_directory, FLUID_DOMAIN_DIR_MESH, NULL);

  ss << "lMesh_####" << mformat;
  BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDir, ss.str().c_str());
  BLI_path_frame(targetFile, framenr, 0);

  if (BLI_exists(targetFile)) {
    updateMeshFromFile(targetFile);
  }

  if (mUsingMVel) {
    ss.str("");
    ss << "lVelMesh_####" << dformat;
    BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDir, ss.str().c_str());
    BLI_path_frame(targetFile, framenr, 0);

    if (BLI_exists(targetFile)) {
      updateMeshFromFile(targetFile);
    }
  }
  return 1;
}

int MANTA::updateParticleStructures(MantaModifierData *mmd, int framenr)
{
  if (MANTA::with_debug)
    std::cout << "MANTA::updateParticleStructures()" << std::endl;

  if (!mUsingDrops && !mUsingBubbles && !mUsingFloats && !mUsingTracers)
    return 0;
  if (BLI_path_is_rel(mmd->domain->cache_directory))
    return 0;

  // Ensure empty data structures at start
  if (mSndParticleData)
    mSndParticleData->clear();
  if (mSndParticleVelocity)
    mSndParticleVelocity->clear();
  if (mSndParticleLife)
    mSndParticleLife->clear();

  std::ostringstream ss;
  char cacheDir[FILE_MAX], targetFile[FILE_MAX];
  cacheDir[0] = '\0';
  targetFile[0] = '\0';

  std::string pformat = getCacheFileEnding(mmd->domain->cache_particle_format);
  BLI_path_join(
      cacheDir, sizeof(cacheDir), mmd->domain->cache_directory, FLUID_DOMAIN_DIR_PARTICLES, NULL);

  ss << "ppSnd_####" << pformat;
  BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDir, ss.str().c_str());
  BLI_path_frame(targetFile, framenr, 0);

  if (BLI_exists(targetFile)) {
    updateParticlesFromFile(targetFile, true, false);
  }

  ss.str("");
  ss << "pVelSnd_####" << pformat;
  BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDir, ss.str().c_str());
  BLI_path_frame(targetFile, framenr, 0);

  if (BLI_exists(targetFile)) {
    updateParticlesFromFile(targetFile, true, true);
  }

  ss.str("");
  ss << "pLifeSnd_####" << pformat;
  BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDir, ss.str().c_str());
  BLI_path_frame(targetFile, framenr, 0);

  if (BLI_exists(targetFile)) {
    updateParticlesFromFile(targetFile, true, false);
  }
  return 1;
}

/* Dirty hack: Needed to format paths from python code that is run via PyRun_SimpleString */
static std::string escapeSlashes(std::string const &s)
{
  std::string result = "";
  for (std::string::const_iterator i = s.begin(), end = s.end(); i != end; ++i) {
    unsigned char c = *i;
    if (c == '\\')
      result += "\\\\";
    else
      result += c;
  }
  return result;
}

int MANTA::writeConfiguration(MantaModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::writeConfiguration()" << std::endl;

  MantaDomainSettings *mds = mmd->domain;
  std::ostringstream ss;
  char cacheDir[FILE_MAX], targetFile[FILE_MAX];;
  cacheDir[0] = '\0';
  targetFile[0] = '\0';

  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);

  BLI_path_join(cacheDir,
                sizeof(cacheDir),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_CONFIG,
                NULL);
  BLI_path_make_safe(cacheDir);
  BLI_dir_create_recursive(cacheDir); /* Create 'config' subdir if it does not exist already */

  ss.str("");
  ss << "config_####" << dformat;
  BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDir, ss.str().c_str());
  BLI_path_frame(targetFile, framenr, 0);

  gzFile gzf = gzopen(targetFile, "wb1"); // do some compression
  if (!gzf)
    std::cerr << "writeConfiguration: can't open file: " << targetFile << std::endl;

  gzwrite(gzf, &mds->active_fields, sizeof(int));
  gzwrite(gzf, &mds->res, 3*sizeof(int));
  gzwrite(gzf, &mds->dx, sizeof(float));
  gzwrite(gzf, &mds->dt, sizeof(float));
  gzwrite(gzf, &mds->p0, 3*sizeof(float));
  gzwrite(gzf, &mds->p1, 3*sizeof(float));
  gzwrite(gzf, &mds->dp0, 3*sizeof(float));
  gzwrite(gzf, &mds->shift, 3*sizeof(int));
  gzwrite(gzf, &mds->obj_shift_f, 3*sizeof(float));
  gzwrite(gzf, &mds->obmat, 16*sizeof(float));
  gzwrite(gzf, &mds->base_res, 3*sizeof(int));
  gzwrite(gzf, &mds->res_min, 3*sizeof(int));
  gzwrite(gzf, &mds->res_max, 3*sizeof(int));
  gzwrite(gzf, &mds->active_color, 3*sizeof(float));

  gzclose(gzf);

  return 1;
}

int MANTA::writeData(MantaModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::writeData()" << std::endl;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  char cacheDirData[FILE_MAX];
  cacheDirData[0] = '\0';

  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);
  std::string pformat = getCacheFileEnding(mmd->domain->cache_particle_format);

  BLI_path_join(cacheDirData,
                sizeof(cacheDirData),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_DATA,
                NULL);
  BLI_path_make_safe(cacheDirData);

  ss.str("");
  ss << "fluid_save_data_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', " << framenr
     << ", '" << dformat << "')";
  pythonCommands.push_back(ss.str());

  if (mUsingSmoke) {
    ss.str("");
    ss << "smoke_save_data_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', "
       << framenr << ", '" << dformat << "')";
    pythonCommands.push_back(ss.str());
  }
  if (mUsingLiquid) {
    ss.str("");
    ss << "liquid_save_data_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', "
       << framenr << ", '" << dformat << "')";
    pythonCommands.push_back(ss.str());
    ss.str("");
    ss << "liquid_save_flip_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', "
       << framenr << ", '" << pformat << "')";
    pythonCommands.push_back(ss.str());
  }
  runPythonString(pythonCommands);
  return 1;
}

int MANTA::readConfiguration(MantaModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::readConfiguration()" << std::endl;

  MantaDomainSettings *mds = mmd->domain;
  std::ostringstream ss;
  char cacheDir[FILE_MAX], targetFile[FILE_MAX];
  cacheDir[0] = '\0';
  targetFile[0] = '\0';
  float dummy;

  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);

  BLI_path_join(cacheDir,
                sizeof(cacheDir),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_CONFIG,
                NULL);
  BLI_path_make_safe(cacheDir);

  ss.str("");
  ss << "config_####" << dformat;
  BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDir, ss.str().c_str());
  BLI_path_frame(targetFile, framenr, 0);

  if (!BLI_exists(targetFile))
    return 0;

  gzFile gzf = gzopen(targetFile, "rb"); // do some compression
  if (!gzf)
    std::cerr << "readConfiguration: can't open file: " << targetFile << std::endl;

  gzread(gzf, &mds->active_fields, sizeof(int));
  gzread(gzf, &mds->res, 3*sizeof(int));
  gzread(gzf, &mds->dx, sizeof(float));
  gzread(gzf, &dummy, sizeof(float)); // dt not needed right now
  gzread(gzf, &mds->p0, 3*sizeof(float));
  gzread(gzf, &mds->p1, 3*sizeof(float));
  gzread(gzf, &mds->dp0, 3*sizeof(float));
  gzread(gzf, &mds->shift, 3*sizeof(int));
  gzread(gzf, &mds->obj_shift_f, 3*sizeof(float));
  gzread(gzf, &mds->obmat, 16*sizeof(float));
  gzread(gzf, &mds->base_res, 3*sizeof(int));
  gzread(gzf, &mds->res_min, 3*sizeof(int));
  gzread(gzf, &mds->res_max, 3*sizeof(int));
  gzread(gzf, &mds->active_color, 3*sizeof(float));
  mds->total_cells = mds->res[0] * mds->res[1] * mds->res[2];

  gzclose(gzf);
  return 1;
}

int MANTA::readData(MantaModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::readData()" << std::endl;

  if (!mUsingSmoke && !mUsingLiquid)
    return 0;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  char cacheDirData[FILE_MAX], targetFile[FILE_MAX];
  cacheDirData[0] = '\0';
  targetFile[0] = '\0';

  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);
  std::string pformat = getCacheFileEnding(mmd->domain->cache_particle_format);

  BLI_path_join(cacheDirData,
                sizeof(cacheDirData),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_DATA,
                NULL);
  BLI_path_make_safe(cacheDirData);

  if (mUsingSmoke) {
    /* Exit early if there is nothing present in the cache for this frame */
    ss.str("");
    ss << "density_####" << dformat;
    BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDirData, ss.str().c_str());
    BLI_path_frame(targetFile, framenr, 0);
    if (!BLI_exists(targetFile))
      return 0;

    ss.str("");
    ss << "fluid_load_data_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', "
       << framenr << ", '" << dformat << "')";
    pythonCommands.push_back(ss.str());
    ss.str("");
    ss << "smoke_load_data_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', "
       << framenr << ", '" << dformat << "')";
    pythonCommands.push_back(ss.str());
  }
  if (mUsingLiquid) {
    /* Exit early if there is nothing present in the cache for this frame */
    ss.str("");
    ss << "phiIn_####" << dformat;
    BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDirData, ss.str().c_str());
    BLI_path_frame(targetFile, framenr, 0);
    if (!BLI_exists(targetFile))
      return 0;

    ss.str("");
    ss << "fluid_load_data_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', "
       << framenr << ", '" << dformat << "')";
    pythonCommands.push_back(ss.str());
    ss.str("");
    ss << "liquid_load_data_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', "
       << framenr << ", '" << dformat << "')";
    pythonCommands.push_back(ss.str());
    ss.str("");
    ss << "liquid_load_flip_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', "
       << framenr << ", '" << pformat << "')";
    pythonCommands.push_back(ss.str());
  }
  runPythonString(pythonCommands);
  updatePointers();
  return 1;
}

int MANTA::readNoise(MantaModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::readNoise()" << std::endl;

  if (!mUsingNoise)
    return 0;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  char cacheDirNoise[FILE_MAX], targetFile[FILE_MAX];
  cacheDirNoise[0] = '\0';
  targetFile[0] = '\0';

  std::string nformat = getCacheFileEnding(mmd->domain->cache_noise_format);

  BLI_path_join(cacheDirNoise,
                sizeof(cacheDirNoise),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_NOISE,
                NULL);
  BLI_path_make_safe(cacheDirNoise);

  /* Exit early if there is nothing present in the cache for this frame */
  ss.str("");
  ss << "density_noise_####" << nformat;
  BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDirNoise, ss.str().c_str());
  BLI_path_frame(targetFile, framenr, 0);
  if (!BLI_exists(targetFile))
    return 0;

  if (mUsingSmoke && mUsingNoise) {
    ss.str("");
    ss << "smoke_load_noise_" << mCurrentID << "('" << escapeSlashes(cacheDirNoise) << "', "
       << framenr << ", '" << nformat << "')";
    pythonCommands.push_back(ss.str());
  }
  runPythonString(pythonCommands);
  updatePointersNoise();
  return 1;
}

int MANTA::readMesh(MantaModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::readMesh()" << std::endl;

  if (!mUsingMesh)
    return 0;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  char cacheDirMesh[FILE_MAX], targetFile[FILE_MAX];
  cacheDirMesh[0] = '\0';
  targetFile[0] = '\0';

  std::string mformat = getCacheFileEnding(mmd->domain->cache_mesh_format);

  BLI_path_join(cacheDirMesh,
                sizeof(cacheDirMesh),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_MESH,
                NULL);
  BLI_path_make_safe(cacheDirMesh);

  if (mUsingLiquid) {
    /* Exit early if there is nothing present in the cache for this frame */
    ss.str("");
    ss << "lMesh_####" << mformat;
    BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDirMesh, ss.str().c_str());
    BLI_path_frame(targetFile, framenr, 0);
    if (!BLI_exists(targetFile))
      return 0;

    ss.str("");
    ss << "liquid_load_mesh_" << mCurrentID << "('" << escapeSlashes(cacheDirMesh) << "', "
       << framenr << ", '" << mformat << "')";
    pythonCommands.push_back(ss.str());

    if (mUsingMVel) {
      ss.str("");
      ss << "liquid_load_meshvel_" << mCurrentID << "('" << escapeSlashes(cacheDirMesh) << "', "
         << framenr << ", '" << mformat << "')";
      pythonCommands.push_back(ss.str());
    }
  }
  runPythonString(pythonCommands);
  updatePointers();
  return 1;
}

int MANTA::readParticles(MantaModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::readParticles()" << std::endl;

  if (!mUsingDrops && !mUsingBubbles && !mUsingFloats && !mUsingTracers)
    return 0;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  char cacheDirParticles[FILE_MAX], targetFile[FILE_MAX];
  cacheDirParticles[0] = '\0';
  targetFile[0] = '\0';

  std::string pformat = getCacheFileEnding(mmd->domain->cache_particle_format);

  BLI_path_join(cacheDirParticles,
                sizeof(cacheDirParticles),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_PARTICLES,
                NULL);
  BLI_path_make_safe(cacheDirParticles);

  /* Exit early if there is nothing present in the cache for this frame */
  ss.str("");
  ss << "ppSnd_####" << pformat;
  BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDirParticles, ss.str().c_str());
  BLI_path_frame(targetFile, framenr, 0);
  if (!BLI_exists(targetFile))
    return 0;

  if (mUsingDrops || mUsingBubbles || mUsingFloats || mUsingTracers) {
    ss.str("");
    ss << "fluid_load_particles_" << mCurrentID << "('" << escapeSlashes(cacheDirParticles)
       << "', " << framenr << ", '" << pformat << "')";
    pythonCommands.push_back(ss.str());
    ss.str("");
    ss << "liquid_load_particles_" << mCurrentID << "('" << escapeSlashes(cacheDirParticles)
       << "', " << framenr << ", '" << pformat << "')";
    pythonCommands.push_back(ss.str());
  }
  runPythonString(pythonCommands);
  updatePointers();
  return 1;
}

int MANTA::readGuiding(MantaModifierData *mmd, int framenr, bool sourceDomain)
{
  if (with_debug)
    std::cout << "MANTA::readGuiding()" << std::endl;

  if (!mUsingGuiding)
    return 0;
  if (!mmd->domain)
    return 0;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  char cacheDirGuiding[FILE_MAX], targetFile[FILE_MAX];
  cacheDirGuiding[0] = '\0';
  targetFile[0] = '\0';

  std::string gformat = getCacheFileEnding(mmd->domain->cache_data_format);
  const char *subdir = (sourceDomain) ? FLUID_DOMAIN_DIR_DATA : FLUID_DOMAIN_DIR_GUIDING;

  BLI_path_join(
      cacheDirGuiding, sizeof(cacheDirGuiding), mmd->domain->cache_directory, subdir, NULL);
  BLI_path_make_safe(cacheDirGuiding);

  /* Exit early if there is nothing present in the cache for this frame */
  ss.str("");
  ss << "guidevel_####" << gformat;
  BLI_join_dirfile(targetFile, sizeof(targetFile), cacheDirGuiding, ss.str().c_str());
  BLI_path_frame(targetFile, framenr, 0);
  if (!BLI_exists(targetFile))
    return 0;

  if (sourceDomain) {
    ss.str("");
    ss << "fluid_load_vel_" << mCurrentID << "('" << escapeSlashes(cacheDirGuiding) << "', "
       << framenr << ", '" << gformat << "')";
  }
  else {
    ss.str("");
    ss << "fluid_load_guiding_" << mCurrentID << "('" << escapeSlashes(cacheDirGuiding) << "', "
       << framenr << ", '" << gformat << "')";
  }
  pythonCommands.push_back(ss.str());

  runPythonString(pythonCommands);
  updatePointers();
  return 1;
}

int MANTA::bakeData(MantaModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::bakeData()" << std::endl;

  std::string tmpString, finalString;
  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  char cacheDirData[FILE_MAX], cacheDirGuiding[FILE_MAX];
  cacheDirData[0] = '\0';
  cacheDirGuiding[0] = '\0';

  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);
  std::string pformat = getCacheFileEnding(mmd->domain->cache_particle_format);
  std::string gformat = dformat;  // Use same data format for guiding format

  BLI_path_join(cacheDirData,
                sizeof(cacheDirData),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_DATA,
                NULL);
  BLI_path_join(cacheDirGuiding,
                sizeof(cacheDirGuiding),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_GUIDING,
                NULL);
  BLI_path_make_safe(cacheDirData);
  BLI_path_make_safe(cacheDirGuiding);

  ss.str("");
  ss << "bake_fluid_data_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', '"
     << escapeSlashes(cacheDirGuiding) << "', " << framenr << ", '" << dformat << "', '" << pformat
     << "', '" << gformat << "')";
  pythonCommands.push_back(ss.str());

  runPythonString(pythonCommands);
  return 1;
}

int MANTA::bakeNoise(MantaModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::bakeNoise()" << std::endl;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  char cacheDirData[FILE_MAX], cacheDirNoise[FILE_MAX];
  cacheDirData[0] = '\0';
  cacheDirNoise[0] = '\0';

  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);
  std::string nformat = getCacheFileEnding(mmd->domain->cache_noise_format);

  BLI_path_join(cacheDirData,
                sizeof(cacheDirData),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_DATA,
                NULL);
  BLI_path_join(cacheDirNoise,
                sizeof(cacheDirNoise),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_NOISE,
                NULL);
  BLI_path_make_safe(cacheDirData);
  BLI_path_make_safe(cacheDirNoise);

  ss.str("");
  ss << "bake_noise_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', '"
     << escapeSlashes(cacheDirNoise) << "', " << framenr << ", '" << dformat << "', '" << nformat
     << "')";
  pythonCommands.push_back(ss.str());

  runPythonString(pythonCommands);
  return 1;
}

int MANTA::bakeMesh(MantaModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::bakeMesh()" << std::endl;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  char cacheDirData[FILE_MAX], cacheDirMesh[FILE_MAX];
  cacheDirData[0] = '\0';
  cacheDirMesh[0] = '\0';

  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);
  std::string mformat = getCacheFileEnding(mmd->domain->cache_mesh_format);
  std::string pformat = getCacheFileEnding(mmd->domain->cache_particle_format);

  BLI_path_join(cacheDirData,
                sizeof(cacheDirData),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_DATA,
                NULL);
  BLI_path_join(cacheDirMesh,
                sizeof(cacheDirMesh),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_MESH,
                NULL);
  BLI_path_make_safe(cacheDirData);
  BLI_path_make_safe(cacheDirMesh);

  ss.str("");
  ss << "bake_mesh_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', '"
     << escapeSlashes(cacheDirMesh) << "', " << framenr << ", '" << dformat << "', '" << mformat
     << "', '" << pformat << "')";
  pythonCommands.push_back(ss.str());

  runPythonString(pythonCommands);
  return 1;
}

int MANTA::bakeParticles(MantaModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::bakeParticles()" << std::endl;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  char cacheDirData[FILE_MAX], cacheDirParticles[FILE_MAX];
  cacheDirData[0] = '\0';
  cacheDirParticles[0] = '\0';

  std::string dformat = getCacheFileEnding(mmd->domain->cache_data_format);
  std::string pformat = getCacheFileEnding(mmd->domain->cache_particle_format);

  BLI_path_join(cacheDirData,
                sizeof(cacheDirData),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_DATA,
                NULL);
  BLI_path_join(cacheDirParticles,
                sizeof(cacheDirParticles),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_PARTICLES,
                NULL);
  BLI_path_make_safe(cacheDirData);
  BLI_path_make_safe(cacheDirParticles);

  ss.str("");
  ss << "bake_particles_" << mCurrentID << "('" << escapeSlashes(cacheDirData) << "', '"
     << escapeSlashes(cacheDirParticles) << "', " << framenr << ", '" << dformat << "', '"
     << pformat << "')";
  pythonCommands.push_back(ss.str());

  runPythonString(pythonCommands);
  return 1;
}

int MANTA::bakeGuiding(MantaModifierData *mmd, int framenr)
{
  if (with_debug)
    std::cout << "MANTA::bakeGuiding()" << std::endl;

  std::ostringstream ss;
  std::vector<std::string> pythonCommands;

  char cacheDirGuiding[FILE_MAX];
  cacheDirGuiding[0] = '\0';

  std::string gformat = getCacheFileEnding(mmd->domain->cache_data_format);

  BLI_path_join(cacheDirGuiding,
                sizeof(cacheDirGuiding),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_GUIDING,
                NULL);
  BLI_path_make_safe(cacheDirGuiding);

  ss.str("");
  ss << "bake_guiding_" << mCurrentID << "('" << escapeSlashes(cacheDirGuiding) << "', " << framenr
     << ", '" << gformat << "')";
  pythonCommands.push_back(ss.str());

  runPythonString(pythonCommands);
  return 1;
}

void MANTA::updateVariables(MantaModifierData *mmd)
{
  std::string tmpString, finalString;
  std::vector<std::string> pythonCommands;

  tmpString += fluid_variables;
  if (mUsingSmoke)
    tmpString += smoke_variables;
  if (mUsingLiquid)
    tmpString += liquid_variables;
  if (mUsingGuiding)
    tmpString += fluid_variables_guiding;
  if (mUsingNoise) {
    tmpString += fluid_variables_noise;
    tmpString += smoke_variables_noise;
    tmpString += smoke_wavelet_noise;
  }
  if (mUsingDrops || mUsingBubbles || mUsingFloats || mUsingTracers) {
    tmpString += fluid_variables_particles;
    tmpString += liquid_variables_particles;
  }
  if (mUsingMesh)
    tmpString += fluid_variables_mesh;

  finalString = parseScript(tmpString, mmd);
  pythonCommands.push_back(finalString);

  runPythonString(pythonCommands);
}

void MANTA::exportSmokeScript(MantaModifierData *mmd)
{
  if (with_debug)
    std::cout << "MANTA::exportSmokeScript()" << std::endl;

  char cacheDirScript[FILE_MAX];
  cacheDirScript[0] = '\0';

  BLI_path_join(cacheDirScript,
                sizeof(cacheDirScript),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_SCRIPT,
                NULL);
  BLI_path_make_safe(cacheDirScript);
  BLI_dir_create_recursive(cacheDirScript); /* Create 'script' subdir if it does not exist already */
  BLI_path_join(
      cacheDirScript, sizeof(cacheDirScript), cacheDirScript, FLUID_DOMAIN_SMOKE_SCRIPT, NULL);
  BLI_path_make_safe(cacheDirScript);

  bool noise = mmd->domain->flags & FLUID_DOMAIN_USE_NOISE;
  bool heat = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_HEAT;
  bool colors = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_COLORS;
  bool fire = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_FIRE;
  bool obstacle = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_OBSTACLE;
  bool guiding = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_GUIDING;
  bool invel = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_INVEL;

  std::string manta_script;

  // Libraries
  manta_script += header_libraries + manta_import;

  // Variables
  manta_script += header_variables + fluid_variables + smoke_variables;
  if (noise) {
    manta_script += fluid_variables_noise + smoke_variables_noise;
  }
  if (guiding)
    manta_script += fluid_variables_guiding;

  // Solvers
  manta_script += header_solvers + fluid_solver;
  if (noise)
    manta_script += fluid_solver_noise;
  if (guiding)
    manta_script += fluid_solver_guiding;

  // Grids
  manta_script += header_grids + fluid_alloc + smoke_alloc;
  if (noise) {
    manta_script += smoke_alloc_noise;
    if (colors)
      manta_script += smoke_alloc_colors_noise;
    if (fire)
      manta_script += smoke_alloc_fire_noise;
  }
  if (heat)
    manta_script += smoke_alloc_heat;
  if (colors)
    manta_script += smoke_alloc_colors;
  if (fire)
    manta_script += smoke_alloc_fire;
  if (guiding)
    manta_script += fluid_alloc_guiding;
  if (obstacle)
    manta_script += fluid_alloc_obstacle;
  if (invel)
    manta_script += fluid_alloc_invel;

  // Noise field
  if (noise)
    manta_script += smoke_wavelet_noise;

  // Time
  manta_script += header_time + fluid_time_stepping + fluid_adapt_time_step;
  if (noise) {
    manta_script += fluid_time_stepping_noise;
  }

  // Import
  manta_script += header_import + fluid_file_import + fluid_cache_helper + fluid_load_data +
                  smoke_load_data;
  if (noise)
    manta_script += smoke_load_noise;
  if (guiding)
    manta_script += fluid_load_guiding;

  // Pre/Post Steps
  manta_script += header_prepost + fluid_pre_step + fluid_post_step;

  // Steps
  manta_script += header_steps + smoke_adaptive_step + smoke_step;
  if (noise) {
    manta_script += smoke_step_noise;
  }

  // Main
  manta_script += header_main + smoke_standalone + fluid_standalone;

  // Fill in missing variables in script
  std::string final_script = MANTA::parseScript(manta_script, mmd);

  // Write script
  std::ofstream myfile;
  myfile.open(cacheDirScript);
  myfile << final_script;
  myfile.close();
}

void MANTA::exportLiquidScript(MantaModifierData *mmd)
{
  if (with_debug)
    std::cout << "MANTA::exportLiquidScript()" << std::endl;

  char cacheDirScript[FILE_MAX];
  cacheDirScript[0] = '\0';

  BLI_path_join(cacheDirScript,
                sizeof(cacheDirScript),
                mmd->domain->cache_directory,
                FLUID_DOMAIN_DIR_SCRIPT,
                NULL);
  BLI_path_make_safe(cacheDirScript);
  BLI_dir_create_recursive(cacheDirScript); /* Create 'script' subdir if it does not exist already */
  BLI_path_join(
      cacheDirScript, sizeof(cacheDirScript), cacheDirScript, FLUID_DOMAIN_LIQUID_SCRIPT, NULL);
  BLI_path_make_safe(cacheDirScript);

  bool mesh = mmd->domain->flags & FLUID_DOMAIN_USE_MESH;
  bool drops = mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_SPRAY;
  bool bubble = mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_BUBBLE;
  bool floater = mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_FOAM;
  bool tracer = mmd->domain->particle_type & FLUID_DOMAIN_PARTICLE_TRACER;
  bool obstacle = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_OBSTACLE;
  bool guiding = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_GUIDING;
  bool invel = mmd->domain->active_fields & FLUID_DOMAIN_ACTIVE_INVEL;

  std::string manta_script;

  // Libraries
  manta_script += header_libraries + manta_import;

  // Variables
  manta_script += header_variables + fluid_variables + liquid_variables;
  if (mesh)
    manta_script += fluid_variables_mesh;
  if (drops || bubble || floater || tracer)
    manta_script += fluid_variables_particles + liquid_variables_particles;
  if (guiding)
    manta_script += fluid_variables_guiding;

  // Solvers
  manta_script += header_solvers + fluid_solver;
  if (mesh)
    manta_script += fluid_solver_mesh;
  if (drops || bubble || floater || tracer)
    manta_script += fluid_solver_particles;
  if (guiding)
    manta_script += fluid_solver_guiding;

  // Grids
  manta_script += header_grids + fluid_alloc + liquid_alloc;
  if (mesh)
    manta_script += liquid_alloc_mesh;
  if (drops || bubble || floater || tracer)
    manta_script += fluid_alloc_sndparts + liquid_alloc_particles;
  if (guiding)
    manta_script += fluid_alloc_guiding;
  if (obstacle)
    manta_script += fluid_alloc_obstacle;
  if (invel)
    manta_script += fluid_alloc_invel;

  // Domain init
  manta_script += header_gridinit + liquid_init_phi;

  // Time
  manta_script += header_time + fluid_time_stepping + fluid_adapt_time_step;

  // Import
  manta_script += header_import + fluid_file_import + fluid_cache_helper + fluid_load_data +
                  liquid_load_data + liquid_load_flip;
  if (mesh)
    manta_script += liquid_load_mesh;
  if (drops || bubble || floater || tracer)
    manta_script += fluid_load_particles + liquid_load_particles;
  if (guiding)
    manta_script += fluid_load_guiding;

  // Pre/Post Steps
  manta_script += header_prepost + fluid_pre_step + fluid_post_step;

  // Steps
  manta_script += header_steps + liquid_adaptive_step + liquid_step;
  if (mesh)
    manta_script += liquid_step_mesh;
  if (drops || bubble || floater || tracer)
    manta_script += liquid_step_particles;

  // Main
  manta_script += header_main + liquid_standalone + fluid_standalone;

  // Fill in missing variables in script
  std::string final_script = MANTA::parseScript(manta_script, mmd);

  // Write script
  std::ofstream myfile;
  myfile.open(cacheDirScript);
  myfile << final_script;
  myfile.close();
}

/* Call Mantaflow python functions through this function. Use isAttribute for object attributes, e.g. s.cfl (here 's' is varname, 'cfl' functionName, and isAttribute true) */
static PyObject *callPythonFunction(std::string varName,
                                    std::string functionName,
                                    bool isAttribute = false)
{
  if ((varName == "") || (functionName == "")) {
    if (MANTA::with_debug)
      std::cout << "Missing Python variable name and/or function name -- name is: " << varName
                << ", function name is: " << functionName << std::endl;
    return NULL;
  }

  PyGILState_STATE gilstate = PyGILState_Ensure();
  PyObject *main, *var, *func, *returnedValue;

  // Get pyobject that holds result value
  main = PyImport_ImportModule("__main__");
  var = PyObject_GetAttrString(main, varName.c_str());
  func = PyObject_GetAttrString(var, functionName.c_str());

  Py_DECREF(var);

  if (!isAttribute) {
    returnedValue = PyObject_CallObject(func, NULL);
    Py_DECREF(func);
  }

  PyGILState_Release(gilstate);
  return (!isAttribute) ? returnedValue : func;
}

static char *pyObjectToString(PyObject *inputObject)
{
  PyObject *encoded = PyUnicode_AsUTF8String(inputObject);
  char *result = PyBytes_AsString(encoded);
  Py_DECREF(encoded);
  Py_DECREF(inputObject);
  return result;
}

static double pyObjectToDouble(PyObject *inputObject)
{
  // Cannot use PyFloat_AsDouble() since its error check crashes - likely because of Real (aka float) type in Mantaflow
  return PyFloat_AS_DOUBLE(inputObject);
}

static long pyObjectToLong(PyObject *inputObject)
{
  return PyLong_AsLong(inputObject);
}

static void *stringToPointer(char *inputString)
{
  std::string str(inputString);
  std::istringstream in(str);
  void *dataPointer = NULL;
  in >> dataPointer;
  return dataPointer;
}

int MANTA::getFrame()
{
  if (with_debug)
    std::cout << "MANTA::getFrame()" << std::endl;

  std::string func = "frame";
  std::string id = std::to_string(mCurrentID);
  std::string solver = "s" + id;

  return pyObjectToLong(callPythonFunction(solver, func, true));
}

float MANTA::getTimestep()
{
  if (with_debug)
    std::cout << "MANTA::getTimestep()" << std::endl;

  std::string func = "timestep";
  std::string id = std::to_string(mCurrentID);
  std::string solver = "s" + id;

  return pyObjectToDouble(callPythonFunction(solver, func, true));
}

bool MANTA::needsRealloc(MantaModifierData *mmd)
{
  MantaDomainSettings *mds = mmd->domain;
  return (mds->res[0] != mResX || mds->res[1] != mResY || mds->res[2] != mResZ);
}

void MANTA::adaptTimestep()
{
  if (with_debug)
    std::cout << "MANTA::adaptTimestep()" << std::endl;

  std::vector<std::string> pythonCommands;
  std::ostringstream ss;

  ss << "fluid_adapt_time_step_" << mCurrentID << "()";
  pythonCommands.push_back(ss.str());

  runPythonString(pythonCommands);
}

void MANTA::updateMeshFromFile(const char *filename)
{
  std::string fname(filename);
  std::string::size_type idx;

  idx = fname.rfind('.');
  if (idx != std::string::npos) {
    std::string extension = fname.substr(idx + 1);

    if (extension.compare("gz") == 0)
      updateMeshFromBobj(filename);
    else if (extension.compare("obj") == 0)
      updateMeshFromObj(filename);
    else if (extension.compare("uni") == 0)
      updateMeshFromUni(filename);
    else
      std::cerr << "updateMeshFromFile: invalid file extension in file: " << filename << std::endl;
  }
  else {
    std::cerr << "updateMeshFromFile: unable to open file: " << filename << std::endl;
  }
}

void MANTA::updateMeshFromBobj(const char *filename)
{
  if (with_debug)
    std::cout << "MANTA::updateMeshFromBobj()" << std::endl;

  gzFile gzf;
  float fbuffer[3];
  int ibuffer[3];
  int numBuffer = 0;

  gzf = (gzFile)BLI_gzopen(filename, "rb1");  // do some compression
  if (!gzf)
    std::cerr << "updateMeshData: unable to open file: " << filename << std::endl;

  // Num vertices
  gzread(gzf, &numBuffer, sizeof(int));

  if (with_debug)
    std::cout << "read mesh , num verts: " << numBuffer << " , in file: " << filename << std::endl;

  if (numBuffer) {
    // Vertices
    mMeshNodes->resize(numBuffer);
    for (std::vector<Node>::iterator it = mMeshNodes->begin(); it != mMeshNodes->end(); ++it) {
      gzread(gzf, fbuffer, sizeof(float) * 3);
      it->pos[0] = fbuffer[0];
      it->pos[1] = fbuffer[1];
      it->pos[2] = fbuffer[2];
    }
  }

  // Num normals
  gzread(gzf, &numBuffer, sizeof(int));

  if (with_debug)
    std::cout << "read mesh , num normals : " << numBuffer << " , in file: " << filename
              << std::endl;

  if (numBuffer) {
    // Normals
    if (!getNumVertices())
      mMeshNodes->resize(numBuffer);
    for (std::vector<Node>::iterator it = mMeshNodes->begin(); it != mMeshNodes->end(); ++it) {
      gzread(gzf, fbuffer, sizeof(float) * 3);
      it->normal[0] = fbuffer[0];
      it->normal[1] = fbuffer[1];
      it->normal[2] = fbuffer[2];
    }
  }

  // Num triangles
  gzread(gzf, &numBuffer, sizeof(int));

  if (with_debug)
    std::cout << "read mesh , num triangles : " << numBuffer << " , in file: " << filename
              << std::endl;

  if (numBuffer) {
    // Triangles
    mMeshTriangles->resize(numBuffer);
    MANTA::Triangle *bufferTriangle;
    for (std::vector<Triangle>::iterator it = mMeshTriangles->begin(); it != mMeshTriangles->end();
         ++it) {
      gzread(gzf, ibuffer, sizeof(int) * 3);
      bufferTriangle = (MANTA::Triangle *)ibuffer;
      it->c[0] = bufferTriangle->c[0];
      it->c[1] = bufferTriangle->c[1];
      it->c[2] = bufferTriangle->c[2];
    }
  }
  gzclose(gzf);
}

void MANTA::updateMeshFromObj(const char *filename)
{
  if (with_debug)
    std::cout << "MANTA::updateMeshFromObj()" << std::endl;

  std::ifstream ifs(filename);
  float fbuffer[3];
  int ibuffer[3];
  int cntVerts = 0, cntNormals = 0, cntTris = 0;

  if (!ifs.good())
    std::cerr << "updateMeshDataFromObj: unable to open file: " << filename << std::endl;

  while (ifs.good() && !ifs.eof()) {
    std::string id;
    ifs >> id;

    if (id[0] == '#') {
      // comment
      getline(ifs, id);
      continue;
    }
    if (id == "vt") {
      // tex coord, ignore
    }
    else if (id == "vn") {
      // normals
      if (getNumVertices() != cntVerts)
        std::cerr << "updateMeshDataFromObj: invalid amount of mesh nodes" << std::endl;

      ifs >> fbuffer[0] >> fbuffer[1] >> fbuffer[2];
      MANTA::Node *node = &mMeshNodes->at(cntNormals);
      (*node).normal[0] = fbuffer[0];
      (*node).normal[1] = fbuffer[1];
      (*node).normal[2] = fbuffer[2];
      cntNormals++;
    }
    else if (id == "v") {
      // vertex
      ifs >> fbuffer[0] >> fbuffer[1] >> fbuffer[2];
      MANTA::Node node;
      node.pos[0] = fbuffer[0];
      node.pos[1] = fbuffer[1];
      node.pos[2] = fbuffer[2];
      mMeshNodes->push_back(node);
      cntVerts++;
    }
    else if (id == "g") {
      // group
      std::string group;
      ifs >> group;
    }
    else if (id == "f") {
      // face
      std::string face;
      for (int i = 0; i < 3; i++) {
        ifs >> face;
        if (face.find('/') != std::string::npos)
          face = face.substr(0, face.find('/'));  // ignore other indices
        int idx = atoi(face.c_str()) - 1;
        if (idx < 0)
          std::cerr << "updateMeshDataFromObj: invalid face encountered" << std::endl;
        ibuffer[i] = idx;
      }
      MANTA::Triangle triangle;
      triangle.c[0] = ibuffer[0];
      triangle.c[1] = ibuffer[1];
      triangle.c[2] = ibuffer[2];
      mMeshTriangles->push_back(triangle);
      cntTris++;
    }
    else {
      // whatever, ignore
    }
    // kill rest of line
    getline(ifs, id);
  }
  ifs.close();
}

void MANTA::updateMeshFromUni(const char *filename)
{
  if (with_debug)
    std::cout << "MANTA::updateMeshFromUni()" << std::endl;

  gzFile gzf;
  float fbuffer[4];
  int ibuffer[4];

  gzf = (gzFile)BLI_gzopen(filename, "rb1");  // do some compression
  if (!gzf)
    std::cout << "updateMeshFromUni: unable to open file" << std::endl;

  char ID[5] = {0, 0, 0, 0, 0};
  gzread(gzf, ID, 4);

  std::vector<pVel> *velocityPointer = mMeshVelocities;

  // mdata uni header
  const int STR_LEN_PDATA = 256;
  int elementType, bytesPerElement, numParticles;
  char info[STR_LEN_PDATA];      // mantaflow build information
  unsigned long long timestamp;  // creation time

  // read mesh header
  gzread(gzf, &ibuffer, sizeof(int) * 4);  // num particles, dimX, dimY, dimZ
  gzread(gzf, &elementType, sizeof(int));
  gzread(gzf, &bytesPerElement, sizeof(int));
  gzread(gzf, &info, sizeof(info));
  gzread(gzf, &timestamp, sizeof(unsigned long long));

  if (with_debug)
    std::cout << "read " << ibuffer[0] << " vertices in file: " << filename << std::endl;

  // Sanity checks
  const int meshSize = sizeof(float) * 3 + sizeof(int);
  if (!(bytesPerElement == meshSize) && (elementType == 0)) {
    std::cout << "particle type doesn't match" << std::endl;
  }
  if (!ibuffer[0]) {  // Any vertices present?
    if (with_debug)
      std::cout << "no vertices present yet" << std::endl;
    return;
  }

  // Reading mesh
  if (!strcmp(ID, "MB01")) {
    // TODO (sebbas): Future update could add uni mesh support
  }
  // Reading mesh data file v1 with vec3
  else if (!strcmp(ID, "MD01")) {
    numParticles = ibuffer[0];

    velocityPointer->resize(numParticles);
    MANTA::pVel *bufferPVel;
    for (std::vector<pVel>::iterator it = velocityPointer->begin(); it != velocityPointer->end();
         ++it) {
      gzread(gzf, fbuffer, sizeof(float) * 3);
      bufferPVel = (MANTA::pVel *)fbuffer;
      it->pos[0] = bufferPVel->pos[0];
      it->pos[1] = bufferPVel->pos[1];
      it->pos[2] = bufferPVel->pos[2];
    }
  }

  gzclose(gzf);
}

void MANTA::updateParticlesFromFile(const char *filename, bool isSecondarySys, bool isVelData)
{
  if (with_debug)
    std::cout << "MANTA::updateParticlesFromFile()" << std::endl;

  std::string fname(filename);
  std::string::size_type idx;

  idx = fname.rfind('.');
  if (idx != std::string::npos) {
    std::string extension = fname.substr(idx + 1);

    if (extension.compare("uni") == 0)
      updateParticlesFromUni(filename, isSecondarySys, isVelData);
    else
      std::cerr << "updateParticlesFromFile: invalid file extension in file: " << filename
                << std::endl;
  }
  else {
    std::cerr << "updateParticlesFromFile: unable to open file: " << filename << std::endl;
  }
}

void MANTA::updateParticlesFromUni(const char *filename, bool isSecondarySys, bool isVelData)
{
  if (with_debug)
    std::cout << "MANTA::updateParticlesFromUni()" << std::endl;

  gzFile gzf;
  float fbuffer[4];
  int ibuffer[4];

  gzf = (gzFile)BLI_gzopen(filename, "rb1");  // do some compression
  if (!gzf)
    std::cout << "updateParticlesFromUni: unable to open file" << std::endl;

  char ID[5] = {0, 0, 0, 0, 0};
  gzread(gzf, ID, 4);

  if (!strcmp(ID, "PB01")) {
    std::cout << "particle uni file format v01 not supported anymore" << std::endl;
    return;
  }

  // Pointer to FLIP system or to secondary particle system
  std::vector<pData> *dataPointer = NULL;
  std::vector<pVel> *velocityPointer = NULL;
  std::vector<float> *lifePointer = NULL;

  if (isSecondarySys) {
    dataPointer = mSndParticleData;
    velocityPointer = mSndParticleVelocity;
    lifePointer = mSndParticleLife;
  }
  else {
    dataPointer = mFlipParticleData;
    velocityPointer = mFlipParticleVelocity;
  }

  // pdata uni header
  const int STR_LEN_PDATA = 256;
  int elementType, bytesPerElement, numParticles;
  char info[STR_LEN_PDATA];      // mantaflow build information
  unsigned long long timestamp;  // creation time

  // read particle header
  gzread(gzf, &ibuffer, sizeof(int) * 4);  // num particles, dimX, dimY, dimZ
  gzread(gzf, &elementType, sizeof(int));
  gzread(gzf, &bytesPerElement, sizeof(int));
  gzread(gzf, &info, sizeof(info));
  gzread(gzf, &timestamp, sizeof(unsigned long long));

  if (with_debug)
    std::cout << "read " << ibuffer[0] << " particles in file: " << filename << std::endl;

  // Sanity checks
  const int partSysSize = sizeof(float) * 3 + sizeof(int);
  if (!(bytesPerElement == partSysSize) && (elementType == 0)) {
    std::cout << "particle type doesn't match" << std::endl;
  }
  if (!ibuffer[0]) {  // Any particles present?
    if (with_debug)
      std::cout << "no particles present yet" << std::endl;
    return;
  }

  numParticles = ibuffer[0];

  // Reading base particle system file v2
  if (!strcmp(ID, "PB02")) {
    dataPointer->resize(numParticles);
    MANTA::pData *bufferPData;
    for (std::vector<pData>::iterator it = dataPointer->begin(); it != dataPointer->end(); ++it) {
      gzread(gzf, fbuffer, sizeof(float) * 3 + sizeof(int));
      bufferPData = (MANTA::pData *)fbuffer;
      it->pos[0] = bufferPData->pos[0];
      it->pos[1] = bufferPData->pos[1];
      it->pos[2] = bufferPData->pos[2];
      it->flag = bufferPData->flag;
    }
  }
  // Reading particle data file v1 with velocities
  else if (!strcmp(ID, "PD01") && isVelData) {
    velocityPointer->resize(numParticles);
    MANTA::pVel *bufferPVel;
    for (std::vector<pVel>::iterator it = velocityPointer->begin(); it != velocityPointer->end();
         ++it) {
      gzread(gzf, fbuffer, sizeof(float) * 3);
      bufferPVel = (MANTA::pVel *)fbuffer;
      it->pos[0] = bufferPVel->pos[0];
      it->pos[1] = bufferPVel->pos[1];
      it->pos[2] = bufferPVel->pos[2];
    }
  }
  // Reading particle data file v1 with lifetime
  else if (!strcmp(ID, "PD01")) {
    lifePointer->resize(numParticles);
    float *bufferPLife;
    for (std::vector<float>::iterator it = lifePointer->begin(); it != lifePointer->end(); ++it) {
      gzread(gzf, fbuffer, sizeof(float));
      bufferPLife = (float *)fbuffer;
      *it = *bufferPLife;
    }
  }

  gzclose(gzf);
}

void MANTA::updatePointers()
{
  if (with_debug)
    std::cout << "MANTA::updatePointers()" << std::endl;

  std::string func = "getDataPointer";
  std::string funcNodes = "getNodesDataPointer";
  std::string funcTris = "getTrisDataPointer";

  std::string id = std::to_string(mCurrentID);
  std::string solver = "s" + id;
  std::string parts = "pp" + id;
  std::string snd = "sp" + id;
  std::string mesh = "sm" + id;
  std::string mesh2 = "mesh" + id;
  std::string solver_ext = "_" + solver;
  std::string parts_ext = "_" + parts;
  std::string snd_ext = "_" + snd;
  std::string mesh_ext = "_" + mesh;
  std::string mesh_ext2 = "_" + mesh2;

  mObstacle = (int *)stringToPointer(
      pyObjectToString(callPythonFunction("flags" + solver_ext, func)));
  mPhiIn = (float *)stringToPointer(
      pyObjectToString(callPythonFunction("phiIn" + solver_ext, func)));

  mVelocityX = (float *)stringToPointer(
      pyObjectToString(callPythonFunction("x_vel" + solver_ext, func)));
  mVelocityY = (float *)stringToPointer(
      pyObjectToString(callPythonFunction("y_vel" + solver_ext, func)));
  mVelocityZ = (float *)stringToPointer(
      pyObjectToString(callPythonFunction("z_vel" + solver_ext, func)));

  mForceX = (float *)stringToPointer(
      pyObjectToString(callPythonFunction("x_force" + solver_ext, func)));
  mForceY = (float *)stringToPointer(
      pyObjectToString(callPythonFunction("y_force" + solver_ext, func)));
  mForceZ = (float *)stringToPointer(
      pyObjectToString(callPythonFunction("z_force" + solver_ext, func)));

  if (mUsingOutflow) {
    mPhiOutIn = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("phiOutIn" + solver_ext, func)));
  }

  if (mUsingObstacle) {
    mPhiObsIn = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("phiObsIn" + solver_ext, func)));
    mNumObstacle = (int *)stringToPointer(
        pyObjectToString(callPythonFunction("numObs" + solver_ext, func)));

    mObVelocityX = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("x_obvel" + solver_ext, func)));
    mObVelocityY = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("y_obvel" + solver_ext, func)));
    mObVelocityZ = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("z_obvel" + solver_ext, func)));
  }

  if (mUsingGuiding) {
    mPhiGuideIn = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("phiGuideIn" + solver_ext, func)));
    mNumGuide = (int *)stringToPointer(
        pyObjectToString(callPythonFunction("numGuides" + solver_ext, func)));

    mGuideVelocityX = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("x_guidevel" + solver_ext, func)));
    mGuideVelocityY = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("y_guidevel" + solver_ext, func)));
    mGuideVelocityZ = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("z_guidevel" + solver_ext, func)));
  }

  if (mUsingInvel) {
    mInVelocityX = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("x_invel" + solver_ext, func)));
    mInVelocityY = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("y_invel" + solver_ext, func)));
    mInVelocityZ = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("z_invel" + solver_ext, func)));
  }

  // Liquid
  if (mUsingLiquid) {
    mPhi = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("phi" + solver_ext, func)));

    mFlipParticleData = (std::vector<pData> *)stringToPointer(
        pyObjectToString(callPythonFunction("pp" + solver_ext, func)));
    mFlipParticleVelocity = (std::vector<pVel> *)stringToPointer(
        pyObjectToString(callPythonFunction("pVel" + parts_ext, func)));

    if (mUsingMesh) {
      mMeshNodes = (std::vector<Node> *)stringToPointer(
          pyObjectToString(callPythonFunction("mesh" + mesh_ext, funcNodes)));
      mMeshTriangles = (std::vector<Triangle> *)stringToPointer(
          pyObjectToString(callPythonFunction("mesh" + mesh_ext, funcTris)));
      if (mUsingMVel)
        mMeshVelocities = (std::vector<pVel> *)stringToPointer(
            pyObjectToString(callPythonFunction("mVel" + mesh_ext2, func)));
    }

    if (mUsingDrops || mUsingBubbles || mUsingFloats || mUsingTracers) {
      mSndParticleData = (std::vector<pData> *)stringToPointer(
          pyObjectToString(callPythonFunction("ppSnd" + snd_ext, func)));
      mSndParticleVelocity = (std::vector<pVel> *)stringToPointer(
          pyObjectToString(callPythonFunction("pVelSnd" + parts_ext, func)));
      mSndParticleLife = (std::vector<float> *)stringToPointer(
          pyObjectToString(callPythonFunction("pLifeSnd" + parts_ext, func)));
    }
  }

  // Smoke
  if (mUsingSmoke) {
    mDensity = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("density" + solver_ext, func)));
    mDensityIn = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("densityIn" + solver_ext, func)));
    mShadow = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("shadow" + solver_ext, func)));
    mEmissionIn = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("emissionIn" + solver_ext, func)));

    if (mUsingHeat) {
      mHeat = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("heat" + solver_ext, func)));
      mHeatIn = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("heatIn" + solver_ext, func)));
    }
    if (mUsingFire) {
      mFlame = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("flame" + solver_ext, func)));
      mFuel = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("fuel" + solver_ext, func)));
      mReact = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("react" + solver_ext, func)));

      mFuelIn = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("fuelIn" + solver_ext, func)));
      mReactIn = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("reactIn" + solver_ext, func)));
    }
    if (mUsingColors) {
      mColorR = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("color_r" + solver_ext, func)));
      mColorG = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("color_g" + solver_ext, func)));
      mColorB = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("color_b" + solver_ext, func)));

      mColorRIn = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("color_r_in" + solver_ext, func)));
      mColorGIn = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("color_g_in" + solver_ext, func)));
      mColorBIn = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("color_b_in" + solver_ext, func)));
    }
  }
}

void MANTA::updatePointersNoise()
{
  if (with_debug)
    std::cout << "MANTA::updatePointersHigh()" << std::endl;

  std::string func = "getDataPointer";
  std::string id = std::to_string(mCurrentID);
  std::string solver = "s" + id;
  std::string solver_ext = "_" + solver;

  std::string noise = "sn" + id;
  std::string noise_ext = "_" + noise;

  // Liquid
  if (mUsingLiquid) {
    // Nothing to do here
  }

  // Smoke
  if (mUsingSmoke) {
    mDensityHigh = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("density" + noise_ext, func)));
    mShadow = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("shadow" + solver_ext, func)));
    mTextureU = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("texture_u" + solver_ext, func)));
    mTextureV = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("texture_v" + solver_ext, func)));
    mTextureW = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("texture_w" + solver_ext, func)));
    mTextureU2 = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("texture_u2" + solver_ext, func)));
    mTextureV2 = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("texture_v2" + solver_ext, func)));
    mTextureW2 = (float *)stringToPointer(
        pyObjectToString(callPythonFunction("texture_w2" + solver_ext, func)));

    if (mUsingFire) {
      mFlameHigh = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("flame" + noise_ext, func)));
      mFuelHigh = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("fuel" + noise_ext, func)));
      mReactHigh = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("react" + noise_ext, func)));
    }
    if (mUsingColors) {
      mColorRHigh = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("color_r" + noise_ext, func)));
      mColorGHigh = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("color_g" + noise_ext, func)));
      mColorBHigh = (float *)stringToPointer(
          pyObjectToString(callPythonFunction("color_b" + noise_ext, func)));
    }
  }
}

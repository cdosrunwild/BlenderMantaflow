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

/** \file mantaflow/intern/MANTA.h
 *  \ingroup mantaflow
 */

#ifndef MANTA_A_H
#define MANTA_A_H

#include <string>
#include <vector>
#include <atomic>

struct MANTA {
 public:
  MANTA(int *res, struct MantaModifierData *mmd);
  MANTA(){};
  virtual ~MANTA();

  // Mirroring Mantaflow structures for particle data (pVel also used for mesh vert vels)
  typedef struct pData {
    float pos[3];
    int flag;
  } pData;
  typedef struct pVel {
    float pos[3];
  } pVel;

  // Mirroring Mantaflow structures for meshes
  typedef struct Node {
    int flags;
    float pos[3], normal[3];
  } Node;
  typedef struct Triangle {
    int c[3];
    int flags;
  } Triangle;

  // Manta step, handling everything
  void step(struct MantaModifierData *mmd, int startFrame);

  // Grid initialization functions
  void initHeat(struct MantaModifierData *mmd);
  void initFire(struct MantaModifierData *mmd);
  void initColors(struct MantaModifierData *mmd);
  void initFireHigh(struct MantaModifierData *mmd);
  void initColorsHigh(struct MantaModifierData *mmd);
  void initLiquid(MantaModifierData *mmd);
  void initLiquidMesh(MantaModifierData *mmd);
  void initObstacle(MantaModifierData *mmd);
  void initGuiding(MantaModifierData *mmd);
  void initInVelocity(MantaModifierData *mmd);
  void initOutflow(MantaModifierData *mmd);
  void initSndParts(MantaModifierData *mmd);
  void initLiquidSndParts(MantaModifierData *mmd);

  // Pointer transfer: Mantaflow -> Blender
  void updatePointers();
  void updatePointersNoise();

  // Write cache
  int writeConfiguration(MantaModifierData *mmd, int framenr);
  int writeData(MantaModifierData *mmd, int framenr);
  // write call for noise, mesh and particles were left in bake calls for now

  // Read cache (via Manta save/load)
  int readConfiguration(MantaModifierData *mmd, int framenr);
  int readData(MantaModifierData *mmd, int framenr);
  int readNoise(MantaModifierData *mmd, int framenr);
  int readMesh(MantaModifierData *mmd, int framenr);
  int readParticles(MantaModifierData *mmd, int framenr);
  int readGuiding(MantaModifierData *mmd, int framenr, bool sourceDomain);

  // Read cache (via file read functions in MANTA - e.g. read .bobj.gz meshes, .uni particles)
  int updateMeshStructures(MantaModifierData *mmd, int framenr);
  int updateFlipStructures(MantaModifierData *mmd, int framenr);
  int updateParticleStructures(MantaModifierData *mmd, int framenr);
  void updateVariables(MantaModifierData *mmd);

  // Bake cache
  int bakeData(MantaModifierData *mmd, int framenr);
  int bakeNoise(MantaModifierData *mmd, int framenr);
  int bakeMesh(MantaModifierData *mmd, int framenr);
  int bakeParticles(MantaModifierData *mmd, int framenr);
  int bakeGuiding(MantaModifierData *mmd, int framenr);

  // IO for Mantaflow scene script
  void exportSmokeScript(struct MantaModifierData *mmd);
  void exportLiquidScript(struct MantaModifierData *mmd);

  inline size_t getTotalCells()
  {
    return mTotalCells;
  }
  inline size_t getTotalCellsHigh()
  {
    return mTotalCellsHigh;
  }
  inline bool usingNoise()
  {
    return mUsingNoise;
  }
  inline int getResX()
  {
    return mResX;
  }
  inline int getResY()
  {
    return mResY;
  }
  inline int getResZ()
  {
    return mResZ;
  }
  inline int getParticleResX()
  {
    return mResXParticle;
  }
  inline int getParticleResY()
  {
    return mResYParticle;
  }
  inline int getParticleResZ()
  {
    return mResZParticle;
  }
  inline int getMeshResX()
  {
    return mResXMesh;
  }
  inline int getMeshResY()
  {
    return mResYMesh;
  }
  inline int getMeshResZ()
  {
    return mResZMesh;
  }
  inline int getResXHigh()
  {
    return mResXNoise;
  }
  inline int getResYHigh()
  {
    return mResYNoise;
  }
  inline int getResZHigh()
  {
    return mResZNoise;
  }
  inline int getMeshUpres()
  {
    return mUpresMesh;
  }
  inline int getParticleUpres()
  {
    return mUpresParticle;
  }

  // Smoke getters
  inline float *getDensity()
  {
    return mDensity;
  }
  inline float *getHeat()
  {
    return mHeat;
  }
  inline float *getVelocityX()
  {
    return mVelocityX;
  }
  inline float *getVelocityY()
  {
    return mVelocityY;
  }
  inline float *getVelocityZ()
  {
    return mVelocityZ;
  }
  inline float *getObVelocityX()
  {
    return mObVelocityX;
  }
  inline float *getObVelocityY()
  {
    return mObVelocityY;
  }
  inline float *getObVelocityZ()
  {
    return mObVelocityZ;
  }
  inline float *getGuideVelocityX()
  {
    return mGuideVelocityX;
  }
  inline float *getGuideVelocityY()
  {
    return mGuideVelocityY;
  }
  inline float *getGuideVelocityZ()
  {
    return mGuideVelocityZ;
  }
  inline float *getInVelocityX()
  {
    return mInVelocityX;
  }
  inline float *getInVelocityY()
  {
    return mInVelocityY;
  }
  inline float *getInVelocityZ()
  {
    return mInVelocityZ;
  }
  inline float *getForceX()
  {
    return mForceX;
  }
  inline float *getForceY()
  {
    return mForceY;
  }
  inline float *getForceZ()
  {
    return mForceZ;
  }
  inline int *getObstacle()
  {
    return mObstacle;
  }
  inline int *getNumObstacle()
  {
    return mNumObstacle;
  }
  inline int *getNumGuide()
  {
    return mNumGuide;
  }
  inline float *getFlame()
  {
    return mFlame;
  }
  inline float *getFuel()
  {
    return mFuel;
  }
  inline float *getReact()
  {
    return mReact;
  }
  inline float *getColorR()
  {
    return mColorR;
  }
  inline float *getColorG()
  {
    return mColorG;
  }
  inline float *getColorB()
  {
    return mColorB;
  }
  inline float *getShadow()
  {
    return mShadow;
  }
  inline float *getDensityIn()
  {
    return mDensityIn;
  }
  inline float *getHeatIn()
  {
    return mHeatIn;
  }
  inline float *getColorRIn()
  {
    return mColorRIn;
  }
  inline float *getColorGIn()
  {
    return mColorGIn;
  }
  inline float *getColorBIn()
  {
    return mColorBIn;
  }
  inline float *getFuelIn()
  {
    return mFuelIn;
  }
  inline float *getReactIn()
  {
    return mReactIn;
  }
  inline float *getEmissionIn()
  {
    return mEmissionIn;
  }

  inline float *getDensityHigh()
  {
    return mDensityHigh;
  }
  inline float *getFlameHigh()
  {
    return mFlameHigh;
  }
  inline float *getFuelHigh()
  {
    return mFuelHigh;
  }
  inline float *getReactHigh()
  {
    return mReactHigh;
  }
  inline float *getColorRHigh()
  {
    return mColorRHigh;
  }
  inline float *getColorGHigh()
  {
    return mColorGHigh;
  }
  inline float *getColorBHigh()
  {
    return mColorBHigh;
  }
  inline float *getTextureU()
  {
    return mTextureU;
  }
  inline float *getTextureV()
  {
    return mTextureV;
  }
  inline float *getTextureW()
  {
    return mTextureW;
  }
  inline float *getTextureU2()
  {
    return mTextureU2;
  }
  inline float *getTextureV2()
  {
    return mTextureV2;
  }
  inline float *getTextureW2()
  {
    return mTextureW2;
  }

  inline float *getPhiIn()
  {
    return mPhiIn;
  }
  inline float *getPhiObsIn()
  {
    return mPhiObsIn;
  }
  inline float *getPhiGuideIn()
  {
    return mPhiGuideIn;
  }
  inline float *getPhiOutIn()
  {
    return mPhiOutIn;
  }
  inline float *getPhi()
  {
    return mPhi;
  }

  static std::atomic<bool> mantaInitialized;
  static std::atomic<int> solverID;
  static int with_debug;  // on or off (1 or 0), also sets manta debug level

  // Mesh getters
  inline int getNumVertices()
  {
    return (mMeshNodes && !mMeshNodes->empty()) ? mMeshNodes->size() : 0;
  }
  inline int getNumNormals()
  {
    return (mMeshNodes && !mMeshNodes->empty()) ? mMeshNodes->size() : 0;
  }
  inline int getNumTriangles()
  {
    return (mMeshTriangles && !mMeshTriangles->empty()) ? mMeshTriangles->size() : 0;
  }

  inline float getVertexXAt(int i)
  {
    return (mMeshNodes && !mMeshNodes->empty() && mMeshNodes->size() > i) ?
               mMeshNodes->at(i).pos[0] :
               0.f;
  }
  inline float getVertexYAt(int i)
  {
    return (mMeshNodes && !mMeshNodes->empty() && mMeshNodes->size() > i) ?
               mMeshNodes->at(i).pos[1] :
               0.f;
  }
  inline float getVertexZAt(int i)
  {
    return (mMeshNodes && !mMeshNodes->empty() && mMeshNodes->size() > i) ?
               mMeshNodes->at(i).pos[2] :
               0.f;
  }

  inline float getNormalXAt(int i)
  {
    return (mMeshNodes && !mMeshNodes->empty() && mMeshNodes->size() > i) ?
               mMeshNodes->at(i).normal[0] :
               0.f;
  }
  inline float getNormalYAt(int i)
  {
    return (mMeshNodes && !mMeshNodes->empty() && mMeshNodes->size() > i) ?
               mMeshNodes->at(i).normal[1] :
               0.f;
  }
  inline float getNormalZAt(int i)
  {
    return (mMeshNodes && !mMeshNodes->empty() && mMeshNodes->size() > i) ?
               mMeshNodes->at(i).normal[2] :
               0.f;
  }

  inline int getTriangleXAt(int i)
  {
    return (mMeshTriangles && !mMeshTriangles->empty() && mMeshTriangles->size() > i) ?
               mMeshTriangles->at(i).c[0] :
               0;
  }
  inline int getTriangleYAt(int i)
  {
    return (mMeshTriangles && !mMeshTriangles->empty() && mMeshTriangles->size() > i) ?
               mMeshTriangles->at(i).c[1] :
               0;
  }
  inline int getTriangleZAt(int i)
  {
    return (mMeshTriangles && !mMeshTriangles->empty() && mMeshTriangles->size() > i) ?
               mMeshTriangles->at(i).c[2] :
               0;
  }

  inline float getVertVelXAt(int i)
  {
    return (mMeshVelocities && !mMeshVelocities->empty() && mMeshVelocities->size() > i) ?
               mMeshVelocities->at(i).pos[0] :
               0.f;
  }
  inline float getVertVelYAt(int i)
  {
    return (mMeshVelocities && !mMeshVelocities->empty() && mMeshVelocities->size() > i) ?
               mMeshVelocities->at(i).pos[1] :
               0.f;
  }
  inline float getVertVelZAt(int i)
  {
    return (mMeshVelocities && !mMeshVelocities->empty() && mMeshVelocities->size() > i) ?
               mMeshVelocities->at(i).pos[2] :
               0.f;
  }

  // Particle getters
  inline int getFlipParticleFlagAt(int i)
  {
    return (mFlipParticleData && !mFlipParticleData->empty() && mFlipParticleData->size() > i) ?
               ((std::vector<pData> *)mFlipParticleData)->at(i).flag :
               0;
  }
  inline int getSndParticleFlagAt(int i)
  {
    return (mSndParticleData && !mSndParticleData->empty() && mSndParticleData->size() > i) ?
               ((std::vector<pData> *)mSndParticleData)->at(i).flag :
               0;
  }

  inline float getFlipParticlePositionXAt(int i)
  {
    return (mFlipParticleData && !mFlipParticleData->empty() && mFlipParticleData->size() > i) ?
               mFlipParticleData->at(i).pos[0] :
               0.f;
  }
  inline float getFlipParticlePositionYAt(int i)
  {
    return (mFlipParticleData && !mFlipParticleData->empty() && mFlipParticleData->size() > i) ?
               mFlipParticleData->at(i).pos[1] :
               0.f;
  }
  inline float getFlipParticlePositionZAt(int i)
  {
    return (mFlipParticleData && !mFlipParticleData->empty() && mFlipParticleData->size() > i) ?
               mFlipParticleData->at(i).pos[2] :
               0.f;
  }

  inline float getSndParticlePositionXAt(int i)
  {
    return (mSndParticleData && !mSndParticleData->empty() && mSndParticleData->size() > i) ?
               mSndParticleData->at(i).pos[0] :
               0.f;
  }
  inline float getSndParticlePositionYAt(int i)
  {
    return (mSndParticleData && !mSndParticleData->empty() && mSndParticleData->size() > i) ?
               mSndParticleData->at(i).pos[1] :
               0.f;
  }
  inline float getSndParticlePositionZAt(int i)
  {
    return (mSndParticleData && !mSndParticleData->empty() && mSndParticleData->size() > i) ?
               mSndParticleData->at(i).pos[2] :
               0.f;
  }

  inline float getFlipParticleVelocityXAt(int i)
  {
    return (mFlipParticleVelocity && !mFlipParticleVelocity->empty() &&
            mFlipParticleVelocity->size() > i) ?
               mFlipParticleVelocity->at(i).pos[0] :
               0.f;
  }
  inline float getFlipParticleVelocityYAt(int i)
  {
    return (mFlipParticleVelocity && !mFlipParticleVelocity->empty() &&
            mFlipParticleVelocity->size() > i) ?
               mFlipParticleVelocity->at(i).pos[1] :
               0.f;
  }
  inline float getFlipParticleVelocityZAt(int i)
  {
    return (mFlipParticleVelocity && !mFlipParticleVelocity->empty() &&
            mFlipParticleVelocity->size() > i) ?
               mFlipParticleVelocity->at(i).pos[2] :
               0.f;
  }

  inline float getSndParticleVelocityXAt(int i)
  {
    return (mSndParticleVelocity && !mSndParticleVelocity->empty() &&
            mSndParticleVelocity->size() > i) ?
               mSndParticleVelocity->at(i).pos[0] :
               0.f;
  }
  inline float getSndParticleVelocityYAt(int i)
  {
    return (mSndParticleVelocity && !mSndParticleVelocity->empty() &&
            mSndParticleVelocity->size() > i) ?
               mSndParticleVelocity->at(i).pos[1] :
               0.f;
  }
  inline float getSndParticleVelocityZAt(int i)
  {
    return (mSndParticleVelocity && !mSndParticleVelocity->empty() &&
            mSndParticleVelocity->size() > i) ?
               mSndParticleVelocity->at(i).pos[2] :
               0.f;
  }

  inline float *getFlipParticleData()
  {
    return (mFlipParticleData && !mFlipParticleData->empty()) ?
               (float *)&mFlipParticleData->front() :
               NULL;
  }
  inline float *getSndParticleData()
  {
    return (mSndParticleData && !mSndParticleData->empty()) ? (float *)&mSndParticleData->front() :
                                                              NULL;
  }

  inline float *getFlipParticleVelocity()
  {
    return (mFlipParticleVelocity && !mFlipParticleVelocity->empty()) ?
               (float *)&mFlipParticleVelocity->front() :
               NULL;
  }
  inline float *getSndParticleVelocity()
  {
    return (mSndParticleVelocity && !mSndParticleVelocity->empty()) ?
               (float *)&mSndParticleVelocity->front() :
               NULL;
  }
  inline float *getSndParticleLife()
  {
    return (mSndParticleLife && !mSndParticleLife->empty()) ? (float *)&mSndParticleLife->front() :
                                                              NULL;
  }

  inline int getNumFlipParticles()
  {
    return (mFlipParticleData && !mFlipParticleData->empty()) ? mFlipParticleData->size() : 0;
  }
  inline int getNumSndParticles()
  {
    return (mSndParticleData && !mSndParticleData->empty()) ? mSndParticleData->size() : 0;
  }

  // Direct access to solver time attributes
  int getFrame();
  float getTimestep();
  void adaptTimestep();

  bool needsRealloc(MantaModifierData *mmd);

 private:
  // simulation constants
  size_t mTotalCells;
  size_t mTotalCellsHigh;
  size_t mTotalCellsMesh;
  size_t mTotalCellsParticles;

  int mCurrentID;

  bool mUsingHeat;
  bool mUsingColors;
  bool mUsingFire;
  bool mUsingObstacle;
  bool mUsingGuiding;
  bool mUsingInvel;
  bool mUsingOutflow;
  bool mUsingNoise;
  bool mUsingMesh;
  bool mUsingMVel;
  bool mUsingLiquid;
  bool mUsingSmoke;
  bool mUsingDrops;
  bool mUsingBubbles;
  bool mUsingFloats;
  bool mUsingTracers;

  int mResX;
  int mResY;
  int mResZ;
  int mMaxRes;

  int mResXNoise;
  int mResYNoise;
  int mResZNoise;
  int mResXMesh;
  int mResYMesh;
  int mResZMesh;
  int mResXParticle;
  int mResYParticle;
  int mResZParticle;
  int *mResGuiding;

  int mUpresMesh;
  int mUpresParticle;

  float mTempAmb; /* ambient temperature */
  float mConstantScaling;

  // Fluid grids
  float *mVelocityX;
  float *mVelocityY;
  float *mVelocityZ;
  float *mObVelocityX;
  float *mObVelocityY;
  float *mObVelocityZ;
  float *mGuideVelocityX;
  float *mGuideVelocityY;
  float *mGuideVelocityZ;
  float *mInVelocityX;
  float *mInVelocityY;
  float *mInVelocityZ;
  float *mForceX;
  float *mForceY;
  float *mForceZ;
  int *mObstacle;
  int *mNumObstacle;
  int *mNumGuide;

  // Smoke grids
  float *mDensity;
  float *mHeat;
  float *mFlame;
  float *mFuel;
  float *mReact;
  float *mColorR;
  float *mColorG;
  float *mColorB;
  float *mShadow;
  float *mDensityIn;
  float *mHeatIn;
  float *mFuelIn;
  float *mReactIn;
  float *mEmissionIn;
  float *mColorRIn;
  float *mColorGIn;
  float *mColorBIn;
  float *mDensityHigh;
  float *mFlameHigh;
  float *mFuelHigh;
  float *mReactHigh;
  float *mColorRHigh;
  float *mColorGHigh;
  float *mColorBHigh;
  float *mTextureU;
  float *mTextureV;
  float *mTextureW;
  float *mTextureU2;
  float *mTextureV2;
  float *mTextureW2;

  // Liquid grids
  float *mPhiIn;
  float *mPhiObsIn;
  float *mPhiGuideIn;
  float *mPhiOutIn;
  float *mPhi;

  // Mesh fields
  std::vector<Node> *mMeshNodes;
  std::vector<Triangle> *mMeshTriangles;
  std::vector<pVel> *mMeshVelocities;

  // Particle fields
  std::vector<pData> *mFlipParticleData;
  std::vector<pVel> *mFlipParticleVelocity;

  std::vector<pData> *mSndParticleData;
  std::vector<pVel> *mSndParticleVelocity;
  std::vector<float> *mSndParticleLife;

  void initDomain(struct MantaModifierData *mmd);
  void initNoise(struct MantaModifierData *mmd);
  void initMesh(struct MantaModifierData *mmd);
  void initSmoke(struct MantaModifierData *mmd);
  void initSmokeNoise(struct MantaModifierData *mmd);
  void initializeMantaflow();
  void terminateMantaflow();
  void runPythonString(std::vector<std::string> commands);
  std::string getRealValue(const std::string &varName, MantaModifierData *mmd);
  std::string parseLine(const std::string &line, MantaModifierData *mmd);
  std::string parseScript(const std::string &setup_string, MantaModifierData *mmd = NULL);
  void updateMeshFromBobj(const char *filename);
  void updateMeshFromObj(const char *filename);
  void updateMeshFromUni(const char *filename);
  void updateParticlesFromUni(const char *filename, bool isSecondarySys, bool isVelData);
  void updateMeshFromFile(const char *filename);
  void updateParticlesFromFile(const char *filename, bool isSecondarySys, bool isVelData);
};

#endif

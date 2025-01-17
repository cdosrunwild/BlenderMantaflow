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
 * The Original Code is Copyright (C) Blender Foundation
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Sebastian Barschkis (sebbas)
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/physics/physics_manta.c
 *  \ingroup edphys
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "MEM_guardedalloc.h"

/* types */
#include "DNA_action_types.h"
#include "DNA_object_types.h"

#include "BLI_blenlib.h"
#include "BLI_path_util.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "BKE_context.h"
#include "BKE_customdata.h"
#include "BKE_main.h"
#include "BKE_modifier.h"
#include "BKE_object.h"
#include "BKE_report.h"
#include "BKE_scene.h"
#include "BKE_screen.h"
#include "BKE_manta.h"
#include "BKE_global.h"

#include "DEG_depsgraph.h"

#include "ED_screen.h"
#include "PIL_time.h"

#include "WM_types.h"
#include "WM_api.h"

#include "physics_intern.h"  // own include
#include "manta_fluid_API.h"

#include "DNA_scene_types.h"
#include "DNA_manta_types.h"
#include "DNA_mesh_types.h"

typedef struct MantaJob {
  /* from wmJob */
  void *owner;
  short *stop, *do_update;
  float *progress;
  const char *type;
  const char *name;

  struct Main *bmain;
  Scene *scene;
  Depsgraph *depsgraph;
  Object *ob;

  MantaModifierData *mmd;

  int success;
  double start;

  int *pause_frame;
} MantaJob;


static bool manta_initjob(
    bContext *C, MantaJob *job, wmOperator *op, char *error_msg, int error_size)
{
  MantaModifierData *mmd = NULL;
  MantaDomainSettings *mds;
  Object *ob = CTX_data_active_object(C);

  mmd = (MantaModifierData *)modifiers_findByType(ob, eModifierType_Manta);
  if (!mmd) {
    BLI_strncpy(error_msg, N_("Bake failed: no Fluid modifier found"), error_size);
    return false;
  }
  mds = mmd->domain;
  if (!mds) {
    BLI_strncpy(error_msg, N_("Bake failed: invalid domain"), error_size);
    return false;
  }

  job->bmain = CTX_data_main(C);
  job->scene = CTX_data_scene(C);
  job->depsgraph = CTX_data_depsgraph_pointer(C);
  job->ob = CTX_data_active_object(C);
  job->mmd = mmd;
  job->type = op->type->idname;
  job->name = op->type->name;

  return true;
}

static bool manta_initpaths(MantaJob *job, ReportList *reports)
{
  MantaDomainSettings *mds = job->mmd->domain;
  char tmpDir[FILE_MAX];
  tmpDir[0] = '\0';

  const char *relbase = modifier_path_relbase(job->bmain, job->ob);

  /* We do not accept empty paths, they can end in random places silently, see T51176. */
  if (mds->cache_directory[0] == '\0') {
    modifier_path_init(
        mds->cache_directory, sizeof(mds->cache_directory), FLUID_DOMAIN_DIR_DEFAULT);
    BKE_reportf(reports,
                RPT_WARNING,
                "Fluid Mantaflow: Empty cache path, reset to default '%s'",
                mds->cache_directory);
  }

  BLI_strncpy(tmpDir, mds->cache_directory, FILE_MAXDIR);
  BLI_path_abs(tmpDir, relbase);

  /* Ensure whole path exists */
  const bool dir_exists = BLI_dir_create_recursive(tmpDir);

  /* We change path to some presumably valid default value, but do not allow bake process to continue,
   * this gives user chance to set manually another path. */
  if (!dir_exists) {
    modifier_path_init(
        mds->cache_directory, sizeof(mds->cache_directory), FLUID_DOMAIN_DIR_DEFAULT);

    BKE_reportf(reports,
                RPT_ERROR,
                "Fluid Mantaflow: Could not create cache directory '%s', reset to default '%s'",
                tmpDir,
                mds->cache_directory);

    BLI_strncpy(tmpDir, mds->cache_directory, FILE_MAXDIR);
    BLI_path_abs(tmpDir, relbase);

    /* Ensure whole path exists and is writable. */
    if (!BLI_dir_create_recursive(tmpDir)) {
      BKE_reportf(reports,
                  RPT_ERROR,
                  "Fluid Mantaflow: Could not use default cache directory '%s', "
                  "please define a valid cache path manually",
                  tmpDir);
    }
    /* Copy final dir back into domain settings */
    BLI_strncpy(mds->cache_directory, tmpDir, FILE_MAXDIR);

    return false;
  }

  /* Copy final dir back into domain settings */
  BLI_strncpy(mds->cache_directory, tmpDir, FILE_MAXDIR);
  return true;
}

static void manta_bake_free(void *customdata)
{
  MantaJob *job = customdata;
  MEM_freeN(job);
}

static void manta_bake_sequence(MantaJob *job)
{
  MantaDomainSettings *mds = job->mmd->domain;
  Scene *scene = job->scene;
  int frame = 1, orig_frame;
  int frames;
  int *pause_frame = NULL;
  bool is_first_frame;

  frames = mds->cache_frame_end - mds->cache_frame_start + 1;

  if (frames <= 0) {
    BLI_strncpy(mds->error, N_("No frames to bake"), sizeof(mds->error));
    return;
  }

  /* Show progress bar. */
  if (job->do_update)
    *(job->do_update) = true;

  /* Get current pause frame (pointer) - depending on bake type */
  pause_frame = job->pause_frame;

  /* Set frame to start point (depending on current pause frame value) */
  is_first_frame = ((*pause_frame) == 0);
  frame = is_first_frame ? mds->cache_frame_start : (*pause_frame);

  /* Save orig frame and update scene frame */
  orig_frame = CFRA;
  CFRA = frame;

  /* Loop through selected frames */
  for (; frame <= mds->cache_frame_end; frame++) {
    const float progress = (frame - mds->cache_frame_start) / (float)frames;

    /* Keep track of pause frame - needed to init future loop */
    (*pause_frame) = frame;

    /* If user requested stop, quit baking */
    if (G.is_break) {
      job->success = 0;
      return;
    }

    /* Update progress bar */
    if (job->do_update)
      *(job->do_update) = true;
    if (job->progress)
      *(job->progress) = progress;

    CFRA = frame;

    /* Update animation system */
    ED_update_for_newframe(job->bmain, job->depsgraph);
  }

  /* Restore frame position that we were on before bake */
  CFRA = orig_frame;
}

static void manta_bake_endjob(void *customdata)
{
  MantaJob *job = customdata;
  MantaDomainSettings *mds = job->mmd->domain;

  if (STREQ(job->type, "MANTA_OT_bake_data")) {
    mds->cache_flag &= ~FLUID_DOMAIN_BAKING_DATA;
    mds->cache_flag |= FLUID_DOMAIN_BAKED_DATA;
  }
  else if (STREQ(job->type, "MANTA_OT_bake_noise")) {
    mds->cache_flag &= ~FLUID_DOMAIN_BAKING_NOISE;
    mds->cache_flag |= FLUID_DOMAIN_BAKED_NOISE;
  }
  else if (STREQ(job->type, "MANTA_OT_bake_mesh")) {
    mds->cache_flag &= ~FLUID_DOMAIN_BAKING_MESH;
    mds->cache_flag |= FLUID_DOMAIN_BAKED_MESH;
  }
  else if (STREQ(job->type, "MANTA_OT_bake_particles")) {
    mds->cache_flag &= ~FLUID_DOMAIN_BAKING_PARTICLES;
    mds->cache_flag |= FLUID_DOMAIN_BAKED_PARTICLES;
  }
  else if (STREQ(job->type, "MANTA_OT_bake_guiding")) {
    mds->cache_flag &= ~FLUID_DOMAIN_BAKING_GUIDING;
    mds->cache_flag |= FLUID_DOMAIN_BAKED_GUIDING;
  }
  DEG_id_tag_update(&job->ob->id, ID_RECALC_GEOMETRY);

  G.is_rendering = false;
  BKE_spacedata_draw_locks(false);
  WM_set_locked_interface(G_MAIN->wm.first, false);

  /* Bake was successful:
   *  Report for ended bake and how long it took */
  if (job->success) {
    /* Show bake info */
    WM_reportf(RPT_INFO,
               "Fluid Mantaflow: %s complete! (%.2f)",
               job->name,
               PIL_check_seconds_timer() - job->start);
  }
  else {
    if (strlen(mds->error)) { /* If an error occurred */
      WM_reportf(RPT_ERROR, "Fluid Mantaflow: %s failed: %s", job->name, mds->error);
    }
    else { /* User canceled the bake */
      WM_reportf(RPT_WARNING, "Fluid Mantaflow: %s canceled!", job->name);
    }
  }
}

static void manta_bake_startjob(void *customdata,
                                      short *stop,
                                      short *do_update,
                                      float *progress)
{
  MantaJob *job = customdata;
  MantaDomainSettings *mds = job->mmd->domain;

  char tmpDir[FILE_MAX];
  tmpDir[0] = '\0';

  job->stop = stop;
  job->do_update = do_update;
  job->progress = progress;
  job->start = PIL_check_seconds_timer();
  job->success = 1;

  G.is_break = false;
  G.is_rendering = true;
  BKE_spacedata_draw_locks(true);

  if (STREQ(job->type, "MANTA_OT_bake_data")) {
    BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_CONFIG, NULL);
    BLI_dir_create_recursive(tmpDir); /* Create 'config' subdir if it does not exist already */
    tmpDir[0] = '\0';
    BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_DATA, NULL);
    BLI_dir_create_recursive(tmpDir); /* Create 'data' subdir if it does not exist already */
    mds->cache_flag &= ~FLUID_DOMAIN_BAKED_DATA;
    mds->cache_flag |= FLUID_DOMAIN_BAKING_DATA;
    job->pause_frame = &mds->cache_frame_pause_data;

    if (mds->flags & FLUID_DOMAIN_EXPORT_MANTA_SCRIPT) {
      BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_SCRIPT, NULL);
      BLI_dir_create_recursive(tmpDir); /* Create 'script' subdir if it does not exist already */
    }
  }
  else if (STREQ(job->type, "MANTA_OT_bake_noise")) {
    BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_NOISE, NULL);
    BLI_dir_create_recursive(tmpDir); /* Create 'noise' subdir if it does not exist already */
    mds->cache_flag &= ~FLUID_DOMAIN_BAKED_NOISE;
    mds->cache_flag |= FLUID_DOMAIN_BAKING_NOISE;
    job->pause_frame = &mds->cache_frame_pause_noise;
  }
  else if (STREQ(job->type, "MANTA_OT_bake_mesh")) {
    BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_MESH, NULL);
    BLI_dir_create_recursive(tmpDir); /* Create 'mesh' subdir if it does not exist already */
    mds->cache_flag &= ~FLUID_DOMAIN_BAKED_MESH;
    mds->cache_flag |= FLUID_DOMAIN_BAKING_MESH;
    job->pause_frame = &mds->cache_frame_pause_mesh;
  }
  else if (STREQ(job->type, "MANTA_OT_bake_particles")) {
    BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_PARTICLES, NULL);
    BLI_dir_create_recursive(tmpDir); /* Create 'particles' subdir if it does not exist already */
    mds->cache_flag &= ~FLUID_DOMAIN_BAKED_PARTICLES;
    mds->cache_flag |= FLUID_DOMAIN_BAKING_PARTICLES;
    job->pause_frame = &mds->cache_frame_pause_particles;
  }
  else if (STREQ(job->type, "MANTA_OT_bake_guiding")) {
    BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_GUIDING, NULL);
    BLI_dir_create_recursive(tmpDir); /* Create 'guiding' subdir if it does not exist already */
    mds->cache_flag &= ~FLUID_DOMAIN_BAKED_GUIDING;
    mds->cache_flag |= FLUID_DOMAIN_BAKING_GUIDING;
    job->pause_frame = &mds->cache_frame_pause_guiding;
  }
  DEG_id_tag_update(&job->ob->id, ID_RECALC_GEOMETRY);

  manta_bake_sequence(job);

  if (do_update)
    *do_update = true;
  if (stop)
    *stop = 0;
}

static void manta_free_endjob(void *customdata)
{
  MantaJob *job = customdata;
  MantaDomainSettings *mds = job->mmd->domain;

  G.is_rendering = false;
  BKE_spacedata_draw_locks(false);
  WM_set_locked_interface(G_MAIN->wm.first, false);

  /* Free was successful:
   *  Report for ended free job and how long it took */
  if (job->success) {
    /* Show free job info */
    WM_reportf(RPT_INFO,
               "Fluid Mantaflow: %s complete! (%.2f)",
               job->name,
               PIL_check_seconds_timer() - job->start);
  }
  else {
    if (strlen(mds->error)) { /* If an error occurred */
      WM_reportf(RPT_ERROR, "Fluid Mantaflow: %s failed: %s", job->name, mds->error);
    }
    else { /* User canceled the free job */
      WM_reportf(RPT_WARNING, "Fluid Mantaflow: %s canceled!", job->name);
    }
  }
}

static void manta_free_startjob(void *customdata,
                                      short *stop,
                                      short *do_update,
                                      float *progress)
{
  MantaJob *job = customdata;
  MantaDomainSettings *mds = job->mmd->domain;
  Scene *scene = job->scene;

  char tmpDir[FILE_MAX];
  tmpDir[0] = '\0';

  job->stop = stop;
  job->do_update = do_update;
  job->progress = progress;
  job->start = PIL_check_seconds_timer();
  job->success = 1;

  G.is_break = false;
  G.is_rendering = true;
  BKE_spacedata_draw_locks(true);

  if (STREQ(job->type, "MANTA_OT_free_data")) {
    mds->cache_flag &= ~(FLUID_DOMAIN_BAKING_DATA | FLUID_DOMAIN_BAKED_DATA |
                         FLUID_DOMAIN_BAKING_NOISE | FLUID_DOMAIN_BAKED_NOISE |
                         FLUID_DOMAIN_BAKING_MESH | FLUID_DOMAIN_BAKED_MESH |
                         FLUID_DOMAIN_BAKING_PARTICLES | FLUID_DOMAIN_BAKED_PARTICLES);

    BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_CONFIG, NULL);
    if (BLI_exists(tmpDir))
      BLI_delete(tmpDir, true, true);
    BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_DATA, NULL);
    if (BLI_exists(tmpDir))
      BLI_delete(tmpDir, true, true);
    BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_NOISE, NULL);
    if (BLI_exists(tmpDir))
      BLI_delete(tmpDir, true, true);

    /* Free optional mesh and particles as well - otherwise they would not be in sync with data cache */
    BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_MESH, NULL);
    if (BLI_exists(tmpDir))
      BLI_delete(tmpDir, true, true);
    BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_PARTICLES, NULL);
    if (BLI_exists(tmpDir))
      BLI_delete(tmpDir, true, true);

    /* Free optional mantaflow script */
    BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_SCRIPT, NULL);
    if (BLI_exists(tmpDir))
      BLI_delete(tmpDir, true, true);

    /* Reset pause frame */
    mds->cache_frame_pause_data = 0;
  }
  else if (STREQ(job->type, "MANTA_OT_free_noise")) {
    mds->cache_flag &= ~(FLUID_DOMAIN_BAKING_NOISE | FLUID_DOMAIN_BAKED_NOISE);

    BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_NOISE, NULL);
    if (BLI_exists(tmpDir))
      BLI_delete(tmpDir, true, true);

    /* Reset pause frame */
    mds->cache_frame_pause_noise = 0;
  }
  else if (STREQ(job->type, "MANTA_OT_free_mesh")) {
    mds->cache_flag &= ~(FLUID_DOMAIN_BAKING_MESH | FLUID_DOMAIN_BAKED_MESH);

    BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_MESH, NULL);
    if (BLI_exists(tmpDir))
      BLI_delete(tmpDir, true, true);

    /* Reset pause frame */
    mds->cache_frame_pause_mesh = 0;
  }
  else if (STREQ(job->type, "MANTA_OT_free_particles")) {
    mds->cache_flag &= ~(FLUID_DOMAIN_BAKING_PARTICLES | FLUID_DOMAIN_BAKED_PARTICLES);

    BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_PARTICLES, NULL);
    if (BLI_exists(tmpDir))
      BLI_delete(tmpDir, true, true);

    /* Reset pause frame */
    mds->cache_frame_pause_particles = 0;
  }
  else if (STREQ(job->type, "MANTA_OT_free_guiding")) {
    mds->cache_flag &= ~(FLUID_DOMAIN_BAKING_GUIDING | FLUID_DOMAIN_BAKED_GUIDING);

    BLI_path_join(tmpDir, sizeof(tmpDir), mds->cache_directory, FLUID_DOMAIN_DIR_GUIDING, NULL);
    if (BLI_exists(tmpDir))
      BLI_delete(tmpDir, true, true);

    /* Reset pause frame */
    mds->cache_frame_pause_guiding = 0;
  }
  DEG_id_tag_update(&job->ob->id, ID_RECALC_GEOMETRY);

  *do_update = true;
  *stop = 0;

  /* Reset scene frame to cache frame start */
  CFRA = mds->cache_frame_start;

  /* Update scene so that viewport shows freed up scene */
  ED_update_for_newframe(job->bmain, job->depsgraph);
}


/***************************** Operators ******************************/

static int manta_bake_exec(struct bContext *C, struct wmOperator *op)
{
  MantaJob *job = MEM_mallocN(sizeof(MantaJob), "MantaJob");
  char error_msg[256] = "\0";

  if (!manta_initjob(C, job, op, error_msg, sizeof(error_msg))) {
    if (error_msg[0]) {
      BKE_report(op->reports, RPT_ERROR, error_msg);
    }
    manta_bake_free(job);
    return OPERATOR_CANCELLED;
  }
  if (!manta_initpaths(job, op->reports)) {
    return OPERATOR_CANCELLED;
  }
  manta_bake_startjob(job, NULL, NULL, NULL);
  manta_bake_endjob(job);
  manta_bake_free(job);

  return OPERATOR_FINISHED;
}

static int manta_bake_invoke(struct bContext *C,
                                   struct wmOperator *op,
                                   const wmEvent *UNUSED(_event))
{
  Scene *scene = CTX_data_scene(C);
  MantaJob *job = MEM_mallocN(sizeof(MantaJob), "MantaJob");
  char error_msg[256] = "\0";

  if (!manta_initjob(C, job, op, error_msg, sizeof(error_msg))) {
    if (error_msg[0]) {
      BKE_report(op->reports, RPT_ERROR, error_msg);
    }
    manta_bake_free(job);
    return OPERATOR_CANCELLED;
  }

  if (!manta_initpaths(job, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  wmJob *wm_job = WM_jobs_get(CTX_wm_manager(C),
                              CTX_wm_window(C),
                              scene,
                              "Fluid Mantaflow Bake",
                              WM_JOB_PROGRESS,
                              WM_JOB_TYPE_OBJECT_SIM_MANTA);

  WM_jobs_customdata_set(wm_job, job, manta_bake_free);
  WM_jobs_timer(wm_job, 0.01, NC_OBJECT | ND_MODIFIER, NC_OBJECT | ND_MODIFIER);
  WM_jobs_callbacks(wm_job, manta_bake_startjob, NULL, NULL, manta_bake_endjob);

  WM_set_locked_interface(CTX_wm_manager(C), true);

  WM_jobs_start(CTX_wm_manager(C), wm_job);
  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static int manta_bake_modal(bContext *C, wmOperator *UNUSED(op), const wmEvent *event)
{
  /* no running blender, remove handler and pass through */
  if (0 == WM_jobs_test(CTX_wm_manager(C), CTX_data_scene(C), WM_JOB_TYPE_OBJECT_SIM_MANTA))
    return OPERATOR_FINISHED | OPERATOR_PASS_THROUGH;

  switch (event->type) {
    case ESCKEY:
      return OPERATOR_RUNNING_MODAL;
  }
  return OPERATOR_PASS_THROUGH;
}

static int manta_free_exec(struct bContext *C, struct wmOperator *op)
{
  MantaModifierData *mmd = NULL;
  MantaDomainSettings *mds;
  Object *ob = CTX_data_active_object(C);
  Scene *scene = CTX_data_scene(C);

  /*
   * Get modifier data
   */
  mmd = (MantaModifierData *)modifiers_findByType(ob, eModifierType_Manta);
  if (!mmd) {
    BKE_report(op->reports, RPT_ERROR, "Bake free failed: no Fluid modifier found");
    return OPERATOR_CANCELLED;
  }
  mds = mmd->domain;
  if (!mds) {
    BKE_report(op->reports, RPT_ERROR, "Bake free failed: invalid domain");
    return OPERATOR_CANCELLED;
  }

  /* Cannot free data if other bakes currently working */
  if (mmd->domain->cache_flag & (FLUID_DOMAIN_BAKING_DATA | FLUID_DOMAIN_BAKING_NOISE |
                                 FLUID_DOMAIN_BAKING_MESH | FLUID_DOMAIN_BAKING_PARTICLES)) {
    BKE_report(op->reports, RPT_ERROR, "Bake free failed: pending bake jobs found");
    return OPERATOR_CANCELLED;
  }

  MantaJob *job = MEM_mallocN(sizeof(MantaJob), "MantaJob");
  job->bmain = CTX_data_main(C);
  job->scene = scene;
  job->depsgraph = CTX_data_depsgraph_pointer(C);
  job->ob = ob;
  job->mmd = mmd;
  job->type = op->type->idname;
  job->name = op->type->name;

  if (!manta_initpaths(job, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  wmJob *wm_job = WM_jobs_get(CTX_wm_manager(C),
                              CTX_wm_window(C),
                              scene,
                              "Fluid Mantaflow Free",
                              WM_JOB_PROGRESS,
                              WM_JOB_TYPE_OBJECT_SIM_MANTA);

  WM_jobs_customdata_set(wm_job, job, manta_bake_free);
  WM_jobs_timer(wm_job, 0.01, NC_OBJECT | ND_MODIFIER, NC_OBJECT | ND_MODIFIER);
  WM_jobs_callbacks(wm_job, manta_free_startjob, NULL, NULL, manta_free_endjob);

  WM_set_locked_interface(CTX_wm_manager(C), true);

  /*  Free Fluid Geometry */
  WM_jobs_start(CTX_wm_manager(C), wm_job);

  return OPERATOR_FINISHED;
}

static int manta_pause_exec(struct bContext *C, struct wmOperator *op)
{
  MantaModifierData *mmd = NULL;
  MantaDomainSettings *mds;
  Object *ob = CTX_data_active_object(C);

  /*
   * Get modifier data
   */
  mmd = (MantaModifierData *)modifiers_findByType(ob, eModifierType_Manta);
  if (!mmd) {
    BKE_report(op->reports, RPT_ERROR, "Bake free failed: no Fluid modifier found");
    return OPERATOR_CANCELLED;
  }
  mds = mmd->domain;
  if (!mds) {
    BKE_report(op->reports, RPT_ERROR, "Bake free failed: invalid domain");
    return OPERATOR_CANCELLED;
  }

  G.is_break = true;

  return OPERATOR_FINISHED;
}

void MANTA_OT_bake_data(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Bake Data";
  ot->description = "Bake Fluid Data";
  ot->idname = "MANTA_OT_bake_data";

  /* api callbacks */
  ot->exec = manta_bake_exec;
  ot->invoke = manta_bake_invoke;
  ot->modal = manta_bake_modal;
  ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_free_data(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Free Data";
  ot->description = "Free Fluid Data";
  ot->idname = "MANTA_OT_free_data";

  /* api callbacks */
  ot->exec = manta_free_exec;
  ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_bake_noise(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Bake Noise";
  ot->description = "Bake Fluid Noise";
  ot->idname = "MANTA_OT_bake_noise";

  /* api callbacks */
  ot->exec = manta_bake_exec;
  ot->invoke = manta_bake_invoke;
  ot->modal = manta_bake_modal;
  ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_free_noise(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Free Noise";
  ot->description = "Free Fluid Noise";
  ot->idname = "MANTA_OT_free_noise";

  /* api callbacks */
  ot->exec = manta_free_exec;
  ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_bake_mesh(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Bake Mesh";
  ot->description = "Bake Fluid Mesh";
  ot->idname = "MANTA_OT_bake_mesh";

  /* api callbacks */
  ot->exec = manta_bake_exec;
  ot->invoke = manta_bake_invoke;
  ot->modal = manta_bake_modal;
  ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_free_mesh(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Free Mesh";
  ot->description = "Free Fluid Mesh";
  ot->idname = "MANTA_OT_free_mesh";

  /* api callbacks */
  ot->exec = manta_free_exec;
  ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_bake_particles(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Bake Particles";
  ot->description = "Bake Fluid Particles";
  ot->idname = "MANTA_OT_bake_particles";

  /* api callbacks */
  ot->exec = manta_bake_exec;
  ot->invoke = manta_bake_invoke;
  ot->modal = manta_bake_modal;
  ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_free_particles(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Free Particles";
  ot->description = "Free Fluid Particles";
  ot->idname = "MANTA_OT_free_particles";

  /* api callbacks */
  ot->exec = manta_free_exec;
  ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_bake_guiding(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Bake Guiding";
  ot->description = "Bake Fluid Guiding";
  ot->idname = "MANTA_OT_bake_guiding";

  /* api callbacks */
  ot->exec = manta_bake_exec;
  ot->invoke = manta_bake_invoke;
  ot->modal = manta_bake_modal;
  ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_free_guiding(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Free Guiding";
  ot->description = "Free Fluid Guiding";
  ot->idname = "MANTA_OT_free_guiding";

  /* api callbacks */
  ot->exec = manta_free_exec;
  ot->poll = ED_operator_object_active_editable;
}

void MANTA_OT_pause_bake(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Pause Bake";
  ot->description = "Pause Bake";
  ot->idname = "MANTA_OT_pause_bake";

  /* api callbacks */
  ot->exec = manta_pause_exec;
  ot->poll = ED_operator_object_active_editable;
}

#pragma once

// RemeshApp — the single source of truth for the quad-remesh debug app's
// non-rendering state and actions. Both the ImGui panel (RemeshUi) and the
// named-pipe control channel (PipeServer) drive the app through this one object,
// so a button click and a `node tools/remesh_dbg.mjs run_remesh` do exactly the
// same thing. Everything here runs on the MAIN thread (the pipe marshals its
// commands onto it), so no locking is needed inside.

#include "subprocess_win.h"

#include "remesh/extract/reproject.h"
#include "remesh/preremesh.h"
#include "remesh/remesh_params.h"

#include <string>
#include <vector>

namespace sculptcore::debug_app {

struct Scene;

class RemeshApp {
public:
  enum class Job { None, Remesh, Meshy };

  RemeshApp(Scene &scene) : scene_(scene)
  {
  }

  /* Scan the assets dir for *.obj and refresh `assets`. Keeps the current
   * selection if its name still exists. */
  void rescanAssets();

  /* Load an asset by bare name (no extension) from the assets dir. Applies the
   * asset's quad-counts.txt entry to params.target_quad_count when present. */
  bool loadAsset(const std::string &name, std::string &err);
  /* Copy an external file into the assets dir, rescan, and select it. */
  bool importAsset(const std::string &srcPath, std::string &err);
  /* Record the selected asset's params.target_quad_count in the assets dir's
   * quad-counts.txt (the per-asset default the CLI / loadAsset read back). */
  bool saveAssetQuadCount(std::string &err);

  /* Spawn remesh_cli on the selected asset with the current params. */
  bool runRemesh(std::string &err);
  /* Spawn `node meshy_gen.mjs --prompt <prompt>` to generate a new asset. */
  bool meshyGen(const std::string &prompt, std::string &err);

  /* Tier 9e: run the whole input pre-pass (preRemesh) in-process on the loaded
   * mesh, rebuild the spatial tree, and leave the rough cross field in
   * .remesh.f.theta so the existing overlay visualizes it. In-process (not via
   * remesh_cli) so the cleaned triangle mesh is inspectable before the full quad
   * pipeline. preParams.target <= 0 resolves via resolvePreRemeshTarget. */
  bool runPreRemesh(std::string &err);
  /* Frame-driven per-iteration stepping: animate convergence by applying one
   * outer pre-pass iter per frame. start arms it; advance() (called each frame)
   * applies the next iter and rebuilds; reset() reloads the asset from disk. */
  void preStepStart();
  void preStepAdvance();
  bool preStepReset(std::string &err);

  /* Drain the running subprocess's stdout, parse the protocol lines, and apply
   * results (load the output mesh on RESULT). Call once per frame. */
  void update();

  /* Frame the camera to the mesh AABB (robust min/max, not calcAABB). */
  void cameraFit();

  /* Render the offscreen target and write it to `path` (shaded mesh; the live
   * quad-wireframe overlay is window-only). */
  bool screenshot(const std::string &path, std::string &err);

  /* Execute one pipe command line; returns the reply body. */
  std::string handleCommand(const std::string &line);

  /* Resolve <app-dir>/remesh_cli.exe (copied next to the app at build). */
  static std::wstring remeshCliPath();

  Scene &scene()
  {
    return scene_;
  }
  bool busy() const
  {
    return job_ != Job::None && proc_.running();
  }

  // --- State the UI reads/writes directly (all main-thread) ---
  remesh::RemeshParams params;
  std::vector<std::string> assets;
  int selected = -1; // index into `assets`, or -1
  bool showWireframe = true;
  bool showCurvature = false;
  // Principal-curvature overlay line length, as a fraction of the mesh bbox
  // diagonal (scale-independent so the slider reads the same across assets).
  float curvatureScale = 0.02f;
  // Tier 2: smoothing knobs the curvature overlay's TEMP layer was last computed
  // with; prepareOverlays recomputes when these change (or the layer is new), so
  // a slider change re-denoises the displayed field.
  int curvatureOverlayIters = -1;
  float curvatureOverlayLambda = -1.0f;

  // Cross-field (4-RoSy) overlay: per-face "+" glyphs + singularity dots.
  bool showCrossField = false;
  float crossScale = 0.02f;     // glyph arm length, fraction of bbox diagonal
  bool crossAnisotropy = false; // scale glyph length/alpha by curvature anisotropy
  // Per-edge field colouring: 0 = period jump (0..3), 1 = curl residual.
  bool showFieldEdges = false;
  int fieldEdgeMode = 0;
  // Streamlines traced along the field across faces.
  bool showStreamlines = false;
  float streamlineScale = 0.01f; // integration step, fraction of bbox diagonal
  int streamlineSeeds = 200;

  // Tier 9e input pre-pass (field-aligned pre-remesh) state. App-side standalone
  // params; Tier 9d maps RemeshParams.pre_remesh_* onto these for the real
  // pipeline. The UI edits these fields directly.
  remesh::PreRemeshParams preParams;
  // After a pre-pass run, auto-enable the cross-field overlay so the rough field
  // it left in .remesh.f.theta is visible (only meaningful when align > 0).
  bool preShowField = true;
  // Reproject the pre-pass result back onto the original input surface (the
  // canonical Botsch-Kobbelt "remesh then snap" step) so field-aligned
  // tangential smoothing can't drift verts off the surface into spikes. The
  // real QuadRemesh pipeline already reprojects (buildTriCopy + M6); this mirrors
  // it for the standalone pre-pass. Default on; toggle off to inspect raw drift.
  bool preReproject = true;
  // Print the standalone pre-pass's per-iter convergence summary + oscillation
  // verdict (dyntopo::printTraceSummary) to stderr.
  bool preTrace = false;
  // Frame-driven convergence stepping (one outer iter per frame while armed).
  bool preStepping = false;
  int preStepIter = 0; // outer iters already applied in the current step run

  std::string meshyPrompt;

  // Live job feedback.
  float progress = 0.0f; // 0..1
  std::string stage;     // last PROGRESS stage tag
  std::string status;    // human-readable status line (errors land here)

  std::string stats;

  // Last completed run.
  std::string lastObj;
  std::string lastManifest;
  std::string lastStats;

  void updateStats();

private:
  bool loadObjFile(const std::string &path, std::string &err);
  /* Tier 9e: snap the current (pre-passed) mesh back onto the original input
   * surface, reloaded fresh from disk. No-op when preReproject is off; returns a
   * parenthetical status note ("" if nothing to report, e.g. reprojected or
   * disabled; " (reproject skipped: ...)" when it couldn't run). */
  std::string reprojectToInput();
  bool startJob(Job kind,
                const std::wstring &exe,
                const std::vector<std::wstring> &args,
                const std::string &label);
  void handleLine(const std::string &line);
  std::string assetPath(const std::string &name) const;

  Scene &scene_;
  Subprocess proc_;
  Job job_ = Job::None;
  std::string currentAssetName_; // for manifest --name on remesh
};

} // namespace sculptcore::debug_app

#pragma once

// RemeshApp — the single source of truth for the quad-remesh debug app's
// non-rendering state and actions. Both the ImGui panel (RemeshUi) and the
// named-pipe control channel (PipeServer) drive the app through this one object,
// so a button click and a `node tools/remesh_dbg.mjs run_remesh` do exactly the
// same thing. Everything here runs on the MAIN thread (the pipe marshals its
// commands onto it), so no locking is needed inside.

#include "subprocess_win.h"

#include "remesh/remesh_params.h"

#include <string>
#include <vector>

namespace sculptcore::debug_app {

struct Scene;

class RemeshApp {
public:
  enum class Job { None, Remesh, Meshy };

  RemeshApp(Scene &scene) : scene_(scene) {}

  /* Scan the assets dir for *.obj and refresh `assets`. Keeps the current
   * selection if its name still exists. */
  void rescanAssets();

  /* Load an asset by bare name (no extension) from the assets dir. */
  bool loadAsset(const std::string &name, std::string &err);
  /* Copy an external file into the assets dir, rescan, and select it. */
  bool importAsset(const std::string &srcPath, std::string &err);

  /* Spawn remesh_cli on the selected asset with the current params. */
  bool runRemesh(std::string &err);
  /* Spawn `node meshy_gen.mjs --prompt <prompt>` to generate a new asset. */
  bool meshyGen(const std::string &prompt, std::string &err);

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

  Scene &scene() { return scene_; }
  bool busy() const { return job_ != Job::None && proc_.running(); }

  // --- State the UI reads/writes directly (all main-thread) ---
  remesh::RemeshParams params;
  std::vector<std::string> assets;
  int selected = -1;          // index into `assets`, or -1
  bool showWireframe = true;
  bool showCurvature = false;
  // Principal-curvature overlay line length, as a fraction of the mesh bbox
  // diagonal (scale-independent so the slider reads the same across assets).
  float curvatureScale = 0.02f;
  std::string meshyPrompt;

  // Live job feedback.
  float progress = 0.0f;      // 0..1
  std::string stage;          // last PROGRESS stage tag
  std::string status;         // human-readable status line (errors land here)

  // Last completed run.
  std::string lastObj;
  std::string lastManifest;
  std::string lastStats;

private:
  bool loadObjFile(const std::string &path, std::string &err);
  bool startJob(Job kind, const std::wstring &exe,
                const std::vector<std::wstring> &args, const std::string &label);
  void handleLine(const std::string &line);
  std::string assetPath(const std::string &name) const;

  Scene &scene_;
  Subprocess proc_;
  Job job_ = Job::None;
  std::string currentAssetName_; // for manifest --name on remesh
};

} // namespace sculptcore::debug_app

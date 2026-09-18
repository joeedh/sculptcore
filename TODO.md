[ ]: split ../sculptcore_addon/convert.py into smaller files
[ ]: split source/spatial/spatial.cc into smaller files 
[ ]: split source/napi/napi_runtime.cc into smaller files
[ ]: split source/meshlog/meshlog_base.h into smaller files
[ ]: split source/brush/brush_executor.h into smaller files
[ ]: split source/brush/compiler/emit_cpp.cc into smaller files
[ ]: split source/subdiv/multires.cc into smaller files
[ ]: split tests/test_grid_stroke.cc into smaller files if possible

## blender add-on todos

[ ]: expose the feature align smooth brush in the blender addon
  - make sure to expose options in brush ui
[ ]: sculptcore strokes don't update all 3d viewports on stroke end, only the active one
[ ]: right-clicking on a brush asset icon in sculptcore mode crashes, check recent 
     crash dumps.
[ ]: there are two automasking panels in sculptcore mode, the more complete one 
     should be retained make sure it maps properly to sculptcore properties.

## shift smooth panel 
[ ]: create a new panel in the properties editor for shift-smooth properties
[ ]: shift-smooth properties should be stored in the scene and the active brush,
     the scene props are used by default but there should be a per-prop option to use the 
	 local brush props instead.
[ ]: add an option to enable dyntopo in shift-smooth 
[ ]: add an option to control whether smooth uses normal smooth or feature align smooth
  - make sure to add options in ui for feature align if it's enabled for shift smooth
[ ]: add property to control shift-smooth strength

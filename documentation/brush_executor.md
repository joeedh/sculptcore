## Intro

Brushes are meant to be chained together to create composite brush.
For example, you might have a brush that adds color changes the mesh autosmooths
and dyntopos the mesh:

Dyntopo -> Draw -> Paint Vertex Colors -> Smooth

### Brush properties

Brush properties will have a sophisticated inheritance system.  
Each brush command takes a set of dynamic properties, which it
either reads directly or from a set of direct helper props on
the brush class that exist for better performance (e.g. 'strength',
'radius', etc).

### Inheritance

Properties can inherit from properties in 'parent' property sets.
For example, a client application might categorize brushes, with
three levels of defaults:

    category defaults -> user settings overrides -> scene overrides.

Typically there is a brush instance level that itself has overrides, plus the individual commands
that make up brushes can each have their own overrides too:

    category defaults -> user settings overrides -> scene overrides -> brush instance overrides -> brush subcommand overrides

### Input Device Transformation

Brush properties can also be transformed by various input device,
e.g. tablet/pen pressure, xy tilt, or even generated 'devices' like stroke angle, speed,
accelleration, etc.  These operate in a stack, each device is mapped to a min/max range,
optionally put through a custom curve and then applied to the property value in a user-defined
way (e.g. linear mixing, multiplication, addition, whatever).

### Various UI flags

Brush properties have various flags to control visibility in UIs (e.g. visible in toolbar,
visible in sidebar, visible in context menu, etc, etc).  Sculptcore itself likely doesn't  
need to worry about this, as that's a purely UI thing.

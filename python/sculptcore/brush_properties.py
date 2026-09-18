"""Validated Python access to checked native brush properties and device stacks."""

from contextlib import ExitStack
from dataclasses import dataclass
import math
import ctypes

from ._bulk import construct_from_items
from ._descriptors import read_litestl_string

FLOAT32 = 0
INT32 = 4
BOOL = 10
MAX_SAMPLES = 65536


def set_command_scalar(manager, program, command, name, scalar_type, value):
    """Set an exact typed command override; execution checks its manifest membership."""
    command = _integer(command)
    if not isinstance(name, str) or not name or '\x00' in name:
        raise ValueError("Expected a nonempty property name without NUL")
    value = _scalar(scalar_type, value)
    function = manager.capi.lib.BrushProgram_setScalarChecked
    function.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_double]
    function.restype = ctypes.c_int
    _check(function(program.ptr, command, name.encode('utf-8'), scalar_type, value))


def replace_command_cavity_curve(manager, program, command, samples):
    """Replace the command's owned table atomically; exactly 256 finite float32 samples."""
    command = _integer(command)
    samples = tuple(_scalar(FLOAT32, value) for value in samples)
    if len(samples) != 256:
        raise ValueError("Cavity curve requires exactly 256 samples")
    function = manager.capi.lib.BrushProgram_replaceCavityCurveChecked
    function.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
    function.restype = ctypes.c_int
    with _primitive_vector(manager, 'float', samples) as values:
        _check(function(program.ptr, command, values.ptr))


def inherit_command_cavity_curve(manager, program, command):
    """Resume inheritance of the authoritative Brush table on the next preparation."""
    function = manager.capi.lib.BrushProgram_removeCavityCurveChecked
    function.argtypes = [ctypes.c_void_p, ctypes.c_int]
    function.restype = ctypes.c_int
    _check(function(program.ptr, _integer(command)))


class BrushPropertyError(RuntimeError):
    def __init__(self, status):
        self.status = int(status)
        super().__init__("Brush property operation failed (status {})".format(status))


def _check(status):
    if status:
        raise BrushPropertyError(status)


def _integer(value):
    if isinstance(value, bool) or not isinstance(value, int) or not -(2 ** 31) <= value < 2 ** 31:
        raise ValueError("Expected a signed 32-bit integer")
    return int(value)


def _float(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TypeError("Expected a numeric value")
    value = float(value)
    if not math.isfinite(value) or abs(value) > 3.4028234663852886e38:
        raise ValueError("Expected a finite float32 value")
    return value


def _scalar(scalar_type, value):
    if scalar_type == BOOL:
        if not isinstance(value, bool):
            raise TypeError("Expected a boolean property value")
        return float(value)
    if scalar_type == INT32:
        return float(_integer(value))
    if scalar_type == FLOAT32:
        return _float(value)
    raise ValueError("Unsupported scalar type")


def _samples(samples):
    count = len(samples)
    if count == 1 or count > MAX_SAMPLES:
        raise ValueError("A response table needs zero or 2..65536 samples")
    return [_float(value) for value in samples]


def _double(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TypeError("Expected a numeric response parameter")
    value = float(value)
    if not math.isfinite(value):
        raise ValueError("Expected a finite response parameter")
    return value


def _primitive_vector(manager, kind, values):
    """Copy validated primitive values in bulk; never retain an alias after resize."""
    if kind not in ('int32', 'float', 'double'):
        raise ValueError("Unsupported primitive vector")
    vector = construct_from_items(manager, manager.get(kind), ())
    try:
        vector.resize(len(values))
        if values:
            vector.numpy()[:] = values
        return vector
    except BaseException:
        vector.dispose()
        raise


def replace_fixed_curve(manager, brush, kind, samples):
    """Atomically install a finite 256-entry legacy falloff or cavity table."""
    if kind not in ('falloff', 'cavity') or len(samples) != 256:
        raise ValueError("Expected a falloff/cavity table with exactly 256 entries")
    with _primitive_vector(manager, 'float', _samples(samples)) as vector:
        method = brush.replaceFalloffCurveChecked if kind == 'falloff' else brush.replaceCavityCurveChecked
        if not method(vector):
            raise BrushPropertyError(-1)


@dataclass(frozen=True)
class DeviceLayer:
    device: int
    mode: int = 1
    factor: float = 1.0
    enabled: bool = True
    samples: tuple = ()
    response_kind: str = 'TABLE'
    parameters: tuple = ()

    def __post_init__(self):
        object.__setattr__(self, 'samples', tuple(self.samples))
        object.__setattr__(self, 'parameters', tuple(self.parameters))


@dataclass(frozen=True)
class Uniform:
    index: int
    name: str
    scalar_type: int
    dynamic: bool
    default: float
    has_default: bool
    minimum: float | None
    maximum: float | None


def _stack_arrays(layers):
    if len(layers) > 4:
        raise ValueError("At most four unique input devices are supported")
    devices, modes, factors, enabled, offsets, samples, kinds, parameters = [], [], [], [], [0], [], [], []
    for layer in layers:
        device, mode, factor = _integer(layer.device), _integer(layer.mode), _float(layer.factor)
        if device not in range(4) or device in devices or mode not in range(5) or not 0 <= factor <= 1:
            raise ValueError("Invalid or duplicate device, mode or mixing factor")
        if not isinstance(layer.enabled, bool):
            raise TypeError("enabled must be boolean")
        if layer.response_kind not in ('TABLE', 'CONSTANT', 'TWO_STEP'):
            raise ValueError("Unknown response kind")
        params = tuple(_double(value) for value in layer.parameters)
        kind = ('TABLE', 'CONSTANT', 'TWO_STEP').index(layer.response_kind)
        if len(params) != (0, 1, 3)[kind] or (kind and len(layer.samples)):
            raise ValueError("Invalid response parameters or analytic table payload")
        if kind == 2 and not 0 <= params[0] <= 1:
            raise ValueError("Response threshold must be in [0, 1]")
        kinds.append(kind)
        parameters.extend((0, 0, 0) if kind == 0 else (0, params[0], 0) if kind == 1 else params)
        devices.append(device)
        modes.append(mode)
        factors.append(factor)
        enabled.append(int(layer.enabled))
        samples.extend(_samples(layer.samples))
        offsets.append(len(samples))
    return (("int32", devices), ("int32", modes), ("float", factors),
            ("int32", enabled), ("int32", offsets), ("float", samples),
            ("int32", kinds), ("double", parameters))


def replace_command_stack(manager, program, command, name, scalar_type, layers):
    """Explicit empty disables inheritance; complete upload preserves state on failure."""
    command = _integer(command)
    if not isinstance(name, str) or not name or '\x00' in name:
        raise ValueError("Expected a nonempty property name without NUL")
    if _integer(scalar_type) not in (FLOAT32, INT32, BOOL):
        raise ValueError("Unsupported scalar type")
    arrays = _stack_arrays(layers)
    with ExitStack() as stack:
        vectors = [stack.enter_context(_primitive_vector(manager, kind, values)) for kind, values in arrays]
        function = manager.capi.lib.BrushProgram_replaceResponseDynamicsChecked
        function.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int] + [ctypes.c_void_p] * 8
        function.restype = ctypes.c_int
        _check(function(program.ptr, command, name.encode('utf-8'), scalar_type, *(vector.ptr for vector in vectors)))


def inherit_command_stack(program, command, name, scalar_type):
    """Remove an explicit override to inherit the live brush stack."""
    command = _integer(command)
    if not isinstance(name, str) or not name or '\x00' in name:
        raise ValueError("Expected a nonempty property name without NUL")
    if _integer(scalar_type) not in (FLOAT32, INT32, BOOL):
        raise ValueError("Unsupported scalar type")
    function = program.manager.capi.lib.BrushProgram_removeDynamicsChecked
    function.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
    function.restype = ctypes.c_int
    _check(function(program.ptr, command, name.encode('utf-8'), scalar_type))


class _PropertyAccess:
    def read(self, index, *, evaluate=False):
        uniform = self._uniform(index)
        if not isinstance(evaluate, bool):
            raise TypeError("evaluate must be boolean")
        with self._call("read{}ScalarChecked", uniform, evaluate) as result:
            _check(result.status)
            return bool(result.value) if uniform.scalar_type == BOOL else (
                int(result.value) if uniform.scalar_type == INT32 else result.value)

    def write(self, index, value):
        uniform = self._uniform(index)
        _check(self._call("write{}ScalarChecked", uniform, _scalar(uniform.scalar_type, value)))

    def replace_stack(self, index, layers):
        uniform = self._uniform(index)
        arrays = _stack_arrays(layers)
        with ExitStack() as stack:
            vectors = [stack.enter_context(_primitive_vector(self.manager, kind, values))
                       for kind, values in arrays]
            _check(self._call("replace{}ResponseDynamicsChecked", uniform, *vectors))

    def set_sample(self, index, device, sample_index, count, value):
        uniform = self._uniform(index)
        device, sample_index, count = _integer(device), _integer(sample_index), _integer(count)
        if device not in range(4) or not 2 <= count <= MAX_SAMPLES or not 0 <= sample_index < count:
            raise ValueError("Invalid device or sample index/count")
        _check(self._call("set{}DynamicSampleChecked", uniform, device, sample_index, count, _float(value)))

    def clear_stack(self, index):
        uniform = self._uniform(index)
        _check(self._call("clear{}DynamicsChecked", uniform))

    def configure(self, index, device, mode=1, factor=1.0):
        uniform = self._uniform(index)
        device, mode, factor = _integer(device), _integer(mode), _float(factor)
        if device not in range(4) or mode not in range(5) or not 0 <= factor <= 1:
            raise ValueError("Invalid device, mode or mixing factor")
        _check(self._call("configure{}DynamicChecked", uniform, device, mode, factor))

    def enable(self, index, device, enabled):
        uniform = self._uniform(index)
        device = _integer(device)
        if device not in range(4) or not isinstance(enabled, bool):
            raise ValueError("Expected a supported device and a boolean enabled flag")
        _check(self._call("enable{}DynamicChecked", uniform, device, int(enabled)))

    def move(self, index, device, destination):
        uniform = self._uniform(index)
        device, destination = _integer(device), _integer(destination)
        if device not in range(4) or destination not in range(4):
            raise ValueError("Invalid device or destination index")
        _check(self._call("move{}DynamicChecked", uniform, device, destination))

    def replace_table(self, index, device, samples):
        uniform = self._uniform(index)
        device = _integer(device)
        if device not in range(4):
            raise ValueError("Invalid device")
        with _primitive_vector(self.manager, "float", _samples(samples)) as table:
            _check(self._call("replace{}DynamicTableChecked", uniform, device, table))


class UniformProperties(_PropertyAccess):
    """A query-scoped view; requerying its executor invalidates this view.

    The caller must keep the executor and its native Brush alive for this view.
    """

    def __init__(self, manager, executor, brush_type):
        self.manager = manager
        self.executor = executor
        count = executor.queryUniformManifest(_integer(brush_type))
        if count < 0:
            raise BrushPropertyError(-1)
        self._token = int(executor.uniformQueryToken())
        uniforms = []
        for index in range(count):
            with executor.uniformSnapshotChecked(self.token, index) as entry:
                _check(entry.status)
                uniforms.append(Uniform(
                    index, read_litestl_string(entry.name.ptr), int(entry.scalarType), bool(entry.dynamic),
                    getattr(entry, "def"), bool(entry.hasDefault),
                    entry.rangeMin if entry.hasRange else None, entry.rangeMax if entry.hasRange else None,
                ))
        self.uniforms = tuple(uniforms)

    @property
    def token(self):
        return self._token

    def _uniform(self, index):
        index = _integer(index)
        if index < 0 or index >= len(self.uniforms):
            raise IndexError(index)
        return self.uniforms[index]

    def _call(self, method, uniform, *args):
        return getattr(self.executor, method.format("Uniform"))(
            self.token, uniform.index, uniform.scalar_type, *args)


@dataclass(frozen=True)
class _ScalarTarget:
    index: int
    scalar_type: int


class CommonProperties(_PropertyAccess):
    """Validated access to fixed BrushProp IDs: 0 strength, 1 radius, 2 autosmooth,
    3 plane offset, 4 spacing, 5 invert. Keep the native Brush alive.

    Native declaration eligibility remains authoritative after kernel queries.
    """

    def __init__(self, manager, brush):
        self.manager = manager
        self.brush = brush

    def _uniform(self, index):
        index = _integer(index)
        if index not in range(6):
            raise IndexError(index)
        return _ScalarTarget(index, BOOL if index == 5 else FLOAT32)

    def _call(self, method, uniform, *args):
        return getattr(self.brush, method.format("Common"))(uniform.index, uniform.scalar_type, *args)

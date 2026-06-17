from __future__ import annotations

import os

from build123d import (
    Align,
    Axis,
    Box,
    BuildPart,
    BuildSketch,
    Circle,
    Color,
    Compound,
    Cylinder,
    Location,
    Locations,
    Mode,
    Plane,
    Rectangle,
    RectangleRounded,
    RegularPolygon,
    add,
    chamfer,
    extrude,
    fillet,
)


# Coordinate system follows the EasyEDA STEP export:
# X/Y are board coordinates in mm, Z=0 is PCB bottom, +Z is top side.

DESIGN_VERSION = "v1.1.4"

PCB_X = 30.0
PCB_Y = 40.0
PCB_THICKNESS = 1.586
PCB_CORNER_R = 2.0

BOARD_CLEARANCE = 1.5
WALL = 1.8
FLOOR = 1.6
LID_THICKNESS = 1.8
LID_OVERLAP = 3.0
LID_GAP = 0.25
LIP_FIT_GAP = 0.25
LIP_WALL = 1.2
CASE_CORNER_R = 3.0

BATTERY_X = 30.0
BATTERY_Y = 40.0
BATTERY_Z = 8.0
BATTERY_CLEARANCE_XY = 0.45
BATTERY_CLEARANCE_Z = 0.35
BOTTOM_COMPONENT_CLEARANCE = 0.8
BOTTOM_COMPONENT_MIN_Z = -3.4

PCB_BOTTOM_Z = 0.0
PCB_TOP_Z = PCB_THICKNESS
BATTERY_TOP_Z = BOTTOM_COMPONENT_MIN_Z - BOTTOM_COMPONENT_CLEARANCE
BATTERY_BOTTOM_Z = BATTERY_TOP_Z - BATTERY_Z
INNER_BOTTOM_Z = BATTERY_BOTTOM_Z - BATTERY_CLEARANCE_Z
OUTER_BOTTOM_Z = INNER_BOTTOM_Z - FLOOR
BASE_TOP_Z = PCB_TOP_Z + 1.0
LID_BOTTOM_Z = BASE_TOP_Z - LID_OVERLAP
LID_TOP_Z = 11.2

INNER_X_MIN = -BOARD_CLEARANCE
INNER_X_MAX = PCB_X + BOARD_CLEARANCE
INNER_Y_MIN = -BOARD_CLEARANCE
INNER_Y_MAX = PCB_Y + BOARD_CLEARANCE

OUTER_X_MIN = INNER_X_MIN - WALL
OUTER_X_MAX = INNER_X_MAX + WALL
OUTER_Y_MIN = INNER_Y_MIN - WALL
OUTER_Y_MAX = INNER_Y_MAX + WALL

OUTER_X = OUTER_X_MAX - OUTER_X_MIN
OUTER_Y = OUTER_Y_MAX - OUTER_Y_MIN
OUTER_CX = (OUTER_X_MIN + OUTER_X_MAX) / 2.0
OUTER_CY = (OUTER_Y_MIN + OUTER_Y_MAX) / 2.0

INNER_X = INNER_X_MAX - INNER_X_MIN
INNER_Y = INNER_Y_MAX - INNER_Y_MIN
INNER_CX = (INNER_X_MIN + INNER_X_MAX) / 2.0
INNER_CY = (INNER_Y_MIN + INNER_Y_MAX) / 2.0

M1_HOLE_D = 1.25
POST_OD = 3.4
POST_HOLE_D = 0.9
POST_CLEARANCE_FROM_TOP = 0.15
POST_Z_BOTTOM = BATTERY_TOP_Z + 0.3
POST_HEIGHT = PCB_BOTTOM_Z - POST_Z_BOTTOM - POST_CLEARANCE_FROM_TOP
MOUNT_POINTS = [
    (2.0, 2.0),
    (28.0, 2.0),
    (2.0, 38.0),
    (28.0, 38.0),
]

LID_STANDOFF_OD = 4.2
LID_STANDOFF_BOTTOM_Z = PCB_TOP_Z
LID_STANDOFF_TOP_Z = LID_TOP_Z - LID_THICKNESS
M1_NUT_AF = 2.7
M1_NUT_DEPTH = 1.05
M1_NUT_Z_OFFSET = 0.15
M1_TOP_COUNTERBORE_D = 4.4
M1_TOP_COUNTERBORE_DEPTH = 0.75
M1_TOP_NUT_AF = 3.3
M1_TOP_NUT_DEPTH = 1.2
M1_TOP_THROUGH_HOLE_D = 1.6

SNAP_PROTRUSION = 0.35
SNAP_WIDTH = 5.2
SNAP_HEIGHT = 1.0
SNAP_Z_CENTER = 0.8
SNAP_Y_POINTS = (12.0, 28.0)

LIP_X_MIN = INNER_X_MIN + LIP_FIT_GAP
LIP_X_MAX = INNER_X_MAX - LIP_FIT_GAP
LIP_Y_MIN = INNER_Y_MIN + LIP_FIT_GAP
LIP_Y_MAX = INNER_Y_MAX - LIP_FIT_GAP

BATTERY_GUIDE_GAP = 0.25
BATTERY_GUIDE_RIB = 0.4
BATTERY_GUIDE_Z_MIN = BATTERY_BOTTOM_Z + 0.3
BATTERY_GUIDE_Z_MAX = BATTERY_TOP_Z - 0.3
BATTERY_RETAINER_Z_MIN = BATTERY_BOTTOM_Z - 0.65
BATTERY_RETAINER_Z_MAX = BATTERY_BOTTOM_Z + 0.05
BATTERY_RETAINER_OVERLAP = 0.25
BATTERY_RETAINER_TAB_Y = 5.2

SLIDE_RAIL_WIDTH = 1.2
SLIDE_RAIL_LOWER_Z_MIN = OUTER_BOTTOM_Z
SLIDE_RAIL_LOWER_Z_MAX = OUTER_BOTTOM_Z + 0.4
SLIDE_CLEARANCE_Z = 0.35
SLIDE_COVER_Z_MIN = SLIDE_RAIL_LOWER_Z_MAX + SLIDE_CLEARANCE_Z
SLIDE_COVER_THICKNESS = 1.2
SLIDE_COVER_Z_MAX = SLIDE_COVER_Z_MIN + SLIDE_COVER_THICKNESS
SLIDE_RAIL_UPPER_Z_MIN = SLIDE_COVER_Z_MAX + SLIDE_CLEARANCE_Z
SLIDE_RAIL_UPPER_Z_MAX = SLIDE_RAIL_UPPER_Z_MIN + 0.55
SLIDE_COVER_X_MIN = INNER_X_MIN + 0.15
SLIDE_COVER_X_MAX = INNER_X_MAX - 0.15
SLIDE_COVER_Y_MIN = INNER_Y_MIN + 0.25
SLIDE_COVER_Y_MAX = INNER_Y_MAX - 0.75
SLIDE_PULL_TAB_X_MIN = 9.0
SLIDE_PULL_TAB_X_MAX = 21.0
SLIDE_PULL_TAB_Y_MIN = OUTER_Y_MIN - 1.2
SLIDE_PULL_TAB_Y_MAX = INNER_Y_MIN + 1.0

OUTER_VERTICAL_FILLET = 0.45
EXPOSED_EDGE_CHAMFER = 0.18
WINDOW_EDGE_CHAMFER = 0.28
SLIDE_COVER_EDGE_FILLET = 0.18
SLIDE_COVER_LEAD_CHAMFER = 0.12

# Openings and keepouts extracted from the local PCB STEP export.
USB_OPENING = {
    "x_min": 9.7,
    "x_max": 20.3,
    "z_min": 0.35,
    "z_max": 5.35,
}
RIGHT_SWITCH_OPENING = {
    "y_min": 2.7,
    "y_max": 10.25,
    "z_min": 0.55,
    "z_max": 3.55,
}
ANTENNA_OPENING = {
    "x_min": 5.3,
    "x_max": 24.7,
    "z_min": -3.35,
    "z_max": 0.35,
}
GPS_WINDOW = {
    "cx": OUTER_CX,
    "cy": 29.889,
    "x": 17.2,
    "y": 17.2,
}
GPS_THIN_SKIN = 0.8
GPS_THIN_ZONE = {
    "cx": OUTER_CX,
    "cy": 31.0,
    "x": 18.2,
    "y": 15.6,
}
DISPLAY_WINDOW = {
    # First-pass 1.14 inch horizontal display window. Adjust to the exact module.
    "cx": OUTER_CX,
    "cy": 13.4,
    "x": 28.5,
    "y": 15.5,
}
BUZZER_SOUND_HOLE = {
    "cx": 24.765,
    "cy": 14.732,
    "d": 7.0,
}


def _rounded_plate(length: float, width: float, radius: float, z_min: float, height: float):
    with BuildPart() as part:
        with BuildSketch(Plane.XY.offset(z_min)):
            RectangleRounded(length, width, radius)
        extrude(amount=height)
    return part.part


def _box_at(
    x_min: float,
    x_max: float,
    y_min: float,
    y_max: float,
    z_min: float,
    z_max: float,
    *,
    mode: Mode = Mode.ADD,
):
    with Locations((x_min, y_min, z_min)):
        Box(
            x_max - x_min,
            y_max - y_min,
            z_max - z_min,
            align=(Align.MIN, Align.MIN, Align.MIN),
            mode=mode,
        )


def _hex_pocket_at(cx: float, cy: float, z_min: float, depth: float, across_flats: float):
    # Hex recess for a captured M1 nut.
    with BuildSketch(Plane.XY.offset(z_min)) as pocket:
        with Locations((cx, cy)):
            RegularPolygon(across_flats / 2, 6, major_radius=False, rotation=30)
    extrude(pocket.sketch, amount=depth, mode=Mode.SUBTRACT)


def _top_fastener_mount_at(cx: float, cy: float):
    # Through-hole plus top-access counterbore/hex pocket for an M1 fastener.
    through_height = LID_TOP_Z - LID_STANDOFF_BOTTOM_Z + 0.5
    with Locations((cx, cy, LID_STANDOFF_BOTTOM_Z + through_height / 2 - 0.25)):
        Cylinder(M1_TOP_THROUGH_HOLE_D / 2, through_height, mode=Mode.SUBTRACT)

    with Locations((cx, cy, LID_TOP_Z - M1_TOP_COUNTERBORE_DEPTH / 2 + 0.05)):
        Cylinder(M1_TOP_COUNTERBORE_D / 2, M1_TOP_COUNTERBORE_DEPTH + 0.1, mode=Mode.SUBTRACT)

    _hex_pocket_at(
        cx,
        cy,
        LID_TOP_Z - M1_TOP_NUT_DEPTH,
        M1_TOP_NUT_DEPTH + 0.1,
        M1_TOP_NUT_AF,
    )


def _safe_fillet(edges, radius: float):
    edge_list = list(edges)
    if not edge_list:
        return
    try:
        fillet(edge_list, radius=radius)
    except Exception:
        pass


def _safe_chamfer(edges, length: float):
    edge_list = list(edges)
    if not edge_list:
        return
    try:
        chamfer(edge_list, length=length)
    except Exception:
        pass


def _coord(value, attr: str) -> float:
    if hasattr(value, attr):
        return float(getattr(value, attr))
    return float(getattr(value, attr.lower()))


def _center_xyz(edge) -> tuple[float, float, float]:
    c = edge.center()
    return _coord(c, "X"), _coord(c, "Y"), _coord(c, "Z")


def _near(value: float, target: float, tol: float = 0.08) -> bool:
    return abs(value - target) <= tol


def _outside_vertical_edges(part):
    edges = []
    for edge in part.edges().filter_by(Axis.Z):
        x, y, _ = _center_xyz(edge)
        on_outer_x = _near(x, OUTER_X_MIN) or _near(x, OUTER_X_MAX)
        on_outer_y = _near(y, OUTER_Y_MIN) or _near(y, OUTER_Y_MAX)
        if (on_outer_x or on_outer_y) and edge.length > 4.0:
            edges.append(edge)
    return edges


def _exposed_horizontal_edges(part):
    edges = []
    for edge in part.edges():
        x, y, z = _center_xyz(edge)
        if edge.length < 1.0:
            continue
        if _near(z, LID_TOP_Z) and _inside_any_window_xy(x, y, margin=0.65):
            continue
        if _near(z, LID_TOP_Z) or _near(z, OUTER_BOTTOM_Z):
            edges.append(edge)
    return edges


def _inside_any_window_xy(x: float, y: float, margin: float = 0.0) -> bool:
    for window in (DISPLAY_WINDOW,):
        x0 = window["cx"] - window["x"] / 2 - margin
        x1 = window["cx"] + window["x"] / 2 + margin
        y0 = window["cy"] - window["y"] / 2 - margin
        y1 = window["cy"] + window["y"] / 2 + margin
        if x0 <= x <= x1 and y0 <= y <= y1:
            return True
    return False


def _add_gps_thin_cover():
    x0 = GPS_THIN_ZONE["cx"] - GPS_THIN_ZONE["x"] / 2
    x1 = GPS_THIN_ZONE["cx"] + GPS_THIN_ZONE["x"] / 2
    y0 = GPS_THIN_ZONE["cy"] - GPS_THIN_ZONE["y"] / 2
    y1 = GPS_THIN_ZONE["cy"] + GPS_THIN_ZONE["y"] / 2
    _box_at(
        x0,
        x1,
        y0,
        y1,
        LID_TOP_Z - LID_THICKNESS - 0.05,
        LID_TOP_Z - GPS_THIN_SKIN,
        mode=Mode.SUBTRACT,
    )


def _window_top_edges(part, windows):
    edges = []
    tol = 0.12
    for edge in part.edges():
        x, y, z = _center_xyz(edge)
        if not _near(z, LID_TOP_Z, tol):
            continue
        if edge.length < 1.0:
            continue
        for window in windows:
            x0 = window["cx"] - window["x"] / 2
            x1 = window["cx"] + window["x"] / 2
            y0 = window["cy"] - window["y"] / 2
            y1 = window["cy"] + window["y"] / 2
            on_vertical_side = (_near(x, x0, tol) or _near(x, x1, tol)) and (y0 - tol <= y <= y1 + tol)
            on_horizontal_side = (_near(y, y0, tol) or _near(y, y1, tol)) and (x0 - tol <= x <= x1 + tol)
            if on_vertical_side or on_horizontal_side:
                edges.append(edge)
                break
    return edges


def _edges_at_z(part, z_values: tuple[float, ...], min_length: float = 1.0):
    edges = []
    for edge in part.edges():
        _, _, z = _center_xyz(edge)
        if edge.length < min_length:
            continue
        if any(_near(z, z_value) for z_value in z_values):
            edges.append(edge)
    return edges


def _add_battery_pocket_features():
    # Guide ribs center the 30 x 40 x 10 mm battery without blocking bottom insertion.
    guide_y0 = 3.0
    guide_y1 = PCB_Y - 3.0
    guide_x0 = 3.0
    guide_x1 = PCB_X - 3.0
    _box_at(
        INNER_X_MIN,
        -BATTERY_GUIDE_GAP,
        guide_y0,
        guide_y1,
        BATTERY_GUIDE_Z_MIN,
        BATTERY_GUIDE_Z_MAX,
        mode=Mode.ADD,
    )
    _box_at(
        PCB_X + BATTERY_GUIDE_GAP,
        INNER_X_MAX,
        guide_y0,
        guide_y1,
        BATTERY_GUIDE_Z_MIN,
        BATTERY_GUIDE_Z_MAX,
        mode=Mode.ADD,
    )
    _box_at(
        guide_x0,
        guide_x1,
        INNER_Y_MIN,
        -BATTERY_GUIDE_GAP,
        BATTERY_GUIDE_Z_MIN,
        BATTERY_GUIDE_Z_MAX,
        mode=Mode.ADD,
    )
    _box_at(
        guide_x0,
        guide_x1,
        PCB_Y + BATTERY_GUIDE_GAP,
        INNER_Y_MAX,
        BATTERY_GUIDE_Z_MIN,
        BATTERY_GUIDE_Z_MAX,
        mode=Mode.ADD,
    )

def _add_slide_cover_rails():
    # C-channel rails capture a thin battery cover that slides in from the USB/front edge.
    rail_y0 = INNER_Y_MIN + 0.35
    rail_y1 = INNER_Y_MAX - 0.35
    for z0, z1 in (
        (SLIDE_RAIL_LOWER_Z_MIN, SLIDE_RAIL_LOWER_Z_MAX),
        (SLIDE_RAIL_UPPER_Z_MIN, SLIDE_RAIL_UPPER_Z_MAX),
    ):
        _box_at(
            INNER_X_MIN,
            INNER_X_MIN + SLIDE_RAIL_WIDTH,
            rail_y0,
            rail_y1,
            z0,
            z1,
            mode=Mode.ADD,
        )
        _box_at(
            INNER_X_MAX - SLIDE_RAIL_WIDTH,
            INNER_X_MAX,
            rail_y0,
            rail_y1,
            z0,
            z1,
            mode=Mode.ADD,
        )

    # Rear stop so the cover has a positive end position when pushed in.
    _box_at(
        INNER_X_MIN + SLIDE_RAIL_WIDTH,
        INNER_X_MAX - SLIDE_RAIL_WIDTH,
        SLIDE_COVER_Y_MAX,
        SLIDE_COVER_Y_MAX + 0.45,
        SLIDE_RAIL_LOWER_Z_MIN,
        SLIDE_RAIL_UPPER_Z_MAX,
        mode=Mode.ADD,
    )

    # Front insertion slot through the wall and rails.
    _box_at(
        SLIDE_COVER_X_MIN - 0.2,
        SLIDE_COVER_X_MAX + 0.2,
        OUTER_Y_MIN - 0.25,
        INNER_Y_MIN + 0.6,
        SLIDE_RAIL_LOWER_Z_MIN - 0.05,
        SLIDE_RAIL_UPPER_Z_MAX + 0.08,
        mode=Mode.SUBTRACT,
    )


def _label(shape, name: str, color: tuple[float, float, float, float] | None = None):
    shape.label = name
    if color is not None:
        shape.color = Color(*color)
    return shape


def make_base():
    """Lower printed shell with battery cavity, side openings, and snap receivers."""

    with BuildPart() as base:
        add(_rounded_plate(OUTER_X, OUTER_Y, CASE_CORNER_R, OUTER_BOTTOM_Z, BASE_TOP_Z - OUTER_BOTTOM_Z).locate(Location((OUTER_CX, OUTER_CY, 0))))

        # Main interior cavity.
        _box_at(INNER_X_MIN, INNER_X_MAX, INNER_Y_MIN, INNER_Y_MAX, INNER_BOTTOM_Z, BASE_TOP_Z + 0.1, mode=Mode.SUBTRACT)

        # Battery pocket below the PCB. Battery nominal envelope is 30 x 40 x 10 mm.
        bx_margin = BATTERY_CLEARANCE_XY
        by_margin = BATTERY_CLEARANCE_XY
        _box_at(
            (PCB_X - BATTERY_X) / 2 - bx_margin,
            (PCB_X + BATTERY_X) / 2 + bx_margin,
            (PCB_Y - BATTERY_Y) / 2 - by_margin,
            (PCB_Y + BATTERY_Y) / 2 + by_margin,
            BATTERY_BOTTOM_Z - BATTERY_CLEARANCE_Z,
            BATTERY_TOP_Z + 0.05,
            mode=Mode.SUBTRACT,
        )

        # Front USB-C port.
        _box_at(
            USB_OPENING["x_min"],
            USB_OPENING["x_max"],
            OUTER_Y_MIN - 0.2,
            INNER_Y_MIN + 0.4,
            USB_OPENING["z_min"],
            USB_OPENING["z_max"],
            mode=Mode.SUBTRACT,
        )

        # Top-edge bottom module/antenna relief, because U9 extends beyond the PCB outline.
        _box_at(
            ANTENNA_OPENING["x_min"],
            ANTENNA_OPENING["x_max"],
            INNER_Y_MAX - 0.15,
            OUTER_Y_MAX + 0.2,
            ANTENNA_OPENING["z_min"],
            ANTENNA_OPENING["z_max"],
            mode=Mode.SUBTRACT,
        )

        # Right-side slide switch relief.
        _box_at(
            INNER_X_MAX - 0.2,
            OUTER_X_MAX + 0.2,
            RIGHT_SWITCH_OPENING["y_min"],
            RIGHT_SWITCH_OPENING["y_max"],
            RIGHT_SWITCH_OPENING["z_min"],
            RIGHT_SWITCH_OPENING["z_max"],
            mode=Mode.SUBTRACT,
        )

        # Side snap receiver pockets. The PCB now mounts to the lid, so the battery bay is clear.
        snap_z0 = SNAP_Z_CENTER - SNAP_HEIGHT / 2
        snap_z1 = SNAP_Z_CENTER + SNAP_HEIGHT / 2
        for y in SNAP_Y_POINTS:
            _box_at(
                INNER_X_MIN - SNAP_PROTRUSION - 0.15,
                INNER_X_MIN + 0.1,
                y - SNAP_WIDTH / 2,
                y + SNAP_WIDTH / 2,
                snap_z0,
                snap_z1,
                mode=Mode.SUBTRACT,
            )
            _box_at(
                INNER_X_MAX - 0.1,
                INNER_X_MAX + SNAP_PROTRUSION + 0.15,
                y - SNAP_WIDTH / 2,
                y + SNAP_WIDTH / 2,
                snap_z0,
                snap_z1,
                mode=Mode.SUBTRACT,
            )

        try:
            fillet(base.edges().filter_by(Axis.Z), radius=0.35)
        except Exception:
            pass

    return _label(base.part, "sle_enclosure_base", (0.16, 0.22, 0.24, 1.0))


def make_lid():
    """Upper printed cover with PCB standoffs, nut pockets, windows, and snap tabs."""

    with BuildPart() as lid:
        # Upper visible shell sits on the base rim.
        add(_rounded_plate(OUTER_X, OUTER_Y, CASE_CORNER_R, BASE_TOP_Z, LID_TOP_Z - BASE_TOP_Z).locate(Location((OUTER_CX, OUTER_CY, 0))))

        # Hollow underside. This leaves the top skin and the upper side walls.
        _box_at(
            INNER_X_MIN,
            INNER_X_MAX,
            INNER_Y_MIN,
            INNER_Y_MAX,
            BASE_TOP_Z - 0.1,
            LID_TOP_Z - LID_THICKNESS,
            mode=Mode.SUBTRACT,
        )

        # Insert lip that drops into the base for alignment.
        _box_at(LIP_X_MIN, LIP_X_MIN + LIP_WALL, LIP_Y_MIN, LIP_Y_MAX, LID_BOTTOM_Z, BASE_TOP_Z, mode=Mode.ADD)
        _box_at(LIP_X_MAX - LIP_WALL, LIP_X_MAX, LIP_Y_MIN, LIP_Y_MAX, LID_BOTTOM_Z, BASE_TOP_Z, mode=Mode.ADD)
        _box_at(LIP_X_MIN, LIP_X_MAX, LIP_Y_MIN, LIP_Y_MIN + LIP_WALL, LID_BOTTOM_Z, BASE_TOP_Z, mode=Mode.ADD)
        _box_at(LIP_X_MIN, LIP_X_MAX, LIP_Y_MAX - LIP_WALL, LIP_Y_MAX, LID_BOTTOM_Z, BASE_TOP_Z, mode=Mode.ADD)

        # Small side snap bumps on the insert lip.
        snap_z0 = SNAP_Z_CENTER - SNAP_HEIGHT / 2
        snap_z1 = SNAP_Z_CENTER + SNAP_HEIGHT / 2
        for y in SNAP_Y_POINTS:
            _box_at(
                LIP_X_MIN - SNAP_PROTRUSION,
                LIP_X_MIN,
                y - SNAP_WIDTH / 2,
                y + SNAP_WIDTH / 2,
                snap_z0,
                snap_z1,
                mode=Mode.ADD,
            )
            _box_at(
                LIP_X_MAX,
                LIP_X_MAX + SNAP_PROTRUSION,
                y - SNAP_WIDTH / 2,
                y + SNAP_WIDTH / 2,
                snap_z0,
                snap_z1,
                mode=Mode.ADD,
            )

        # Four internal standoffs. The PCB is screwed to these from the board side.
        standoff_height = LID_STANDOFF_TOP_Z - LID_STANDOFF_BOTTOM_Z
        for x, y in MOUNT_POINTS:
            with Locations((x, y, LID_STANDOFF_BOTTOM_Z + standoff_height / 2)):
                Cylinder(LID_STANDOFF_OD / 2, standoff_height, mode=Mode.ADD)
            _top_fastener_mount_at(x, y)

        # Top window: only the display is open. GPS uses a thin internal cover zone.
        for window in (DISPLAY_WINDOW,):
            x0 = window["cx"] - window["x"] / 2
            x1 = window["cx"] + window["x"] / 2
            y0 = window["cy"] - window["y"] / 2
            y1 = window["cy"] + window["y"] / 2
            _box_at(x0, x1, y0, y1, LID_TOP_Z - LID_THICKNESS - 0.2, LID_TOP_Z + 0.2, mode=Mode.SUBTRACT)
        _add_gps_thin_cover()

        # Round sound hole above buzzer in case the display module does not cover it.
        with Locations((BUZZER_SOUND_HOLE["cx"], BUZZER_SOUND_HOLE["cy"], LID_TOP_Z - LID_THICKNESS / 2)):
            Cylinder(BUZZER_SOUND_HOLE["d"] / 2, LID_THICKNESS + 0.5, mode=Mode.SUBTRACT)

        # Side openings repeated in the lid overlap so ports remain accessible after assembly.
        _box_at(
            USB_OPENING["x_min"],
            USB_OPENING["x_max"],
            OUTER_Y_MIN - 0.3,
            INNER_Y_MIN + 0.8,
            USB_OPENING["z_min"],
            USB_OPENING["z_max"] + 0.5,
            mode=Mode.SUBTRACT,
        )
        _box_at(
            INNER_X_MAX - 0.5,
            OUTER_X_MAX + 0.5,
            RIGHT_SWITCH_OPENING["y_min"],
            RIGHT_SWITCH_OPENING["y_max"],
            RIGHT_SWITCH_OPENING["z_min"],
            RIGHT_SWITCH_OPENING["z_max"] + 0.6,
            mode=Mode.SUBTRACT,
        )
        _box_at(
            ANTENNA_OPENING["x_min"],
            ANTENNA_OPENING["x_max"],
            INNER_Y_MAX - 0.4,
            OUTER_Y_MAX + 0.3,
            ANTENNA_OPENING["z_min"],
            ANTENNA_OPENING["z_max"] + 0.4,
            mode=Mode.SUBTRACT,
        )

        try:
            _safe_chamfer(_window_top_edges(lid, (DISPLAY_WINDOW,)), WINDOW_EDGE_CHAMFER)
            fillet(lid.edges().filter_by(Axis.Z), radius=0.3)
        except Exception:
            pass

    return _label(lid.part, "sle_enclosure_lid", (0.84, 0.84, 0.78, 1.0))


def make_one_piece_shell():
    """Single printed shell: top, side walls, PCB standoffs, and openings in one part."""

    with BuildPart() as shell:
        add(
            _rounded_plate(
                OUTER_X,
                OUTER_Y,
                CASE_CORNER_R,
                OUTER_BOTTOM_Z,
                LID_TOP_Z - OUTER_BOTTOM_Z,
            ).locate(Location((OUTER_CX, OUTER_CY, 0)))
        )

        # Bottom stays open for assembly: PCB screws are accessible and the battery slides in last.
        _box_at(
            INNER_X_MIN,
            INNER_X_MAX,
            INNER_Y_MIN,
            INNER_Y_MAX,
            OUTER_BOTTOM_Z - 0.2,
            LID_TOP_Z - LID_THICKNESS,
            mode=Mode.SUBTRACT,
        )

        # Top window: only the display is open. GPS uses a thin internal cover zone.
        for window in (DISPLAY_WINDOW,):
            x0 = window["cx"] - window["x"] / 2
            x1 = window["cx"] + window["x"] / 2
            y0 = window["cy"] - window["y"] / 2
            y1 = window["cy"] + window["y"] / 2
            _box_at(x0, x1, y0, y1, LID_TOP_Z - LID_THICKNESS - 0.2, LID_TOP_Z + 0.2, mode=Mode.SUBTRACT)
        _add_gps_thin_cover()

        with Locations((BUZZER_SOUND_HOLE["cx"], BUZZER_SOUND_HOLE["cy"], LID_TOP_Z - LID_THICKNESS / 2)):
            Cylinder(BUZZER_SOUND_HOLE["d"] / 2, LID_THICKNESS + 0.5, mode=Mode.SUBTRACT)

        # Side openings in the single-piece wall.
        _box_at(
            USB_OPENING["x_min"],
            USB_OPENING["x_max"],
            OUTER_Y_MIN - 0.3,
            INNER_Y_MIN + 0.8,
            USB_OPENING["z_min"],
            USB_OPENING["z_max"] + 0.5,
            mode=Mode.SUBTRACT,
        )
        _box_at(
            INNER_X_MAX - 0.5,
            OUTER_X_MAX + 0.5,
            RIGHT_SWITCH_OPENING["y_min"],
            RIGHT_SWITCH_OPENING["y_max"],
            RIGHT_SWITCH_OPENING["z_min"],
            RIGHT_SWITCH_OPENING["z_max"] + 0.6,
            mode=Mode.SUBTRACT,
        )
        _box_at(
            ANTENNA_OPENING["x_min"],
            ANTENNA_OPENING["x_max"],
            INNER_Y_MAX - 0.4,
            OUTER_Y_MAX + 0.3,
            ANTENNA_OPENING["z_min"],
            ANTENNA_OPENING["z_max"] + 0.4,
            mode=Mode.SUBTRACT,
        )

        _add_battery_pocket_features()
        _add_slide_cover_rails()

        # PCB mounts to the top shell from below, leaving the battery bay unobstructed.
        standoff_height = LID_STANDOFF_TOP_Z - LID_STANDOFF_BOTTOM_Z
        for x, y in MOUNT_POINTS:
            with Locations((x, y, LID_STANDOFF_BOTTOM_Z + standoff_height / 2)):
                Cylinder(LID_STANDOFF_OD / 2, standoff_height, mode=Mode.ADD)
            _top_fastener_mount_at(x, y)

        try:
            _safe_chamfer(_window_top_edges(shell, (DISPLAY_WINDOW,)), WINDOW_EDGE_CHAMFER)
            _safe_fillet(_outside_vertical_edges(shell), OUTER_VERTICAL_FILLET)
            _safe_chamfer(_exposed_horizontal_edges(shell), EXPOSED_EDGE_CHAMFER)
        except Exception:
            pass

    return _label(shell.part, f"sle_enclosure_one_piece_slide_shell_{DESIGN_VERSION}", (0.82, 0.83, 0.76, 1.0))


def make_battery_slide_cover():
    """Thin bottom drawer cover that slides into the one-piece shell rails."""

    with BuildPart() as cover:
        _box_at(
            SLIDE_COVER_X_MIN,
            SLIDE_COVER_X_MAX,
            SLIDE_COVER_Y_MIN,
            SLIDE_COVER_Y_MAX,
            SLIDE_COVER_Z_MIN,
            SLIDE_COVER_Z_MAX,
            mode=Mode.ADD,
        )

        # A shallow front pull tab makes the cover removable after printing.
        _box_at(
            SLIDE_PULL_TAB_X_MIN,
            SLIDE_PULL_TAB_X_MAX,
            SLIDE_PULL_TAB_Y_MIN,
            SLIDE_PULL_TAB_Y_MAX,
            SLIDE_COVER_Z_MIN,
            SLIDE_COVER_Z_MAX,
            mode=Mode.ADD,
        )
        for x in (12.0, 15.0, 18.0):
            _box_at(
                x - 0.35,
                x + 0.35,
                SLIDE_PULL_TAB_Y_MIN + 0.25,
                SLIDE_PULL_TAB_Y_MAX - 0.25,
                SLIDE_COVER_Z_MAX,
                SLIDE_COVER_Z_MAX + 0.35,
                mode=Mode.ADD,
            )

        try:
            _safe_fillet(cover.edges().filter_by(Axis.Z), SLIDE_COVER_EDGE_FILLET)
            _safe_chamfer(
                _edges_at_z(cover, (SLIDE_COVER_Z_MIN, SLIDE_COVER_Z_MAX), min_length=1.0),
                SLIDE_COVER_LEAD_CHAMFER,
            )
        except Exception:
            pass

    return _label(cover.part, f"sle_battery_slide_cover_{DESIGN_VERSION}", (0.12, 0.14, 0.15, 1.0))


def make_battery_keepout():
    with BuildPart() as part:
        _box_at(
            (PCB_X - BATTERY_X) / 2,
            (PCB_X + BATTERY_X) / 2,
            (PCB_Y - BATTERY_Y) / 2,
            (PCB_Y + BATTERY_Y) / 2,
            BATTERY_BOTTOM_Z,
            BATTERY_TOP_Z,
        )
    return _label(part.part, "battery_keepout_30x40x10", (0.15, 0.15, 0.15, 0.35))


def make_board_keepout():
    with BuildPart() as part:
        add(_rounded_plate(PCB_X, PCB_Y, PCB_CORNER_R, PCB_BOTTOM_Z, PCB_THICKNESS).locate(Location((PCB_X / 2, PCB_Y / 2, 0))))

        # Simplified envelopes for major components, used only for preview.
        _box_at(10.53, 19.47, -0.99, 6.59, 0.686, 4.846)  # USB-C
        _box_at(25.871, 31.021, 3.477, 9.477, 1.086, 2.986)  # right switch
        _box_at(20.265, 29.265, 10.232, 19.232, -2.914, 7.096)  # buzzer
        _box_at(6.798, 22.798, 21.889, 37.889, 1.586, 8.536)  # GPS
        _box_at(5.999, 24.001, 20.250, 45.750, -2.82, 0.0)  # bottom 2.4 GHz module
    return _label(part.part, "pcb_and_component_keepout", (0.95, 0.78, 0.28, 0.45))


def make_display_keepout():
    with BuildPart() as part:
        _box_at(
            DISPLAY_WINDOW["cx"] - DISPLAY_WINDOW["x"] / 2,
            DISPLAY_WINDOW["cx"] + DISPLAY_WINDOW["x"] / 2,
            DISPLAY_WINDOW["cy"] - DISPLAY_WINDOW["y"] / 2,
            DISPLAY_WINDOW["cy"] + DISPLAY_WINDOW["y"] / 2,
            LID_TOP_Z,
            LID_TOP_Z + 1.2,
        )
    return _label(part.part, "display_window_1p14_reference", (0.02, 0.02, 0.02, 0.35))


def make_assembly():
    shell = make_one_piece_shell()
    slide_cover = make_battery_slide_cover()
    battery = make_battery_keepout()
    board = make_board_keepout()
    display = make_display_keepout()
    assembly = Compound(children=[shell, slide_cover, battery, board, display], label=f"sle_pcb_enclosure_one_piece_preview_{DESIGN_VERSION}")
    return assembly


def gen_step():
    target = os.environ.get("MODEL_TARGET", "assembly").strip().lower()
    if target in {"one_piece", "one-piece", "single"}:
        return make_one_piece_shell()
    if target in {"slide_cover", "battery_cover", "drawer"}:
        return make_battery_slide_cover()
    if target == "base":
        return make_base()
    if target == "lid":
        return make_lid()
    if target == "battery":
        return make_battery_keepout()
    if target == "board":
        return make_board_keepout()
    return make_assembly()

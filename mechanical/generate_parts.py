#!/usr/bin/env python3
"""Printable parts for the dryer scale, built from parameters.

Five parts, in two groups:

    antenna_bracket + antenna_clamp   hold the PN5180 module against the inside
                                      of the dryer's side lid, its coil facing
                                      the spool flange where the tag lives
    cell_mount (x2), isolator_foot    the scale sandwich: the load cell between
    (x4), overload_stop               a base board and a platform, with the hot
                                      dryer standing on insulating feet

Everything that a real dryer, a real load cell or a real reader module could
differ on is a parameter at the top of its section, and the two or three that
are worth measuring before printing say so. The geometry is built with mesh
booleans and every part is checked for being a closed solid before it is
written, so a bad parameter fails here rather than on the printer.

    python3 -m venv .venv
    .venv/bin/pip install trimesh manifold3d shapely rtree
    .venv/bin/python generate_parts.py

Writes stl/*.stl and prints a table of sizes and estimated masses. With
--preview, and matplotlib installed, it also writes preview/*.png.
"""

import os
import sys

import numpy as np
import trimesh
from shapely.geometry import Point
from shapely.geometry import box as shapely_box
from shapely.ops import unary_union

HERE = os.path.dirname(os.path.abspath(__file__))
STL_DIR = os.path.join(HERE, "stl")
PREVIEW_DIR = os.path.join(HERE, "preview")

# Print bed of the Prusa Mini+, which is what these are sized for.
BED = (180.0, 180.0, 180.0)

# PETG, which is the material these want. Print settings are in README.md.
DENSITY_G_PER_CM3 = 1.27


# ---------------------------------------------------------------------------
# Geometry helpers
#
# Everything is built from prisms with a 2D profile, which is how printed parts
# actually want to be shaped: constant cross-section in the print direction, no
# support material, no overhangs to argue about.
# ---------------------------------------------------------------------------


def rounded_rect(width, height, radius=1.5, at=(0.0, 0.0)):
    """A rectangle with rounded corners, centred on `at`."""
    radius = min(radius, width / 2 - 0.01, height / 2 - 0.01)
    core = shapely_box(
        at[0] - width / 2 + radius,
        at[1] - height / 2 + radius,
        at[0] + width / 2 - radius,
        at[1] + height / 2 - radius,
    )
    return core.buffer(radius, resolution=8, join_style=1)


def pocket_profile(width, depth, relief=1.3):
    """A rectangular pocket for a rectangular part.

    An extruded corner is never sharp, whether it is milled or printed, so a
    pocket built from a plain rectangle always fouls the corners of the thing it
    is meant to hold. Small circles at the corners give the corner somewhere to
    go, which is the same trick a machinist uses.
    """
    profile = rounded_rect(width, depth, 1.0)
    corners = [
        Point(sx * width / 2, sy * depth / 2).buffer(relief, resolution=8)
        for sx in (-1, 1)
        for sy in (-1, 1)
    ]
    return unary_union([profile] + corners)


def prism(profile, thickness, z0=0.0):
    """Extrude a 2D profile into a solid sitting between z0 and z0+thickness."""
    solid = trimesh.creation.extrude_polygon(profile, height=thickness)
    solid.apply_translation((0.0, 0.0, z0))
    return solid


def block(width, depth, height, at=(0.0, 0.0, 0.0), radius=0.0):
    """A box, positioned by the centre of its base."""
    profile = (
        rounded_rect(width, depth, radius, (at[0], at[1]))
        if radius > 0
        else shapely_box(
            at[0] - width / 2, at[1] - depth / 2, at[0] + width / 2, at[1] + depth / 2
        )
    )
    return prism(profile, height, at[2])


def rod(diameter, height, at=(0.0, 0.0, 0.0), sections=48):
    """A cylinder, positioned by the centre of its base."""
    solid = trimesh.creation.cylinder(
        radius=diameter / 2.0, height=height, sections=sections
    )
    solid.apply_translation((at[0], at[1], at[2] + height / 2.0))
    return solid


def union(*solids):
    return trimesh.boolean.union(list(solids))


def diff(base, *cuts):
    return trimesh.boolean.difference([base] + list(cuts))


# ---------------------------------------------------------------------------
# The reader bracket
# ---------------------------------------------------------------------------

# Measure your module. The boards on sale are 69-70 x 37-40 mm; this is the
# only number in this file that a different purchase is likely to change.
MODULE = dict(x=70.0, y=40.0, thickness=1.6)

MODULE_FIT = 0.4      # clearance per side, so the board drops in
BRACKET_WALL = 2.4
BRACKET_LIP = 2.4     # material between the board and the lid
BRACKET_RIM = 6.0     # how far the lip reaches in from the pocket edge
BRACKET_EAR = (20.0, 14.0)
M3_CLEAR = 3.4
M3_PILOT = 2.9        # for a self-tapping screw into printed plastic
CLAMP_POST_D = 8.0
CLAMP_RISE = 3.0      # how far the clamp post stands above the board
HEADER_NOTCH = (26.0, 5.0)


def bracket_dimensions():
    pocket_x = MODULE["x"] + 2 * MODULE_FIT
    pocket_y = MODULE["y"] + 2 * MODULE_FIT
    board_top = BRACKET_LIP + MODULE["thickness"]
    return dict(
        pocket_x=pocket_x,
        pocket_y=pocket_y,
        outer_x=pocket_x + 2 * BRACKET_WALL,
        outer_y=pocket_y + 2 * BRACKET_WALL,
        board_top=board_top,
        # The walls stop a hair below the board so the clamps bear on the board
        # and not on the walls.
        wall_top=board_top - 0.3,
        post_top=board_top + CLAMP_RISE,
        post_x=pocket_x / 2 + CLAMP_POST_D / 2 - 1.0,
        ear_y=pocket_y / 2 + BRACKET_WALL + BRACKET_EAR[1] / 2 - 3.0,
    )


def build_antenna_bracket():
    """A tray for the reader module that screws or tapes to the lid.

    Flat side against the lid, module dropped in from behind, two clamps over
    its short edges. Nothing snaps: a snap fit would have to be tuned to one
    board thickness and one printer's tolerances, and a screwed clamp holds any
    module a lot more predictably.
    """
    dim = bracket_dimensions()

    outline = unary_union(
        [
            rounded_rect(dim["outer_x"], dim["outer_y"], 3.0),
            rounded_rect(*BRACKET_EAR, 3.0, (0.0, dim["ear_y"])),
            rounded_rect(*BRACKET_EAR, 3.0, (0.0, -dim["ear_y"])),
            rounded_rect(CLAMP_POST_D, CLAMP_POST_D, CLAMP_POST_D / 2 - 0.01,
                         (dim["post_x"], 0.0)),
            rounded_rect(CLAMP_POST_D, CLAMP_POST_D, CLAMP_POST_D / 2 - 0.01,
                         (-dim["post_x"], 0.0)),
        ]
    )
    pocket = pocket_profile(dim["pocket_x"], dim["pocket_y"])

    # The lip, with a window through it. The window is not needed for radio --
    # 2.4 mm of PETG is invisible at 13.56 MHz -- but it saves plastic, lets the
    # module's own LEDs be seen, and keeps the coil out of contact with the lid.
    window = rounded_rect(
        dim["pocket_x"] - 2 * BRACKET_RIM, dim["pocket_y"] - 2 * BRACKET_RIM, 4.0
    )
    body = prism(outline.difference(window), BRACKET_LIP)

    # Walls around the pocket, and the two clamp posts.
    body = union(
        body,
        prism(
            outline.difference(pocket),
            dim["wall_top"] - BRACKET_LIP,
            BRACKET_LIP,
        ),
        rod(CLAMP_POST_D, dim["post_top"], (dim["post_x"], 0.0, 0.0)),
        rod(CLAMP_POST_D, dim["post_top"], (-dim["post_x"], 0.0, 0.0)),
    )

    # A notch in one wall for whatever the module's header carries, and the
    # holes: pilots in the posts, clearance in the ears.
    body = diff(
        body,
        block(
            HEADER_NOTCH[0],
            2 * BRACKET_WALL + 2.0,
            dim["post_top"],
            (0.0, dim["pocket_y"] / 2 + BRACKET_WALL / 2, BRACKET_LIP),
        ),
        rod(M3_PILOT, dim["post_top"], (dim["post_x"], 0.0, 1.2)),
        rod(M3_PILOT, dim["post_top"], (-dim["post_x"], 0.0, 1.2)),
        rod(M3_CLEAR, BRACKET_LIP * 3, (0.0, dim["ear_y"], -1.0)),
        rod(M3_CLEAR, BRACKET_LIP * 3, (0.0, -dim["ear_y"], -1.0)),
    )
    return body


def build_antenna_clamp():
    """Print two. Each one screws to a post and steps down onto the board."""
    dim = bracket_dimensions()
    bar_length = CLAMP_POST_D + 6.0
    width = 12.0
    bar_thickness = 3.0
    step = dim["post_top"] - dim["board_top"]

    # Local origin is the screw axis, x pointing in towards the board.
    bar = block(bar_length, width, bar_thickness, (-1.0, 0.0, step))
    foot_x = -bar_length / 2 - 1.0 + 3.0 / 2
    foot = block(3.0, width, step, (foot_x, 0.0, 0.0))
    return diff(union(bar, foot), rod(M3_CLEAR, 20.0, (0.0, 0.0, -1.0)))


# ---------------------------------------------------------------------------
# The scale sandwich
# ---------------------------------------------------------------------------

# A TAL220-class 5 kg single point cell. Measure the hole pitch before printing:
# the body size is consistent across sellers, the threads are not always M5.
CELL = dict(width=12.7, hole_pitch=15.0, thread_clearance=5.4)

MOUNT_LENGTH = 34.0
MOUNT_WIDTH = 20.0
MOUNT_HEIGHT = 10.0
MOUNT_RECESS = 3.0    # depth of the channel the cell end sits in

# Stack heights, which set what the overload stop and the feet have to match.
CELL_BODY_HEIGHT = 12.7
PLATFORM_GAP = 2 * (MOUNT_HEIGHT - MOUNT_RECESS) + CELL_BODY_HEIGHT
OVERLOAD_TRAVEL = 1.5  # how far the platform may sink before it hits the stop


def build_cell_mount():
    """Print two. The same part goes under the fixed end and over the load end.

    Both ends of a single point cell bolt to a flat surface with the same hole
    pattern, so one spacer serves both, flipped over: recess up under the base,
    recess down under the platform. The spacer's job is to hold the cell's two
    ends apart from the surfaces they bolt to, since the beam has to be free to
    bend for the cell to weigh anything at all.
    """
    body = block(MOUNT_LENGTH, MOUNT_WIDTH, MOUNT_HEIGHT, radius=2.0)

    # The channel the cell drops into: locates it across its width, and leaves
    # both ends open so the cell's cable is never pinched.
    channel = block(
        MOUNT_LENGTH + 2.0,
        CELL["width"] + 0.3,
        MOUNT_RECESS,
        (0.0, 0.0, MOUNT_HEIGHT - MOUNT_RECESS),
    )

    holes = [
        rod(
            CELL["thread_clearance"],
            MOUNT_HEIGHT + 2.0,
            (offset * CELL["hole_pitch"] / 2, 0.0, -1.0),
        )
        for offset in (-1, 1)
    ]
    return diff(body, channel, *holes)


def build_overload_stop():
    """A pillar under the platform that catches it if somebody leans on it.

    The cell is rated at 5 kg and carries about 2.5 kg here, so the stop is
    insurance rather than routine protection: it limits travel to a millimetre
    and a half, which is several times the cell's own deflection at full load
    and nowhere near enough to bend anything.
    """
    height = PLATFORM_GAP - OVERLOAD_TRAVEL
    body = block(24.0, 20.0, height, radius=2.0)
    # Pilot holes for two M4 screws driven up through the base board. The screws
    # have to come from below: once the platform is on, nothing above the pillar
    # is reachable, and there is no room up there for a screwdriver anyway.
    holes = [
        rod(3.4, height * 0.6, (offset * 8.0, 0.0, -0.5)) for offset in (-1, 1)
    ]
    return diff(body, *holes)


# The dryer's own rubber feet sit in these. Measure yours: 20 mm across is
# typical, and anything larger than the pocket will perch on the rim instead of
# dropping in, which works but slides more easily.
DRYER_FOOT_D = 20.0
FOOT_POCKET_D = DRYER_FOOT_D + 2.0
FOOT_BORE_D = 18.0
FOOT_OUTER_D = 26.0
FOOT_FLANGE_D = 34.0
FOOT_FLANGE_H = 2.5
FOOT_HEIGHT = 22.0
FOOT_POCKET_DEPTH = 3.0


def build_isolator_foot():
    """Print four. These are the whole thermal isolation strategy.

    The dryer runs at 70 C and stands on the platform the load cell carries, so
    heat has a short path into the cell, and the cell's zero moves with its
    temperature. What breaks that path is making the load path long and thin: an
    open column, bored right through, that carries the dryer on a narrow rim at
    the top. Four of these come to roughly 11 mW/K, so a 48 K difference pushes
    about half a watt into the platform -- little enough that the firmware's
    compensation is trimming a small residual rather than chasing the heater.

    Being open at both ends is also why it prints without a single overhang:
    every surface is either vertical or facing up.
    """
    body = union(
        rod(FOOT_FLANGE_D, FOOT_FLANGE_H),
        rod(FOOT_OUTER_D, FOOT_HEIGHT),
    )
    return diff(
        body,
        rod(FOOT_BORE_D, FOOT_HEIGHT + 2.0, (0.0, 0.0, -1.0)),
        rod(
            FOOT_POCKET_D,
            FOOT_POCKET_DEPTH + 1.0,
            (0.0, 0.0, FOOT_HEIGHT - FOOT_POCKET_DEPTH),
        ),
    )


# ---------------------------------------------------------------------------
# Checks
#
# A part that builds without error can still be wrong: a pocket a millimetre
# too small, a hole that missed, a recess deep enough to break through. These
# ask the finished mesh where its material actually is.
# ---------------------------------------------------------------------------


def assert_material(part, name, expected, points):
    """`expected` is True where there must be plastic, False where there must
    not be. Points are (x, y, z) with z measured from the part's own base."""
    inside = part.contains(np.array(points, dtype=float))
    for point, actual in zip(points, inside):
        if bool(actual) != expected:
            where = ", ".join(f"{value:.1f}" for value in point)
            wanted = "material" if expected else "clearance"
            raise SystemExit(f"{name}: expected {wanted} at ({where})")


def verify_antenna_bracket(part, name):
    dim = bracket_dimensions()
    board_z = BRACKET_LIP + MODULE["thickness"] / 2

    # The module's own corners have to be clear of the walls, or it will not
    # drop into the pocket.
    corners = [
        (sx * MODULE["x"] / 2, sy * MODULE["y"] / 2, board_z)
        for sx in (-1, 1)
        for sy in (-1, 1)
    ]
    assert_material(part, name, False, corners)

    assert_material(
        part,
        name,
        True,
        [
            # Walls either side of the pocket, and the lip the board rests on.
            (dim["pocket_x"] / 2 + BRACKET_WALL / 2, 0.0, board_z),
            (-dim["pocket_x"] / 2 - BRACKET_WALL / 2, 0.0, board_z),
            (0.0, dim["pocket_y"] / 2 - BRACKET_RIM / 2, BRACKET_LIP / 2),
            # Clamp posts, standing above the board.
            (dim["post_x"] + CLAMP_POST_D / 4, 0.0, dim["board_top"] + 1.0),
        ],
    )
    assert_material(
        part,
        name,
        False,
        [
            # Pilot holes down the posts, window through the lip, ear holes.
            (dim["post_x"], 0.0, dim["board_top"]),
            (0.0, 0.0, BRACKET_LIP / 2),
            (0.0, dim["ear_y"], BRACKET_LIP / 2),
            (0.0, -dim["ear_y"], BRACKET_LIP / 2),
            # The header notch, cut through one wall above the lip.
            (0.0, dim["pocket_y"] / 2 + BRACKET_WALL / 2, board_z),
        ],
    )


def verify_antenna_clamp(part, name):
    dim = bracket_dimensions()
    step = dim["post_top"] - dim["board_top"]
    if abs(part.extents[2] - (step + 3.0)) > 0.01:
        raise SystemExit(f"{name}: does not span the step down to the board")
    # Screw clearance through the bar, and a foot at the far end that reaches
    # all the way down to where the board's top surface will be.
    assert_material(part, name, False, [(0.0, 0.0, step + 1.5)])
    assert_material(part, name, True, [(-(CLAMP_POST_D + 6.0) / 2 + 0.5, 0.0, 0.5)])


def verify_cell_mount(part, name):
    assert_material(
        part,
        name,
        True,
        [
            (0.0, 0.0, (MOUNT_HEIGHT - MOUNT_RECESS) / 2),
            (0.0, MOUNT_WIDTH / 2 - 1.0, MOUNT_HEIGHT - MOUNT_RECESS / 2),
        ],
    )
    assert_material(
        part,
        name,
        False,
        [
            # The channel the cell sits in, at its full width.
            (0.0, CELL["width"] / 2, MOUNT_HEIGHT - MOUNT_RECESS / 2),
            # Both bolt holes, right through.
            (CELL["hole_pitch"] / 2, 0.0, 0.5),
            (-CELL["hole_pitch"] / 2, 0.0, 0.5),
        ],
    )


def verify_overload_stop(part, name):
    height = PLATFORM_GAP - OVERLOAD_TRAVEL
    if abs(part.extents[2] - height) > 0.01:
        raise SystemExit(f"{name}: wrong height for the platform gap")
    assert_material(
        part,
        name,
        True,
        [
            (0.0, 0.0, height / 2),
            # The top face has to be solid: it is what the platform lands on.
            (8.0, 0.0, height - 1.0),
        ],
    )
    assert_material(part, name, False, [(8.0, 0.0, 1.0), (-8.0, 0.0, 1.0)])


def verify_isolator_foot(part, name):
    rim = (FOOT_BORE_D + FOOT_POCKET_D) / 4  # midway across the rim the dryer sits on
    assert_material(
        part,
        name,
        True,
        [
            # The column wall, the flange, and the rim itself.
            ((FOOT_BORE_D + FOOT_OUTER_D) / 4, 0.0, FOOT_HEIGHT / 2),
            (FOOT_OUTER_D / 2 + 1.0, 0.0, FOOT_FLANGE_H / 2),
            (rim, 0.0, FOOT_HEIGHT - FOOT_POCKET_DEPTH - 1.0),
        ],
    )
    assert_material(
        part,
        name,
        False,
        [
            (0.0, 0.0, FOOT_HEIGHT / 2),          # bored through, for low conduction
            (0.0, 0.0, 0.5),
            (rim, 0.0, FOOT_HEIGHT - 0.5),        # pocket the dryer's foot drops into
        ],
    )


# Parts that are modelled the way they are assembled but printed the other way
# up. The clamp is the only one: assembled, its foot points down at the board,
# and printing it that way would leave the bar overhanging on nothing.
PRINT_UPSIDE_DOWN = {"antenna_clamp"}

PARTS = [
    ("antenna_bracket", 1, build_antenna_bracket, verify_antenna_bracket),
    ("antenna_clamp", 2, build_antenna_clamp, verify_antenna_clamp),
    ("cell_mount", 2, build_cell_mount, verify_cell_mount),
    ("overload_stop", 1, build_overload_stop, verify_overload_stop),
    ("isolator_foot", 4, build_isolator_foot, verify_isolator_foot),
]


def check(part, name, quantity):
    """Refuse to write anything that is not a closed solid that fits the bed."""
    problems = []
    if not part.is_volume:
        problems.append(
            "not a closed solid (watertight: {}, consistent winding: {})".format(
                part.is_watertight, part.is_winding_consistent
            )
        )
    size = part.extents
    if any(size[axis] > BED[axis] for axis in range(3)):
        problems.append(f"does not fit the bed at {size}")
    if problems:
        raise SystemExit(f"{name}: " + "; ".join(problems))

    return dict(
        name=name,
        quantity=quantity,
        size=size,
        grams=part.volume / 1000.0 * DENSITY_G_PER_CM3,
    )


def render_preview(part, name):
    """A shaded view of the part, so a change can be reviewed in a diff."""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection

    light = np.array([0.3, -0.5, 0.8])
    light /= np.linalg.norm(light)
    shade = 0.4 + 0.6 * np.clip(part.face_normals @ light, 0.0, 1.0)
    colors = np.stack(
        [shade * 0.35, shade * 0.55, shade * 0.78, np.ones_like(shade)], axis=1
    )

    figure = plt.figure(figsize=(5.0, 4.0), dpi=110)
    axes = figure.add_subplot(projection="3d")
    axes.add_collection3d(Poly3DCollection(part.triangles, facecolors=colors))
    low, high = part.bounds
    axes.set_xlim(low[0], high[0])
    axes.set_ylim(low[1], high[1])
    axes.set_zlim(low[2], high[2])
    axes.set_box_aspect(tuple(float(value) for value in part.extents))
    axes.view_init(elev=32, azim=-58)
    axes.set_axis_off()
    figure.tight_layout()
    figure.savefig(os.path.join(PREVIEW_DIR, f"{name}.png"))
    plt.close(figure)


def main():
    preview = "--preview" in sys.argv[1:]
    os.makedirs(STL_DIR, exist_ok=True)
    if preview:
        os.makedirs(PREVIEW_DIR, exist_ok=True)
    rows = []
    for name, quantity, builder, verifier in PARTS:
        part = builder()
        # The checks are written in the part's own coordinates, so they run
        # before the part is moved onto the bed.
        verifier(part, name)
        rows.append(check(part, name, quantity))

        if name in PRINT_UPSIDE_DOWN:
            part.apply_transform(
                trimesh.transformations.rotation_matrix(np.pi, (1.0, 0.0, 0.0))
            )

        # Sit the part on the bed and centre it, which is what a slicer wants.
        part.apply_translation(
            [
                -(part.bounds[0][0] + part.extents[0] / 2.0),
                -(part.bounds[0][1] + part.extents[1] / 2.0),
                -part.bounds[0][2],
            ]
        )
        part.export(os.path.join(STL_DIR, f"{name}.stl"))
        if preview:
            render_preview(part, name)

    width = max(len(row["name"]) for row in rows)
    print(f"{'part':<{width}}  qty   size (mm)              mass")
    total = 0.0
    for row in rows:
        size = " x ".join(f"{value:.1f}" for value in row["size"])
        grams = row["grams"] * row["quantity"]
        total += grams
        print(f"{row['name']:<{width}}  {row['quantity']:>3}   {size:<22} {grams:5.1f} g")
    print(f"{'':<{width}}                              {total:5.1f} g total")
    print(f"\nwrote {len(rows)} files to {os.path.relpath(STL_DIR, os.getcwd())}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())

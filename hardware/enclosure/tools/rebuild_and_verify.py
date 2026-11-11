#!/usr/bin/env python3
"""Parametric SolidWorks build, interference check, and export pipeline for the
Power Outage Monitor enclosure.

Builds from scratch:
1. Case_Bottom.SLDPRT
2. Case_Lid.SLDPRT
3. Cable_Clamp.SLDPRT
4. ESP32_38P_TypeC.SLDPRT
5. Power_Outage_Monitor_Case.SLDASM (opened with read-only LM2596_module.SLDPRT)

Exports STEP (AP214) to ../step, assembly-frame STLs to ../stl-assembly-frame and
assembly renders to docs/img, then runs print_orient.py, build_plate.py, and
stl_gate.py to produce and verify the print-ready STLs in ../stl.
"""

import argparse
from dataclasses import dataclass
import math
import os
import subprocess
import sys

import pythoncom

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import sw_api
from sw_api import M

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(line_buffering=True)

HOLE_AXIS_Y_MIN = 0.99
AXIS_MATCH_TOL_MM = 0.05
MM3_PER_M3 = 1.0 / (M ** 3)
PLATE_SHELL_COUNT = 3
PRINTABLE_STL_SPECS = (
    ("Case_Bottom.stl", 1),
    ("Case_Lid.stl", 1),
    ("Cable_Clamp.stl", 1),
    ("Power_Outage_Monitor_All_Parts_Plate.stl", PLATE_SHELL_COUNT),
)


@dataclass(frozen=True)
class EnclosureParams:
    """Single source of truth for enclosure and mockup dimensions (millimetres)."""

    # Outer shell
    case_outer_length_mm: float = 126.0       # Total enclosure X length
    case_outer_width_mm: float = 48.0         # Total enclosure Z width
    case_body_height_mm: float = 25.5         # Case bottom outer Y height (lid seating plane)
    case_floor_thickness_mm: float = 2.5      # Bottom wall Y thickness
    case_wall_thickness_mm: float = 3.0       # Perimeter side wall X/Z thickness
    case_bed_chamfer_mm: float = 1.5          # Print-bed perimeter chamfer distance
    case_bed_chamfer_deg: float = 45.0        # Print-bed perimeter chamfer angle (deg)

    # Lid and alignment lip
    lid_thickness_mm: float = 2.5             # Lid top plate Y thickness
    lid_top_chamfer_mm: float = 1.0           # Lid top perimeter chamfer distance
    lid_top_chamfer_deg: float = 45.0         # Lid top perimeter chamfer angle (deg)
    lid_lip_depth_mm: float = 1.2             # Underside alignment lip Y depth below lid plane
    lid_lip_outer_clearance_mm: float = 0.3   # Radial clearance between lip and case inner wall
    lid_lip_thickness_mm: float = 1.6         # Alignment lip wall thickness
    lid_lip_boss_clearance_r_mm: float = 4.5  # Corner boss cutout radius in alignment lip
    lid_lip_boss_clearance_cut_mm: float = 1.5  # Cut depth for corner boss lip clearance

    # Corner M3 screw bosses and lid fasteners
    corner_boss_x_mm: float = 56.0            # Corner boss X offset from centre
    corner_boss_z_mm: float = 18.0            # Corner boss Z offset from centre
    corner_boss_dia_mm: float = 6.8           # Corner boss outer diameter
    pilot_hole_dia_mm: float = 2.4            # M3 self-tapping pilot hole diameter
    corner_pilot_depth_mm: float = 13.0       # Corner boss pilot hole blind depth
    lid_screw_hole_dia_mm: float = 3.4        # Lid M3 clearance hole diameter
    lid_countersink_dia_mm: float = 6.2       # Lid screw head counterbore/countersink diameter
    lid_countersink_depth_mm: float = 1.5     # Lid screw head recess depth

    # LM2596 buck converter mounting bosses (model X, Z coordinates)
    lm2596_boss_1_ref_x_mm: float = -39.59    # Unshifted LM2596 boss 1 X centre
    lm2596_boss_1_z_mm: float = -8.21         # LM2596 boss 1 model Z centre
    lm2596_boss_2_ref_x_mm: float = -10.59    # Unshifted LM2596 boss 2 X centre
    lm2596_boss_2_z_mm: float = 8.21          # LM2596 boss 2 model Z centre
    lm2596_x_shift_mm: float = 3.0            # +X shift leaving ~3.9 mm wire clearance to clamp bar
    lm2596_yaw_deg: float = 180.0             # Yaw about Y so IN pads face -X cable entry
    lm2596_boss_dia_mm: float = 5.6           # LM2596 boss outer diameter
    lm2596_boss_height_mm: float = 6.5        # LM2596 boss top Y height from Y=0
    lm2596_pilot_depth_mm: float = 5.0        # LM2596 boss pilot hole blind depth
    lm2596_hole_dia_mm: float = 3.0           # Expected LM2596 PCB mounting hole diameter

    @property
    def lm2596_boss_1_x_mm(self) -> float:
        return self.lm2596_boss_1_ref_x_mm + self.lm2596_x_shift_mm

    @property
    def lm2596_boss_2_x_mm(self) -> float:
        return self.lm2596_boss_2_ref_x_mm + self.lm2596_x_shift_mm

    # Cable strain-relief clamp and bosses
    clamp_x_mm: float = -51.5                 # Clamp and boss X centre
    clamp_boss_z_mm: float = 7.5              # Clamp boss Z offset from centre
    clamp_boss_dia_mm: float = 5.6            # Clamp boss outer diameter
    clamp_boss_height_mm: float = 6.0         # Clamp boss top Y height from Y=0
    clamp_pilot_depth_mm: float = 5.0         # Clamp boss pilot hole blind depth
    clamp_pedestal_half_x_mm: float = 3.0     # Cable support pedestal X half-width
    clamp_pedestal_half_z_mm: float = 3.0     # Cable support pedestal Z half-width
    clamp_pedestal_height_mm: float = 4.0     # Cable support pedestal Y height
    clamp_bar_length_x_mm: float = 7.0        # Clamp bar X length
    clamp_bar_width_z_mm: float = 24.0        # Clamp bar Z width
    clamp_bar_thickness_mm: float = 4.5       # Clamp bar Y thickness
    clamp_pad_width_z_mm: float = 5.0         # Central cable pressing pad Z width
    clamp_pad_depth_mm: float = 0.5           # Pressing pad downward Y protrusion
    clamp_screw_hole_dia_mm: float = 3.4      # Clamp M3 clearance hole diameter
    clamp_counterbore_dia_mm: float = 6.0     # Clamp screw head counterbore diameter
    clamp_counterbore_depth_mm: float = 2.0   # Clamp screw head counterbore depth

    # Left-wall cable entry slot
    cable_slot_width_mm: float = 4.4          # Cable entry slot Z width
    cable_slot_bottom_height_mm: float = 4.0  # Cable entry slot floor Y height from Y=0
    cable_slot_cut_margin_mm: float = 1.0     # Sketch overhang beyond wall faces for clean cut

    # ESP32 cradle, right-wall pocket, and USB Type-C cutout
    esp32_cradle_x_min_mm: float = 8.0        # Cradle inner-left X edge
    esp32_cradle_outer_half_z_mm: float = 16.0  # Cradle outer Z half-width
    esp32_cradle_inner_x_min_mm: float = 12.0   # Cradle cutout X min
    esp32_cradle_inner_x_max_mm: float = 58.0   # Cradle cutout X max
    esp32_cradle_inner_half_z_mm: float = 13.5  # Cradle cutout Z half-width
    esp32_cradle_height_mm: float = 9.0         # Cradle top Y plane (PCB bottom surface)
    esp32_pocket_width_z_mm: float = 28.4       # Right-wall PCB pocket Z width
    esp32_pocket_height_y_mm: float = 7.0       # Right-wall PCB pocket Y height
    esp32_pocket_depth_mm: float = 2.0          # Right-wall PCB pocket X cut depth
    usb_cutout_width_z_mm: float = 9.5          # USB Type-C portal Z width
    usb_cutout_y_min_mm: float = 10.5           # USB Type-C portal bottom Y height
    usb_cutout_y_max_mm: float = 14.3           # USB Type-C portal top Y height
    usb_cutout_cut_depth_mm: float = 2.0        # USB Type-C portal X cut depth

    # ESP32 38-pin Type-C mockup dimensions
    esp32_pcb_x_min_mm: float = 9.0             # Mockup PCB left X edge
    esp32_pcb_x_max_mm: float = 61.0            # Mockup PCB right X edge (seated in pocket)
    esp32_pcb_half_z_mm: float = 14.0           # Mockup PCB Z half-width
    esp32_pcb_thickness_mm: float = 1.6         # Mockup PCB Y thickness
    esp32_shield_x_min_mm: float = 16.0         # Mockup RF shield X min
    esp32_shield_x_max_mm: float = 34.0         # Mockup RF shield X max
    esp32_shield_half_z_mm: float = 9.0         # Mockup RF shield Z half-width
    esp32_shield_height_mm: float = 3.2         # Mockup RF shield Y height
    esp32_usb_x_min_mm: float = 59.2            # Mockup Type-C shell X min
    esp32_usb_x_max_mm: float = 62.4            # Mockup Type-C shell X max
    esp32_usb_half_z_mm: float = 4.45           # Mockup Type-C shell Z half-width
    esp32_usb_height_mm: float = 3.2            # Mockup Type-C shell Y height
    esp32_pin_x_min_mm: float = 14.0            # Mockup pin header X min
    esp32_pin_x_max_mm: float = 57.0            # Mockup pin header X max
    esp32_pin_z_inner_mm: float = 10.16         # Mockup pin header inner Z offset
    esp32_pin_z_outer_mm: float = 12.7          # Mockup pin header outer Z offset
    esp32_pin_length_mm: float = 5.8            # Mockup pin length below PCB


def rect_pts(u0, v0, u1, v1):
    """Build rectangle vertices in 2D sketch coordinates."""
    return [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]


def top_sketch_xy(x_mm, z_mm):
    """Convert model (X, Z) coordinates (mm) to SolidWorks Top-plane sketch (X, Y).

    SolidWorks Top-plane sketches map sketch +X to model +X and sketch +Y to model -Z.
    Every Top-plane (and Top-offset) sketch uses this conversion so Z signs stay aligned
    with model and assembly coordinates.
    """
    return (x_mm, -z_mm)


def top_rect_pts(xmin, zmin, xmax, zmax):
    """Build Top-plane sketch rectangle vertices from model (X, Z) bounds (mm)."""
    return [
        top_sketch_xy(xmin, zmin),
        top_sketch_xy(xmax, zmin),
        top_sketch_xy(xmax, zmax),
        top_sketch_xy(xmin, zmax),
    ]


def export(name, stl_raw_dir, step_dir):
    """Export active SolidWorks document to assembly-frame binary STL and AP214 STEP."""
    stl_path = os.path.join(stl_raw_dir, f"{name}.STL")
    step_path = os.path.join(step_dir, f"{name}.STEP")
    sw_api.stl_fine()
    _, _, _, size_stl = sw_api.save_as(stl_path)
    sw_api.step_ap214()
    _, _, _, size_step = sw_api.save_as(step_path)
    print(f"  Exported {name}: STL={size_stl} B, STEP={size_step} B")


def build_case_bottom(p, cad_dir, stl_raw_dir, step_dir):
    print("\n--- 1. Building Case_Bottom.SLDPRT ---")
    doc = sw_api.new_part()
    title = sw_api.prop(doc, "GetTitle")

    hx = p.case_outer_length_mm / 2.0
    hz = p.case_outer_width_mm / 2.0
    ix = hx - p.case_wall_thickness_mm
    iz = hz - p.case_wall_thickness_mm
    cavity_depth = p.case_body_height_mm - p.case_floor_thickness_mm
    slot_depth = p.case_body_height_mm - p.cable_slot_bottom_height_mm

    sk = sw_api.sketch_on("Top")
    sw_api.polygon(top_rect_pts(-hx, -hz, hx, hz))
    sw_api.sk_done()
    sw_api.extrude(sk, p.case_body_height_mm)

    bed_pts = [(-hx, 0.0, 0.0), (hx, 0.0, 0.0), (0.0, 0.0, -hz), (0.0, 0.0, hz)]
    sw_api.chamfer_points(bed_pts, p.case_bed_chamfer_mm, p.case_bed_chamfer_deg)

    pl_top = sw_api.plane_offset("Top", p.case_body_height_mm)
    sk_cav = sw_api.sketch_on(pl_top)
    sw_api.polygon(top_rect_pts(-ix, -iz, ix, iz))
    sw_api.sk_done()
    sw_api.cut_extrude(sk_cav, cavity_depth)

    sk_boss = sw_api.sketch_on("Top")
    for bx in (-p.corner_boss_x_mm, p.corner_boss_x_mm):
        for bz in (-p.corner_boss_z_mm, p.corner_boss_z_mm):
            sw_api.circle(*top_sketch_xy(bx, bz), p.corner_boss_dia_mm / 2.0)
    sw_api.sk_done()
    sw_api.extrude(sk_boss, p.case_body_height_mm)

    sk_holes = sw_api.sketch_on(pl_top)
    for bx in (-p.corner_boss_x_mm, p.corner_boss_x_mm):
        for bz in (-p.corner_boss_z_mm, p.corner_boss_z_mm):
            sw_api.circle(*top_sketch_xy(bx, bz), p.pilot_hole_dia_mm / 2.0)
    sw_api.sk_done()
    sw_api.cut_extrude(sk_holes, p.corner_pilot_depth_mm)

    sk_lm = sw_api.sketch_on("Top")
    sw_api.circle(*top_sketch_xy(p.lm2596_boss_1_x_mm, p.lm2596_boss_1_z_mm), p.lm2596_boss_dia_mm / 2.0)
    sw_api.circle(*top_sketch_xy(p.lm2596_boss_2_x_mm, p.lm2596_boss_2_z_mm), p.lm2596_boss_dia_mm / 2.0)
    sw_api.sk_done()
    sw_api.extrude(sk_lm, p.lm2596_boss_height_mm)

    pl_lm = sw_api.plane_offset("Top", p.lm2596_boss_height_mm)
    sk_lm_h = sw_api.sketch_on(pl_lm)
    sw_api.circle(*top_sketch_xy(p.lm2596_boss_1_x_mm, p.lm2596_boss_1_z_mm), p.pilot_hole_dia_mm / 2.0)
    sw_api.circle(*top_sketch_xy(p.lm2596_boss_2_x_mm, p.lm2596_boss_2_z_mm), p.pilot_hole_dia_mm / 2.0)
    sw_api.sk_done()
    sw_api.cut_extrude(sk_lm_h, p.lm2596_pilot_depth_mm)

    sk_cl_boss = sw_api.sketch_on("Top")
    sw_api.circle(*top_sketch_xy(p.clamp_x_mm, -p.clamp_boss_z_mm), p.clamp_boss_dia_mm / 2.0)
    sw_api.circle(*top_sketch_xy(p.clamp_x_mm, p.clamp_boss_z_mm), p.clamp_boss_dia_mm / 2.0)
    sw_api.sk_done()
    sw_api.extrude(sk_cl_boss, p.clamp_boss_height_mm)

    sk_ped = sw_api.sketch_on("Top")
    sw_api.polygon(
        top_rect_pts(
            p.clamp_x_mm - p.clamp_pedestal_half_x_mm,
            -p.clamp_pedestal_half_z_mm,
            p.clamp_x_mm + p.clamp_pedestal_half_x_mm,
            p.clamp_pedestal_half_z_mm,
        )
    )
    sw_api.sk_done()
    sw_api.extrude(sk_ped, p.clamp_pedestal_height_mm)

    pl_cl = sw_api.plane_offset("Top", p.clamp_boss_height_mm)
    sk_cl_h = sw_api.sketch_on(pl_cl)
    sw_api.circle(*top_sketch_xy(p.clamp_x_mm, -p.clamp_boss_z_mm), p.pilot_hole_dia_mm / 2.0)
    sw_api.circle(*top_sketch_xy(p.clamp_x_mm, p.clamp_boss_z_mm), p.pilot_hole_dia_mm / 2.0)
    sw_api.sk_done()
    sw_api.cut_extrude(sk_cl_h, p.clamp_pilot_depth_mm)

    sk_cslot = sw_api.sketch_on(pl_top)
    sw_api.polygon(
        top_rect_pts(
            -hx - p.cable_slot_cut_margin_mm,
            -p.cable_slot_width_mm / 2.0,
            -ix + p.cable_slot_cut_margin_mm,
            p.cable_slot_width_mm / 2.0,
        )
    )
    sw_api.sk_done()
    sw_api.cut_extrude(sk_cslot, slot_depth)

    sk_esp = sw_api.sketch_on("Top")
    sw_api.polygon(top_rect_pts(p.esp32_cradle_x_min_mm, -p.esp32_cradle_outer_half_z_mm, ix, p.esp32_cradle_outer_half_z_mm))
    sw_api.polygon(
        top_rect_pts(
            p.esp32_cradle_inner_x_min_mm,
            -p.esp32_cradle_inner_half_z_mm,
            p.esp32_cradle_inner_x_max_mm,
            p.esp32_cradle_inner_half_z_mm,
        )
    )
    sw_api.sk_done()
    sw_api.extrude(sk_esp, p.esp32_cradle_height_mm)

    pl_in_wall = sw_api.plane_offset("Right", ix)
    sk_pocket = sw_api.sketch_on(pl_in_wall)
    sw_api.polygon(
        rect_pts(
            -p.esp32_pocket_width_z_mm / 2.0,
            p.esp32_cradle_height_mm,
            p.esp32_pocket_width_z_mm / 2.0,
            p.esp32_cradle_height_mm + p.esp32_pocket_height_y_mm,
        )
    )
    sw_api.sk_done()
    sw_api.cut_extrude(sk_pocket, p.esp32_pocket_depth_mm)

    pl_out_face = sw_api.plane_offset("Right", hx)
    sk_usb = sw_api.sketch_on(pl_out_face)
    sw_api.polygon(
        rect_pts(
            -p.usb_cutout_width_z_mm / 2.0,
            p.usb_cutout_y_min_mm,
            p.usb_cutout_width_z_mm / 2.0,
            p.usb_cutout_y_max_mm,
        )
    )
    sw_api.sk_done()
    sw_api.cut_extrude(sk_usb, p.usb_cutout_cut_depth_mm)

    sw_api.rebuild()
    path_part = os.path.join(cad_dir, "Case_Bottom.SLDPRT")
    _, _, _, size = sw_api.save_as(path_part)
    print(f"  Saved {path_part} ({size} B)")
    export("Case_Bottom", stl_raw_dir, step_dir)
    sw_api.close(title)
    return path_part


def build_case_lid(p, cad_dir, stl_raw_dir, step_dir):
    print("\n--- 2. Building Case_Lid.SLDPRT ---")
    doc = sw_api.new_part()
    title = sw_api.prop(doc, "GetTitle")

    hx = p.case_outer_length_mm / 2.0
    hz = p.case_outer_width_mm / 2.0
    ix = hx - p.case_wall_thickness_mm
    iz = hz - p.case_wall_thickness_mm
    lid_y0 = p.case_body_height_mm
    lid_y_top = lid_y0 + p.lid_thickness_mm

    lip_ox = ix - p.lid_lip_outer_clearance_mm
    lip_oz = iz - p.lid_lip_outer_clearance_mm
    lip_ix = lip_ox - p.lid_lip_thickness_mm
    lip_iz = lip_oz - p.lid_lip_thickness_mm

    pl_lid = sw_api.plane_offset("Top", lid_y0)
    sk_lid = sw_api.sketch_on(pl_lid)
    sw_api.polygon(top_rect_pts(-hx, -hz, hx, hz))
    sw_api.sk_done()
    sw_api.extrude(sk_lid, p.lid_thickness_mm)

    top_pts = [(-hx, lid_y_top, 0.0), (hx, lid_y_top, 0.0), (0.0, lid_y_top, -hz), (0.0, lid_y_top, hz)]
    sw_api.chamfer_points(top_pts, p.lid_top_chamfer_mm, p.lid_top_chamfer_deg)

    sk_lip = sw_api.sketch_on(pl_lid)
    sw_api.polygon(top_rect_pts(-lip_ox, -lip_oz, lip_ox, lip_oz))
    sw_api.polygon(top_rect_pts(-lip_ix, -lip_iz, lip_ix, lip_iz))
    sw_api.sk_done()
    sw_api.extrude(sk_lip, p.lid_lip_depth_mm, dir_flip=True)

    sk_lip_clear = sw_api.sketch_on(pl_lid)
    for bx in (-p.corner_boss_x_mm, p.corner_boss_x_mm):
        for bz in (-p.corner_boss_z_mm, p.corner_boss_z_mm):
            sw_api.circle(*top_sketch_xy(bx, bz), p.lid_lip_boss_clearance_r_mm)
    sw_api.sk_done()
    sw_api.cut_extrude(sk_lip_clear, p.lid_lip_boss_clearance_cut_mm)

    pl_lid_top = sw_api.plane_offset("Top", lid_y_top)
    sk_lh = sw_api.sketch_on(pl_lid_top)
    for bx in (-p.corner_boss_x_mm, p.corner_boss_x_mm):
        for bz in (-p.corner_boss_z_mm, p.corner_boss_z_mm):
            sw_api.circle(*top_sketch_xy(bx, bz), p.lid_screw_hole_dia_mm / 2.0)
    sw_api.sk_done()
    sw_api.cut_extrude(sk_lh, through=True)

    sk_lcb = sw_api.sketch_on(pl_lid_top)
    for bx in (-p.corner_boss_x_mm, p.corner_boss_x_mm):
        for bz in (-p.corner_boss_z_mm, p.corner_boss_z_mm):
            sw_api.circle(*top_sketch_xy(bx, bz), p.lid_countersink_dia_mm / 2.0)
    sw_api.sk_done()
    sw_api.cut_extrude(sk_lcb, p.lid_countersink_depth_mm)

    sw_api.rebuild()
    path_part = os.path.join(cad_dir, "Case_Lid.SLDPRT")
    _, _, _, size = sw_api.save_as(path_part)
    print(f"  Saved {path_part} ({size} B)")
    export("Case_Lid", stl_raw_dir, step_dir)
    sw_api.close(title)
    return path_part


def build_cable_clamp(p, cad_dir, stl_raw_dir, step_dir):
    print("\n--- 3. Building Cable_Clamp.SLDPRT ---")
    doc = sw_api.new_part()
    title = sw_api.prop(doc, "GetTitle")

    x0 = p.clamp_x_mm - p.clamp_bar_length_x_mm / 2.0
    x1 = p.clamp_x_mm + p.clamp_bar_length_x_mm / 2.0
    hz = p.clamp_bar_width_z_mm / 2.0
    clamp_y0 = p.clamp_boss_height_mm
    clamp_y_top = clamp_y0 + p.clamp_bar_thickness_mm

    pl_clamp = sw_api.plane_offset("Top", clamp_y0)
    sk_cl = sw_api.sketch_on(pl_clamp)
    sw_api.polygon(top_rect_pts(x0, -hz, x1, hz))
    sw_api.sk_done()
    sw_api.extrude(sk_cl, p.clamp_bar_thickness_mm)

    sk_pad = sw_api.sketch_on(pl_clamp)
    sw_api.polygon(top_rect_pts(x0, -p.clamp_pad_width_z_mm / 2.0, x1, p.clamp_pad_width_z_mm / 2.0))
    sw_api.sk_done()
    sw_api.extrude(sk_pad, p.clamp_pad_depth_mm, dir_flip=True)

    pl_clamp_top = sw_api.plane_offset("Top", clamp_y_top)
    sk_ch = sw_api.sketch_on(pl_clamp_top)
    sw_api.circle(*top_sketch_xy(p.clamp_x_mm, -p.clamp_boss_z_mm), p.clamp_screw_hole_dia_mm / 2.0)
    sw_api.circle(*top_sketch_xy(p.clamp_x_mm, p.clamp_boss_z_mm), p.clamp_screw_hole_dia_mm / 2.0)
    sw_api.sk_done()
    sw_api.cut_extrude(sk_ch, through=True)

    sk_ccb = sw_api.sketch_on(pl_clamp_top)
    sw_api.circle(*top_sketch_xy(p.clamp_x_mm, -p.clamp_boss_z_mm), p.clamp_counterbore_dia_mm / 2.0)
    sw_api.circle(*top_sketch_xy(p.clamp_x_mm, p.clamp_boss_z_mm), p.clamp_counterbore_dia_mm / 2.0)
    sw_api.sk_done()
    sw_api.cut_extrude(sk_ccb, p.clamp_counterbore_depth_mm)

    sw_api.rebuild()
    path_part = os.path.join(cad_dir, "Cable_Clamp.SLDPRT")
    _, _, _, size = sw_api.save_as(path_part)
    print(f"  Saved {path_part} ({size} B)")
    export("Cable_Clamp", stl_raw_dir, step_dir)
    sw_api.close(title)
    return path_part


def build_esp32_mockup(p, cad_dir, stl_raw_dir, step_dir):
    print("\n--- 4. Building ESP32_38P_TypeC.SLDPRT ---")
    doc = sw_api.new_part()
    title = sw_api.prop(doc, "GetTitle")

    pcb_y0 = p.esp32_cradle_height_mm
    pcb_y_top = pcb_y0 + p.esp32_pcb_thickness_mm

    pl_pcb = sw_api.plane_offset("Top", pcb_y0)
    sk_pcb = sw_api.sketch_on(pl_pcb)
    sw_api.polygon(top_rect_pts(p.esp32_pcb_x_min_mm, -p.esp32_pcb_half_z_mm, p.esp32_pcb_x_max_mm, p.esp32_pcb_half_z_mm))
    sw_api.sk_done()
    sw_api.extrude(sk_pcb, p.esp32_pcb_thickness_mm)

    pl_shield = sw_api.plane_offset("Top", pcb_y_top)
    sk_shield = sw_api.sketch_on(pl_shield)
    sw_api.polygon(
        top_rect_pts(p.esp32_shield_x_min_mm, -p.esp32_shield_half_z_mm, p.esp32_shield_x_max_mm, p.esp32_shield_half_z_mm)
    )
    sw_api.sk_done()
    sw_api.extrude(sk_shield, p.esp32_shield_height_mm)

    sk_usb = sw_api.sketch_on(pl_shield)
    sw_api.polygon(top_rect_pts(p.esp32_usb_x_min_mm, -p.esp32_usb_half_z_mm, p.esp32_usb_x_max_mm, p.esp32_usb_half_z_mm))
    sw_api.sk_done()
    sw_api.extrude(sk_usb, p.esp32_usb_height_mm)

    sk_pins = sw_api.sketch_on(pl_pcb)
    sw_api.polygon(top_rect_pts(p.esp32_pin_x_min_mm, -p.esp32_pin_z_outer_mm, p.esp32_pin_x_max_mm, -p.esp32_pin_z_inner_mm))
    sw_api.polygon(top_rect_pts(p.esp32_pin_x_min_mm, p.esp32_pin_z_inner_mm, p.esp32_pin_x_max_mm, p.esp32_pin_z_outer_mm))
    sw_api.sk_done()
    sw_api.extrude(sk_pins, p.esp32_pin_length_mm, dir_flip=True)

    sw_api.rebuild()
    path_part = os.path.join(cad_dir, "ESP32_38P_TypeC.SLDPRT")
    _, _, _, size = sw_api.save_as(path_part)
    print(f"  Saved {path_part} ({size} B)")
    export("ESP32_38P_TypeC", stl_raw_dir, step_dir)
    sw_api.close(title)
    return path_part


IDENTITY_ROT = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))


def _yaw_y_matrix(angle_deg):
    rad = math.radians(angle_deg)
    c = math.cos(rad)
    s = math.sin(rad)
    return ((c, 0.0, s), (0.0, 1.0, 0.0), (-s, 0.0, c))


def _apply_sw_xform(xf, pt_mm):
    x, y, z = pt_mm
    return (
        xf[0] * x + xf[3] * y + xf[6] * z + xf[9] / M,
        xf[1] * x + xf[4] * y + xf[7] * z + xf[10] / M,
        xf[2] * x + xf[5] * y + xf[8] * z + xf[11] / M,
    )


def _transform_box_mm(xf, box_m):
    lo = [v / M for v in box_m[:3]]
    hi = [v / M for v in box_m[3:6]]
    corners = [
        (x, y, z)
        for x in (lo[0], hi[0])
        for y in (lo[1], hi[1])
        for z in (lo[2], hi[2])
    ]
    pts = [_apply_sw_xform(xf, c) for c in corners]
    return [
        min(p[0] for p in pts), min(p[1] for p in pts), min(p[2] for p in pts),
        max(p[0] for p in pts), max(p[1] for p in pts), max(p[2] for p in pts),
    ]


def measure_lm2596_mount(doc_lm, hole_dia_mm):
    """Measure PCB bottom Y, mounting hole (X, Z) centres, and body boxes in part coords (mm)."""
    bodies = list(doc_lm.GetBodies2(sw_api.SW_BODY_SOLID, True) or [])
    pcb_body = next((b for b in bodies if b.Name == "PCB-1-solid7"), None)
    if pcb_body is None:
        raise RuntimeError("PCB-1-solid7 body not found in LM2596_module.SLDPRT")

    pcb_bb = pcb_body.GetBodyBox()
    pcb_y_min_mm = pcb_bb[1] / M
    target_r_mm = hole_dia_mm / 2.0

    holes = []
    for fc in (pcb_body.GetFaces() or []):
        surf = sw_api.prop(fc, "GetSurface")
        if sw_api.prop(surf, "Identity") != sw_api.SW_SURF_CYLINDER:
            continue
        params = surf.CylinderParams
        r_mm = params[6] / M
        ay = abs(params[4])
        if abs(r_mm - target_r_mm) > AXIS_MATCH_TOL_MM or ay < HOLE_AXIS_Y_MIN:
            continue
        hx, hz = params[0] / M, params[2] / M
        if not any(math.hypot(hx - ex, hz - ez) < AXIS_MATCH_TOL_MM for ex, ez in holes):
            holes.append((hx, hz))

    if len(holes) != 2:
        raise RuntimeError(f"Expected 2 mounting holes (dia={hole_dia_mm} mm), found {len(holes)}: {holes}")

    holes.sort(key=lambda pt: pt[0])
    body_boxes = [(b.Name, b.GetBodyBox()) for b in bodies]
    return pcb_y_min_mm, holes, body_boxes


def measure_case_bottom_lm2596_bosses(bottom_comp, bottom_xf, p):
    """Measure built LM2596 boss cylinder axes (X, Z) on Case_Bottom in assembly coords (mm)."""
    target_r_mm = p.lm2596_boss_dia_mm / 2.0
    min_lm_x_mm = p.clamp_x_mm + p.clamp_bar_length_x_mm / 2.0
    bosses = []
    for body in (bottom_comp.GetBodies2(sw_api.SW_BODY_SOLID) or []):
        for fc in (body.GetFaces() or []):
            surf = sw_api.prop(fc, "GetSurface")
            if sw_api.prop(surf, "Identity") != sw_api.SW_SURF_CYLINDER:
                continue
            params = surf.CylinderParams
            r_mm = params[6] / M
            ay = abs(params[4])
            if abs(r_mm - target_r_mm) > AXIS_MATCH_TOL_MM or ay < HOLE_AXIS_Y_MIN:
                continue
            face_box = sw_api.prop(fc, "GetBox")
            top_pt = _apply_sw_xform(bottom_xf, (params[0] / M, face_box[4] / M, params[2] / M))
            if abs(top_pt[1] - p.lm2596_boss_height_mm) > AXIS_MATCH_TOL_MM:
                continue
            if top_pt[0] <= min_lm_x_mm:
                continue
            bx, bz = top_pt[0], top_pt[2]
            if not any(math.hypot(bx - ex, bz - ez) < AXIS_MATCH_TOL_MM for ex, ez in bosses):
                bosses.append((bx, bz))

    if len(bosses) != 2:
        raise RuntimeError(
            f"Expected 2 Case_Bottom LM2596 boss cylinders (dia={p.lm2596_boss_dia_mm} mm, "
            f"H={p.lm2596_boss_height_mm} mm), found {len(bosses)}: {bosses}"
        )
    bosses.sort(key=lambda pt: pt[0])
    return bosses


def compute_lm2596_pose(p, pcb_y_min_mm, holes):
    """Compute rotation and translation (mm) placing rotated hole midpoint on boss midpoint."""
    rot = _yaw_y_matrix(p.lm2596_yaw_deg)
    hm_x = 0.5 * (holes[0][0] + holes[1][0])
    hm_z = 0.5 * (holes[0][1] + holes[1][1])
    bm_x = 0.5 * (p.lm2596_boss_1_x_mm + p.lm2596_boss_2_x_mm)
    bm_z = 0.5 * (p.lm2596_boss_1_z_mm + p.lm2596_boss_2_z_mm)

    rhm_x = rot[0][0] * hm_x + rot[0][2] * hm_z
    rhm_z = rot[2][0] * hm_x + rot[2][2] * hm_z
    t_mm = (
        bm_x - rhm_x,
        p.lm2596_boss_height_mm - pcb_y_min_mm,
        bm_z - rhm_z,
    )
    return rot, t_mm


def verify_assembly_metrics(p, inserted, lm_holes, lm_body_boxes):
    """Verify built Case_Bottom boss alignment to LM2596 holes, orientation, and clearances."""
    boxes = {}
    xforms = {}
    comps = {}
    for name, comp in inserted:
        box = comp.GetBox(False, False)
        boxes[name] = [v / M for v in box]
        xforms[name] = comp.Transform2.ArrayData
        comps[name] = comp

    lm_xf = xforms["LM2596_module"]
    bosses = measure_case_bottom_lm2596_bosses(comps["Case_Bottom"], xforms["Case_Bottom"], p)
    print("\n--- 6b. LM2596 Placement & Clearances ---")
    print(f"  Measured Case_Bottom boss axes (mm): {[(round(bx, 4), round(bz, 4)) for bx, bz in bosses]}")
    max_err = 0.0
    matched_bosses = set()
    for idx, (hx, hz) in enumerate(lm_holes, start=1):
        tx, _, tz = _apply_sw_xform(lm_xf, (hx, 0.0, hz))
        b_idx, nearest = min(
            enumerate(bosses),
            key=lambda item: math.hypot(tx - item[1][0], tz - item[1][1]),
        )
        matched_bosses.add(b_idx)
        err = math.hypot(tx - nearest[0], tz - nearest[1])
        max_err = max(max_err, err)
        print(
            f"  Hole {idx}: local=({hx:.4f}, {hz:.4f}) -> asm=({tx:.4f}, {tz:.4f}) mm, "
            f"measured_boss=({nearest[0]:.4f}, {nearest[1]:.4f}), offset={err:.4f} mm"
        )
    if len(matched_bosses) != len(bosses):
        raise RuntimeError("LM2596 mounting holes did not match distinct Case_Bottom bosses")
    if max_err > AXIS_MATCH_TOL_MM:
        raise RuntimeError(
            f"LM2596 hole-to-boss concentricity {max_err:.4f} mm exceeds {AXIS_MATCH_TOL_MM:.2f} mm"
        )

    key_prefixes = (
        "PCB-1-solid7",
        "CAPACITOR 10x10 SMD-2-solid2",
        "XL4015E1",
        "D-1-solid3",
        "Inductor-1-solid1",
        "Trimpot 3296W",
        "CAPACITOR 10x10 SMD-3-solid2",
    )
    for bname, b_box_m in lm_body_boxes:
        if bname.startswith(key_prefixes):
            tb = _transform_box_mm(lm_xf, b_box_m)
            print(
                f"  LM2596 body {bname[:34]:34s}: "
                f"X=[{tb[0]:7.3f}, {tb[3]:7.3f}], Y=[{tb[1]:6.3f}, {tb[4]:6.3f}], Z=[{tb[2]:7.3f}, {tb[5]:7.3f}]"
            )

    lm_box = boxes["LM2596_module"]
    clamp_box = boxes["Cable_Clamp"]
    esp_box = boxes["ESP32_38P_TypeC"]
    bottom_box = boxes["Case_Bottom"]
    lid_box = boxes["Case_Lid"]

    clamp_gap = lm_box[0] - clamp_box[3]
    cradle_gap = p.esp32_cradle_x_min_mm - lm_box[3]
    lm_lid_gap = p.case_body_height_mm - lm_box[4]
    esp_lid_gap = p.case_body_height_mm - esp_box[4]
    asm_size = (
        max(bottom_box[3], lid_box[3]) - min(bottom_box[0], lid_box[0]),
        max(bottom_box[4], lid_box[4]) - min(bottom_box[1], lid_box[1]),
        max(bottom_box[5], lid_box[5]) - min(bottom_box[2], lid_box[2]),
    )
    print(f"  Clearance LM2596 board edge to clamp bar: {clamp_gap:.3f} mm")
    print(f"  Clearance LM2596 board edge to ESP32 cradle: {cradle_gap:.3f} mm")
    print(f"  Clearance LM2596 tallest body to lid underside: {lm_lid_gap:.3f} mm")
    print(f"  Clearance ESP32 mockup top to lid underside: {esp_lid_gap:.3f} mm")
    print(f"  Assembled outer size (X x Z x Y): {asm_size[0]:.2f} x {asm_size[2]:.2f} x {asm_size[1]:.2f} mm")


def assemble(p, part_paths, cad_dir, step_dir):
    print("\n--- 5. Assembling Power_Outage_Monitor_Case.SLDASM ---")
    components_spec = [
        ("Case_Bottom", part_paths["Case_Bottom"], IDENTITY_ROT, (0.0, 0.0, 0.0), False),
        ("Cable_Clamp", part_paths["Cable_Clamp"], IDENTITY_ROT, (0.0, 0.0, 0.0), False),
        ("Case_Lid", part_paths["Case_Lid"], IDENTITY_ROT, (0.0, 0.0, 0.0), False),
        ("ESP32_38P_TypeC", part_paths["ESP32_38P_TypeC"], IDENTITY_ROT, (0.0, 0.0, 0.0), False),
    ]

    opened_titles = []
    drop_plan = []
    for name, path, rot, t_mm, read_only in components_spec:
        doc = sw_api.open_doc(path, doc_type=sw_api.SW_DOC_PART, read_only=read_only)
        opened_titles.append(sw_api.prop(doc, "GetTitle"))
        drop_plan.append((name, path, rot, t_mm))

    doc_lm = sw_api.open_doc(part_paths["LM2596_module"], doc_type=sw_api.SW_DOC_PART, read_only=True)
    opened_titles.append(sw_api.prop(doc_lm, "GetTitle"))
    pcb_y_min_mm, lm_holes, lm_body_boxes = measure_lm2596_mount(doc_lm, p.lm2596_hole_dia_mm)
    lm_rot, lm_t_mm = compute_lm2596_pose(p, pcb_y_min_mm, lm_holes)
    print(f"  LM2596 measured holes (mm): {[(round(x, 4), round(z, 4)) for x, z in lm_holes]}, PCB Y min={pcb_y_min_mm:.4f} mm")
    print(f"  LM2596 computed T_mm={[round(v, 4) for v in lm_t_mm]}, yaw={p.lm2596_yaw_deg:.1f} deg")
    drop_plan.append(("LM2596_module", part_paths["LM2596_module"], lm_rot, lm_t_mm))

    asm = sw_api.new_assembly()
    asm_title = asm.GetTitle
    sw_api.activate(asm_title)

    inserted = []
    for name, path, rot, t_mm in drop_plan:
        comp = sw_api.add_component_at_pose(asm, path, rot, t_mm)
        comp_name = comp.Name2
        asm.ClearSelection2(True)
        if not asm.Extension.SelectByID2(
            f"{comp_name}@{asm_title}", "COMPONENT", 0, 0, 0, False, 0, sw_api.callout(), 0
        ):
            raise RuntimeError(f"SelectByID2 failed for component {comp_name}@{asm_title}")
        asm.FixComponent()
        if not sw_api.prop(comp, "IsFixed"):
            raise RuntimeError(f"FixComponent failed for component {comp_name}")

        box = comp.GetBox(False, False)
        b_mm = [round(v / M, 2) for v in box]
        print(
            f"  + {comp_name:18}: X=[{b_mm[0]:6.2f}, {b_mm[3]:6.2f}], "
            f"Y=[{b_mm[1]:6.2f}, {b_mm[4]:6.2f}], Z=[{b_mm[2]:6.2f}, {b_mm[5]:6.2f}]"
        )
        inserted.append((name, comp))

    if not asm.ForceRebuild3(False):
        raise RuntimeError("Assembly ForceRebuild3 failed")
    check_interference(asm)
    verify_assembly_metrics(p, inserted, lm_holes, lm_body_boxes)

    asm_path = os.path.join(cad_dir, "Power_Outage_Monitor_Case.SLDASM")
    _, _, _, asm_size = sw_api.save_as(asm_path)
    asm_title = sw_api.prop(asm, "GetTitle")
    print(f"  Saved assembly: {asm_path} ({asm_size} B)")

    step_path = os.path.join(step_dir, "Power_Outage_Monitor_Case.STEP")
    sw_api.step_ap214()
    _, _, _, step_size = sw_api.save_as(step_path)
    print(f"  Exported assembly STEP: {step_path} ({step_size} B)")

    return asm, asm_title, inserted, opened_titles


def check_interference(asm):
    print("\n--- 6. Running Interference Detection ---")
    mgr = asm.InterferenceDetectionManager
    mgr.TreatCoincidenceAsInterference = False
    mgr.TreatSubAssembliesAsComponents = True
    int_count = mgr.GetInterferenceCount
    print(
        f"  Interference check: count={int_count}, "
        f"TreatCoincidenceAsInterference={mgr.TreatCoincidenceAsInterference}, "
        f"TreatSubAssembliesAsComponents={mgr.TreatSubAssembliesAsComponents}"
    )
    if int_count > 0:
        for idx, item in enumerate(mgr.GetInterferences):
            vol = item.Volume * MM3_PER_M3
            names = [c.Name2 for c in item.Components]
            print(f"  Collision #{idx + 1}: {vol:.3f} mm^3 between {names}")
        raise RuntimeError(f"Assembly has {int_count} interferences")


def capture_views(asm, asm_title, inserted_comps, ss_dir):
    print("\n--- 7. Capturing Assembly Views ---")
    asm.ShowNamedView2("*Isometric", sw_api.SW_VIEW_ISOMETRIC)
    asm.ViewZoomtofit2()
    closed_name = "assembly_closed_isometric.png"
    _, _, _, closed_size = sw_api.save_as(os.path.join(ss_dir, closed_name))
    print(f"  Saved {closed_name} ({closed_size} B)")

    lid_comp = next(comp for name, comp in inserted_comps if name == "Case_Lid")
    asm_stem = os.path.splitext(asm_title)[0]
    asm.ClearSelection2(True)
    if not asm.Extension.SelectByID2(
        f"{lid_comp.Name2}@{asm_stem}", "COMPONENT", 0, 0, 0, False, 0, sw_api.callout(), 0
    ):
        raise RuntimeError(f"SelectByID2 failed for component {lid_comp.Name2}@{asm_stem}")
    asm.HideComponent2()
    if not lid_comp.IsHidden(False):
        raise RuntimeError(f"HideComponent2 failed for component {lid_comp.Name2}")

    views = [
        ("*Isometric", sw_api.SW_VIEW_ISOMETRIC, "assembly_open_isometric.png"),
        ("*Top", sw_api.SW_VIEW_TOP, "assembly_open_top.png"),
    ]
    for view_name, view_id, filename in views:
        asm.ShowNamedView2(view_name, view_id)
        asm.ViewZoomtofit2()
        out_path = os.path.join(ss_dir, filename)
        _, _, _, size = sw_api.save_as(out_path)
        print(f"  Saved {filename} ({size} B)")


def _sldworks_running():
    out = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq SLDWORKS.exe"],
        capture_output=True,
        text=True,
        check=False,
    ).stdout
    return "SLDWORKS.exe" in out


def parse_args(argv=None):
    ap = argparse.ArgumentParser(
        description="Rebuild enclosure CAD/STEP/STL deliverables, run assembly checks, "
        "build plate, STL gate, and export assembly renders."
    )
    return ap.parse_args(argv)


def main():
    parse_args()

    enclosure_dir = os.path.abspath(os.path.join(_HERE, ".."))
    repo_root = os.path.abspath(os.path.join(enclosure_dir, "..", ".."))
    cad_dir = os.path.join(enclosure_dir, "cad")
    step_dir = os.path.join(enclosure_dir, "step")
    stl_raw_dir = os.path.join(enclosure_dir, "stl-assembly-frame")
    print_stl_dir = os.path.join(enclosure_dir, "stl")
    ss_dir = os.path.join(repo_root, "docs", "img")
    for d in (step_dir, stl_raw_dir, print_stl_dir, ss_dir):
        os.makedirs(d, exist_ok=True)

    was_running = _sldworks_running()
    sw = sw_api.connect(visible=True)
    print("SolidWorks Connected:", sw.RevisionNumber)

    params = EnclosureParams()
    opened_titles = []
    asm_title = None
    try:
        part_paths = {
            "Case_Bottom": build_case_bottom(params, cad_dir, stl_raw_dir, step_dir),
            "Case_Lid": build_case_lid(params, cad_dir, stl_raw_dir, step_dir),
            "Cable_Clamp": build_cable_clamp(params, cad_dir, stl_raw_dir, step_dir),
            "ESP32_38P_TypeC": build_esp32_mockup(params, cad_dir, stl_raw_dir, step_dir),
            "LM2596_module": os.path.join(cad_dir, "LM2596_module.SLDPRT"),
        }
        asm, asm_title, inserted, opened_titles = assemble(params, part_paths, cad_dir, step_dir)
        capture_views(asm, asm_title, inserted, ss_dir)
    finally:
        for title in ([asm_title] if asm_title else []) + opened_titles:
            try:
                sw.QuitDoc(title)
            except pythoncom.com_error as exc:
                print(f"  Warning: QuitDoc failed for {title}: {exc}", file=sys.stderr)
        if not was_running and _sldworks_running():
            try:
                sw.ExitApp()
            except pythoncom.com_error as exc:
                print(f"  Warning: ExitApp failed: {exc}", file=sys.stderr)

    print("\n--- 8. Generating Print-Ready STLs ---")
    script_orient = os.path.join(_HERE, "print_orient.py")
    subprocess.run([sys.executable, "-I", script_orient, stl_raw_dir, print_stl_dir], check=True)

    print("\n--- 9. Generating Combined Build Plate ---")
    script_plate = os.path.join(_HERE, "build_plate.py")
    subprocess.run([sys.executable, "-I", script_plate, "--stl-dir", print_stl_dir], check=True)

    print("\n--- 10. Running STL Gate on Print Files ---")
    script_gate = os.path.join(_HERE, "stl_gate.py")
    for stl_name, expected_shells in PRINTABLE_STL_SPECS:
        stl_path = os.path.join(print_stl_dir, stl_name)
        subprocess.run(
            [
                sys.executable,
                "-I",
                script_gate,
                stl_path,
                "--up",
                "+Z",
                "--expected-shells",
                str(expected_shells),
            ],
            check=True,
        )

    print("\n[OK] Enclosure pipeline finished.")


if __name__ == "__main__":
    main()

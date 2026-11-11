#!/usr/bin/env python3
"""SolidWorks COM helper module for enclosure CAD builds.

Lengths at the helper boundary are in millimetres and converted to metres (M)
for SolidWorks COM API calls.
"""

import math
import os
import sys
import types
import pythoncom
import win32com.client

M = 0.001

# swconst.tlb constants used below
SW_DOC_PART = 1                       # swDocumentTypes_e.swDocPART
SW_DOC_ASSEMBLY = 2                   # swDocumentTypes_e.swDocASSEMBLY
SW_BODY_SOLID = 0                     # swBodyType_e.swSolidBody
SW_SURF_CYLINDER = 4002               # swSurfaceTypes_e.CYLINDER_TYPE
SW_VIEW_TOP = 5                       # swStandardViews_e.swTopView
SW_VIEW_ISOMETRIC = 7                 # swStandardViews_e.swIsometricView
SW_DEFAULT_TEMPLATE_PART = 8          # swUserPreferenceStringValue_e.swDefaultTemplatePart
SW_DEFAULT_TEMPLATE_ASSEMBLY = 9      # swUserPreferenceStringValue_e.swDefaultTemplateAssembly
SW_OPEN_SILENT = 1                    # swOpenDocOptions_e.swOpenDocOptions_Silent
SW_OPEN_READ_ONLY = 2                 # swOpenDocOptions_e.swOpenDocOptions_ReadOnly
SW_END_BLIND = 0                      # swEndConditions_e.swEndCondBlind
SW_END_THROUGH_ALL = 1                # swEndConditions_e.swEndCondThroughAll
SW_REF_PLANE_DISTANCE = 8             # swRefPlaneReferenceConstraint_Distance
SW_CHAMFER_ANGLE_DISTANCE = 1         # swChamferType_e.swFeatureChamferAngleDistance
SW_SAVEAS_CURRENT_VERSION = 0         # swSaveAsVersion_e.swSaveAsCurrentVersion
SW_SAVEAS_OPTIONS_SILENT = 1          # swSaveAsOptions_e.swSaveAsOptions_Silent
SW_ADD_COMP_CURRENT_CONFIG = 0        # swAddComponentConfigOptions_CurrentSelectedConfig
SW_PREF_STL_DEVIATION = 2             # swUserPreferenceDoubleValue_e.swSTLDeviation
SW_PREF_STL_ANGLE_TOL = 3             # swUserPreferenceDoubleValue_e.swSTLAngleTolerance
SW_PREF_STL_BINARY = 69               # swUserPreferenceToggle_e.swSTLBinaryFormat
SW_PREF_STL_SHOW_INFO = 70            # swUserPreferenceToggle_e.swSTLShowInfoOnSave
SW_PREF_STL_DONT_TRANSLATE = 71       # swUserPreferenceToggle_e.swSTLDontTranslateToPositive
SW_PREF_STEP_AP = 75                  # swUserPreferenceIntegerValue_e.swStepAP
SW_PREF_STL_QUALITY = 78              # swUserPreferenceIntegerValue_e.swSTLQuality
SW_STL_QUALITY_CUSTOM = 3             # swSTLQuality_e.swSTLQuality_Custom
SW_PREF_STEP_EXPORT_PREF = 142        # swUserPreferenceIntegerValue_e.swStepExportPreference
SW_STEP_SOLID_SURFACE = 0             # swAcisOutputGeometryPreference_e.swAcisOutputAsSolidAndSurface
SW_PREF_STL_PREVIEW = 191             # swUserPreferenceToggle_e.swSTLPreview
SLDWORKS_TLB_GUID = "{83A33D31-27C5-11CE-BFD4-00400513BB57}"
_STD_PLANE_INDEX = {"Front": 0, "Top": 1, "Right": 2}
_STD_PLANE_NAMES = {"Front": "Front Plane", "Top": "Top Plane", "Right": "Right Plane"}

_sw = None
_sw_mod = None


def connect(visible=True):
    """Attach to a running SolidWorks COM instance or start one, and store it."""
    global _sw, _sw_mod
    _sw = win32com.client.Dispatch("SldWorks.Application")
    _sw.Visible = visible
    _sw_mod = None
    return _sw


def _tlb_module():
    """Return early-bound SolidWorks type library module for typed COM properties."""
    global _sw_mod
    if _sw_mod is None:
        rev_major = int(str(get_app().RevisionNumber).split(".")[0])
        _sw_mod = win32com.client.gencache.EnsureModule(SLDWORKS_TLB_GUID, 0, rev_major, 0)
    return _sw_mod


def get_app():
    """Return the active SolidWorks application COM object."""
    if _sw is None:
        raise RuntimeError("SolidWorks is not connected; call connect() first")
    return _sw


def _find_template(pref_id, env_var, doc_type):
    env = os.environ.get(env_var)
    if env and os.path.exists(env):
        return env
    sw = get_app()
    tmpl = sw.GetUserPreferenceStringValue(pref_id)
    if tmpl and os.path.exists(tmpl):
        return tmpl
    fallback = sw.GetDocumentTemplate(doc_type, "", 0, 0, 0)
    if fallback and os.path.exists(fallback):
        return fallback
    raise RuntimeError(f"No SolidWorks template found for preference {pref_id}; set {env_var}")


def callout():
    return win32com.client.VARIANT(pythoncom.VT_DISPATCH, None)


def prop(obj, name):
    """Read a late-bound member that may bind as a property or 0-arg method."""
    v = getattr(obj, name)
    return v() if isinstance(v, types.MethodType) else v


def D():
    d = get_app().ActiveDoc
    if d is None:
        raise RuntimeError("No active SolidWorks document")
    return d


def new_part():
    tmpl = _find_template(SW_DEFAULT_TEMPLATE_PART, "SW_PART_TEMPLATE", SW_DOC_PART)
    d = get_app().NewDocument(tmpl, 0, 0, 0)
    if d is None:
        raise RuntimeError(f"NewDocument failed for part template: {tmpl}")
    return d


def new_assembly():
    tmpl = _find_template(SW_DEFAULT_TEMPLATE_ASSEMBLY, "SW_ASM_TEMPLATE", SW_DOC_ASSEMBLY)
    d = get_app().NewDocument(tmpl, 0, 0, 0)
    if d is None:
        raise RuntimeError(f"NewDocument failed for assembly template: {tmpl}")
    return d


def open_doc(path, doc_type=SW_DOC_PART, read_only=False):
    opts = SW_OPEN_SILENT | (SW_OPEN_READ_ONLY if read_only else 0)
    errs = win32com.client.VARIANT(pythoncom.VT_BYREF | pythoncom.VT_I4, 0)
    warns = win32com.client.VARIANT(pythoncom.VT_BYREF | pythoncom.VT_I4, 0)
    doc = get_app().OpenDoc6(path, doc_type, opts, "", errs, warns)
    if doc is None:
        raise RuntimeError(f"OpenDoc6 failed for {path} (err={errs.value}, warn={warns.value})")
    return doc


def activate(title):
    errs = win32com.client.VARIANT(pythoncom.VT_BYREF | pythoncom.VT_I4, 0)
    return get_app().ActivateDoc3(title, False, 0, errs)


def close(title):
    get_app().CloseDoc(title)


def sel_id(name, typ, mark=0, append=False):
    return bool(D().Extension.SelectByID2(name, typ, 0, 0, 0, append, mark, callout(), 0))


def clear():
    D().ClearSelection2(True)


def exit_sketch():
    d = D()
    if d.SketchManager.ActiveSketch is not None:
        d.ClearSelection2(True)
        d.SketchManager.InsertSketch(True)


def latest_sketch():
    for f in reversed(D().FeatureManager.GetFeatures(True) or []):
        if prop(f, "GetTypeName2") == "ProfileFeature":
            return f.Name
    return None


def _resolve_plane_feature(doc, plane):
    """Resolve 'Front'/'Top'/'Right' via the first three RefPlane features, or by name."""
    std_idx = _STD_PLANE_INDEX.get(plane)
    if std_idx is not None:
        ref_planes = [
            f for f in (doc.FeatureManager.GetFeatures(True) or [])
            if prop(f, "GetTypeName2") == "RefPlane"
        ]
        if len(ref_planes) > std_idx:
            return ref_planes[std_idx]
        plane_name = _STD_PLANE_NAMES[plane]
    else:
        plane_name = plane
    feat = doc.FeatureByName(plane_name)
    if feat is None:
        raise RuntimeError(f"Plane feature not found: {plane_name}")
    return feat


def sketch_on(plane):
    """Open a sketch on 'Front'/'Top'/'Right' or a named reference plane."""
    d = D()
    f = _resolve_plane_feature(d, plane)
    d.ClearSelection2(True)
    if not f.Select2(False, 0):
        raise RuntimeError(f"Failed to select plane: {plane}")
    d.SketchManager.InsertSketch(True)
    d.SketchManager.AddToDB = True
    d.SketchManager.DisplayWhenAdded = False
    return latest_sketch()


def sk_done():
    d = D()
    d.SketchManager.AddToDB = False
    d.SketchManager.DisplayWhenAdded = True
    name = latest_sketch()
    exit_sketch()
    return name


def line(x1, y1, x2, y2):
    return D().SketchManager.CreateLine(x1 * M, y1 * M, 0, x2 * M, y2 * M, 0)


def circle(cx, cy, r):
    return D().SketchManager.CreateCircleByRadius(cx * M, cy * M, 0, r * M)


def polygon(points, close=True):
    pts = list(points)
    if close and pts[0] != pts[-1]:
        pts.append(pts[0])
    for (x1, y1), (x2, y2) in zip(pts[:-1], pts[1:]):
        line(x1, y1, x2, y2)


def extrude(sketch, depth, merge=True, reverse=False, dir_flip=False):
    d = D()
    exit_sketch()
    clear()
    f = d.FeatureByName(sketch)
    if f is None:
        raise RuntimeError(f"Sketch feature not found for extrude: {sketch}")
    if not f.Select2(False, 0):
        raise RuntimeError(f"Failed to select sketch for extrude: {sketch}")
    feat = d.FeatureManager.FeatureExtrusion2(
        True, reverse, dir_flip, SW_END_BLIND, SW_END_BLIND, depth * M, 0.0,
        False, False, False, False, 0.0, 0.0,
        False, False, False, False, merge, True, True, 0, 0, False,
    )
    if feat is None:
        raise RuntimeError(f"Extrude failed for {sketch}")
    return feat.Name


def cut_extrude(sketch, depth=0.0, through=False, direction=None, min_removed_mm3=None):
    """Blind or through cut; tries both directions if direction is None."""
    if not through and depth <= 0.0:
        raise ValueError(f"Blind cut_extrude requires depth > 0, got {depth}")
    d = D()
    exit_sketch()
    f = d.FeatureByName(sketch)
    if f is None:
        raise RuntimeError(f"Sketch feature not found for cut_extrude: {sketch}")
    end_cond = SW_END_THROUGH_ALL if through else SW_END_BLIND
    tries = [direction] if direction is not None else [False, True]
    for dir_flag in tries:
        v0 = volume_mm3()
        clear()
        if not f.Select2(False, 0):
            raise RuntimeError(f"Failed to select sketch for cut_extrude: {sketch}")
        feat = d.FeatureManager.FeatureCut4(
            True, False, dir_flag, end_cond, SW_END_BLIND, depth * M, 0.0,
            False, False, False, False, 0.0, 0.0, False, False, False, False,
            False, False, True, False, True, False, 0, 0.0, False, False,
        )
        if feat is None:
            continue
        if min_removed_mm3 is not None and (v0 - volume_mm3()) < min_removed_mm3:
            delete_feature(feat.Name, "BODYFEATURE")
            continue
        return feat.Name
    raise RuntimeError(f"Cut failed for {sketch}")


def plane_offset(base, offset):
    d = D()
    exit_sketch()
    clear()
    f = _resolve_plane_feature(d, base)
    if not f.Select2(False, 0):
        raise RuntimeError(f"Failed to select base plane: {base}")
    feat = d.FeatureManager.InsertRefPlane(SW_REF_PLANE_DISTANCE, offset * M, 0, 0, 0, 0)
    if feat is None:
        raise RuntimeError(f"Reference plane failed: {base} @ {offset}")
    return feat.Name


def bodies():
    return list(D().GetBodies2(SW_BODY_SOLID, True) or [])


def closest_entities(kind, pts, tol=0.5):
    """Find nearest edge or face entity per point (mm) within tol mm."""
    out = []
    for (x, y, z) in pts:
        best, bd = None, tol * M
        for b in bodies():
            ents = b.GetEdges() if kind == "edge" else b.GetFaces()
            for e in ents or []:
                try:
                    cp = e.GetClosestPointOn(x * M, y * M, z * M)
                except pythoncom.com_error:
                    continue
                if cp is None:
                    continue
                dd = ((cp[0] - x * M) ** 2 + (cp[1] - y * M) ** 2 + (cp[2] - z * M) ** 2) ** 0.5
                if dd < bd:
                    best, bd = e, dd
        out.append(best)
    return out


def select_entities(ents, mark=0):
    d = D()
    sd = prop(d.SelectionManager, "CreateSelectData")
    sd.Mark = mark
    return sum(1 for e in ents if e is not None and e.Select4(True, sd))


def chamfer_points(pts, dist, angle_deg=45.0):
    d = D()
    exit_sketch()
    d.ForceRebuild3(True)
    clear()
    ents = closest_entities("edge", pts)
    if any(e is None for e in ents):
        raise RuntimeError("Chamfer: edge not found for some points")
    n = select_entities(ents, 0)
    v0 = volume_mm3()
    feat = d.FeatureManager.InsertFeatureChamfer(
        0, SW_CHAMFER_ANGLE_DISTANCE, dist * M, math.radians(angle_deg), dist * M, 0, 0, 0
    )
    if feat is None:
        raise RuntimeError("Chamfer failed")
    if abs(volume_mm3() - v0) < 1e-4:
        delete_feature(feat.Name)
        raise RuntimeError("Chamfer produced no geometry")
    return feat.Name, n


def delete_feature(name, typ="BODYFEATURE"):
    d = D()
    exit_sketch()
    clear()
    ok = sel_id(name, typ, 0, False)
    if ok:
        d.Extension.DeleteSelection2(0)
    clear()
    return ok


def rebuild():
    d = D()
    ok = bool(d.ForceRebuild3(False))
    problems = []
    for f in d.FeatureManager.GetFeatures(True) or []:
        try:
            arg = win32com.client.VARIANT(pythoncom.VT_BYREF | pythoncom.VT_BOOL, False)
            code = f.GetErrorCode2(arg)
        except pythoncom.com_error as exc:
            print(f"  Warning: GetErrorCode2 failed on {f.Name}: {exc}", file=sys.stderr)
            continue
        if code:
            problems.append((f.Name, code))
    if problems:
        raise RuntimeError(f"Rebuild errors: {problems}")
    if not ok:
        raise RuntimeError("ForceRebuild3 returned False with no feature error codes")
    return ok, problems


def volume_mm3():
    mp = prop(D().Extension, "CreateMassProperty")
    return mp.Volume / (M ** 3)


def save_as(path):
    """Native save or export by extension. Returns (ok, errs, warns, size)."""
    d = D()
    exit_sketch()
    errs = win32com.client.VARIANT(pythoncom.VT_BYREF | pythoncom.VT_I4, 0)
    warns = win32com.client.VARIANT(pythoncom.VT_BYREF | pythoncom.VT_I4, 0)
    ok = d.Extension.SaveAs(path, SW_SAVEAS_CURRENT_VERSION, SW_SAVEAS_OPTIONS_SILENT, callout(), errs, warns)
    if not ok or errs.value != 0:
        raise RuntimeError(f"SaveAs failed for {path}: ok={ok}, err={errs.value}, warn={warns.value}")
    return ok, errs.value, warns.value, os.path.exists(path) and os.path.getsize(path)


def stl_fine(dev_mm=0.01, ang_deg=0.5):
    """Custom STL tessellation: binary, model coordinates, no popups."""
    sw = get_app()
    sw.SetUserPreferenceToggle(SW_PREF_STL_SHOW_INFO, False)
    sw.SetUserPreferenceToggle(SW_PREF_STL_PREVIEW, False)
    sw.SetUserPreferenceToggle(SW_PREF_STL_BINARY, True)
    sw.SetUserPreferenceToggle(SW_PREF_STL_DONT_TRANSLATE, True)
    sw.SetUserPreferenceIntegerValue(SW_PREF_STL_QUALITY, SW_STL_QUALITY_CUSTOM)
    sw.SetUserPreferenceDoubleValue(SW_PREF_STL_DEVIATION, dev_mm * M)
    sw.SetUserPreferenceDoubleValue(SW_PREF_STL_ANGLE_TOL, math.radians(ang_deg))


def step_ap214():
    sw = get_app()
    sw.SetUserPreferenceIntegerValue(SW_PREF_STEP_AP, 214)
    sw.SetUserPreferenceIntegerValue(SW_PREF_STEP_EXPORT_PREF, SW_STEP_SOLID_SURFACE)


def set_component_transform(comp, rot_3x3, t_mm):
    """Set exact component rotation (3x3 row-major) and origin translation (mm)."""
    sw = get_app()
    mod = _tlb_module()
    r = rot_3x3
    arr = [
        float(r[0][0]), float(r[1][0]), float(r[2][0]),
        float(r[0][1]), float(r[1][1]), float(r[2][1]),
        float(r[0][2]), float(r[1][2]), float(r[2][2]),
        float(t_mm[0]) * M, float(t_mm[1]) * M, float(t_mm[2]) * M,
        1.0, 0.0, 0.0, 0.0,
    ]
    mu = mod.IMathUtility(prop(sw, "GetMathUtility")._oleobj_)
    xf = mu.CreateTransform(win32com.client.VARIANT(pythoncom.VT_ARRAY | pythoncom.VT_R8, arr))
    mod.IComponent2(comp._oleobj_).Transform2 = xf


def add_component_at_pose(asm, path, rot_3x3, t_mm, config=""):
    """Insert a component at an exact rotation (3x3 row-major) and origin translation (mm)."""
    comp = asm.AddComponent5(path, SW_ADD_COMP_CURRENT_CONFIG, "", False, config, 0.0, 0.0, 0.0)
    if comp is None:
        raise RuntimeError(f"AddComponent5 failed for {path}")
    set_component_transform(comp, rot_3x3, t_mm)
    return comp

"""Value of every texture the pixel stage samples, and of render target 0, at a draw for a
list of pixels (appends to RDC_OUT/pick.txt). Compares a draw's inputs and output pixel by
pixel between our build and Xenia.

Run: RDC_CAP=capture.rdc RDC_OUT=dir RDC_EID=eid RDC_PTS="x,y;x,y" qrenderdoc --python pick.py
"""
import os, traceback
import renderdoc as rd
out = os.environ["RDC_OUT"]; os.makedirs(out, exist_ok=True)
log = open(os.path.join(out, "pick.txt"), "a")
try:
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDC_CAP"], '', None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    eid = int(os.environ["RDC_EID"])
    ctrl.SetFrameEvent(eid, True)
    p = ctrl.GetPipelineState()
    refl = p.GetShaderReflection(rd.ShaderStage.Pixel)
    texinfo = {t.resourceId: t for t in ctrl.GetTextures()}
    targets = []
    for b in p.GetReadOnlyResources(rd.ShaderStage.Pixel):
        rid = b.descriptor.resource
        if rid in texinfo and texinfo[rid].width > 64:
            targets.append((refl.readOnlyResources[b.access.index].name, rid))
    targets.append(("rt0", p.GetOutputTargets()[0].resource))
    log.write(f"== {os.path.basename(os.environ['RDC_CAP'])} EID {eid}\n")
    for pt in os.environ["RDC_PTS"].split(";"):
        x, y = [int(v) for v in pt.split(",")]
        for nm, rid in targets:
            v = ctrl.PickPixel(rid, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            f = v.floatValue; u = v.uintValue
            log.write(f"  ({x},{y}) {nm:18s} f=({f[0]:.4f} {f[1]:.4f} {f[2]:.4f} {f[3]:.4f}) u=({u[0]:#x} {u[1]:#x} {u[2]:#x} {u[3]:#x})\n")
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    log.write(traceback.format_exc())
log.close(); os._exit(0)

"""Pixel history of one pixel on every color render target in a capture: each passing
modification with the shader output and the value after blending (RDC_OUT/phist_X_Y.txt).
Shows which draw, transfer or blend leaves a wrong value behind.

Run: RDC_CAP=capture.rdc RDC_OUT=dir RDC_XY=x,y [RDC_TO=eid] qrenderdoc --python phist.py
"""
import os, traceback
import renderdoc as rd
out = os.environ["RDC_OUT"]; os.makedirs(out, exist_ok=True)
x, y = [int(v) for v in os.environ["RDC_XY"].split(",")]; to = int(os.environ.get("RDC_TO", "999999"))
log = open(os.path.join(out, f"phist_{x}_{y}.txt"), "w")
try:
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDC_CAP"], '', None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    ctrl.SetFrameEvent(to, True)
    for t in ctrl.GetTextures():
        if not (t.creationFlags & rd.TextureCategory.ColorTarget): continue
        if x >= t.width or y >= t.height: continue
        hist = ctrl.PixelHistory(t.resourceId, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        mods = [m for m in hist if m.eventId <= to]
        if not mods: continue
        log.write(f"=== RT {t.resourceId} {t.width}x{t.height} {t.format.Name()} mods={len(mods)}\n")
        for m in mods:
            if not m.Passed(): continue
            po = m.postMod.col.floatValue; so = m.shaderOut.col.floatValue
            log.write("  EID %d out=(%.3f %.3f %.3f %.3f) post=(%.3f %.3f %.3f %.3f)\n" % (
                m.eventId, so[0], so[1], so[2], so[3], po[0], po[1], po[2], po[3]))
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    log.write(traceback.format_exc())
log.close(); os._exit(0)

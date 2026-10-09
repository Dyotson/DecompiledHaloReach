"""Save every texture the pixel stage samples at a draw, and its render targets, as PNG
(RDC_OUT/<prefix>_<binding>.png).

Run: RDC_CAP=capture.rdc RDC_OUT=dir RDC_EID=eid [RDC_PREFIX=name] qrenderdoc --python savetex.py
"""
import os, traceback
import renderdoc as rd
out = os.environ["RDC_OUT"]; os.makedirs(out, exist_ok=True)
eid = int(os.environ["RDC_EID"]); prefix = os.environ.get("RDC_PREFIX", str(eid))
log = open(os.path.join(out, f"savetex_{prefix}.txt"), "w")
try:
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDC_CAP"], '', None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    ctrl.SetFrameEvent(eid, True)
    p = ctrl.GetPipelineState()
    refl = p.GetShaderReflection(rd.ShaderStage.Pixel)
    texinfo = {t.resourceId: t for t in ctrl.GetTextures()}
    seen = set()
    for b in p.GetReadOnlyResources(rd.ShaderStage.Pixel):
        rid = b.descriptor.resource
        if rid in seen or rid not in texinfo: continue
        seen.add(rid)
        t = texinfo[rid]
        nm = refl.readOnlyResources[b.access.index].name
        ts = rd.TextureSave(); ts.resourceId = rid; ts.destType = rd.FileType.PNG
        ts.mip = 0; ts.alpha = rd.AlphaMapping.Discard
        path = os.path.join(out, f"{prefix}_{nm}.png")
        r = ctrl.SaveTexture(ts, path)
        log.write(f"{nm} {rid} {t.width}x{t.height} {t.format.Name()} -> {r}\n")
    for i, o in enumerate(p.GetOutputTargets()):
        if o.resource == rd.ResourceId.Null(): continue
        ts = rd.TextureSave(); ts.resourceId = o.resource; ts.destType = rd.FileType.PNG; ts.alpha = rd.AlphaMapping.Discard
        ctrl.SaveTexture(ts, os.path.join(out, f"{prefix}_rt{i}.png"))
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    log.write(traceback.format_exc())
log.close(); os._exit(0)

"""Hash the contents (first mips) of every texture the pixel stage samples at RDC_EID
(RDC_OUT/texdata_<eid>.txt) and save mip 0 next to it.

Run: RDC_CAP=capture.rdc RDC_OUT=dir [...] qrenderdoc --python texdata.py
"""
import os, traceback, hashlib
CAP = os.environ["RDC_CAP"]; OUT = os.environ["RDC_OUT"]; EID = int(os.environ["RDC_EID"])
log = open(os.path.join(OUT, f"texdata_{EID}.txt"), "w")
try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile(); cap.OpenFile(CAP, '', None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    ctrl.SetFrameEvent(EID, True)
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
        for mip in range(min(t.mips, 3)):
            data = ctrl.GetTextureData(rid, rd.Subresource(mip, 0, 0))
            nz = sum(1 for x in data if x)
            log.write(f"{nm} {t.width}x{t.height} {t.format.Name()} mips={t.mips} mip{mip}: len={len(data)} nonzero={nz} sha={hashlib.sha1(data).hexdigest()[:16]}\n")
            if mip == 0:
                open(os.path.join(OUT, f"tex_{EID}_{nm}.bin"), "wb").write(data)
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    log.write(traceback.format_exc())
log.close()
os._exit(0)

"""Replace the pixel shader at RDC_EID with each RDC_INST/<name>.spv in turn and print the
shader output at pixel RDC_XY from pixel history (RDC_OUT/inst_<eid>.txt). Build the
variants by editing the spirv-dis output of spirv.py (for example, store a Xenos
register instead of the colour before the final OpStore) and assembling it with
`spirv-as --target-env vulkan1.2`.

Run: RDC_CAP=capture.rdc RDC_OUT=dir [...] qrenderdoc --python instrument.py
"""
import os, traceback
CAP = os.environ["RDC_CAP"]; OUT = os.environ["RDC_OUT"]; EID = int(os.environ["RDC_EID"]); DIR = os.environ["RDC_INST"]
X, Y = [int(v) for v in os.environ.get("RDC_XY", "576,360").split(",")]
log = open(os.path.join(OUT, f"inst_{EID}.txt"), "w")
try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile(); cap.OpenFile(CAP, '', None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    ctrl.SetFrameEvent(EID, True)
    p = ctrl.GetPipelineState()
    orig = p.GetShader(rd.ShaderStage.Pixel)
    target = [o.resource for o in p.GetOutputTargets()][0]
    t = [tt for tt in ctrl.GetTextures() if tt.resourceId == target][0]
    log.write(f"target fmt {t.format.Name()}\n")
    def out_at():
        hist = ctrl.PixelHistory(target, X, Y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        return [m.shaderOut.col.floatValue[:4] for m in hist if m.eventId == EID]
    log.write(f"orig: {out_at()}\n")
    for nm in [f"r{n}" for n in range(10)] + ["ps"]:
        data = open(os.path.join(DIR, nm + ".spv"), "rb").read()
        newid, err = ctrl.BuildTargetShader("main", rd.ShaderEncoding.SPIRV, data, rd.ShaderCompileFlags(), rd.ShaderStage.Pixel)
        if newid == rd.ResourceId.Null():
            log.write(f"{nm}: build failed {err}\n"); continue
        ctrl.ReplaceResource(orig, newid)
        ctrl.SetFrameEvent(EID, True)
        vals = out_at()
        log.write(f"{nm}: {[['%.5g' % x for x in v] for v in vals]}\n"); log.flush()
        ctrl.RemoveReplacement(orig)
        ctrl.FreeTargetResource(newid)
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    log.write(traceback.format_exc())
log.close()
os._exit(0)

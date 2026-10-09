"""Save the raw SPIR-V of the vertex and pixel shaders at RDC_EID (RDC_OUT/{vs,ps}_<eid>.spv,
disassemble with spirv-dis) plus the sampler state.

Run: RDC_CAP=capture.rdc RDC_OUT=dir [...] qrenderdoc --python spirv.py
"""
import os, traceback
CAP = os.environ["RDC_CAP"]; OUT = os.environ["RDC_OUT"]; EID = int(os.environ["RDC_EID"])
log = open(os.path.join(OUT, f"spv_{EID}.txt"), "w")
try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile(); cap.OpenFile(CAP, '', None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    ctrl.SetFrameEvent(EID, True)
    p = ctrl.GetPipelineState()
    for stage, nm in ((rd.ShaderStage.Pixel, "ps"), (rd.ShaderStage.Vertex, "vs")):
        refl = p.GetShaderReflection(stage)
        open(os.path.join(OUT, f"{nm}_{EID}.spv"), "wb").write(bytes(refl.rawBytes))
        log.write(f"{nm} encoding={refl.encoding} bytes={len(refl.rawBytes)}\n")
    # sampler state
    for s in p.GetSamplers(rd.ShaderStage.Pixel):
        d = s.sampler
        log.write(f"sampler idx={s.access.index} filter={d.filter.minify},{d.filter.magnify},{d.filter.mip} addr={d.addressU},{d.addressV} lod=[{d.minLOD},{d.maxLOD}] bias={d.mipBias} aniso={d.maxAnisotropy} border={list(d.borderColorValue.floatValue)}\n")
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    log.write(traceback.format_exc())
log.close()
os._exit(0)

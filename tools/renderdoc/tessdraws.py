"""List the tessellated draws in a capture (pipelines with a hull/tessellation control stage):
event, index count, topology and the shaders bound (RDC_OUT/tessdraws.txt).

Run: RDC_CAP=capture.rdc RDC_OUT=dir qrenderdoc --python tessdraws.py
"""
import os, traceback
import renderdoc as rd
out = os.environ["RDC_OUT"]; os.makedirs(out, exist_ok=True)
log = open(os.path.join(out, "tessdraws.txt"), "w")
try:
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDC_CAP"], '', None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    def walk(actions):
        for a in actions:
            yield a
            yield from walk(a.children)
    draws = [a for a in walk(ctrl.GetRootActions()) if a.flags & rd.ActionFlags.Drawcall]
    n = 0
    for a in draws:
        ctrl.SetFrameEvent(a.eventId, False)
        p = ctrl.GetPipelineState()
        if p.GetShader(rd.ShaderStage.Hull) == rd.ResourceId.Null():
            continue
        n += 1
        log.write(f"EID {a.eventId} idx={a.numIndices} topo={p.GetPrimitiveTopology()} "
                  f"hs={str(p.GetShader(rd.ShaderStage.Hull)).split('::')[-1]} "
                  f"ds={str(p.GetShader(rd.ShaderStage.Domain)).split('::')[-1]} "
                  f"ps={str(p.GetShader(rd.ShaderStage.Pixel)).split('::')[-1]}\n")
    log.write(f"draws={len(draws)} tessellated={n}\n")
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    log.write(traceback.format_exc())
log.close(); os._exit(0)

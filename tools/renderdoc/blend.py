"""Color blend state and write masks of draws (appends to RDC_OUT/blend.txt).

Run: RDC_CAP=capture.rdc RDC_OUT=dir RDC_EIDS=eid,eid qrenderdoc --python blend.py
"""
import os, traceback
import renderdoc as rd
out = os.environ["RDC_OUT"]; os.makedirs(out, exist_ok=True)
log = open(os.path.join(out, "blend.txt"), "a")
try:
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDC_CAP"], '', None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    for e in os.environ["RDC_EIDS"].split(","):
        ctrl.SetFrameEvent(int(e), True)
        p = ctrl.GetPipelineState()
        vk = ctrl.GetVulkanPipelineState()
        cb = vk.colorBlend
        log.write(f"== {os.path.basename(os.environ['RDC_CAP'])} EID {e} blendConst={list(cb.blendFactor)}\n")
        for i, b in enumerate(cb.blends):
            log.write(f"  rt{i} enabled={b.enabled} color=({b.colorBlend.source},{b.colorBlend.destination},{b.colorBlend.operation}) alpha=({b.alphaBlend.source},{b.alphaBlend.destination},{b.alphaBlend.operation}) mask={b.writeMask:#x}\n")
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    log.write(traceback.format_exc())
log.close(); os._exit(0)

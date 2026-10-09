"""Dump byte ranges of buffers at given events, e.g. guest memory in the shared-memory
buffer right after a resolve (RDC_OUT/<name>.bin). Find the buffer and offset with
resolves.py; render tiled surfaces with tools/frame_to_png.py.

Run: RDC_CAP=capture.rdc RDC_OUT=dir RDC_JOBS="eid:resource:offset:size:name;..." qrenderdoc --python bufdump.py
"""
import os, traceback
import renderdoc as rd
out = os.environ["RDC_OUT"]; os.makedirs(out, exist_ok=True)
log = open(os.path.join(out, "bufdump.txt"), "w")
try:
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RDC_CAP"], '', None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    res = {str(r.resourceId).split("::")[-1]: r.resourceId for r in ctrl.GetBuffers()}
    for job in os.environ["RDC_JOBS"].split(";"):
        eid, rid, off, size, name = job.split(":")
        ctrl.SetFrameEvent(int(eid), True)
        data = ctrl.GetBufferData(res[rid], int(off, 0), int(size, 0))
        open(os.path.join(out, name + ".bin"), "wb").write(bytes(data))
        log.write(f"{job} -> {len(data)} bytes\n")
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    log.write(traceback.format_exc())
log.close(); os._exit(0)

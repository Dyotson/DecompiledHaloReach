"""Dump the raw constant buffers (system, float, bool/loop, fetch constants) of the
vertex and pixel stages at RDC_EIDS (comma-separated) into RDC_OUT/cb.txt.

Run: RDC_CAP=capture.rdc RDC_OUT=dir [...] qrenderdoc --python cbuffers.py
"""
import os, traceback, struct
CAP = os.environ["RDC_CAP"]; OUT = os.environ["RDC_OUT"]; EIDS = [int(x) for x in os.environ["RDC_EIDS"].split(",")]
log = open(os.path.join(OUT, "cb.txt"), "w")
try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile(); cap.OpenFile(CAP, '', None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    for eid in EIDS:
        ctrl.SetFrameEvent(eid, True)
        p = ctrl.GetPipelineState()
        for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
            refl = p.GetShaderReflection(stage)
            if refl is None: continue
            for i, cb in enumerate(refl.constantBlocks):
                desc = p.GetConstantBlock(stage, i, 0).descriptor
                data = ctrl.GetBufferData(desc.resource, desc.byteOffset, min(desc.byteSize, cb.byteSize) if cb.byteSize else desc.byteSize)
                n = len(data) // 4
                dw = struct.unpack(f"<{n}I", data[:n*4])
                log.write(f"== EID {eid} stage {int(stage)} cb{i} {cb.name} bytes={len(data)}\n")
                for j in range(0, n, 4):
                    row = dw[j:j+4]
                    fl = struct.unpack(f"<{len(row)}f", struct.pack(f"<{len(row)}I", *row))
                    log.write(f"  [{j//4:3d}] " + " ".join(f"{x:08x}" for x in row) + "   " + " ".join(f"{x:.5g}" for x in fl) + "\n")
        # pixel history-ish: pick center of target
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    log.write(traceback.format_exc())
log.close()
os._exit(0)

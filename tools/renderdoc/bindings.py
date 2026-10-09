"""Pixel-stage texture bindings at RDC_EID: view format and swizzle, 1x1 texel values,
the 32 fetch constants and the system constants (RDC_OUT/tex_<eid>.txt).

Run: RDC_CAP=capture.rdc RDC_OUT=dir [...] qrenderdoc --python bindings.py
"""
import os, traceback, struct
CAP = os.environ["RDC_CAP"]; OUT = os.environ["RDC_OUT"]; EID = int(os.environ["RDC_EID"])
log = open(os.path.join(OUT, f"tex_{EID}.txt"), "w")
try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile(); cap.OpenFile(CAP, '', None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    ctrl.SetFrameEvent(EID, True)
    p = ctrl.GetPipelineState()
    refl = p.GetShaderReflection(rd.ShaderStage.Pixel)
    texinfo = {t.resourceId: t for t in ctrl.GetTextures()}
    for b in p.GetReadOnlyResources(rd.ShaderStage.Pixel):
        d = b.descriptor
        idx = b.access.index
        nm = refl.readOnlyResources[idx].name if refl and idx < len(refl.readOnlyResources) else '?'
        rid = d.resource
        t = texinfo.get(rid)
        sw = d.swizzle
        log.write(f"{nm}: res={rid} {t.width if t else ''}x{t.height if t else ''} {t.format.Name() if t else ''} viewfmt={d.format.Name()} swizzle=({sw.red},{sw.green},{sw.blue},{sw.alpha})\n")
        if t and t.width == 1 and t.height == 1:
            data = ctrl.GetTextureData(rid, rd.Subresource(0, 0, 0))
            log.write(f"    data={data.hex()}\n")
            pv = ctrl.PickPixel(rid, 0, 0, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            log.write(f"    pick={list(pv.floatValue)}\n")
    # fetch constants + system constants raw
    for i, cb in enumerate(refl.constantBlocks):
        desc = p.GetConstantBlock(rd.ShaderStage.Pixel, i, 0).descriptor
        if cb.name in ("xe_uniform_fetch_constants", "xe_uniform_system_constants"):
            data = ctrl.GetBufferData(desc.resource, desc.byteOffset, cb.byteSize)
            n = len(data)//4; dw = struct.unpack(f"<{n}I", data[:n*4])
            log.write(f"{cb.name}:\n")
            if cb.name == "xe_uniform_fetch_constants":
                for f in range(n//6):
                    log.write(f"   tf{f}: " + " ".join(f"{x:08x}" for x in dw[f*6:f*6+6]) + "\n")
            else:
                def walk(vs, pre=''):
                    for v in vs:
                        if v.type == rd.VarType.Struct or v.members:
                            walk(v.members, pre + v.name + '.')
                        else:
                            log.write(f"   {pre}{v.name} off={v.byteOffset} rows={v.rows} cols={v.columns} elems={getattr(v,'elements',None)}\n")
                try:
                    walk(cb.variables)
                except Exception as e:
                    log.write(f"walk err {e}\n")
                for j in range(0, n, 4):
                    log.write(f"   [{j*4:4d}] " + " ".join(f"{x:08x}" for x in dw[j:j+4]) + "\n")
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    log.write(traceback.format_exc())
log.close()
os._exit(0)

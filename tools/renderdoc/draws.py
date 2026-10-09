"""List every draw with its pixel-shader disassembly hash, outputs and bound textures
(RDC_OUT/cmp.txt; the disassembly of each new shader goes to RDC_OUT/ps_<hash>.txt).
Use it to line up the same draw between a capture of our build and one of Xenia.

Run: RDC_CAP=capture.rdc RDC_OUT=dir [...] qrenderdoc --python draws.py
"""
import os, traceback, hashlib, struct
CAP = os.environ["RDC_CAP"]; OUT = os.environ["RDC_OUT"]
os.makedirs(OUT, exist_ok=True)
log = open(os.path.join(OUT, "cmp.txt"), "w")
try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile(); cap.OpenFile(CAP, '', None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    names = {r.resourceId: r.name for r in ctrl.GetResources()}
    texinfo = {t.resourceId: t for t in ctrl.GetTextures()}
    targets = ctrl.GetDisassemblyTargets(True)
    def walk(actions):
        for a in actions:
            yield a
            yield from walk(a.children)
    shader_hash = {}
    draws = [a for a in walk(ctrl.GetRootActions()) if a.flags & rd.ActionFlags.Drawcall]
    log.write(f"draws {len(draws)}\n")
    for a in draws:
        ctrl.SetFrameEvent(a.eventId, False)
        p = ctrl.GetPipelineState()
        sid = p.GetShader(rd.ShaderStage.Pixel)
        if sid not in shader_hash:
            refl = p.GetShaderReflection(rd.ShaderStage.Pixel)
            if refl is None:
                shader_hash[sid] = "none"
            else:
                txt = ctrl.DisassembleShader(p.GetGraphicsPipelineObject(), refl, targets[0])
                h = hashlib.sha1(txt.encode()).hexdigest()[:12]
                shader_hash[sid] = h
                open(os.path.join(OUT, f"ps_{h}.txt"), "w").write(txt)
        outs = [o.resource for o in p.GetOutputTargets() if o.resource != rd.ResourceId.Null()]
        out_desc = ",".join(f"{str(o).split('::')[-1]}:{texinfo[o].width}x{texinfo[o].height}" if o in texinfo else str(o) for o in outs)
        ro = []
        refl = p.GetShaderReflection(rd.ShaderStage.Pixel)
        for b in p.GetReadOnlyResources(rd.ShaderStage.Pixel):
            rid = b.descriptor.resource
            idx = b.access.index
            nm = refl.readOnlyResources[idx].name if refl and idx < len(refl.readOnlyResources) else '?'
            if rid != rd.ResourceId.Null():
                t = texinfo.get(rid)
                ro.append(f"{nm.replace('xe_texture','t')}={str(rid).split('::')[-1]}" + (f":{t.width}x{t.height}:{t.format.Name()}" if t else ""))
        log.write(f"EID {a.eventId} ps={shader_hash[sid]} idx={a.numIndices} out=[{out_desc}] tex=[{' '.join(ro)}]\n")
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    log.write(traceback.format_exc())
log.close()
os._exit(0)

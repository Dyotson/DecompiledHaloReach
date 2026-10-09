"""Vertex-shader outputs (interpolators) and the named system constants at RDC_EID
(RDC_OUT/vs_<eid>.txt).

Run: RDC_CAP=capture.rdc RDC_OUT=dir [...] qrenderdoc --python postvs.py
"""
import os, traceback, struct
CAP = os.environ["RDC_CAP"]; OUT = os.environ["RDC_OUT"]; EID = int(os.environ["RDC_EID"])
log = open(os.path.join(OUT, f"vs_{EID}.txt"), "w")
try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile(); cap.OpenFile(CAP, '', None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    ctrl.SetFrameEvent(EID, True)
    p = ctrl.GetPipelineState()
    vrefl = p.GetShaderReflection(rd.ShaderStage.Vertex)
    mesh = ctrl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
    log.write(f"postvs numIndices={mesh.numIndices} stride={mesh.vertexByteStride} buf={mesh.vertexResourceId} off={mesh.vertexByteOffset}\n")
    sig = vrefl.outputSignature
    for s in sig:
        log.write(f"  out {s.varName} sem={s.semanticName}{s.semanticIndex} reg={s.regIndex} comps={s.compCount} type={s.varType}\n")
    nverts = max(mesh.numIndices, 4)
    data = ctrl.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset, mesh.vertexByteStride * nverts)
    for v in range(nverts):
        row = data[v*mesh.vertexByteStride:(v+1)*mesh.vertexByteStride]
        fl = struct.unpack(f"<{len(row)//4}f", row[:len(row)//4*4])
        log.write(f"  v{v}: " + " ".join("%.4g" % x for x in fl) + "\n")
    # system constants raw
    refl = p.GetShaderReflection(rd.ShaderStage.Pixel)
    for i, cb in enumerate(refl.constantBlocks):
        if cb.name != "xe_uniform_system_constants": continue
        desc = p.GetConstantBlock(rd.ShaderStage.Pixel, i, 0).descriptor
        vars = ctrl.GetCBufferVariableContents(p.GetGraphicsPipelineObject(), p.GetShader(rd.ShaderStage.Pixel), rd.ShaderStage.Pixel, p.GetShaderEntryPoint(rd.ShaderStage.Pixel), i, desc.resource, desc.byteOffset, desc.byteSize)
        def walk(vs, pre=''):
            for v in vs:
                if v.members: walk(v.members, pre + v.name + '.')
                else:
                    n = v.rows * v.columns
                    log.write(f"   {pre}{v.name} = f{['%.5g' % v.value.f32v[k] for k in range(min(n,4))]} u{[hex(v.value.u32v[k]) for k in range(min(n,4))]}\n")
        walk(vars)
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    log.write(traceback.format_exc())
log.close()
os._exit(0)

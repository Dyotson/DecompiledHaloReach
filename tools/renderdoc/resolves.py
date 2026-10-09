"""List the EDRAM resolves and texture loads in a capture of our build or of Xenia
(RDC_OUT/resolves.txt): for every compute dispatch, the guest memory range it touches
and, for resolves, the decoded EDRAM source (base tile, pitch, MSAA, format) and the
destination (RB_COPY_DEST_INFO format and endianness). Use it to see which resolve
fills the memory a texture fetch reads, and to compare that sequence between builds.

The resolve and texture load shaders bind the guest shared memory as a storage buffer
at the destination/source page (resource RDC_SHARED, auto-detected as the most bound
buffer if unset) and pass ResolveCopyShaderConstants / LoadConstants as push constants.

Run: RDC_CAP=capture.rdc RDC_OUT=dir [RDC_FROM=eid RDC_TO=eid] qrenderdoc --python resolves.py
"""
import os, traceback, collections
CAP = os.environ["RDC_CAP"]; OUT = os.environ["RDC_OUT"]
FROM = int(os.environ.get("RDC_FROM", "0")); TO = int(os.environ.get("RDC_TO", str(1 << 31)))
os.makedirs(OUT, exist_ok=True)
log = open(os.path.join(OUT, "resolves.txt"), "w")

COLOR_RT = {0: "8888", 1: "8888_GAMMA", 2: "2_10_10_10", 3: "2_10_10_10_FLOAT", 4: "16_16",
            5: "16_16_16_16", 6: "16_16_FLOAT", 7: "16_16_16_16_FLOAT", 10: "2_10_10_10_AS_10_10_10_10",
            12: "2_10_10_10_FLOAT_AS_16_16_16_16", 14: "32_FLOAT", 15: "32_32_FLOAT"}
DEPTH_RT = {0: "D24S8", 1: "D24FS8"}
COLOR_FMT = {2: "8", 6: "8_8_8_8", 7: "2_10_10_10", 10: "8_8", 15: "4_4_4_4", 16: "10_11_11",
             17: "11_11_10", 24: "16", 25: "16_16", 26: "16_16_16_16", 30: "16_FLOAT",
             31: "16_16_FLOAT", 32: "16_16_16_16_FLOAT", 36: "32_FLOAT", 37: "32_32_FLOAT",
             38: "32_32_32_32_FLOAT"}


def edram(v):
    pitch, msaa, depth = v & 0x3FF, (v >> 10) & 3, (v >> 12) & 1
    base, fmt, b64 = (v >> 13) & 0x7FF, (v >> 24) & 0xF, (v >> 28) & 1
    name = (DEPTH_RT if depth else COLOR_RT).get(fmt, str(fmt))
    return f"edram base={base} pitch={pitch} msaa={1 << msaa}x {'depth' if depth else 'color'} {name}{' 64bpp' if b64 else ''}"


def dest(v):
    fmt = (v >> 7) & 0x3F
    return f"dest {COLOR_FMT.get(fmt, str(fmt))} endian={v & 7} exp_bias={((v >> 16) & 0x3F) - (64 if (v >> 16) & 0x20 else 0)} swap={(v >> 24) & 1}"


try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile(); cap.OpenFile(CAP, '', None)
    st, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)

    def walk(actions):
        for a in actions:
            yield a
            yield from walk(a.children)
    dispatches = [a for a in walk(ctrl.GetRootActions())
                  if a.flags & rd.ActionFlags.Dispatch and FROM <= a.eventId <= TO]
    log.write(f"dispatches {len(dispatches)}\n")

    rows = []
    usage = collections.Counter()
    for a in dispatches:
        ctrl.SetFrameEvent(a.eventId, False)
        p = ctrl.GetPipelineState()
        binds = []
        for b in p.GetReadWriteResources(rd.ShaderStage.Compute):
            d = b.descriptor
            binds.append((str(d.resource).split("::")[-1], d.byteOffset, d.byteSize))
            usage[binds[-1][0]] += 1
        consts = []
        refl = p.GetShaderReflection(rd.ShaderStage.Compute)
        if refl is not None:
            entry = p.GetShaderEntryPoint(rd.ShaderStage.Compute)
            for i, cb in enumerate(refl.constantBlocks):
                bind = p.GetConstantBlock(rd.ShaderStage.Compute, i, 0).descriptor
                try:
                    vs = ctrl.GetCBufferVariableContents(p.GetComputePipelineObject(), p.GetShader(rd.ShaderStage.Compute),
                                                         rd.ShaderStage.Compute, entry, i, bind.resource,
                                                         bind.byteOffset, bind.byteSize)
                except Exception:
                    continue

                def flat(vs):
                    for v in vs:
                        if v.members:
                            yield from flat(v.members)
                        else:
                            for k in range(v.rows * v.columns):
                                yield v.value.u32v[k]
                consts += list(flat(vs))
        rows.append((a.eventId, a.dispatchDimension, binds, consts))

    shared = os.environ.get("RDC_SHARED") or (usage.most_common(1)[0][0] if usage else "")
    log.write(f"shared memory resource {shared}\n")
    for eid, dim, binds, consts in rows:
        mem = [(off, size) for rid, off, size in binds if rid == shared]
        other = [(rid, off, size) for rid, off, size in binds if rid != shared]
        line = f"EID {eid} groups={tuple(dim)}"
        if mem:
            line += " guest=" + ",".join(f"{off:#x}+{size:#x}" for off, size in mem)
        line += " other=" + ",".join(f"{rid}@{off:#x}" for rid, off, _ in other)
        # A resolve binds the EDRAM buffer (whole) plus the destination in shared memory.
        is_resolve = mem and any(size >= (1 << 32) or size == 0xFFFFFFFFFFFFFFFF for _, _, size in other) and len(consts) >= 5
        if is_resolve:
            line += f" RESOLVE {edram(consts[0])} {dest(consts[2])} dest_base={consts[4]:#x}"
        line += " consts=[" + " ".join(f"{c:#x}" for c in consts[:12]) + "]"
        log.write(line + "\n")
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    log.write(traceback.format_exc())
log.close()
os._exit(0)

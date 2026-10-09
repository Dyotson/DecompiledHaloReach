"""Trigger a capture in a process already running under `renderdoccmd capture` (for
example Xenia, which takes no scripted input) and copy it to RDC_OUT/xenia_capture.rdc.

Run: RDC_OUT=dir qrenderdoc --python trigger_capture.py
"""
import os, time, traceback, shutil
OUT = os.environ.get("RDC_OUT", ".")
log = open(os.path.join(OUT, "trigger.txt"), "w")
try:
    import renderdoc as rd
    idents = []
    ident = rd.EnumerateRemoteTargets("localhost", 0)
    while ident != 0:
        idents.append(ident); ident = rd.EnumerateRemoteTargets("localhost", ident)
    log.write(f"targets: {idents}\n"); log.flush()
    tc = None
    for i in idents:
        tc = rd.CreateTargetControl("localhost", i, "reach-agent", True)
        if tc is not None:
            log.write(f"connected to {i}: {tc.GetTarget()} api={tc.GetAPI()} pid={tc.GetPID()}\n"); break
    if tc:
        tc.TriggerCapture(3)
        deadline = time.time() + 60
        got = 0
        while time.time() < deadline and got < 1:
            msg = tc.ReceiveMessage(None)
            if msg.type == rd.TargetControlMessageType.NewCapture:
                path = msg.newCapture.path
                log.write(f"new capture: {path} frame={msg.newCapture.frameNumber}\n")
                shutil.copy(path, os.path.join(OUT, "xenia_capture.rdc")); got += 1
            elif msg.type != rd.TargetControlMessageType.Noop:
                log.write(f"msg {msg.type}\n")
            log.flush()
            time.sleep(0.2)
        tc.Shutdown()
except Exception:
    log.write(traceback.format_exc())
log.close()
os._exit(0)

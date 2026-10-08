// Repairs Xenon register save/restore helpers (__savegprlr_N, __restgprlr_N,
// __savefpr_N, __restfpr_N, __savevmx_N, __restvmx_N).
//
// Each helper is a 4-byte stub that falls through into the next one, so Ghidra's
// non-returning-function analysis marks them all no-return. Every caller then
// stops disassembling right after `bl __savegprlr_N` in its prologue, leaving
// thousands of 8-byte "functions" the decompiler renders as empty.
//
// The analyzer also puts a CALL_RETURN flow override on each `bl` to a helper,
// so Ghidra treats the prologue call as a tail call that ends the function.
//
// This script clears both, disassembles each call's fall-through and recomputes
// the calling functions' bodies.
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.FlowOverride;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;
import java.util.regex.Pattern;

public class ReachFixSaveRestHelpers extends GhidraScript {
    @Override
    public void run() throws Exception {
        Pattern helperName = Pattern.compile("^__(save|rest)(gpr|gprlr|fpr|vmx)_\\d+$");
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing listing = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();

        List<Function> helpers = new ArrayList<>();
        for (Function f : fm.getFunctions(true)) {
            if (helperName.matcher(f.getName()).matches()) helpers.add(f);
        }
        int cleared = 0;
        for (Function f : helpers) {
            if (f.hasNoReturn()) {
                f.setNoReturn(false);
                cleared++;
            }
        }

        int overrides = 0;
        Set<Address> fallThroughs = new TreeSet<>();
        Set<Function> callers = new HashSet<>();
        for (Function f : helpers) {
            for (Reference r : rm.getReferencesTo(f.getEntryPoint())) {
                if (!r.getReferenceType().isCall()) continue;
                Address from = r.getFromAddress();
                Instruction ins = listing.getInstructionAt(from);
                if (ins == null) continue;
                if (ins.getFlowOverride() != FlowOverride.NONE) {
                    ins.setFlowOverride(FlowOverride.NONE);
                    overrides++;
                }
                fallThroughs.add(from.add(4));
                Function caller = fm.getFunctionContaining(from);
                if (caller != null) callers.add(caller);
            }
        }

        int disassembled = 0;
        for (Address ft : fallThroughs) {
            if (monitor.isCancelled()) break;
            if (listing.getInstructionAt(ft) != null || listing.getDefinedDataAt(ft) != null) continue;
            DisassembleCommand cmd = new DisassembleCommand(ft, null, true);
            if (cmd.applyTo(currentProgram, monitor)) disassembled++;
        }

        int grown = 0;
        long bytesBefore = 0, bytesAfter = 0;
        for (Function caller : callers) {
            if (monitor.isCancelled()) break;
            long before = caller.getBody().getNumAddresses();
            CreateFunctionCmd.fixupFunctionBody(currentProgram, caller, monitor);
            long after = caller.getBody().getNumAddresses();
            bytesBefore += before;
            bytesAfter += after;
            if (after > before) grown++;
        }

        println(String.format(
            "helpers=%d no_return_cleared=%d overrides_cleared=%d call_sites=%d disassembled=%d callers=%d grown=%d bytes %d -> %d",
            helpers.size(), cleared, overrides, fallThroughs.size(), disassembled, callers.size(), grown,
            bytesBefore, bytesAfter));
    }
}

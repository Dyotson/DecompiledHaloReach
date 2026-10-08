// Repairs functions that Ghidra truncates at a `bl` in default.xex.
//
// Root cause: the Xenon register save helpers (`__savegprlr_N`, the FPR/VMX
// save chains at 0x827F27xx) are 4-byte stubs that fall through into each
// other, so Ghidra's non-returning-function analysis flags them no-return and
// puts a CALL_RETURN flow override on every `bl` to them. Every prologue then
// ends at its first instruction pair, those truncated functions look
// non-returning too, and the error cascades to their callers (1,300+ wrongly
// no-return functions, ~10k truncated callers).
//
// This script iterates to a fixed point:
//   1. clear the no-return flag on every internal function except imports;
//   2. clear CALL_RETURN overrides on every `bl` and disassemble the
//      fall-through;
//   3. recompute the bodies of all affected functions;
// then re-flows any function whose body still ends mid-flow, and re-flags as
// no-return only the formerly no-return functions whose repaired body has no
// return path (no `b..lr`, no branch leaving the body, no fall-through out of
// it as in the save/restore stub chains).
// Freshly created functions get the override again, so re-run this after
// /create_function.
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.FlowOverride;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.regex.Pattern;

public class ReachFixSaveRestHelpers extends GhidraScript {
    private static final Pattern RETURN_MNEMONIC = Pattern.compile("^b[a-z]*lr[+-]?$");

    @Override
    public void run() throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        Listing listing = currentProgram.getListing();

        // Imports resolve to kernel thunks; keep their flags (KeBugCheck etc.).
        Set<Address> formerlyNoReturn = new HashSet<>();
        for (Function f : fm.getFunctions(true)) {
            if (!f.hasNoReturn() || f.isExternal() || f.isThunk()) continue;
            if (f.getName().startsWith("Ke") || f.getName().startsWith("Ex")
                    || f.getName().startsWith("Xam") || f.getName().startsWith("Nt")) continue;
            formerlyNoReturn.add(f.getEntryPoint());
            f.setNoReturn(false);
        }

        int totalOverrides = 0, totalDisassembled = 0, rounds = 0;
        long bytesBefore = -1, bytesAfter = 0;
        Set<Address> touched = new HashSet<>(formerlyNoReturn);
        while (rounds < 10) {
            rounds++;
            List<Instruction> overridden = new ArrayList<>();
            InstructionIterator it = listing.getInstructions(true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                if (ins.getFlowOverride() != FlowOverride.CALL_RETURN
                        || !ins.getMnemonicString().equals("bl")) continue;
                Address[] flows = ins.getFlows();
                Function target = flows.length > 0 ? fm.getFunctionAt(flows[0]) : null;
                if (target != null && target.hasNoReturn()) continue;
                overridden.add(ins);
            }
            if (overridden.isEmpty() && rounds > 1) break;

            List<Address> fallThroughs = new ArrayList<>();
            for (Instruction ins : overridden) {
                ins.setFlowOverride(FlowOverride.NONE);
                fallThroughs.add(ins.getAddress().add(4));
                Function caller = fm.getFunctionContaining(ins.getAddress());
                if (caller != null) touched.add(caller.getEntryPoint());
            }
            totalOverrides += overridden.size();

            for (Address ft : fallThroughs) {
                if (monitor.isCancelled()) return;
                if (listing.getInstructionAt(ft) != null || listing.getDefinedDataAt(ft) != null) continue;
                if (new DisassembleCommand(ft, null, true).applyTo(currentProgram, monitor)) {
                    totalDisassembled++;
                }
            }

            long before = 0, after = 0;
            for (Address entry : touched) {
                if (monitor.isCancelled()) return;
                Function f = fm.getFunctionAt(entry);
                if (f == null) continue;
                before += f.getBody().getNumAddresses();
                CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
                after += f.getBody().getNumAddresses();
            }
            if (bytesBefore < 0) bytesBefore = before;
            bytesAfter = after;
            if (overridden.isEmpty()) break;
        }

        // Functions created after the analyzer ran (or never reached above) can
        // still end mid-flow at a call whose override is already gone.
        // Analysis also created default-named fragments (FUN_xxxxxxxx) starting
        // right after a truncated prologue; nothing calls them, they are the
        // rest of the real function. Merge those back.
        int refixed = 0, merged = 0;
        List<Function> all = new ArrayList<>();
        for (Function f : fm.getFunctions(true)) all.add(f);
        for (Function f : all) {
            if (monitor.isCancelled()) return;
            if (f.isExternal() || fm.getFunctionAt(f.getEntryPoint()) == null) continue;
            Address cont = endsMidFlow(f);
            if (cont == null) continue;
            Function fragment = fm.getFunctionAt(cont);
            if (fragment != null) {
                if (!fragment.getName().startsWith("FUN_") || isCalled(fragment)) continue;
                fm.removeFunction(cont);
                merged++;
            }
            long before = f.getBody().getNumAddresses();
            CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
            if (f.getBody().getNumAddresses() > before) refixed++;
        }

        int reflagged = 0;
        for (Address entry : formerlyNoReturn) {
            Function f = fm.getFunctionAt(entry);
            if (f != null && !hasReturnPath(f)) {
                f.setNoReturn(true);
                reflagged++;
            }
        }

        println(String.format(
            "rounds=%d noreturn_cleared=%d reflagged_noreturn=%d overrides_cleared=%d disassembled=%d "
                + "touched=%d bytes %d -> %d refixed_mid_flow=%d merged_fragments=%d",
            rounds, formerlyNoReturn.size(), reflagged, totalOverrides, totalDisassembled,
            touched.size(), bytesBefore, bytesAfter, refixed, merged));
    }

    // Returns the address a body falls through to past its end after a call
    // (the body was cut short there), or null. Save/restore stubs, which fall
    // into the next stub without a call, are not reported.
    private Address endsMidFlow(Function f) {
        AddressSetView body = f.getBody();
        InstructionIterator it = currentProgram.getListing().getInstructions(body, true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            Address ft = ins.getFallThrough();
            if (ft != null && !body.contains(ft) && ins.getFlowType().isCall()
                    && currentProgram.getListing().getInstructionAt(ft) != null) {
                return ft;
            }
        }
        return null;
    }

    private boolean isCalled(Function f) {
        for (var ref : currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint())) {
            if (ref.getReferenceType().isCall() || ref.getReferenceType().isData()) return true;
        }
        return false;
    }

    private boolean hasReturnPath(Function f) {
        AddressSetView body = f.getBody();
        InstructionIterator it = currentProgram.getListing().getInstructions(body, true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            String m = ins.getMnemonicString();
            // Save/restore stubs fall through into the next stub of the chain.
            Address ft = ins.getFallThrough();
            if (ft != null && !body.contains(ft)) return true;
            if (RETURN_MNEMONIC.matcher(m).matches()) return true;
            if (m.equals("bctr")) return true;  // jump table or tail call
            if (m.equals("b")) {
                Address[] flows = ins.getFlows();
                if (flows.length > 0 && !body.contains(flows[0])) {
                    Function target = currentProgram.getFunctionManager().getFunctionAt(flows[0]);
                    if (target == null || !target.hasNoReturn()) return true;  // tail call
                }
            }
        }
        return false;
    }
}

// Ghidra headless post-script: prepare an OS-9/68k program module and export decompiled C.
// Args: <entries.txt> <out.c>
//   entries.txt: one hex address per line (function starts), "trap <addr>" lines for TRAP sites.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.data.*;
import ghidra.program.model.symbol.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class DecompileOS9 extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        List<String> lines = Files.readAllLines(Paths.get(args[0]));
        Listing listing = currentProgram.getListing();
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        List<Address> funcs = new ArrayList<>();
        Map<Address, String> traps = new LinkedHashMap<>();
        for (String l : lines) {
            l = l.trim();
            if (l.isEmpty()) continue;
            String[] p = l.split("\\s+");
            if (p[0].equals("trap")) traps.put(sp.getAddress(Long.parseLong(p[1], 16)), p[2]);
            else if (p[0].equals("label")) createLabel(sp.getAddress(Long.parseLong(p[1], 16)), p[2], true);
            else funcs.add(sp.getAddress(Long.parseLong(p[0], 16)));
        }
        // TRAP #n is followed by a function-code word: claim the words as data before
        // any disassembly so flow never decodes them as instructions.
        for (Address a : traps.keySet()) createData(a.add(2), WordDataType.dataType);
        for (Address a : funcs) {
            if (listing.getInstructionAt(a) == null) disassemble(a);
        }
        for (Map.Entry<Address, String> e : traps.entrySet()) {
            Address a = e.getKey();
            if (listing.getInstructionAt(a) == null) disassemble(a);
            Instruction ins = listing.getInstructionAt(a);
            setEOLComment(a, e.getValue());
            if (ins != null && !e.getValue().equals("F$Exit")) {
                ins.setFallThrough(a.add(4));
                disassemble(a.add(4));
            }
        }
        for (Address a : funcs) {
            if (getFunctionAt(a) == null) createFunction(a, null);
        }
        analyzeChanges(currentProgram);
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(new FileWriter(args[1]))) {
            out.println("/* Decompiled by Ghidra from " + currentProgram.getName() + " (OS-9/68k module). */");
            out.println("/* Raw decompiler output: a reference backup, not yet compilable. */\n");
            int ok = 0, bad = 0;
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                DecompileResults r = d.decompileFunction(f, 60, monitor);
                if (r.decompileCompleted()) { out.println(r.getDecompiledFunction().getC()); ok++; }
                else { out.println("/* FAILED " + f.getName() + ": " + r.getErrorMessage() + " */\n"); bad++; }
            }
            println("decompiled " + ok + " functions, " + bad + " failed");
        }
    }
}

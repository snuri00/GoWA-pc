import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;

import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeSet;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args[0]);
        outDir.mkdirs();
        List<String> targets = new ArrayList<>();
        for (int i = 1; i < args.length; i++) {
            if (args[i].startsWith("@")) {
                for (String line : Files.readAllLines(new File(args[i].substring(1)).toPath())) {
                    if (!line.isBlank()) targets.add(line.trim());
                }
            } else if (args[i].startsWith("names=")) {
                applyNames(new File(args[i].substring(6)));
            } else {
                targets.add(args[i]);
            }
        }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        for (String t : targets) {
            Address a = toAddr(Long.parseLong(t.replace("0x", "").replace("f_", ""), 16));
            Function f = getFunctionContaining(a);
            if (f == null) {
                f = createFunction(a, null);
            }
            if (f == null) {
                println("no function at " + t);
                continue;
            }
            DecompileResults r = di.decompileFunction(f, 120, monitor);
            TreeSet<String> callers = new TreeSet<>();
            for (Reference ref : getReferencesTo(f.getEntryPoint())) {
                Function c = getFunctionContaining(ref.getFromAddress());
                callers.add(c == null ? ref.getFromAddress().toString() : c.getName() + "@" + ref.getFromAddress());
            }
            TreeSet<String> callees = new TreeSet<>();
            for (Function c : f.getCalledFunctions(monitor)) callees.add(c.getName());
            File out = new File(outDir, String.format("%08x.c", f.getEntryPoint().getOffset()));
            try (PrintWriter w = new PrintWriter(out)) {
                w.println("// " + f.getName() + " " + f.getEntryPoint());
                w.println("// callers: " + String.join(", ", callers));
                w.println("// callees: " + String.join(", ", callees));
                w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
            }
            println("wrote " + out);
        }
        di.dispose();
    }

    private void applyNames(File f) throws Exception {
        if (!f.exists()) return;
        for (String line : Files.readAllLines(f.toPath())) {
            String[] p = line.trim().split("\\s+");
            if (p.length < 2 || p[0].startsWith("#")) continue;
            Address a = toAddr(Long.parseLong(p[0].replace("0x", ""), 16));
            Function fn = getFunctionAt(a);
            if (fn == null) fn = createFunction(a, null);
            if (fn != null) fn.setName(p[1], SourceType.USER_DEFINED);
        }
    }
}

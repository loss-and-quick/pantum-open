import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.util.task.ConsoleTaskMonitor;
import java.io.*;

public class DecompAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = System.getenv("DECOMP_OUT");
        if (out == null) out = "/tmp/decomp.c";
        PrintWriter pw = new PrintWriter(new BufferedWriter(new FileWriter(out)));
        DecompInterface di = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        di.setOptions(opts);
        di.toggleCCode(true);
        di.toggleSyntaxTree(true);
        di.setSimplificationStyle("decompile");
        di.openProgram(currentProgram);
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        int n = 0;
        while (it.hasNext()) {
            Function f = it.next();
            if (f.isExternal() || f.isThunk()) continue;
            DecompileResults res = di.decompileFunction(f, 120, new ConsoleTaskMonitor());
            pw.println("/* ===== " + f.getName() + " @ " + f.getEntryPoint() + " ===== */");
            if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
                pw.println(res.getDecompiledFunction().getC());
            } else {
                pw.println("// decompile failed: " + (res==null?"null":res.getErrorMessage()));
            }
            pw.println();
            n++;
        }
        pw.close();
        println("decompiled " + n + " functions to " + out);
    }
}

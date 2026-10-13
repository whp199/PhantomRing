// Headless helper: decompile functions / dump disassembly / list xrefs for PhantomRing RE.
// Args (space separated): <outFile> <cmd>:<addr> ...   cmd = dec | dis | xref | name
//   dec:<addr>   decompile the function containing addr
//   dis:<addr>   disassemble 40 instructions from addr
//   xref:<addr>  list references to addr (with containing functions)
//   name:<addr>  function name/bounds at addr
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

import java.io.FileWriter;
import java.io.PrintWriter;

public class Decompile extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		PrintWriter out = new PrintWriter(new FileWriter(args[0]));
		DecompInterface dec = new DecompInterface();
		dec.openProgram(currentProgram);
		for (int i = 1; i < args.length; i++) {
			String[] parts = args[i].split(":", 2);
			String cmd = parts[0];
			Address a = currentProgram.getAddressFactory().getAddress(parts[1]);
			out.println("==== " + args[i] + " ====");
			if (a == null) {
				out.println("bad address");
				continue;
			}
			Function f = getFunctionContaining(a);
			switch (cmd) {
			case "dec": {
				if (f == null) {
					disassemble(a);
					createFunction(a, null);
					f = getFunctionContaining(a);
				}
				if (f == null) {
					out.println("no function");
					break;
				}
				out.println("// " + f.getName() + " @ " + f.getEntryPoint() + " .. " + f.getBody().getMaxAddress());
				DecompileResults r = dec.decompileFunction(f, 120, monitor);
				out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : ("decompile failed: " + r.getErrorMessage()));
				break;
			}
			case "dis": {
				Instruction ins = getInstructionAt(a);
				if (ins == null) {
					disassemble(a);
					ins = getInstructionAt(a);
				}
				for (int n = 0; n < 40 && ins != null; n++) {
					out.println(ins.getAddress() + "  " + ins);
					ins = ins.getNext();
				}
				break;
			}
			case "xref": {
				int n = 0;
				for (Reference r : getReferencesTo(a)) {
					Function from = getFunctionContaining(r.getFromAddress());
					out.println(r.getFromAddress() + "  " + r.getReferenceType() + "  in " + (from == null ? "?" : from.getName() + "@" + from.getEntryPoint()));
					if (++n > 200) {
						out.println("...");
						break;
					}
				}
				break;
			}
			case "name": {
				out.println(f == null ? "no function" : (f.getName() + " " + f.getEntryPoint() + " .. " + f.getBody().getMaxAddress()));
				break;
			}
			default:
				out.println("unknown command " + cmd);
			}
			out.flush();
		}
		out.close();
	}
}

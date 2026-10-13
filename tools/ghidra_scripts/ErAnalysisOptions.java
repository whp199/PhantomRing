// Pre-analysis script for eldenring.exe: Arxan's obfuscated code makes the address-table and
// switch analyzers explode (OutOfMemory), so turn them off before auto-analysis.
import ghidra.app.script.GhidraScript;

public class ErAnalysisOptions extends GhidraScript {
	@Override
	protected void run() throws Exception {
		String[] off = { "Create Address Tables", "Decompiler Switch Analysis", "Aggressive Instruction Finder",
			"Decompiler Parameter ID", "Non-Returning Functions - Discovered", "Condense Filler Bytes" };
		for (String name : off) {
			try {
				setAnalysisOption(currentProgram, name, "false");
				println("disabled analyzer: " + name);
			} catch (Exception e) {
				println("could not disable " + name + ": " + e.getMessage());
			}
		}
	}
}

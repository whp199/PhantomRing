// Pre-analysis script for mgsvtpp.exe 1.0.15.4: Denuvo's 111 MB RWX ".xdata" VM section makes auto
// analysis run out of memory and holds no game code we need (the real MSVC code is in ".xtext").
// Remove it, and turn off the analyzers that explode on obfuscated code.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class MgsvAnalysisOptions extends GhidraScript {
	@Override
	protected void run() throws Exception {
		Memory memory = currentProgram.getMemory();
		for (MemoryBlock block : memory.getBlocks()) {
			if (block.getName().equals(".xdata")) {
				println("removing Denuvo block " + block.getName() + " @ " + block.getStart() + " size " + block.getSize());
				memory.removeBlock(block, monitor);
			}
		}
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

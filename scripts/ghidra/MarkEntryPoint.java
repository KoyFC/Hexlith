// Marks the entry point of a raw binary import and makes a function there.
//
// A headerless image tells Ghidra nothing about where execution starts, so its
// auto-analysis has nowhere to begin. Run as a pre-script with the address as
// the only argument:
//
//   -preScript MarkEntryPoint.java 0x00100000

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

public class MarkEntryPoint extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) {
            throw new IllegalArgumentException("expected the entry address as the only argument");
        }
        Address entry = toAddr(Long.decode(args[0]));
        currentProgram.getSymbolTable().addExternalEntryPoint(entry);
        createFunction(entry, "entry");
    }
}

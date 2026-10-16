// Writes what Ghidra's analysis concluded, in the line format that
// scripts/compare_ghidra_analysis.py reads. Run as a post-script with the
// output directory as the only argument:
//
//   -postScript ExportAnalysisFacts.java <directory>
//
// Three files, one record per line, every address in fixed-width hexadecimal:
//
//   functions.txt  each function entry, its instruction set and whether Ghidra
//                  calls it a thunk
//   code.txt       each run of consecutive instructions in one instruction set
//   jumps.txt      each computed jump, with every target Ghidra resolved for it
//
// Only addresses and counts are written. Nothing here copies an instruction, a
// decompiled line or a byte of the program, so the output is the same kind of
// fact the analysis tables hold. It still belongs in the workspace beside them.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.PrintWriter;
import java.math.BigInteger;

public class ExportAnalysisFacts extends GhidraScript {
    // The context register Ghidra's ARM language uses for the T bit.
    private static final String THUMB_MODE_REGISTER = "TMode";

    private Register thumbMode;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) {
            throw new IllegalArgumentException("expected the output directory as the only argument");
        }
        File directory = new File(args[0]);
        thumbMode = currentProgram.getRegister(THUMB_MODE_REGISTER);

        writeFunctions(new File(directory, "functions.txt"));
        writeCode(new File(directory, "code.txt"));
        writeJumps(new File(directory, "jumps.txt"));
    }

    private String instructionSet(Address address) {
        BigInteger value = currentProgram.getProgramContext().getValue(thumbMode, address, false);
        return value != null && value.intValue() == 1 ? "thumb" : "arm";
    }

    private void writeFunctions(File file) throws IOException {
        try (PrintWriter out = new PrintWriter(new FileWriter(file))) {
            for (Function function : currentProgram.getFunctionManager().getFunctions(true)) {
                Address entry = function.getEntryPoint();
                out.printf("0x%08X isa=%s thunk=%s%n",
                           entry.getOffset(), instructionSet(entry), function.isThunk() ? "yes" : "no");
            }
        }
    }

    private void writeCode(File file) throws IOException {
        try (PrintWriter out = new PrintWriter(new FileWriter(file))) {
            long start = -1;
            long end = -1;
            String set = "";
            InstructionIterator instructions = currentProgram.getListing().getInstructions(true);
            while (instructions.hasNext()) {
                Instruction instruction = instructions.next();
                long address = instruction.getAddress().getOffset();
                String current = instructionSet(instruction.getAddress());
                if (address == end && current.equals(set)) {
                    end = address + instruction.getLength();
                    continue;
                }
                if (start >= 0) {
                    out.printf("0x%08X size=0x%X isa=%s%n", start, end - start, set);
                }
                start = address;
                end = address + instruction.getLength();
                set = current;
            }
            if (start >= 0) {
                out.printf("0x%08X size=0x%X isa=%s%n", start, end - start, set);
            }
        }
    }

    private void writeJumps(File file) throws IOException {
        try (PrintWriter out = new PrintWriter(new FileWriter(file))) {
            InstructionIterator instructions = currentProgram.getListing().getInstructions(true);
            while (instructions.hasNext()) {
                Instruction instruction = instructions.next();
                if (!instruction.getFlowType().isComputed() || !instruction.getFlowType().isJump()) {
                    continue;
                }
                StringBuilder targets = new StringBuilder();
                int count = 0;
                for (Reference reference : instruction.getReferencesFrom()) {
                    if (reference.getReferenceType().isFlow()) {
                        targets.append(String.format(" 0x%08X", reference.getToAddress().getOffset()));
                        ++count;
                    }
                }
                out.printf("0x%08X targets=%d%s%n", instruction.getAddress().getOffset(), count, targets);
            }
        }
    }
}

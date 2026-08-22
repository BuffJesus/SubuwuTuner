// Seed byte-verified A8DH early-boot functions missed by default analysis.
// Evidence: reset vector[0]=0x00000B68; the reset wrapper directly calls
// 0xB7C and 0xBAA, then dispatches through pointer cell 0xCA8 -> 0x6FC.
// These are architecture/control-flow labels only, not semantic claims.
// @category Subaru
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.SourceType;

public class SubuwuSeedA8dhBootChain extends GhidraScript {
    private void seed(long offset, String name) throws Exception {
        Address address = toAddr(offset);
        if (getInstructionAt(address) == null) disassemble(address);
        Function function = getFunctionAt(address);
        if (function == null) function = createFunction(address, name);
        if (function != null) function.setName(name, SourceType.USER_DEFINED);
        addEntryPoint(address);
    }

    @Override
    public void run() throws Exception {
        Memory memory = currentProgram.getMemory();
        long reset = Integer.toUnsignedLong(memory.getInt(toAddr(0)));
        long indirect = Integer.toUnsignedLong(memory.getInt(toAddr(0xCA8)));
        if (reset != 0xB68 || indirect != 0x6FC) {
            throw new IllegalStateException(String.format(
                "unexpected A8DH boot anchors: reset=0x%X indirect=0x%X", reset, indirect));
        }
        seed(0xB38, "exception_entry_distinct_a8dh");
        seed(0xB5A, "exception_entry_default_a8dh");
        seed(reset, "boot_reset_entry_a8dh");
        seed(0xB7C, "boot_stage_1_a8dh");
        seed(0xBAA, "boot_stage_2_a8dh");
        seed(indirect, "boot_stage_3_indirect_a8dh");
        println("A8DH vectors/boot seeded: exceptions={0xB38,0xB5A}; boot=0xB68 -> {0xB7C,0xBAA} -> [0xCA8]=0x6FC");
    }
}

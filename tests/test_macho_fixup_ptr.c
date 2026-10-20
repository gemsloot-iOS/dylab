/* Unit test: macho_fixup_ptr chained rebase vs plain strip. */
#include "../DyLabApp/Bridge/MachOMap.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_eq(const char *name, uint64_t got, uint64_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got 0x%llx want 0x%llx\n", name,
                (unsigned long long)got, (unsigned long long)want);
        failures++;
    } else {
        printf("OK   %s\n", name);
    }
}

int main(void) {
    MachOMap plain;
    MachOMap chained;
    memset(&plain, 0, sizeof(plain));
    memset(&chained, 0, sizeof(chained));
    chained.has_chained_fixups = 1;

    /* Plain image: full strip mask, no 36-bit clamp. */
    expect_eq("plain strip", macho_fixup_ptr(&plain, 0x0000000112345678ull),
              macho_strip_ptr(0x0000000112345678ull));

    /* Chained rebase: bind bit clear, target in low 36 bits. */
    {
        uint64_t target = 0x100004000ull;
        uint64_t next = 0x10ull; /* non-zero next in high nibble area */
        uint64_t raw = target | (next << 36);
        expect_eq("chained rebase", macho_fixup_ptr(&chained, raw), target);
    }

    /* Chained bind: bit 62 set → 0 (not an internal pointer). */
    expect_eq("chained bind",
              macho_fixup_ptr(&chained, (1ull << 62) | 0x1234ull), 0);

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    puts("ALL PASS");
    return 0;
}

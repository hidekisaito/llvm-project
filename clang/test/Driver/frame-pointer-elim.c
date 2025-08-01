// KEEP-ALL-NOT:  warning: argument unused
// KEEP-ALL:      "-mframe-pointer=all"
// KEEP-NON-LEAF-NOT: warning: argument unused
// KEEP-NON-LEAF: "-mframe-pointer=non-leaf"
// KEEP-NONE-NOT: warning: argument unused
// KEEP-NONE:     "-mframe-pointer=none"
// KEEP-RESERVED-NOT: warning: argument unused
// KEEP-RESERVED: "-mframe-pointer=reserved"

// On Linux x86, omit frame pointer when optimization is enabled.
// RUN: %clang -### --target=i386-linux -S -fomit-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s
// RUN: %clang -### --target=i386-linux -S -O1 %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s

// -fno-omit-frame-pointer or -pg disables frame pointer omission.
// RUN: %clang -### --target=i386-linux -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=i386-linux -S -O1 -fno-omit-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=i386-linux -S -O1 -pg %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s

// -momit-leaf-frame-pointer omits leaf frame pointer.
// -fno-omit-frame-pointer loses out to -momit-leaf-frame-pointer.
// RUN: %clang -### --target=i386 -S -momit-leaf-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s
// RUN: %clang -### --target=i386-linux -S -O1 -fno-omit-frame-pointer -momit-leaf-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s
// RUN: %clang -### --target=i386-linux -S -O1 -momit-leaf-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s

// fno-omit-frame-pointer -momit-leaf-frame-pointer can be overwritten by
// fomit-frame-pointer later on the command without warning
// RUN: %clang -### --target=i386-linux -S -O1 -fno-omit-frame-pointer -momit-leaf-frame-pointer -fomit-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s

// RUN: %clang -### --target=i386-linux -S -O1 -fno-omit-frame-pointer -momit-leaf-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s
// Explicit or default -fomit-frame-pointer wins over -mno-omit-leaf-frame-pointer.
// RUN: %clang -### --target=i386 -S %s -fomit-frame-pointer -mno-omit-leaf-frame-pointer 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s
// RUN: %clang -### --target=i386-linux -S %s -O1 -mno-omit-leaf-frame-pointer 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s

// -pg -fomit-frame-pointer => error.
// RUN: not %clang -### --target=i386-linux -S -fomit-frame-pointer -pg %s 2>&1 | FileCheck -check-prefix=CHECK-NO-MIX-OMIT-FP-PG %s
// RUN: %clang -### --target=i386-linux -S -fomit-frame-pointer -fno-omit-frame-pointer -pg %s 2>&1 | FileCheck -check-prefix=CHECK-MIX-NO-OMIT-FP-PG %s
// CHECK-NO-MIX-OMIT-FP-PG: '-fomit-frame-pointer' not allowed with '-pg'
// CHECK-MIX-NO-OMIT-FP-PG-NOT: '-fomit-frame-pointer' not allowed with '-pg'

// NetBSD follows the same rules as Linux.
// RUN: %clang -### --target=x86_64-unknown-netbsd -S -O1 %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s

// RUN: %clang -### --target=x86_64-unknown-netbsd -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s

// Darwin disables omitting the leaf frame pointer even under optimization
// unless the command lines are given.
// RUN: %clang -### -target i386-apple-darwin -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s

// RUN: %clang -### -target i386-apple-darwin -S -O1 %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s

// RUN: %clang -### --target=i386-darwin -S -fomit-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s

// RUN: %clang -### --target=i386-darwin -S -momit-leaf-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s

// RUN: %clang -### -target armv7s-apple-ios -fomit-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=WARN-OMIT-7S %s
// WARN-OMIT-7S: warning: optimization flag '-fomit-frame-pointer' is not supported for target 'armv7s'
// WARN-OMIT-7S: "-mframe-pointer=all"

// RUN: %clang -### -target armv7k-apple-watchos -fomit-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=WARN-OMIT-7K %s
// WARN-OMIT-7K: warning: optimization flag '-fomit-frame-pointer' is not supported for target 'armv7k'
// WARN-OMIT-7K: "-mframe-pointer=all"

// RUN: %clang -### -target armv7s-apple-ios8.0 -momit-leaf-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=WARN-OMIT-LEAF-7S %s
// WARN-OMIT-LEAF-7S-NOT: warning: optimization flag '-momit-leaf-frame-pointer' is not supported for target 'armv7s'
// WARN-OMIT-LEAF-7S: "-mframe-pointer=non-leaf"

// On AArch64, PS4, PS5, and VE, default to omitting the frame pointer on leaf
// functions
// RUN: %clang -### --target=aarch64 -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s
// RUN: %clang -### --target=x86_64-scei-ps4 -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s
// RUN: %clang -### --target=x86_64-scei-ps4 -S -O2 %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s
// RUN: %clang -### --target=x86_64-sie-ps5 -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s
// RUN: %clang -### --target=x86_64-sie-ps5 -S -O2 %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s
// RUN: %clang -### -target aarch64-apple-darwin -arch arm64_32 -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s
// RUN: %clang -### --target=ve-unknown-linux-gnu -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s
// RUN: %clang -### --target=aarch64-linux-android -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s
// RUN: %clang -### --target=aarch64-linux-android -S -O2 %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s
// RUN: %clang -### --target=aarch64-linux-android -S -Os %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s

// RUN: %clang -### --target=powerpc64 -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=powerpc64 -S -O1 %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s

// SPARC targets omit the frame pointer when optimizations are enabled.
// RUN: %clang -### --target=sparc -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=sparc -S -O1 %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s
// RUN: %clang -### --target=sparcel -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=sparcel -S -O1 %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s
// RUN: %clang -### --target=sparc64 -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=sparc64 -S -O1 %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s

// M68k targets omit the frame pointer when optimizations are enabled.
// RUN: %clang -### --target=m68k -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=m68k -S -O1 %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s

// For AAarch32 (A32, T32) linux targets, default omit frame pointer when
// optimizations are enabled.
// RUN: %clang -### --target=arm-linux-gnueabihf- -marm -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=arm-linux-gnueabihf- -mthumb -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=arm-linux-gnueabihf- -marm -mbig-endian -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=arm-linux-gnueabihf- -mthumb -mbig-endian -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=arm-linux-gnueabihf- -marm -O1 -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s
// RUN: %clang -### --target=arm-linux-gnueabihf- -mthumb -O1 -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s
// RUN: %clang -### --target=arm-linux-gnueabihf- -marm -mbig-endian -O1 -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s
// RUN: %clang -### --target=arm-linux-gnueabihf- -mthumb -mbig-endian -O1 -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s
// For Android, keep the framepointers always.
// RUN: %clang -### --target=armv7a-linux-androideabi- -marm -O1 -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=armv7a-linux-androideabi- -mthumb -O1 -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=armv7a-linux-androideabi- -marm -mbig-endian -O1 -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=armv7a-linux-androideabi- -mthumb -mbig-endian -O1 -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=riscv64-linux-android -O1 -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s
// RUN: %clang -### --target=riscv64-linux-android -mbig-endian -O1 -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s

// On ARM backend bare metal targets, frame pointer is omitted
// RUN: %clang -### --target=arm-arm-none-eabi -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s
// RUN: %clang -### --target=arm-arm-none-eabihf -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s
// RUN: %clang -### --target=arm-arm-none-eabi -S -fno-omit-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=arm-arm-none-eabihf -S -fno-omit-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=arm-arm-none-eabi -S -O1 %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s
// RUN: %clang -### --target=arm-arm-none-eabihf -S -O1 %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s
// RUN: %clang -### --target=arm-arm-none-eabi -S -O1 -fno-omit-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=arm-arm-none-eabihf -S -O1 -fno-omit-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=armeb-arm-none-eabi -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s
// RUN: %clang -### --target=thumb-arm-none-eabi -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s
// RUN: %clang -### --target=thumbeb-arm-none-eabi -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-NONE %s

// Check that for Apple bare metal targets, we're keeping frame pointers by default
// RUN: %clang -### --target=thumbv6m-apple-none-macho -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=thumbv6m-apple-none-macho -S -fno-omit-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=arm-apple-none-macho -S %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=arm-apple-none-macho -S -fno-omit-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=thumbv6m-apple-none-macho -S -O1 %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=thumbv6m-apple-none-macho -S -O1 -fno-omit-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=arm-apple-none-macho -S -O1 %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s
// RUN: %clang -### --target=arm-apple-none-macho -S -O1 -fno-omit-frame-pointer %s 2>&1 | \
// RUN:   FileCheck --check-prefix=KEEP-ALL %s

// AArch64 bare metal targets behave like hosted targets
// RUN: %clang -### --target=aarch64-none-elf -S %s 2>&1 |  \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s
// RUN: %clang -### --target=aarch64-none-elf -S -O1 %s 2>&1 |  \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s
// RUN: %clang -### --target=aarch64-none-elf -S -fno-omit-frame-pointer %s 2>&1 |  \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s
// RUN: %clang -### --target=aarch64-none-elf -S -O1 -fno-omit-frame-pointer %s 2>&1 |  \
// RUN:   FileCheck --check-prefix=KEEP-NON-LEAF %s

// AArch64 Windows requires that the frame pointer be reserved
// RUN: %clang -### --target=aarch64-pc-windows-msvc -S -fomit-frame-pointer %s 2>&1 |  \
// RUN:   FileCheck --check-prefix=KEEP-RESERVED %s

void f0() {}
void f1() { f0(); }

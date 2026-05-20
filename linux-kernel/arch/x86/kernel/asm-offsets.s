	.file	"asm-offsets.c"
# GNU C89 (GCC) version 8.5.0 20210514 (Red Hat 8.5.0-28) (x86_64-redhat-linux)
#	compiled by GNU C version 8.5.0 20210514 (Red Hat 8.5.0-28), GMP version 6.1.2, MPFR version 3.1.6-p2, MPC version 1.1.0, isl version none
# GGC heuristics: --param ggc-min-expand=100 --param ggc-min-heapsize=131072
# options passed:  -nostdinc -I ./arch/x86/include
# -I ./arch/x86/include/generated -I ./include/drm-backport -I ./include
# -I ./arch/x86/include/uapi -I ./arch/x86/include/generated/uapi
# -I ./include/uapi -I ./include/generated/uapi -D __KERNEL__
# -D CC_HAVE_ASM_GOTO -D CONFIG_AS_CFI=1 -D CONFIG_AS_CFI_SIGNAL_FRAME=1
# -D CONFIG_AS_CFI_SECTIONS=1 -D CONFIG_AS_SSSE3=1 -D CONFIG_AS_CRC32=1
# -D CONFIG_AS_AVX=1 -D CONFIG_AS_AVX2=1 -D CONFIG_AS_AVX512=1
# -D CONFIG_AS_SHA1_NI=1 -D CONFIG_AS_SHA256_NI=1 -D CONFIG_TPAUSE=1
# -D CC_USING_FENTRY -D KBUILD_BASENAME="asm_offsets"
# -D KBUILD_MODNAME="asm_offsets"
# -isystem /usr/lib/gcc/x86_64-redhat-linux/8/include
# -include ./include/linux/kconfig.h
# -include ./include/linux/compiler_types.h
# -MD arch/x86/kernel/.asm-offsets.s.d arch/x86/kernel/asm-offsets.c
# -mno-sse -mno-mmx -mno-sse2 -mno-3dnow -mno-avx -m64 -mno-80387
# -mno-fp-ret-in-387 -mpreferred-stack-boundary=3 -mskip-rax-setup
# -mtune=generic -mno-red-zone -mcmodel=kernel
# -mindirect-branch=thunk-extern -mindirect-branch-register
# -mindirect-branch-cs-prefix -mfunction-return=thunk-extern
# -mrecord-mcount -mfentry -march=x86-64
# -auxbase-strip arch/x86/kernel/asm-offsets.s -O2 -Wall -Wundef
# -Wstrict-prototypes -Wno-trigraphs -Werror=implicit-function-declaration
# -Wno-format-security -Wno-sign-compare -Wno-frame-address
# -Wformat-truncation=0 -Wformat-overflow=0 -Wno-int-in-bool-context
# -Werror -Wframe-larger-than=2048 -Wno-unused-but-set-variable
# -Wunused-const-variable=0 -Wdeclaration-after-statement -Wno-pointer-sign
# -Wno-stringop-truncation -Wno-maybe-uninitialized -Werror=implicit-int
# -Werror=strict-prototypes -Werror=date-time
# -Werror=incompatible-pointer-types -Werror=designated-init
# -Wno-packed-not-aligned -std=gnu90 -p -fno-strict-aliasing -fno-common
# -fshort-wchar -fno-PIE -falign-jumps=1 -falign-loops=1 -funit-at-a-time
# -fno-asynchronous-unwind-tables -fno-jump-tables
# -fno-delete-null-pointer-checks -fstack-protector-strong
# -fno-inline-functions-called-once -fno-strict-overflow
# -fno-merge-all-constants -fmerge-constants -fstack-check=no
# -fconserve-stack -fmacro-prefix-map=./= -fverbose-asm
# --param allow-store-data-races=0
# options enabled:  -faggressive-loop-optimizations -falign-labels
# -fauto-inc-dec -fbranch-count-reg -fcaller-saves
# -fchkp-check-incomplete-type -fchkp-check-read -fchkp-check-write
# -fchkp-instrument-calls -fchkp-narrow-bounds -fchkp-optimize
# -fchkp-store-bounds -fchkp-use-static-bounds
# -fchkp-use-static-const-bounds -fchkp-use-wrappers -fcode-hoisting
# -fcombine-stack-adjustments -fcompare-elim -fcprop-registers
# -fcrossjumping -fcse-follow-jumps -fdefer-pop -fdevirtualize
# -fdevirtualize-speculatively -fdwarf2-cfi-asm -fearly-inlining
# -feliminate-unused-debug-types -fexpensive-optimizations
# -fforward-propagate -ffp-int-builtin-inexact -ffunction-cse -fgcse
# -fgcse-lm -fgnu-runtime -fgnu-unique -fguess-branch-probability
# -fhoist-adjacent-loads -fident -fif-conversion -fif-conversion2
# -findirect-inlining -finline -finline-atomics -finline-small-functions
# -fipa-bit-cp -fipa-cp -fipa-icf -fipa-icf-functions -fipa-icf-variables
# -fipa-profile -fipa-pure-const -fipa-reference -fipa-sra -fipa-vrp
# -fira-hoist-pressure -fira-share-save-slots -fira-share-spill-slots
# -fisolate-erroneous-paths-dereference -fivopts -fkeep-static-consts
# -fleading-underscore -flifetime-dse -flra-remat -flto-odr-type-merging
# -fmath-errno -fmerge-constants -fmerge-debug-strings
# -fmove-loop-invariants -fomit-frame-pointer -foptimize-sibling-calls
# -foptimize-strlen -fpartial-inlining -fpeephole -fpeephole2 -fplt
# -fprefetch-loop-arrays -fprofile -free -freg-struct-return
# -freorder-blocks -freorder-blocks-and-partition -freorder-functions
# -frerun-cse-after-loop -fsched-critical-path-heuristic
# -fsched-dep-count-heuristic -fsched-group-heuristic -fsched-interblock
# -fsched-last-insn-heuristic -fsched-rank-heuristic -fsched-spec
# -fsched-spec-insn-heuristic -fsched-stalled-insns-dep -fschedule-fusion
# -fschedule-insns2 -fsemantic-interposition -fshow-column -fshrink-wrap
# -fshrink-wrap-separate -fsigned-zeros -fsplit-ivs-in-unroller
# -fsplit-wide-types -fssa-backprop -fssa-phiopt -fstack-protector-strong
# -fstdarg-opt -fstore-merging -fstrict-volatile-bitfields -fsync-libcalls
# -fthread-jumps -ftoplevel-reorder -ftrapping-math -ftree-bit-ccp
# -ftree-builtin-call-dce -ftree-ccp -ftree-ch -ftree-coalesce-vars
# -ftree-copy-prop -ftree-cselim -ftree-dce -ftree-dominator-opts
# -ftree-dse -ftree-forwprop -ftree-fre -ftree-loop-if-convert
# -ftree-loop-im -ftree-loop-ivcanon -ftree-loop-optimize
# -ftree-parallelize-loops= -ftree-phiprop -ftree-pre -ftree-pta
# -ftree-reassoc -ftree-scev-cprop -ftree-sink -ftree-slsr -ftree-sra
# -ftree-switch-conversion -ftree-tail-merge -ftree-ter -ftree-vrp
# -funit-at-a-time -fverbose-asm -fwrapv -fwrapv-pointer
# -fzero-initialized-in-bss -m128bit-long-double -m64 -malign-stringops
# -mavx256-split-unaligned-load -mavx256-split-unaligned-store -mfentry
# -mfxsr -mglibc -mieee-fp -mindirect-branch-register -mlong-double-80
# -mno-fancy-math-387 -mno-red-zone -mno-sse4 -mpush-args -mrecord-mcount
# -mskip-rax-setup -mtls-direct-seg-refs -mvzeroupper

	.text
	.section	.text.startup,"ax",@progbits
	.p2align 4,,15
	.globl	main
	.type	main, @function
main:
1:	call	__fentry__
	.section __mcount_loc, "a",@progbits
	.quad 1b
	.previous
# arch/x86/kernel/asm-offsets_64.c:24: 	OFFSET(PV_CPU_usergs_sysret64, pv_cpu_ops, usergs_sysret64);
#APP
# 24 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->PV_CPU_usergs_sysret64 $232 offsetof(struct pv_cpu_ops, usergs_sysret64)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:25: 	OFFSET(PV_CPU_swapgs, pv_cpu_ops, swapgs);
# 25 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->PV_CPU_swapgs $248 offsetof(struct pv_cpu_ops, swapgs)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:29: 	BLANK();
# 29 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:33: 	OFFSET(KVM_STEAL_TIME_preempted, kvm_steal_time, preempted);
# 33 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->KVM_STEAL_TIME_preempted $16 offsetof(struct kvm_steal_time, preempted)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:34: 	BLANK();
# 34 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:38: 	ENTRY(bx);
# 38 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->pt_regs_bx $40 offsetof(struct pt_regs, bx)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:39: 	ENTRY(cx);
# 39 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->pt_regs_cx $88 offsetof(struct pt_regs, cx)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:40: 	ENTRY(dx);
# 40 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->pt_regs_dx $96 offsetof(struct pt_regs, dx)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:41: 	ENTRY(sp);
# 41 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->pt_regs_sp $152 offsetof(struct pt_regs, sp)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:42: 	ENTRY(bp);
# 42 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->pt_regs_bp $32 offsetof(struct pt_regs, bp)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:43: 	ENTRY(si);
# 43 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->pt_regs_si $104 offsetof(struct pt_regs, si)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:44: 	ENTRY(di);
# 44 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->pt_regs_di $112 offsetof(struct pt_regs, di)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:45: 	ENTRY(r8);
# 45 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->pt_regs_r8 $72 offsetof(struct pt_regs, r8)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:46: 	ENTRY(r9);
# 46 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->pt_regs_r9 $64 offsetof(struct pt_regs, r9)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:47: 	ENTRY(r10);
# 47 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->pt_regs_r10 $56 offsetof(struct pt_regs, r10)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:48: 	ENTRY(r11);
# 48 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->pt_regs_r11 $48 offsetof(struct pt_regs, r11)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:49: 	ENTRY(r12);
# 49 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->pt_regs_r12 $24 offsetof(struct pt_regs, r12)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:50: 	ENTRY(r13);
# 50 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->pt_regs_r13 $16 offsetof(struct pt_regs, r13)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:51: 	ENTRY(r14);
# 51 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->pt_regs_r14 $8 offsetof(struct pt_regs, r14)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:52: 	ENTRY(r15);
# 52 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->pt_regs_r15 $0 offsetof(struct pt_regs, r15)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:53: 	ENTRY(flags);
# 53 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->pt_regs_flags $144 offsetof(struct pt_regs, flags)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:54: 	BLANK();
# 54 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:58: 	ENTRY(cr0);
# 58 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->saved_context_cr0 $200 offsetof(struct saved_context, cr0)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:59: 	ENTRY(cr2);
# 59 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->saved_context_cr2 $208 offsetof(struct saved_context, cr2)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:60: 	ENTRY(cr3);
# 60 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->saved_context_cr3 $216 offsetof(struct saved_context, cr3)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:61: 	ENTRY(cr4);
# 61 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->saved_context_cr4 $224 offsetof(struct saved_context, cr4)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:62: 	ENTRY(cr8);
# 62 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->saved_context_cr8 $232 offsetof(struct saved_context, cr8)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:63: 	ENTRY(gdt_desc);
# 63 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->saved_context_gdt_desc $274 offsetof(struct saved_context, gdt_desc)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:64: 	BLANK();
# 64 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:67: 	BLANK();
# 67 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:70: 	DEFINE(stack_canary_offset, offsetof(struct fixed_percpu_data, stack_canary));
# 70 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->stack_canary_offset $40 offsetof(struct fixed_percpu_data, stack_canary)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:71: 	BLANK();
# 71 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:74: 	DEFINE(__NR_syscall_max, sizeof(syscalls_64) - 1);
# 74 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->__NR_syscall_max $439 sizeof(syscalls_64) - 1"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:75: 	DEFINE(NR_syscalls, sizeof(syscalls_64));
# 75 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->NR_syscalls $440 sizeof(syscalls_64)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:77: 	DEFINE(__NR_syscall_compat_max, sizeof(syscalls_ia32) - 1);
# 77 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->__NR_syscall_compat_max $439 sizeof(syscalls_ia32) - 1"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:78: 	DEFINE(IA32_NR_syscalls, sizeof(syscalls_ia32));
# 78 "arch/x86/kernel/asm-offsets_64.c" 1
	
.ascii "->IA32_NR_syscalls $440 sizeof(syscalls_ia32)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets_64.c:81: }
#NO_APP
	xorl	%eax, %eax	#
	jmp	__x86_return_thunk
	.size	main, .-main
	.text
	.p2align 4,,15
	.globl	common
	.type	common, @function
common:
1:	call	__fentry__
	.section __mcount_loc, "a",@progbits
	.quad 1b
	.previous
# arch/x86/kernel/asm-offsets.c:35: 	BLANK();
#APP
# 35 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:36: 	OFFSET(TASK_threadsp, task_struct, thread.sp);
# 36 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TASK_threadsp $5016 offsetof(struct task_struct, thread.sp)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:38: 	OFFSET(TASK_stack_canary, task_struct, stack_canary);
# 38 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TASK_stack_canary $2312 offsetof(struct task_struct, stack_canary)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:41: 	BLANK();
# 41 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:42: 	OFFSET(TASK_TI_flags, task_struct, thread_info.flags);
# 42 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TASK_TI_flags $0 offsetof(struct task_struct, thread_info.flags)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:43: 	OFFSET(TASK_addr_limit, task_struct, thread.addr_limit);
# 43 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TASK_addr_limit $5144 offsetof(struct task_struct, thread.addr_limit)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:45: 	BLANK();
# 45 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:46: 	OFFSET(crypto_tfm_ctx_offset, crypto_tfm, __crt_ctx);
# 46 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->crypto_tfm_ctx_offset $64 offsetof(struct crypto_tfm, __crt_ctx)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:48: 	BLANK();
# 48 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:49: 	OFFSET(pbe_address, pbe, address);
# 49 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->pbe_address $0 offsetof(struct pbe, address)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:50: 	OFFSET(pbe_orig_address, pbe, orig_address);
# 50 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->pbe_orig_address $8 offsetof(struct pbe, orig_address)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:51: 	OFFSET(pbe_next, pbe, next);
# 51 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->pbe_next $16 offsetof(struct pbe, next)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:54: 	BLANK();
# 54 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:55: 	OFFSET(IA32_SIGCONTEXT_ax, sigcontext_32, ax);
# 55 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->IA32_SIGCONTEXT_ax $44 offsetof(struct sigcontext_32, ax)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:56: 	OFFSET(IA32_SIGCONTEXT_bx, sigcontext_32, bx);
# 56 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->IA32_SIGCONTEXT_bx $32 offsetof(struct sigcontext_32, bx)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:57: 	OFFSET(IA32_SIGCONTEXT_cx, sigcontext_32, cx);
# 57 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->IA32_SIGCONTEXT_cx $40 offsetof(struct sigcontext_32, cx)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:58: 	OFFSET(IA32_SIGCONTEXT_dx, sigcontext_32, dx);
# 58 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->IA32_SIGCONTEXT_dx $36 offsetof(struct sigcontext_32, dx)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:59: 	OFFSET(IA32_SIGCONTEXT_si, sigcontext_32, si);
# 59 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->IA32_SIGCONTEXT_si $20 offsetof(struct sigcontext_32, si)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:60: 	OFFSET(IA32_SIGCONTEXT_di, sigcontext_32, di);
# 60 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->IA32_SIGCONTEXT_di $16 offsetof(struct sigcontext_32, di)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:61: 	OFFSET(IA32_SIGCONTEXT_bp, sigcontext_32, bp);
# 61 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->IA32_SIGCONTEXT_bp $24 offsetof(struct sigcontext_32, bp)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:62: 	OFFSET(IA32_SIGCONTEXT_sp, sigcontext_32, sp);
# 62 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->IA32_SIGCONTEXT_sp $28 offsetof(struct sigcontext_32, sp)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:63: 	OFFSET(IA32_SIGCONTEXT_ip, sigcontext_32, ip);
# 63 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->IA32_SIGCONTEXT_ip $56 offsetof(struct sigcontext_32, ip)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:65: 	BLANK();
# 65 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:66: 	OFFSET(IA32_RT_SIGFRAME_sigcontext, rt_sigframe_ia32, uc.uc_mcontext);
# 66 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->IA32_RT_SIGFRAME_sigcontext $164 offsetof(struct rt_sigframe_ia32, uc.uc_mcontext)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:70: 	BLANK();
# 70 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:71: 	OFFSET(PARAVIRT_PATCH_pv_cpu_ops, paravirt_patch_template, pv_cpu_ops);
# 71 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->PARAVIRT_PATCH_pv_cpu_ops $24 offsetof(struct paravirt_patch_template, pv_cpu_ops)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:72: 	OFFSET(PARAVIRT_PATCH_pv_irq_ops, paravirt_patch_template, pv_irq_ops);
# 72 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->PARAVIRT_PATCH_pv_irq_ops $296 offsetof(struct paravirt_patch_template, pv_irq_ops)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:73: 	OFFSET(PV_IRQ_irq_disable, pv_irq_ops, irq_disable);
# 73 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->PV_IRQ_irq_disable $16 offsetof(struct pv_irq_ops, irq_disable)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:74: 	OFFSET(PV_IRQ_irq_enable, pv_irq_ops, irq_enable);
# 74 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->PV_IRQ_irq_enable $24 offsetof(struct pv_irq_ops, irq_enable)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:75: 	OFFSET(PV_CPU_iret, pv_cpu_ops, iret);
# 75 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->PV_CPU_iret $240 offsetof(struct pv_cpu_ops, iret)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:76: 	OFFSET(PV_MMU_read_cr2, pv_mmu_ops, read_cr2);
# 76 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->PV_MMU_read_cr2 $0 offsetof(struct pv_mmu_ops, read_cr2)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:80: 	BLANK();
# 80 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:81: 	OFFSET(XEN_vcpu_info_mask, vcpu_info, evtchn_upcall_mask);
# 81 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->XEN_vcpu_info_mask $1 offsetof(struct vcpu_info, evtchn_upcall_mask)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:82: 	OFFSET(XEN_vcpu_info_pending, vcpu_info, evtchn_upcall_pending);
# 82 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->XEN_vcpu_info_pending $0 offsetof(struct vcpu_info, evtchn_upcall_pending)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:85: 	BLANK();
# 85 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:86: 	OFFSET(TDX_MODULE_rcx, tdx_module_output, rcx);
# 86 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TDX_MODULE_rcx $0 offsetof(struct tdx_module_output, rcx)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:87: 	OFFSET(TDX_MODULE_rdx, tdx_module_output, rdx);
# 87 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TDX_MODULE_rdx $8 offsetof(struct tdx_module_output, rdx)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:88: 	OFFSET(TDX_MODULE_r8,  tdx_module_output, r8);
# 88 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TDX_MODULE_r8 $16 offsetof(struct tdx_module_output, r8)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:89: 	OFFSET(TDX_MODULE_r9,  tdx_module_output, r9);
# 89 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TDX_MODULE_r9 $24 offsetof(struct tdx_module_output, r9)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:90: 	OFFSET(TDX_MODULE_r10, tdx_module_output, r10);
# 90 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TDX_MODULE_r10 $32 offsetof(struct tdx_module_output, r10)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:91: 	OFFSET(TDX_MODULE_r11, tdx_module_output, r11);
# 91 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TDX_MODULE_r11 $40 offsetof(struct tdx_module_output, r11)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:93: 	BLANK();
# 93 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:94: 	OFFSET(TDX_HYPERCALL_r10, tdx_hypercall_args, r10);
# 94 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TDX_HYPERCALL_r10 $0 offsetof(struct tdx_hypercall_args, r10)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:95: 	OFFSET(TDX_HYPERCALL_r11, tdx_hypercall_args, r11);
# 95 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TDX_HYPERCALL_r11 $8 offsetof(struct tdx_hypercall_args, r11)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:96: 	OFFSET(TDX_HYPERCALL_r12, tdx_hypercall_args, r12);
# 96 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TDX_HYPERCALL_r12 $16 offsetof(struct tdx_hypercall_args, r12)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:97: 	OFFSET(TDX_HYPERCALL_r13, tdx_hypercall_args, r13);
# 97 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TDX_HYPERCALL_r13 $24 offsetof(struct tdx_hypercall_args, r13)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:98: 	OFFSET(TDX_HYPERCALL_r14, tdx_hypercall_args, r14);
# 98 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TDX_HYPERCALL_r14 $32 offsetof(struct tdx_hypercall_args, r14)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:99: 	OFFSET(TDX_HYPERCALL_r15, tdx_hypercall_args, r15);
# 99 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TDX_HYPERCALL_r15 $40 offsetof(struct tdx_hypercall_args, r15)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:101: 	BLANK();
# 101 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:102: 	OFFSET(BP_scratch, boot_params, scratch);
# 102 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->BP_scratch $484 offsetof(struct boot_params, scratch)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:103: 	OFFSET(BP_secure_boot, boot_params, secure_boot);
# 103 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->BP_secure_boot $492 offsetof(struct boot_params, secure_boot)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:104: 	OFFSET(BP_loadflags, boot_params, hdr.loadflags);
# 104 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->BP_loadflags $529 offsetof(struct boot_params, hdr.loadflags)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:105: 	OFFSET(BP_hardware_subarch, boot_params, hdr.hardware_subarch);
# 105 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->BP_hardware_subarch $572 offsetof(struct boot_params, hdr.hardware_subarch)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:106: 	OFFSET(BP_version, boot_params, hdr.version);
# 106 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->BP_version $518 offsetof(struct boot_params, hdr.version)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:107: 	OFFSET(BP_kernel_alignment, boot_params, hdr.kernel_alignment);
# 107 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->BP_kernel_alignment $560 offsetof(struct boot_params, hdr.kernel_alignment)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:108: 	OFFSET(BP_init_size, boot_params, hdr.init_size);
# 108 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->BP_init_size $608 offsetof(struct boot_params, hdr.init_size)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:109: 	OFFSET(BP_pref_address, boot_params, hdr.pref_address);
# 109 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->BP_pref_address $600 offsetof(struct boot_params, hdr.pref_address)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:110: 	OFFSET(BP_code32_start, boot_params, hdr.code32_start);
# 110 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->BP_code32_start $532 offsetof(struct boot_params, hdr.code32_start)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:112: 	BLANK();
# 112 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:113: 	DEFINE(PTREGS_SIZE, sizeof(struct pt_regs));
# 113 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->PTREGS_SIZE $168 sizeof(struct pt_regs)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:116: 	OFFSET(TLB_STATE_user_pcid_flush_mask, tlb_state, user_pcid_flush_mask);
# 116 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TLB_STATE_user_pcid_flush_mask $22 offsetof(struct tlb_state, user_pcid_flush_mask)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:119: 	OFFSET(CPU_ENTRY_AREA_entry_stack, cpu_entry_area, entry_stack_page);
# 119 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->CPU_ENTRY_AREA_entry_stack $4096 offsetof(struct cpu_entry_area, entry_stack_page)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:120: 	DEFINE(SIZEOF_entry_stack, sizeof(struct entry_stack));
# 120 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->SIZEOF_entry_stack $512 sizeof(struct entry_stack)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:121: 	DEFINE(MASK_entry_stack, (~(sizeof(struct entry_stack) - 1)));
# 121 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->MASK_entry_stack $-512 (~(sizeof(struct entry_stack) - 1))"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:124: 	OFFSET(TSS_sp0, tss_struct, x86_tss.sp0);
# 124 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TSS_sp0 $4 offsetof(struct tss_struct, x86_tss.sp0)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:125: 	OFFSET(TSS_sp1, tss_struct, x86_tss.sp1);
# 125 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TSS_sp1 $12 offsetof(struct tss_struct, x86_tss.sp1)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:126: 	OFFSET(TSS_sp2, tss_struct, x86_tss.sp2);
# 126 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->TSS_sp2 $20 offsetof(struct tss_struct, x86_tss.sp2)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:129: 		BLANK();
# 129 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->"
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:130: 		OFFSET(VMX_spec_ctrl, vcpu_vmx, spec_ctrl);
# 130 "arch/x86/kernel/asm-offsets.c" 1
	
.ascii "->VMX_spec_ctrl $8720 offsetof(struct vcpu_vmx, spec_ctrl)"	#
# 0 "" 2
# arch/x86/kernel/asm-offsets.c:132: }
#NO_APP
	jmp	__x86_return_thunk
	.size	common, .-common
	.ident	"GCC: (GNU) 8.5.0 20210514 (Red Hat 8.5.0-28)"
	.section	.note.GNU-stack,"",@progbits

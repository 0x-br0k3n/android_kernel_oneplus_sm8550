# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the linux kernel.
#

CFLAGS_cred.o = -DANDROID_GKI_VFS_EXPORT_ONLY=VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver
obj-y     = fork.o exec_domain.o panic.o \
	    cpu.o exit.o softirq.o resource.o \
	    sysctl.o capability.o ptrace.o user.o \
	    signal.o sys.o umh.o workqueue.o pid.o task_work.o \
	    extable.o params.o \
	    kthread.o sys_ni.o nsproxy.o \
	    notifier.o ksysfs.o cred.o reboot.o \
	    async.o range.o smpboot.o ucount.o regset.o delayacct.o

obj-$(CONFIG_USERMODE_DRIVER) += usermode_driver.o
obj-$(CONFIG_MODULES) += kmod.o
obj-$(CONFIG_MULTIUSER) += groups.o

ifdef CONFIG_FUNCTION_TRACER
# Do not trace internal ftrace files
CFLAGS_REMOVE_irq_work.o = $(CC_FLAGS_FTRACE)
endif

# Prevents flicker of uninteresting __do_softirq()/__local_bh_disable_ip()
# in coverage traces.
KCOV_INSTRUMENT_softirq.o := n
# Avoid KCSAN instrumentation in softirq ("No shared variables, all the data
# are CPU local" => assume no data races), to reduce overhead in interrupts.
KCSAN_SANITIZE_softirq.o = n
# These are called from save_stack_trace() on slub debug path,
# and produce insane amounts of uninteresting coverage.
KCOV_INSTRUMENT_module.o := n
KCOV_INSTRUMENT_extable.o := n
KCOV_INSTRUMENT_stacktrace.o := n
# Don't self-instrument.
KCOV_INSTRUMENT_kcov.o := n
# If sanitizers detect any issues in kcov, it may lead to recursion
# via printk, etc.
KASAN_SANITIZE_kcov.o := n
KCSAN_SANITIZE_kcov.o := n
UBSAN_SANITIZE_kcov.o := n
CFLAGS_kcov.o := $(call cc-option, -fno-conserve-stack) -fno-stack-protector

obj-y += sched/
obj-y += locking/
obj-y += power/
obj-y += printk/
obj-y += irq/
obj-y += rcu/
obj-y += livepatch/
obj-y += dma/
obj-y += entry/

obj-$(CONFIG_KCMP) += kcmp.o
obj-$(CONFIG_FREEZER) += freezer.o
obj-$(CONFIG_PROFILING) += profile.o
obj-$(CONFIG_STACKTRACE) += stacktrace.o
obj-y += time/
obj-$(CONFIG_FUTEX) += futex/
obj-$(CONFIG_GENERIC_ISA_DMA) += dma.o
obj-$(CONFIG_SMP) += smp.o
ifneq ($(CONFIG_SMP),y)
obj-y += up.o
endif
obj-$(CONFIG_UID16) += uid16.o
obj-$(CONFIG_MODULES) += module.o
obj-$(CONFIG_MODULE_SIG) += module_signing.o
obj-$(CONFIG_MODULE_SIG_FORMAT) += module_signature.o
obj-$(CONFIG_MODULE_SIG_PROTECT) += gki_module.o
obj-$(CONFIG_KALLSYMS) += kallsyms.o
obj-$(CONFIG_BSD_PROCESS_ACCT) += acct.o
obj-$(CONFIG_CRASH_CORE) += crash_core.o
obj-$(CONFIG_KEXEC_CORE) += kexec_core.o
obj-$(CONFIG_KEXEC) += kexec.o
obj-$(CONFIG_KEXEC_FILE) += kexec_file.o
obj-$(CONFIG_KEXEC_ELF) += kexec_elf.o
obj-$(CONFIG_BACKTRACE_SELF_TEST) += backtracetest.o
obj-$(CONFIG_COMPAT) += compat.o
obj-$(CONFIG_CGROUPS) += cgroup/
obj-$(CONFIG_UTS_NS) += utsname.o
obj-$(CONFIG_USER_NS) += user_namespace.o
obj-$(CONFIG_PID_NS) += pid_namespace.o
obj-$(CONFIG_IKCONFIG) += configs.o
obj-$(CONFIG_IKHEADERS) += kheaders.o
obj-$(CONFIG_SMP) += stop_machine.o
obj-$(CONFIG_KPROBES_SANITY_TEST) += test_kprobes.o
obj-$(CONFIG_AUDIT) += audit.o auditfilter.o
obj-$(CONFIG_AUDITSYSCALL) += auditsc.o audit_watch.o audit_fsnotify.o audit_tree.o
obj-$(CONFIG_GCOV_KERNEL) += gcov/
obj-$(CONFIG_KCOV) += kcov.o
obj-$(CONFIG_KPROBES) += kprobes.o
obj-$(CONFIG_FAIL_FUNCTION) += fail_function.o
obj-$(CONFIG_KGDB) += debug/
obj-$(CONFIG_DETECT_HUNG_TASK) += hung_task.o
obj-$(CONFIG_LOCKUP_DETECTOR) += watchdog.o
obj-$(CONFIG_HARDLOCKUP_DETECTOR_PERF) += watchdog_hld.o
obj-$(CONFIG_SECCOMP) += seccomp.o
obj-$(CONFIG_RELAY) += relay.o
obj-$(CONFIG_SYSCTL) += utsname_sysctl.o
obj-$(CONFIG_TASKSTATS) += taskstats.o tsacct.o
obj-$(CONFIG_MSM_SYSSTATS) += msm_sysstats.o
obj-$(CONFIG_TRACEPOINTS) += tracepoint.o
obj-$(CONFIG_LATENCYTOP) += latencytop.o
obj-$(CONFIG_FUNCTION_TRACER) += trace/
obj-$(CONFIG_TRACING) += trace/
obj-$(CONFIG_TRACE_CLOCK) += trace/
obj-$(CONFIG_RING_BUFFER) += trace/
obj-$(CONFIG_TRACEPOINTS) += trace/
obj-$(CONFIG_IRQ_WORK) += irq_work.o
obj-$(CONFIG_CPU_PM) += cpu_pm.o
obj-$(CONFIG_BPF) += bpf/
obj-$(CONFIG_KCSAN) += kcsan/
obj-$(CONFIG_SHADOW_CALL_STACK) += scs.o
obj-$(CONFIG_HAVE_STATIC_CALL) += static_call.o
obj-$(CONFIG_HAVE_STATIC_CALL_INLINE) += static_call_inline.o
obj-$(CONFIG_CFI_CLANG) += cfi.o

obj-$(CONFIG_PERF_EVENTS) += events/

obj-$(CONFIG_USER_RETURN_NOTIFIER) += user-return-notifier.o
obj-$(CONFIG_PADATA) += padata.o
obj-$(CONFIG_CRASH_DUMP) += crash_dump.o
obj-$(CONFIG_JUMP_LABEL) += jump_label.o
obj-$(CONFIG_CONTEXT_TRACKING) += context_tracking.o
obj-$(CONFIG_TORTURE_TEST) += torture.o

obj-$(CONFIG_HAS_IOMEM) += iomem.o
obj-$(CONFIG_RSEQ) += rseq.o
obj-$(CONFIG_WATCH_QUEUE) += watch_queue.o

obj-$(CONFIG_RESOURCE_KUNIT_TEST) += resource_kunit.o
obj-$(CONFIG_SYSCTL_KUNIT_TEST) += sysctl-test.o

CFLAGS_stackleak.o += $(DISABLE_STACKLEAK_PLUGIN)
obj-$(CONFIG_GCC_PLUGIN_STACKLEAK) += stackleak.o
KASAN_SANITIZE_stackleak.o := n
KCSAN_SANITIZE_stackleak.o := n
KCOV_INSTRUMENT_stackleak.o := n

obj-$(CONFIG_SCF_TORTURE_TEST) += scftorture.o

$(obj)/configs.o: $(obj)/config_data.gz

targets += config_data config_data.gz
$(obj)/config_data.gz: $(obj)/config_data FORCE
	$(call if_changed,gzip)

filechk_cat = cat $<

$(obj)/config_data: $(KCONFIG_CONFIG) FORCE
	$(call filechk,cat)

$(obj)/kheaders.o: $(obj)/kheaders_data.tar.xz

quiet_cmd_genikh = CHK     $(obj)/kheaders_data.tar.xz
      cmd_genikh = $(CONFIG_SHELL) $(srctree)/kernel/gen_kheaders.sh $@
$(obj)/kheaders_data.tar.xz: FORCE
	$(call cmd,genikh)

clean-files := kheaders_data.tar.xz kheaders.md5

#ifdef OPLUS_FEATURE_PERFORMANCE_SCHED
obj-$(CONFIG_OPLUS_FEATURE_PERFORMANCE_SCHED) += oplus_cpu/sched/
#endif /* OPLUS_FEATURE_PERFORMANCE_SCHED */

#ifdef OPLUS_FEATURE_OMRG
obj-$(CONFIG_OPLUS_OMRG) += oplus_cpu/oplus_omrg/
#endif /* OPLUS_FEATURE_OMRG */

#ifdef OPLUS_FEATURE_SUGOV_POWER_EFFIENCY
obj-$(CONFIG_OPLUS_FEATURE_SUGOV_POWER_EFFIENCY) += oplus_cpu/cpufreq_effiency/
#endif /* OPLUS_FEATURE_SUGOV_POWER_EFFIENCY */

#ifdef OPLUS_FEATURE_GKI_CPUFREQ_BOUNCING
obj-$(CONFIG_OPLUS_FEATURE_GKI_CPUFREQ_BOUNCING) += oplus_cpu/cpufreq_bouncing/
#endif /* OPLUS_FEATURE_GKI_CPUFREQ_BOUNCING */

#ifdef OPLUS_FEATURE_FREQQOS_MONITOR
obj-$(CONFIG_OPLUS_FEATURE_FREQQOS_MONITOR) += oplus_cpu/freqqos_monitor/
#endif /* OPLUS_FEATURE_FREQQOS_MONITOR */

#ifdef OPLUS_FEATURE_OCH
obj-$(CONFIG_OPLUS_FEATURE_OCH) += oplus_cpu/cpufreq_health/
#endif /* OPLUS_FEATURE_OCH */

#ifdef OPLUS_FEATURE_THERMAL_HORAE
obj-$(CONFIG_HORAE_THERMAL_SHELL) += oplus_cpu/thermal/
#endif /* OPLUS_FEATURE_THERMAL_HORAE */
#ifdef OPLUS_FEATURE_ATD
obj-$(CONFIG_OPLUS_FEATURE_ATD) += oplus_cpu/detection/
#endif

obj-$(CONFIG_OPLUS_CPU_FREQ_GOV_UAG) += oplus_cpu/uad/

#ifdef OPLUS_FEATURE_ABNORMAL_FLAG
obj-$(CONFIG_OPLUS_FEATURE_ABNORMAL_FLAG) += oplus_cpu/oplus_overload/
#endif

#ifdef OPLUS_FEATURE_WAKER_IDENTIFY
obj-$(CONFIG_OPLUS_FEATURE_WAKER_IDENTIFY) += oplus_cpu/waker_identify/
#endif OPLUS_FEATURE_WAKER_IDENTIFY

#
# ANDROID: GKI: Generate headerfiles required for gki_module.o
#
# Dependencies on generated files need to be listed explicitly
$(obj)/gki_module.o: $(obj)/gki_module_protected.h $(obj)/gki_module_exported.h

$(obj)/gki_module_protected.h: $(srctree)/android/abi_gki_modules_protected \
				$(srctree)/scripts/gen_gki_modules_headers.sh
	$(Q)$(CONFIG_SHELL) $(srctree)/scripts/gen_gki_modules_headers.sh $@ \
	"$(srctree)"

$(obj)/gki_module_exported.h: $(srctree)/android/abi_gki_modules_exports \
				$(srctree)/scripts/gen_gki_modules_headers.sh
	$(Q)$(CONFIG_SHELL) $(srctree)/scripts/gen_gki_modules_headers.sh $@ \
	"$(srctree)"

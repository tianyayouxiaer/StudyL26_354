/*
 *  Copyright (c) 1991,1992,1995  Linus Torvalds
 *  Copyright (c) 1994  Alan Modra
 *  Copyright (c) 1995  Markus Kuhn
 *  Copyright (c) 1996  Ingo Molnar
 *  Copyright (c) 1998  Andrea Arcangeli
 *  Copyright (c) 2002,2006  Vojtech Pavlik
 *  Copyright (c) 2003  Andi Kleen
 *
 */

#include <linux/clockchips.h>
#include <linux/interrupt.h>
#include <linux/time.h>
#include <linux/mca.h>

#include <asm/vsyscall.h>
#include <asm/x86_init.h>
#include <asm/i8259.h>
#include <asm/i8253.h>
#include <asm/timer.h>
#include <asm/hpet.h>
#include <asm/time.h>

#if defined(CONFIG_X86_32) && defined(CONFIG_X86_IO_APIC)
int timer_ack;
#endif

#ifdef CONFIG_X86_64
volatile unsigned long __jiffies __section_jiffies = INITIAL_JIFFIES;
#endif

unsigned long profile_pc(struct pt_regs *regs)
{
	unsigned long pc = instruction_pointer(regs);

	if (!user_mode_vm(regs) && in_lock_functions(pc)) {
#ifdef CONFIG_FRAME_POINTER
		return *(unsigned long *)(regs->bp + sizeof(long));
#else
		unsigned long *sp =
			(unsigned long *)kernel_stack_pointer(regs);
		/*
		 * Return address is either directly at stack pointer
		 * or above a saved flags. Eflags has bits 22-31 zero,
		 * kernel addresses don't.
		 */
		if (sp[0] >> 22)
			return sp[0];
		if (sp[1] >> 22)
			return sp[1];
#endif
	}
	return pc;
}
EXPORT_SYMBOL(profile_pc);

/*
 * Default timer interrupt handler for PIT/HPET
 */

 // PIT 或 HPET的中断函数，
 //  时钟中断处理程序  
//  函数任务：  
//      1.向中断控制器应答  
//      2.通知全局时钟事件设备处理时间到期 
static irqreturn_t timer_interrupt(int irq, void *dev_id)
{
	/* Keep nmi watchdog up to date */
	inc_irq_stat(irq0_irqs);

	/* Optimized out for !IO_APIC and x86_64 */
	 //向中断控制器应答时钟中断
	if (timer_ack) {
		/*
		 * Subtle, when I/O APICs are used we have to ack timer IRQ
		 * manually to deassert NMI lines for the watchdog if run
		 * on an 82489DX-based system.
		 */
		raw_spin_lock(&i8259A_lock);
		outb(0x0c, PIC_MASTER_OCW3);
		/* Ack the IRQ; AEOI will end it automatically. */
		inb(PIC_MASTER_POLL);
		raw_spin_unlock(&i8259A_lock);
	}
	
	//全局时钟源设备的时间处理函数
	global_clock_event->event_handler(global_clock_event);

	/* MCA bus quirk: Acknowledge irq0 by setting bit 7 in port 0x61 */
	if (MCA_bus)
		outb_p(inb_p(0x61)| 0x80, 0x61);

	return IRQ_HANDLED;
}

//  时钟中断控制块
static struct irqaction irq0  = {
	.handler = timer_interrupt,
	.flags = IRQF_DISABLED | IRQF_NOBALANCING | IRQF_IRQPOLL | IRQF_TIMER,
	.name = "timer"
};

//启动定时器
//  设置时钟中断处理程序  
//  调用路径：hpet_time_init->setup_default_timer_irq  
void __init setup_default_timer_irq(void)
{
	setup_irq(0, &irq0);
}

//  初始化hpet  
//  函数任务：  
//      1.注册hpet clocksource，hpet clockevent  
//      2.注册时钟中断处理程序  
//  调用路径：time_init->hpet_time_init 

/* Default timer init function */
void __init hpet_time_init(void)
{
	//注册hpet clocksource，hpet clockevent 

	if (!hpet_enable())
		setup_pit_timer();
	//分配时钟中断
	setup_default_timer_irq();
}

static __init void x86_late_time_init(void)
{
	x86_init.timers.timer_init();
	tsc_init();
}

/*
 * Initialize TSC and delay the periodic timer init to
 * late x86_late_time_init() so ioremap works.
 */
 /*
 函数x86_late_time_init实际上是初始化tsc时钟源。
 在time_init中只是把该函数的地址赋给全局变量late_time_init，
 以后某个时刻肯定会调用它的。
 */

 /*
 该函数通常定义在体系相关的代码中，它主要完成 machine级别对时钟系统的初始化工作，
 最终通过clockevents_register_device 注册系统中的时钟事件设备，把每个时钟时间设
 备挂在clockevent_device全局链表上， 最后通过clockevent_do_notify触发框架层事先
 注册好的通知链，其实就是调用了 tick_notify函数，
 */

//  时钟初始化  
//  函数任务：  
//      1.初始化hpet时钟源，时钟事件设备  
//      2.初始化tsc时钟源  
//  调用路径start_kernel->time_init  
void __init time_init(void)
{
	//初始化hpet时钟源

	late_time_init = x86_late_time_init;
}

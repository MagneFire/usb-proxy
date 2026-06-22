// SPDX-License-Identifier: GPL-2.0
/*
 * musbfix - runtime fix for sunxi musb gadget bulk-OUT packet loss.
 *
 * Problem
 * -------
 * On boards using the Allwinner sunxi musb controller in peripheral mode
 * (e.g. Orange Pi Zero, H2+/sun8i-h3) driving a USB proxy via raw-gadget, a
 * bulk OUT endpoint accepts the first transfer and then silently drops the
 * next one. With an Android device this means the host's ADB CNXN header is
 * received and forwarded but its follow-up payload never arrives, so the
 * connect handshake never completes and the host reports the device
 * "offline". dwc2/dwc3 based boards (e.g. Raspberry Pi) are unaffected.
 *
 * Root cause
 * ----------
 * musb_ep_restart() (drivers/usb/musb/musb_gadget.c), invoked from
 * musb_gadget_queue() when a request becomes the head of the queue, FLUSHES
 * the RX FIFO for OUT endpoints:
 *
 *	csr |= MUSB_RXCSR_FLUSHFIFO | MUSB_RXCSR_P_WZC_BITS;
 *	musb_writew(epio, MUSB_RXCSR, csr);
 *
 * It never services a packet that is already pending in the FIFO. The sunxi
 * musb is PIO-only and single-buffered (MUSB_EP_FIFO_SINGLE), so when a packet
 * arrives during the brief window where no request is queued -- which happens
 * on every transfer with raw-gadget, since it re-queues OUT reads from user
 * space -- the controller ACKs it into the single FIFO, and the next
 * usb_ep_queue() then flushes it away. The host already considers the packet
 * delivered, so it never resends: deadlock.
 *
 * In-kernel gadgets (mass-storage, ACM, ...) re-queue OUT requests from within
 * the completion callback (IRQ context, no user-space round-trip), so the
 * window is effectively zero and they don't hit this. raw-gadget does, which
 * is why usb-proxy exposes it.
 *
 * Fix
 * ---
 * arm32 has no ftrace-based instruction-pointer modification (live patching is
 * unsupported), so we use a kprobe at musb_ep_restart's entry and ALWAYS
 * redirect execution (pre-handler returns 1). We never let the kprobe
 * single-step the Thumb-2 entry instruction -- on this arm32 Thumb-2 build the
 * single-step emulation (t16_emulate_push) corrupts the kernel.
 *
 *	- OUT (rx): run musb_g_rx(), which services any already-pending FIFO
 *	  packet (or leaves the request armed if none) instead of flushing it.
 *	- IN  (tx): replicate the original musb_ep_restart TX path exactly
 *	  (musb_ep_select + txstate), so IN transfers are unaffected.
 *
 * This mirrors the proper kernel-source fix in musb_gadget-rx-requeue.patch
 * (service RXPKTRDY instead of flushing), but as a loadable, reversible module
 * so the on-disk kernel is never modified.
 *
 * Scope / caveats
 * ---------------
 *   - arm32 Thumb-2 specific (handles the PC Thumb bit explicitly).
 *   - Depends on internal struct layout of the *running* kernel: it includes
 *     the matching drivers/usb/musb headers (fetched by the Makefile) and is
 *     compiled against the kernel's own .config, so offsets match.
 *   - Tied to the exact kernel build (vermagic). A kernel update requires a
 *     rebuild + retest; until then it simply won't load (and the bug returns).
 *   - Worst case on a mismatch is an oops that kills usb-proxy; the on-disk
 *     kernel is untouched, so a reboot fully recovers.
 *
 * Tested on: Orange Pi Zero (H2+/sun8i-h3), Armbian, 6.18.35-current-sunxi.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <asm/ptrace.h>

#include "musb_core.h"		/* struct musb, struct musb_request,
				 * musb_ep_select(), MUSB_INDEX */

/* musb_g_rx() and txstate() are not exported; resolve them by name and tag the
 * pointers as Thumb so calls stay in Thumb state. */
static void (*p_musb_g_rx)(struct musb *musb, u8 epnum);
static void (*p_txstate)(struct musb *musb, struct musb_request *req);

static unsigned long lookup_sym(const char *name)
{
	struct kprobe kp = { .symbol_name = name };
	unsigned long addr;

	if (register_kprobe(&kp) < 0)
		return 0;
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
#ifdef CONFIG_THUMB2_KERNEL
	addr |= 1;	/* kallsyms gives the even address; a Thumb-2 function
			 * pointer must carry bit0 so a call stays in Thumb. */
#endif
	return addr;
}

/* OUT endpoints: musb_g_rx() selects the ep itself and services RXPKTRDY. */
static void notrace repl_rx(struct musb *musb, struct musb_request *req)
{
	p_musb_g_rx(musb, req->epnum);
}

/* IN endpoints: same as the original musb_ep_restart TX path. */
static void notrace repl_tx(struct musb *musb, struct musb_request *req)
{
	musb_ep_select(musb->mregs, req->epnum);
	p_txstate(musb, req);
}

static int notrace musb_ep_restart_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct musb_request *req = (struct musb_request *)regs->ARM_r1;
	unsigned long target = req->tx ? (unsigned long)repl_tx
				       : (unsigned long)repl_rx;

	/*
	 * Always redirect; r0=musb and r1=req are still live and lr still
	 * points at musb_gadget_queue(), so the replacement returns there.
	 * Returning 1 makes the kprobe skip single-stepping the original.
	 */
#ifdef CONFIG_THUMB2_KERNEL
	regs->ARM_cpsr |= PSR_T_BIT;
#endif
	regs->ARM_pc = target & ~1UL;
	return 1;
}

static struct kprobe musb_kp = {
	.symbol_name = "musb_ep_restart",
	.pre_handler = musb_ep_restart_pre,
};

static int __init musbfix_init(void)
{
	int ret;

	p_musb_g_rx = (void *)lookup_sym("musb_g_rx");
	p_txstate   = (void *)lookup_sym("txstate");
	if (!p_musb_g_rx || !p_txstate) {
		pr_err("musbfix: symbol lookup failed (musb_g_rx=%px txstate=%px)\n",
		       p_musb_g_rx, p_txstate);
		return -ENOENT;
	}

	ret = register_kprobe(&musb_kp);
	if (ret) {
		pr_err("musbfix: register_kprobe(musb_ep_restart) failed: %d\n",
		       ret);
		return ret;
	}

	pr_info("musbfix: kprobe musb_ep_restart@%px (rx->musb_g_rx@%px tx->txstate@%px)\n",
		musb_kp.addr, p_musb_g_rx, p_txstate);
	return 0;
}

static void __exit musbfix_exit(void)
{
	unregister_kprobe(&musb_kp);
	pr_info("musbfix: unhooked\n");
}

module_init(musbfix_init);
module_exit(musbfix_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("musbfix");
MODULE_DESCRIPTION("Fix sunxi musb bulk-OUT FIFO flush-on-requeue packet loss");

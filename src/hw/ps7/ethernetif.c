/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *
 */

/*
 * Copyright (c) 2010-2013 Xilinx, Inc.  All rights reserved.
 *
 * Xilinx, Inc.
 * XILINX IS PROVIDING THIS DESIGN, CODE, OR INFORMATION "AS IS" AS A
 * COURTESY TO YOU.  BY PROVIDING THIS DESIGN, CODE, OR INFORMATION AS
 * ONE POSSIBLE   IMPLEMENTATION OF THIS FEATURE, APPLICATION OR
 * STANDARD, XILINX IS MAKING NO REPRESENTATION THAT THIS IMPLEMENTATION
 * IS FREE FROM ANY CLAIMS OF INFRINGEMENT, AND YOU ARE RESPONSIBLE
 * FOR OBTAINING ANY RIGHTS YOU MAY REQUIRE FOR YOUR IMPLEMENTATION.
 * XILINX EXPRESSLY DISCLAIMS ANY WARRANTY WHATSOEVER WITH RESPECT TO
 * THE ADEQUACY OF THE IMPLEMENTATION, INCLUDING BUT NOT LIMITED TO
 * ANY WARRANTIES OR REPRESENTATIONS THAT THIS IMPLEMENTATION IS FREE
 * FROM CLAIMS OF INFRINGEMENT, IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 */

#include <stdio.h>
#include <string.h>

#include <xparameters.h>
#include "lwipopts.h"
//#include "xlwipconfig.h"
#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/stats.h"
#include "lwip/igmp.h"

#include "netif/etharp.h"
#include "xemacpsif.h"
#include "xadapter.h"
//#include "xpqueue.h"
#include "xparameters.h"
//#include "xuartps.h"
//#include "xscugic.h"
#include "xemacps.h"
#include "xadapter.h"


/* Define those to better describe your network interface. */
#define IFNAME0 't'
#define IFNAME1 'e'

#if LWIP_IGMP
static err_t xemacpsif_mac_filter_update (struct netif *netif,
							struct ip_addr *group, u8_t action);

static u8_t xemacps_mcast_entry_mask = 0;
#endif

XEmacPs_Config *mac_config;
struct netif *NetIf;

struct xtopology_t xtopology[2];

struct xemac_s XEMAC;
xemacpsif_s XEMACPSIF;

#define ETH_INPUT_TASK_STACK_SIZE				512
#define ETH_INPUT_TASK_PRIO						20
#define ETH_INPUT_TASK_NAME						"Eth Input Task"

/*
 * this function is always called with interrupts off
 * this function also assumes that there are available BD's
 */
static err_t _unbuffered_low_level_output(xemacpsif_s *xemacpsif,
													struct pbuf *p)
{
	XStatus status = 0;

#if ETH_PAD_SIZE
	pbuf_header(p, -ETH_PAD_SIZE);	/* drop the padding word */
#endif
	status = emacps_sgsend(xemacpsif, p);
	if (status != XST_SUCCESS) {
#if LINK_STATS
	lwip_stats.link.drop++;
#endif
	}

#if ETH_PAD_SIZE
	pbuf_header(p, ETH_PAD_SIZE);	/* reclaim the padding word */
#endif

#if LINK_STATS
	lwip_stats.link.xmit++;
#endif /* LINK_STATS */

	return ERR_OK;

}

/*
 * low_level_output():
 *
 * Should do the actual transmission of the packet. The packet is
 * contained in the pbuf that is passed to the function. This pbuf
 * might be chained.
 *
 */

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
	SYS_ARCH_DECL_PROTECT(lev);
    err_t err;
    s32_t freecnt;
    XEmacPs_BdRing *txring;

	xemacpsif_s *xemacpsif = &XEMACPSIF;

	SYS_ARCH_PROTECT(lev);

	/* check if space is available to send */
    freecnt = is_tx_space_available(xemacpsif);
    if (freecnt <= 5) {
	txring = &(XEmacPs_GetTxRing(&xemacpsif->emacps));
		process_sent_bds(xemacpsif, txring);
	}

    if (is_tx_space_available(xemacpsif)) {
		_unbuffered_low_level_output(xemacpsif, p);
		err = ERR_OK;
	} else {
#if LINK_STATS
		lwip_stats.link.drop++;
#endif
		err = ERR_MEM;
	}

	SYS_ARCH_UNPROTECT(lev);
	return err;
}

/*
 * xemacpsif_output():
 *
 * This function is called by the TCP/IP stack when an IP packet
 * should be sent. It calls the function called low_level_output() to
 * do the actual transmission of the packet.
 *
 */

static err_t xemacpsif_output(struct netif *netif, struct pbuf *p,
		const ip4_addr_t *ipaddr)
{
	/* resolve hardware address, then send (or queue) packet */
	return etharp_output(netif, p, ipaddr);
}


static err_t low_level_init(struct netif *netif)
{
	xemacpsif_s *xemacpsif;
	XEmacPs *xemac;
	u32 dmacrreg;

	NetIf = netif;

	xemacpsif = &XEMACPSIF;
	xemac = &XEMACPSIF.emacps;

	XEMAC.state = (void *)xemacpsif;
	XEMAC.topology_index = 0;
	XEMAC.type = xemac_type_emacps;

	xemacpsif->send_q = NULL;
	sys_mbox_new((sys_mbox_t*)&xemacpsif->recv_q, 64);
	if (!xemacpsif->recv_q)
		return ERR_MEM;

	/* maximum transfer unit */
	netif->mtu = XEMACPS_MTU - XEMACPS_HDR_SIZE;

#if LWIP_IGMP
	netif->igmp_mac_filter = xemacpsif_mac_filter_update;
#endif

	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

#if LWIP_IGMP
	netif->flags |= NETIF_FLAG_IGMP;
#endif

	sys_thread_new(ETH_INPUT_TASK_NAME, xemacif_input_thread, (void*)netif, ETH_INPUT_TASK_STACK_SIZE, ETH_INPUT_TASK_PRIO);

	XEmacPs_CfgInitialize(&xemacpsif->emacps, XPAR_XEMACPS_0_BASEADDR);

	/* initialize the mac */
	init_emacps(xemacpsif, netif);

	dmacrreg = XEmacPs_ReadReg(xemacpsif->emacps.Config.BaseAddress, XEMACPS_DMACR_OFFSET);
	dmacrreg = dmacrreg | (0x00000010);
	XEmacPs_WriteReg(xemacpsif->emacps.Config.BaseAddress, XEMACPS_DMACR_OFFSET, dmacrreg);

	setup_isr(&XEMAC);
	init_dma(&XEMAC);
	start_emacps(xemacpsif);

	/* replace the state in netif (currently the emac baseaddress)
	 * with the mac instance pointer.
	 */
	netif->state = (void *)xemac;

	return ERR_OK;
}

void HandleEmacPsError(struct xemac_s *xemac)
{
	xemacpsif_s   *xemacpsif;
	s32_t status = XST_SUCCESS;
	u32 dmacrreg;

	SYS_ARCH_DECL_PROTECT(lev);
	SYS_ARCH_PROTECT(lev);

	free_txrx_pbufs(xemacpsif);
	xemacpsif = (xemacpsif_s *)(xemac->state);
	status = XEmacPs_CfgInitialize(&xemacpsif->emacps,
						mac_config->BaseAddress);
	if (status != XST_SUCCESS) {
		xil_printf("In %s:EmacPs Configuration Failed....\r\n", __func__);
	}
	/* initialize the mac */
	init_emacps_on_error(xemacpsif, NetIf);
	dmacrreg = XEmacPs_ReadReg(xemacpsif->emacps.Config.BaseAddress,
														XEMACPS_DMACR_OFFSET);
	dmacrreg = dmacrreg | (0x01000000);
	XEmacPs_WriteReg(xemacpsif->emacps.Config.BaseAddress,
											XEMACPS_DMACR_OFFSET, dmacrreg);
	setup_isr(xemac);
	init_dma(xemac);
	start_emacps(xemacpsif);

	SYS_ARCH_UNPROTECT(lev);
}

void HandleTxErrors(struct xemac_s *xemac)
{
	xemacpsif_s   *xemacpsif;
	u32 netctrlreg;

	SYS_ARCH_DECL_PROTECT(lev);
	SYS_ARCH_PROTECT(lev);
	xemacpsif = (xemacpsif_s *)(xemac->state);
	netctrlreg = XEmacPs_ReadReg(xemacpsif->emacps.Config.BaseAddress,
												XEMACPS_NWCTRL_OFFSET);
    netctrlreg = netctrlreg & (~XEMACPS_NWCTRL_TXEN_MASK);
	XEmacPs_WriteReg(xemacpsif->emacps.Config.BaseAddress,
									XEMACPS_NWCTRL_OFFSET, netctrlreg);
	free_onlytx_pbufs(xemacpsif);

	clean_dma_txdescs(xemac);
	netctrlreg = XEmacPs_ReadReg(xemacpsif->emacps.Config.BaseAddress,
													XEMACPS_NWCTRL_OFFSET);
	netctrlreg = netctrlreg | (XEMACPS_NWCTRL_TXEN_MASK);
	XEmacPs_WriteReg(xemacpsif->emacps.Config.BaseAddress,
										XEMACPS_NWCTRL_OFFSET, netctrlreg);
	SYS_ARCH_UNPROTECT(lev);
}


#if LWIP_IGMP
static err_t xemacpsif_mac_filter_update (struct netif *netif, struct ip_addr *group,
								u8_t action)
{
	return 0;
}
#endif

/*
 * xemacpsif_init():
 *
 * Should be called at the beginning of the program to set up the
 * network interface. It calls the function low_level_init() to do the
 * actual setup of the hardware.
 *
 */

err_t xemacpsif_init(struct netif *netif)
{
#if LWIP_SNMP
	/* ifType ethernetCsmacd(6) @see RFC1213 */
	netif->link_type = 6;
	/* your link speed here */
	netif->link_speed = ;
	netif->ts = 0;
	netif->ifinoctets = 0;
	netif->ifinucastpkts = 0;
	netif->ifinnucastpkts = 0;
	netif->ifindiscards = 0;
	netif->ifoutoctets = 0;
	netif->ifoutucastpkts = 0;
	netif->ifoutnucastpkts = 0;
	netif->ifoutdiscards = 0;
#endif

	netif->name[0] = IFNAME0;
	netif->name[1] = IFNAME1;
	netif->output = xemacpsif_output;
	netif->linkoutput = low_level_output;
	netif->hwaddr[0] = 0x50;
	netif->hwaddr[1] = 0xE5;
	netif->hwaddr[2] = 0x49;
	netif->hwaddr[3] = 0xE6;
	netif->hwaddr[4] = 0x8D;
	netif->hwaddr[5] = 0x28;
	netif->hwaddr_len = ETHARP_HWADDR_LEN;
	/* Accept broadcast address and ARP traffic */
	netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;

	low_level_init(netif);
	return ERR_OK;
}

///*
// * xemacpsif_resetrx_on_no_rxdata():
// *
// * Should be called by the user at regular intervals, typically
// * from a timer (100 msecond). This is to provide a SW workaround
// * for the HW bug (SI #692601). Please refer to the function header
// * for the function resetrx_on_no_rxdata in xemacpsif_dma.c to
// * know more about the SI.
// *
// */
//
//void xemacpsif_resetrx_on_no_rxdata(struct netif *netif)
//{
//	struct xemac_s *xemac = (struct xemac_s *)(netif->state);
//	xemacpsif_s *xemacpsif = (xemacpsif_s *)(xemac->state);
//
//	resetrx_on_no_rxdata(xemacpsif);
//}

/*
 * The input thread calls lwIP to process any received packets.
 * This thread waits until a packet is received (sem_rx_data_available),
 * and then calls xemacif_input which processes 1 packet at a time.
 */
void
xemacif_input_thread(struct netif *netif)
{
	struct eth_hdr *ethhdr;
	struct pbuf *p;
	SYS_ARCH_DECL_PROTECT(lev);
	xemacpsif_s *xemacpsif = &XEMACPSIF;

	while (1) {
		/* move received packet into a new pbuf */
		SYS_ARCH_PROTECT(lev);
		sys_mbox_fetch((sys_mbox_t*)&xemacpsif->recv_q,(void*)&p);
		SYS_ARCH_UNPROTECT(lev);

		/* no packet could be read, silently ignore this */
		if (p == NULL) {
			continue;
		}

		/* points to packet payload, which starts with an Ethernet header */
		ethhdr = p->payload;

	#if LINK_STATS
		lwip_stats.link.recv++;
	#endif /* LINK_STATS */

		switch (htons(ethhdr->type)) {
			/* IP or ARP packet? */
			case ETHTYPE_IP:
			case ETHTYPE_ARP:
	#if PPPOE_SUPPORT
				/* PPPoE packet? */
			case ETHTYPE_PPPOEDISC:
			case ETHTYPE_PPPOE:
	#endif /* PPPOE_SUPPORT */
				/* full packet send to tcpip_thread to process */
				if (netif->input(p, netif) != ERR_OK) {
					LWIP_DEBUGF(NETIF_DEBUG, ("xemacpsif_input: IP input error\r\n"));
					pbuf_free(p);
					p = NULL;
				}
				break;

			default:
				pbuf_free(p);
				p = NULL;
				break;
		}
	}
}

void xemacif_isr_wrapper(void* data, uint32_t x){
	XEmacPs_IntrHandler(&XEMACPSIF.emacps);
}

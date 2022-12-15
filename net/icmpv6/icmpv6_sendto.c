/****************************************************************************
 * net/icmpv6/icmpv6_sendto.c
 *
 *   Copyright (C) 2017, 2019 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <semaphore.h>
#include <assert.h>
#include <debug.h>

#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>

#include <nuttx/semaphore.h>
#include <nuttx/mm/iob.h>
#include <nuttx/net/netconfig.h>
#include <nuttx/net/net.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/netstats.h>
#include <nuttx/net/ip.h>
#include <nuttx/net/icmpv6.h>

#include "utils/utils.h"
#include "socket/socket.h"
#include "netdev/netdev.h"
#include "devif/devif.h"
#include "inet/inet.h"
#include "icmpv6/icmpv6.h"

#ifdef CONFIG_NET_ICMPv6_SOCKET

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define IPv6BUF \
  ((struct ipv6_hdr_s *)&dev->d_buf[NET_LL_HDRLEN(dev)])
#define ICMPv6BUF \
  ((struct icmpv6_echo_request_s *)&dev->d_buf[NET_LL_HDRLEN(dev) + IPv6_HDRLEN])

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct icmpv6_sendto_s
{
  FAR struct devif_callback_s *snd_cb; /* Reference to callback instance */
  FAR struct socket *snd_sock; /* IPPROTO_ICMP6 socket structure */
  sem_t snd_sem;               /* Use to manage the wait for send complete */
  clock_t snd_time;            /* Start time for determining timeouts */
  struct in6_addr snd_toaddr;  /* The peer to send the request to */
  FAR const uint8_t *snd_buf;  /* ICMPv6 header + data payload */
  uint16_t snd_buflen;         /* Size of the ICMPv6 header + data payload */
  int16_t snd_result;          /* 0: success; <0:negated errno on fail */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sendto_timeout
 *
 * Description:
 *   Check for send timeout.
 *
 * Input Parameters:
 *   pstate - Reference to instance ot sendto state structure
 *
 * Returned Value:
 *   true: timeout false: no timeout
 *
 * Assumptions:
 *   The network is locked
 *
 ****************************************************************************/

#ifdef CONFIG_NET_SOCKOPTS
static inline int sendto_timeout(FAR struct icmpv6_sendto_s *pstate)
{
  FAR struct socket *psock;

  /* Check for a timeout configured via setsockopts(SO_SNDTIMEO).
   * If none... we will let the send wait forever.
   */

  psock = pstate->snd_sock;
  if (psock != NULL && psock->s_sndtimeo != 0)
    {
      /* Check if the configured timeout has elapsed */

      return net_timeo(pstate->snd_time, psock->s_sndtimeo);
    }

  /* No timeout */

  return false;
}
#endif /* CONFIG_NET_SOCKOPTS */

/****************************************************************************
 * Name: sendto_request
 *
 * Description:
 *   Setup to send an ICMPv6 request packet
 *
 * Input Parameters:
 *   dev    - The device driver structure to use in the send operation
 *   pstate - Reference to an instance of the ICMPv6 sendto state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static void sendto_request(FAR struct net_driver_s *dev,
                           FAR struct icmpv6_sendto_s *pstate)
{
  FAR struct ipv6_hdr_s *ipv6;
  FAR struct icmpv6_echo_request_s *icmpv6;

  IFF_SET_IPv6(dev->d_flags);

  /* The total length to send is the size of the application data plus the
   * IP and ICMPv6 headers (and, eventually, the Ethernet header)
   */

  dev->d_len = IPv6_HDRLEN + pstate->snd_buflen;

  /* The total size of the data (including the size of the ICMPv6 header) */

  dev->d_sndlen += pstate->snd_buflen;

  /* Set up the IPv6 header (most is probably already in place) */

  ipv6           = IPv6BUF;
  ipv6->vtc      = 0x60;                       /* Version/traffic class (MS) */
  ipv6->tcf      = 0;                          /* Traffic class(LS)/Flow label(MS) */
  ipv6->flow     = 0;                          /* Flow label (LS) */
  ipv6->len[0]   = (pstate->snd_buflen >> 8);  /* Length excludes the IPv6 header */
  ipv6->len[1]   = (pstate->snd_buflen & 0xff);
  ipv6->proto    = IP_PROTO_ICMP6;             /* Next header */
  ipv6->ttl      = 255;                        /* Hop limit */

  net_ipv6addr_hdrcopy(ipv6->srcipaddr, dev->d_ipv6addr);
  net_ipv6addr_hdrcopy(ipv6->destipaddr, pstate->snd_toaddr.s6_addr16);

  /* Copy the ICMPv6 request and payload into place after the IPv6 header */

  icmpv6         = ICMPv6BUF;
  memcpy(icmpv6, pstate->snd_buf, pstate->snd_buflen);

  /* Calculate the ICMPv6 checksum over the ICMPv6 header and payload. */

  icmpv6->chksum = 0;
  icmpv6->chksum = ~icmpv6_chksum(dev, IPv6_HDRLEN);
  if (icmpv6->chksum == 0)
    {
      icmpv6->chksum = 0xffff;
    }

  ninfo("Outgoing ICMPv6 packet length: %d (%d)\n",
        dev->d_len, (ipv6->len[0] << 8) | ipv6->len[1]);

#ifdef CONFIG_NET_STATISTICS
  g_netstats.icmpv6.sent++;
  g_netstats.ipv6.sent++;
#endif
}

/****************************************************************************
 * Name: sendto_eventhandler
 *
 * Description:
 *   This function is called with the network locked to perform the actual
 *   ECHO request and/or ECHO reply actions when polled by the lower, device
 *   interfacing layer.
 *
 * Input Parameters:
 *   dev        The structure of the network driver that generated the
 *              event
 *   pvconn     The received packet, cast to (void *)
 *   pvpriv     An instance of struct icmpv6_sendto_s cast to (void *)
 *   flags      Set of events describing why the callback was invoked
 *
 * Returned Value:
 *   Modified value of the input flags
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static uint16_t sendto_eventhandler(FAR struct net_driver_s *dev,
                                    FAR void *pvconn,
                                    FAR void *pvpriv, uint16_t flags)
{
  FAR struct icmpv6_sendto_s *pstate = (struct icmpv6_sendto_s *)pvpriv;

  ninfo("flags: %04x\n", flags);

  if (pstate != NULL)
    {
      /* Check if the network is still up */

      if ((flags & NETDEV_DOWN) != 0)
        {
          nerr("ERROR: Interface is down\n");
          pstate->snd_result = -ENETUNREACH;
          goto end_wait;
        }

      /* Check:
       *   If the outgoing packet is available (it may have been claimed
       *   by a sendto event handler serving a different thread)
       * -OR-
       *   If the output buffer currently contains unprocessed incoming
       *   data.
       * -OR-
       *   If we have already sent the ECHO request
       *
       * In the first two cases, we will just have to wait for the next
       * polling cycle.
       */

      if (dev->d_sndlen <= 0 &&           /* Packet available */
          (flags & ICMPv6_NEWDATA) == 0)  /* No incoming data */
        {
          /* Send the ICMPv6 echo request.  */

          ninfo("Send ICMPv6 ECHO request\n");

          sendto_request(dev, pstate);
          pstate->snd_result = OKK;
          goto end_wait;
        }

#ifdef CONFIG_NET_SOCKOPTS
      /* Check if the selected timeout has elapsed */

      if (sendto_timeout(pstate))
        {
          int failcode;

          /* Check if this device is on the same network as the destination
           * device.
           */

          if (!net_ipv6addr_maskcmp(pstate->snd_toaddr.s6_addr16,
                                    dev->d_ipv6addr, dev->d_ipv6netmask))
            {
              /* Destination address was not on the local network served by this
               * device.  If a timeout occurs, then the most likely reason is
               * that the destination address is not reachable.
               */

              nerr("ERROR:  Not reachable\n");
              failcode = -ENETUNREACH;
            }
          else
            {
              nerr("ERROR:  sendto() timeout\n");
              failcode = -ETIMEDOUT;
            }

          /* Report the failure */

          pstate->snd_result = failcode;
          goto end_wait;
        }
#endif

      /* Continue waiting */
    }

  return flags;

end_wait:
  ninfo("Resuming\n");

  /* Do not allow any further callbacks */

  pstate->snd_cb->flags   = 0;
  pstate->snd_cb->priv    = NULL;
  pstate->snd_cb->event   = NULL;

  /* Wake up the waiting thread */

  nxsem_post(&pstate->snd_sem);
  return flags;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: icmpv6_sendto
 *
 * Description:
 *   Implements the sendto() operation for the case of the IPPROTO_ICMP6
 *   socket.  The 'buf' parameter points to a block of memory that includes
 *   an ICMPv6 request header, followed by any payload that accompanies the
 *   request.  The 'len' parameter includes both the size of the ICMPv6 header
 *   and the following payload.
 *
 * Input Parameters:
 *   psock    A pointer to a NuttX-specific, internal socket structure
 *   buf      Data to send
 *   len      Length of data to send
 *   flags    Send flags
 *   to       Address of recipient
 *   tolen    The length of the address structure
 *
 * Returned Value:
 *   On success, returns the number of characters sent.  On  error, a negated
 *   errno value is returned (see send_to() for the list of appropriate error
 *   values.
 *
 ****************************************************************************/

ssize_t icmpv6_sendto(FAR struct socket *psock, FAR const void *buf, size_t len,
                    int flags, FAR const struct sockaddr *to, socklen_t tolen)
{
  FAR const struct sockaddr_in6 *inaddr;
  FAR struct net_driver_s *dev;
  FAR struct icmpv6_conn_s *conn;
  FAR struct icmpv6_echo_request_s *icmpv6;
  struct icmpv6_sendto_s state;
  int ret;

  /* Some sanity checks */

  DEBUGASSERT(psock != NULL && psock->s_conn != NULL &&
              buf != NULL && to != NULL);

  if (len < ICMPv6_HDRLEN || tolen < sizeof(struct sockaddr_in6))
    {
      return -EINVAL;
    }

  conn   = psock->s_conn;
  inaddr = (FAR const struct sockaddr_in6 *)to;

  /* Get the device that will be used to route this ICMPv6 ECHO request */

  dev = netdev_findby_ripv6addr(g_ipv6_unspecaddr,
                               inaddr->sin6_addr.s6_addr16);
  if (dev == NULL)
    {
      nerr("ERROR: Not reachable\n");
      ret = -ENETUNREACH;
      goto errout;
    }

  /* If we are no longer processing the same ping ID, then flush any pending
   * packets from the read-ahead buffer.
   *
   * REVISIT:  How to we free up any lingering reponses if there are no
   * futher pings?
   */

  icmpv6 = (FAR struct icmpv6_echo_request_s *)buf;
  if (icmpv6->type != ICMPv6_ECHO_REQUEST || icmpv6->id != conn->id ||
      dev != conn->dev)
    {
      conn->id    = 0;
      conn->nreqs = 0;
      conn->dev   = NULL;

      iob_free_queue(&conn->readahead, IOBUSER_NET_SOCK_ICMPv6);
    }

#ifdef CONFIG_NET_ICMPv6_NEIGHBOR
  /* Make sure that the IP address mapping is in the Neighbor Table */

  ret = icmpv6_neighbor(inaddr->sin6_addr.s6_addr16);
  if (ret < 0)
    {
      nerr("ERROR: Not reachable\n");
      ret = -ENETUNREACH;
      goto errout;
    }
#endif /* CONFIG_NET_ICMPv6_NEIGHBOR */

  /* Initialize the state structure */

  /* This semaphore is used for signaling and, hence, should not have
   * priority inheritance enabled.
   */

  nxsem_init(&state.snd_sem, 0, 0);
  nxsem_setprotocol(&state.snd_sem, SEM_PRIO_NONE);

  state.snd_sock   = psock;             /* The IPPROTO_ICMP6 socket instance */
  state.snd_result = -ENOMEM;           /* Assume allocation failure */
  state.snd_buf    = buf;               /* ICMPv6 header + data payload */
  state.snd_buflen = len;               /* Size of the ICMPv6 header+data payload */

  net_ipv6addr_copy(state.snd_toaddr.s6_addr16,
                    inaddr->sin6_addr.s6_addr16);

  net_lock();
  state.snd_time   = clock_systimer();

  /* Set up the callback */

  state.snd_cb = icmpv6_callback_alloc(dev, conn);
  if (state.snd_cb)
    {
      state.snd_cb->flags   = (ICMPv6_POLL | NETDEV_DOWN);
      state.snd_cb->priv    = (FAR void *)&state;
      state.snd_cb->event   = sendto_eventhandler;
      state.snd_result      = -EINTR; /* Assume sem-wait interrupted by signal */

      /* Setup to receive ICMPv6 ECHO replies */

      if (icmpv6->type == ICMPv6_ECHO_REQUEST)
        {
          conn->id    = icmpv6->id;
          conn->nreqs = 1;
        }

        conn->dev     = dev;

      /* Notify the device driver of the availability of TX data */

      netdev_txnotify_dev(dev);

      /* Wait for either the send to complete or for timeout to occur.
       * net_lockedwait will also terminate if a signal is received.
       */

      ninfo("Start time: 0x%08x\n", state.snd_time);
      net_lockedwait(&state.snd_sem);

      icmpv6_callback_free(dev, conn, state.snd_cb);
    }

  net_unlock();

  /* Return the negated error number in the event of a failure, or the
   * number of bytes sent on success.
   */

  if (state.snd_result < 0)
    {
      nerr("ERROR: Return error=%d\n", state.snd_result);
      ret = state.snd_result;
      goto errout;
    }

  return len;

errout:
  conn->id    = 0;
  conn->nreqs = 0;
  conn->dev   = NULL;

  iob_free_queue(&conn->readahead, IOBUSER_NET_SOCK_ICMPv6);
  return ret;
}

#endif /* CONFIG_NET_ICMPv6_SOCKET */

/*
 * Force-included (-include) into every third-party translation unit built for PS4.
 *
 * OpenOrbis ships musl headers (Linux flavoured) on top of a FreeBSD 9 kernel, so a
 * few constants and macros are missing or wrong for the kernel we actually run on.
 */
#ifndef STREAMIO_PS4_COMPAT_H
#define STREAMIO_PS4_COMPAT_H

#include <signal.h>
#include <sys/ioctl.h>

/* musl only provides LINUX_FIONBIO/LINUX_FIONREAD; the PS4 kernel wants FreeBSD's
 * <sys/filio.h> values: _IOW('f', 126, int) and _IOR('f', 127, int). */
#ifndef FIONBIO
#define FIONBIO 0x8004667e
#endif
#ifndef FIONREAD
#define FIONREAD 0x4004667f
#endif

/* OpenOrbis' struct sigaction names the union member __sa_sigaction, but its
 * sa_sigaction macro points at a member that does not exist. */
#undef sa_sigaction
#define sa_sigaction __sa_handler.__sa_sigaction

/* Socket option numbers. SOL_SOCKET options already use FreeBSD values, but the
 * IP, IPv6 and TCP level ones are musl's Linux numbers, and the kernel reads them as
 * FreeBSD's. Linux IP_TOS (1) is FreeBSD IP_OPTIONS: libtorrent setting TOS 0 put
 * an IP options block on every packet it sent, and those were dropped on the way
 * out, so no tracker or DHT node ever answered. Values from FreeBSD <netinet/in.h>,
 * <netinet6/in6.h> and <netinet/tcp.h>. */
#include <netinet/in.h>
#include <netinet/tcp.h>

#undef IP_OPTIONS
#define IP_OPTIONS 1
#undef IP_HDRINCL
#define IP_HDRINCL 2
#undef IP_TOS
#define IP_TOS 3
#undef IP_TTL
#define IP_TTL 4
#undef IP_MULTICAST_IF
#define IP_MULTICAST_IF 9
#undef IP_MULTICAST_TTL
#define IP_MULTICAST_TTL 10
#undef IP_MULTICAST_LOOP
#define IP_MULTICAST_LOOP 11
#undef IP_ADD_MEMBERSHIP
#define IP_ADD_MEMBERSHIP 12
#undef IP_DROP_MEMBERSHIP
#define IP_DROP_MEMBERSHIP 13
#undef IP_DONTFRAG
#define IP_DONTFRAG 67
#undef IP_RECVTOS
#define IP_RECVTOS 68
/* Linux only; FreeBSD sets the don't-fragment bit with IP_DONTFRAG instead */
#undef IP_MTU_DISCOVER
#undef IP_PMTUDISC_DONT
#undef IP_PMTUDISC_DO
#undef IP_PMTUDISC_PROBE

#undef IPV6_UNICAST_HOPS
#define IPV6_UNICAST_HOPS 4
#undef IPV6_MULTICAST_IF
#define IPV6_MULTICAST_IF 9
#undef IPV6_MULTICAST_HOPS
#define IPV6_MULTICAST_HOPS 10
#undef IPV6_MULTICAST_LOOP
#define IPV6_MULTICAST_LOOP 11
#undef IPV6_JOIN_GROUP
#define IPV6_JOIN_GROUP 12
#undef IPV6_LEAVE_GROUP
#define IPV6_LEAVE_GROUP 13
#undef IPV6_V6ONLY
#define IPV6_V6ONLY 27
#undef IPV6_RECVTCLASS
#define IPV6_RECVTCLASS 57
#undef IPV6_TCLASS
#define IPV6_TCLASS 61

#undef TCP_KEEPIDLE
#define TCP_KEEPIDLE 256
#undef TCP_KEEPINTVL
#define TCP_KEEPINTVL 512
#undef TCP_KEEPCNT
#define TCP_KEEPCNT 1024
/* Linux only */
#undef TCP_CORK
#undef TCP_NOTSENT_LOWAT

#endif

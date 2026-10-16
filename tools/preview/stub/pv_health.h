/* What the library's stub headers lack for this app, force-included into
 * every file of the preview by build.zsh (-include): the library's stub/
 * comes first on the include path, so a sysmodule/sysmodule.h here would
 * never be read. pv_app.c defines them. */
#ifndef PV_HEALTH_H
#define PV_HEALTH_H
#include "ppu-types.h"
s32 sysModuleUnload(u32 id);                 /* net_end: the netctl module; audio_end: the audio module */
s32 sysLv2FsRename(const char *from, const char *to);   /* boot.c: boot.txt to boot.old.txt at the cap */
#define SYSMODULE_AUDIO 0x11                 /* the Audio module's start */

/* The Network module's sockets: the library's net/socket.h and netinet/in.h
 * have no raw socket and no ICMP. */
#define SOCK_RAW 3
#define IPPROTO_ICMP 1
#define NET_EPROTONOSUPPORT 43

/* netctl as PSL1GHT's net/netctl.h has it: the library's stub has the
 * address alone. Its include guard is set here, so the library's file adds
 * nothing after this one. */
#define PV_NETCTL_H
#define NET_CTL_INFO_DEVICE 1
#define NET_CTL_INFO_ETHER_ADDR 2
#define NET_CTL_INFO_MTU 3
#define NET_CTL_INFO_LINK 4
#define NET_CTL_INFO_LINK_TYPE 5
#define NET_CTL_INFO_RSSI 11
#define NET_CTL_INFO_CHANNEL 12
#define NET_CTL_INFO_IP_CONFIG 13
#define NET_CTL_INFO_IP_ADDRESS 16
#define NET_CTL_INFO_NETMASK 17
#define NET_CTL_INFO_DEFAULT_ROUTE 18
#define NET_CTL_INFO_PRIMARY_DNS 19
#define NET_CTL_INFO_SECONDARY_DNS 20
#define NET_CTL_DEVICE_WIRED 0
#define NET_CTL_DEVICE_WIRELESS 1
#define NET_CTL_LINK_DISCONNECTED 0
#define NET_CTL_LINK_CONNECTED 1
#define NET_CTL_IP_DHCP 0
#define NET_CTL_IP_STATIC 1
#define NET_CTL_IP_PPPOE 2
union net_ctl_info {
    u32 device;
    u8 ether_addr[8];
    u32 mtu, link, link_type;
    u8 rssi, channel;
    u32 ip_config;
    char ip_address[16], netmask[16], default_route[16], primary_dns[16], secondary_dns[16];
    u32 pad[64];
};
s32 netCtlInit(void);
void netCtlTerm(void);
s32 netCtlGetInfo(s32 code, union net_ctl_info *info);
#endif

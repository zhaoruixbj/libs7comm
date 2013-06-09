#include "pcap.h"
#include "ppkt.h"
#include "err.h"

#include <assert.h>
#include <stdlib.h>
#include <pcap/pcap.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/tcp.h>

struct pcap_dev_t
{
    pcap_t *pcap;
    ppkt_receive_function_t receive;
    void *user;
};

struct pcap_dev_t* pcap_connect(const char *filename, ppkt_receive_function_t receive, void *user)
{
    assert(filename);
    assert(receive);

    char errbuf[PCAP_ERRBUF_SIZE];

    struct pcap_dev_t *dev = malloc(sizeof(struct pcap_dev_t));    
    if (! dev)
        return NULL;

    dev->receive = receive;
    dev->user = user;
    dev->pcap = pcap_open_offline(filename, errbuf);
    if (! dev->pcap)
    {
        free(dev);
        return NULL;
    }

    return dev;
}

void pcap_disconnect(struct pcap_dev_t *dev)
{
    assert(dev);
    pcap_close(dev->pcap);
    free(dev);
}

static void pcap_receive_tcp(struct pcap_dev_t *dev, struct ppkt_t *p)
{
    assert(dev);
    assert(p);

    if (ppkt_size(p) < sizeof(struct tcphdr))
    {
        ppkt_free(p);
        return;
    }

    struct tcphdr *tcph = (struct tcphdr*)ppkt_payload(p);
    uint16_t src_port = ntohs(tcph->source);
    int hdr_len = tcph->doff * 4;
    assert(hdr_len >= sizeof(struct tcphdr));

    ppkt_pull(p, hdr_len);

    if (ppkt_size(p) == 0)
    {
        // ACK packet?
        ppkt_free(p);
        return;
    }

    if (src_port == 102)
        dev->receive(p, dev->user);
    else
        ppkt_free(p);
}

static void pcap_receive_ipv4(struct pcap_dev_t *dev, struct ppkt_t *p)
{
    assert(dev);
    assert(p);

    size_t size = ppkt_size(p);
    if (size < sizeof(struct iphdr))
    {
        ppkt_free(p);
        return;
    }

    struct iphdr *iph = (struct iphdr*)ppkt_payload(p);
    assert(iph->version == 4);

    uint8_t ip_proto = iph->protocol;
    uint8_t header_len = iph->ihl * 4;
    uint16_t tot_len = ntohs(iph->tot_len);

    if (tot_len > size)
    {
        // Invalid packet!
        ppkt_free(p);
        return;
    }

    ppkt_pull(p, header_len);

    // Note that the ppkt size might be bigger than the actual payload size as
    // the ethernet layer tries to avoid runt packets, so cut down the ppkt to
    // match the payload
    ppkt_cut(p, size - tot_len);

    if (ip_proto == IPPROTO_TCP)
        pcap_receive_tcp(dev, p);
    else
        ppkt_free(p);
}

static void pcap_receive(u_char *user, const struct pcap_pkthdr *h,
        const u_char *bytes)
{
    assert(user);
    struct pcap_dev_t *dev = (struct pcap_dev_t*)user;

    // Turn this into a ppkt_t for future processing
    struct ppkt_t *p = ppkt_create((uint8_t*)bytes, h->caplen);

    if (ppkt_size(p) < sizeof(struct ethhdr))
    {
        ppkt_free(p);
        return;
    }
    
    struct ethhdr *eh = (struct ethhdr*)ppkt_payload(p);
    uint16_t eth_proto = ntohs(eh->h_proto);
    if (eth_proto != ETH_P_IP)
    {
        ppkt_free(p);
        return;
    }

    ppkt_pull(p, sizeof(struct ethhdr));

    pcap_receive_ipv4(dev, p);
}

err_t pcap_poll(struct pcap_dev_t *dev)
{
    assert(dev);
    assert(dev->pcap);

    int ret = pcap_dispatch(dev->pcap, 1, pcap_receive, (void*)dev);
    if (ret <= 0)
        return ERR_CONNECTION_CLOSED;

    return ERR_NONE;
}


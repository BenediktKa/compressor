#include <linux/bpf.h>
#include <linux/bpf_common.h>
#include <linux/if_link.h>
#include <libbpf.h>
#include <bpf.h>
#include <signal.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include "compressor_filter_user.h"
#include "config.h"

static void cleanup_interface(void) {
    bpf_set_link_xdp_fd(ifindex, -1, XDP_FLAGS_SKB_MODE);
}

static void int_exit(int sig) {
    cleanup_interface();
    exit(0);
}

int load_xdp_prog(struct service_def **services, struct config *cfg) {
    const char *filename = "/etc/compressor/compressor_filter_kern.o";

    struct bpf_prog_load_attr prog_load_attr = {
        .prog_type = BPF_PROG_TYPE_XDP,
    };
    prog_load_attr.file = filename;

    struct bpf_object *obj;
    int prog_fd;
    if (bpf_prog_load_xattr(&prog_load_attr, &obj, &prog_fd)) {
        fprintf(stderr, "Error loading XDP program");
        return 1;
    }

    struct bpf_map *map;
    map = bpf_map__next(NULL, obj);
    if (!map) {
        fprintf(stderr, "Error finding IP blacklist in XDP program\n");
        return 1;
    }
    int ip_blacklist_fd = bpf_map__fd(map);
    
    map = bpf_map__next(map, obj);
    if (!map) {
        fprintf(stderr, "Error finding TCP service map in XDP program\n");
        return 1;
    }
    int tcp_service_fd = bpf_map__fd(map);

    map = bpf_map__next(map, obj);
    if (!map) {
        fprintf(stderr, "Error finding UDP service map in XDP program\n");
        return 1;
    }
    int udp_service_fd = bpf_map__fd(map);

    map = bpf_map__next(map, obj);
    if (!map) {
        fprintf(stderr, "Error finding config map in XDP program\n");
        return 1;
    }
    int config_map_fd = bpf_map__fd(map);

    struct service_def *service;
    int idx = 0;
    uint8_t enable = 1;

    int err = 0;
    while ((service = services[idx]) != NULL) {
        uint32_t dest = (uint32_t)service->port;
        if (service->proto == PROTO_TCP) {
            err = bpf_map_update_elem(tcp_service_fd, &dest, &enable, BPF_ANY);
        } else if (service->proto == PROTO_UDP) {
            err = bpf_map_update_elem(udp_service_fd, &dest, &enable, BPF_ANY);
        }

        if (err) {
            fprintf(stderr, "Store service port failed: (err:%d)\n", err);
            perror("bpf_map_update_elem");
            return 1;
        }

        idx++;
    }

    uint32_t key = 0;
    err = bpf_map_update_elem(config_map_fd, &key, cfg, BPF_ANY);
    if (err) {
        fprintf(stderr, "Store config failed: (err:%d)\n", err);
        perror("bpf_map_update_elem");
        return 1;
    }
    
    if (!prog_fd) {
        perror("load_bpf_file");
        return 1;
    }


    signal(SIGINT, int_exit);
    signal(SIGTERM, int_exit);
    signal(SIGKILL, int_exit);
    atexit(cleanup_interface);

    if (bpf_set_link_xdp_fd(ifindex, prog_fd, XDP_FLAGS_SKB_MODE) < 0) {
        fprintf(stderr, "link set xdp failed\n");
        return 1;
    }

    return 0;
}

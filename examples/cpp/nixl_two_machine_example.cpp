/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <iostream>
#include <cassert>
#include <cstring>
#include <getopt.h>
#include <unistd.h>

#include <sys/time.h>

#include "nixl.h"
#include "test_utils.h"


// iperf-style usage:
//   Server:  ./nixl_two_machine_example -s -n myAgent -p peerAgent
//   Client:  ./nixl_two_machine_example -c -n myAgent -p peerAgent
//
// Requires NIXL_ETCD_ENDPOINTS env var (e.g. http://etcd-host:2379)

struct Config {
    bool        isClient  = false;
    bool        isServer  = false;
    std::string agentName;
    std::string peerName;
};

void printUsage(const char *prog) {
    std::cerr
        << "Usage: " << prog << " <-c|-s> -n <agent_name> -p <peer_name>\n"
        << "\n"
        << "  -c              Run as client (requestor / initiator)\n"
        << "  -s              Run as server (responder / target)\n"
        << "  -n <name>       This agent's unique name\n"
        << "  -p <name>       Remote peer agent name\n"
        << "  -h              Show this help\n"
        << "\n"
        << "Environment:\n"
        << "  NIXL_ETCD_ENDPOINTS   etcd endpoint (e.g. http://etcd-host:2379)\n";
}

Config parseArgs(int argc, char **argv) {
    Config cfg;
    int opt;

    while ((opt = getopt(argc, argv, "csn:p:h")) != -1) {
        switch (opt) {
        case 'c': cfg.isClient = true;     break;
        case 's': cfg.isServer = true;     break;
        case 'n': cfg.agentName = optarg;  break;
        case 'p': cfg.peerName  = optarg;  break;
        case 'h':
            printUsage(argv[0]);
            exit(0);
        default:
            printUsage(argv[0]);
            exit(1);
        }
    }

    if (cfg.isClient == cfg.isServer) {
        std::cerr << "Error: specify exactly one of -c (client) or -s (server)\n\n";
        printUsage(argv[0]);
        exit(1);
    }
    if (cfg.agentName.empty() || cfg.peerName.empty()) {
        std::cerr << "Error: -n <agent_name> and -p <peer_name> are required\n\n";
        printUsage(argv[0]);
        exit(1);
    }

    return cfg;
}

void check_buf(void* buf, size_t len, const std::string &agent) {
    for (size_t i = 0; i < len; i++) {
        nixl_exit_on_failure((((uint8_t *)buf)[i] == 0xbb), "Data mismatch!", agent);
    }
}

bool equal_buf (void* buf1, void* buf2, size_t len) {
    for (size_t i = 0; i<len; i++)
        if (((uint8_t*) buf1)[i] != ((uint8_t*) buf2)[i])
            return false;
    return true;
}

void printParams(const nixl_b_params_t& params, const nixl_mem_list_t& mems) {
    if (params.empty()) {
        std::cout << "Parameters: (empty)" << std::endl;
        return;
    }

    std::cout << "Parameters:" << std::endl;
    for (const auto& pair : params) {
        std::cout << "  " << pair.first << " = " << pair.second << std::endl;
    }

    if (mems.empty()) {
        std::cout << "Mems: (empty)" << std::endl;
        return;
    }

    std::cout << "Mems:" << std::endl;
    for (const auto& elm : mems) {
        std::cout << "  " << nixlEnumStrings::memTypeStr(elm) << std::endl;
    }
}

int
main(int argc, char **argv) {
    Config config = parseArgs(argc, argv);

    const std::string &myName   = config.agentName;
    const std::string &peerName = config.peerName;
    const bool isRequestor      = config.isClient;

    nixl_status_t ret;
    std::string backend = "UCX";

    std::cout << "Role: " << (isRequestor ? "client" : "server")
              << "  Agent: " << myName
              << "  Peer: " << peerName << "\n";

    // Same setup as nixl_example.cpp, but with a single agent

    nixlAgentConfig cfg;
    cfg.useProgThread = true;
    nixl_b_params_t init;
    nixl_mem_list_t mems;

    nixlAgent agent(myName, cfg);

    std::vector<nixl_backend_t> plugins;

    ret = agent.getAvailPlugins(plugins);
    nixl_exit_on_failure(ret, "Failed to get available plugins", myName);

    std::cout << "Available plugins:\n";

    for (nixl_backend_t b: plugins)
        std::cout << b << "\n";

    std::cout << "Using backend: " << backend << "\n";
    ret = agent.getPluginParams(backend, mems, init);
    nixl_exit_on_failure(ret, "Failed to get plugin params", myName);

    std::cout << "Params before init:\n";
    printParams(init, mems);

    nixlBackendH *bknd;
    ret = agent.createBackend(backend, init, bknd);
    nixl_exit_on_failure(ret, "Failed to create " + backend + " backend", myName);

    nixl_opt_args_t extra_params;
    extra_params.backends.push_back(bknd);

    ret = agent.getBackendParams(bknd, mems, init);
    nixl_exit_on_failure(ret, "Failed to get " + backend + " backend params", myName);

    std::cout << "Params after init:\n";
    printParams(init, mems);

    // User allocates memories, and passes the corresponding address
    // and length to register with the backend
    nixlBlobDesc buff;
    nixl_reg_dlist_t dlist(DRAM_SEG);
    size_t len = 256;
    void* addr = calloc(1, len);

    if (isRequestor) {
        memset(addr, 0xbb, len);
    } else {
        memset(addr, 0, len);
    }

    buff.addr   = (uintptr_t) addr;
    buff.len    = len;
    buff.devId = 0;
    dlist.addDesc(buff);

    ret = agent.registerMem(dlist, &extra_params);
    nixl_exit_on_failure(ret, "Failed to register memory", myName);

    // Step 1: Exchange metadata via etcd (connection setup + memory keys)
    ret = agent.sendLocalMD();
    nixl_exit_on_failure(ret, "Failed to send local MD to etcd", myName);

    std::cout << "Published metadata to etcd\n";

    // fetchRemoteMD is asynchronous — it enqueues a fetch and returns
    // NIXL_SUCCESS immediately. We call it once, then poll genNotif
    // (which requires the remote metadata to be fully loaded) to know
    // when the peer's metadata has actually arrived.
    ret = agent.fetchRemoteMD(peerName);
    nixl_exit_on_failure(ret, "Failed to enqueue fetch of remote MD", myName);

    std::cout << "Waiting for peer metadata from etcd...\n";

    // Step 2: Server sends its buffer address to client via genNotif.
    // NIXL metadata carries memory keys internally but doesn't expose
    // remote addresses, so we exchange them via the notification channel.
    // We retry genNotif until the async fetchRemoteMD has loaded the
    // peer's backend info.
    uintptr_t remote_addr = 0;

    if (!isRequestor) {
        std::string addr_msg(reinterpret_cast<const char*>(&buff.addr), sizeof(buff.addr));
        int retries = 0;
        const int max_retries = 60;
        do {
            ret = agent.genNotif(peerName, addr_msg, &extra_params);
            if (ret != NIXL_SUCCESS) {
                if (++retries > max_retries) {
                    nixl_exit_on_failure(ret, "Timed out sending buffer address", myName);
                }
                sleep(1);
            }
        } while (ret != NIXL_SUCCESS);
        std::cout << "Sent buffer address to client\n";
    }

    if (isRequestor) {
        nixl_notifs_t notif_map;
        int n_notifs = 0;

        std::cout << "Waiting for server buffer address...\n";

        while (n_notifs == 0) {
            ret = agent.getNotifs(notif_map);
            nixl_exit_on_failure(ret, "Failed to get notifs", myName);
            n_notifs = notif_map.size();
        }

        std::string addr_msg = notif_map[peerName].front();
        memcpy(&remote_addr, addr_msg.data(), sizeof(remote_addr));

        std::cout << "Received server buffer address: 0x" << std::hex
                  << remote_addr << std::dec << "\n";
    }

    // Step 3: Client creates and posts the transfer using the server's address
    if (isRequestor) {
        size_t req_size = 8;
        size_t dst_offset = 8;

        nixl_xfer_dlist_t req_src_descs (DRAM_SEG);
        nixlBasicDesc req_src;
        req_src.addr     = (uintptr_t) (((char*) addr) + 16);
        req_src.len      = req_size;
        req_src.devId   = 0;
        req_src_descs.addDesc(req_src);

        nixl_xfer_dlist_t req_dst_descs (DRAM_SEG);
        nixlBasicDesc req_dst;
        req_dst.addr   = remote_addr + dst_offset;
        req_dst.len    = req_size;
        req_dst.devId = 0;
        req_dst_descs.addDesc(req_dst);

        std::cout << "Transfer request from local+" << 16
                  << " to remote+0x" << std::hex << dst_offset << std::dec << "\n";
        nixlXferReqH *req_handle;

        extra_params.notif = "transfer_done";
        int xfer_retries = 0;
        do {
            ret = agent.createXferReq(NIXL_WRITE, req_src_descs, req_dst_descs, peerName, req_handle, &extra_params);
            if (ret != NIXL_SUCCESS) {
                if (++xfer_retries > 60) {
                    nixl_exit_on_failure(ret, "Timed out creating Xfer Req", myName);
                }
                sleep(1);
            }
        } while (ret != NIXL_SUCCESS);

        nixl_status_t status = agent.postXferReq(req_handle);
        nixl_exit_on_failure((status >= NIXL_SUCCESS), "Failed to post Xfer Req", myName);

        std::cout << "Transfer was posted\n";

        while (status != NIXL_SUCCESS) {
            status = agent.getXferStatus(req_handle);
            nixl_exit_on_failure((status >= NIXL_SUCCESS), "Xfer failed", myName);
        }

        std::cout << "Transfer complete\n";

        ret = agent.releaseXferReq(req_handle);
        nixl_exit_on_failure(ret, "Failed to release Xfer Req", myName);
    }

    // Step 4: Server waits for transfer completion notification
    if (!isRequestor) {
        nixl_notifs_t notif_map;
        int n_notifs = 0;

        std::cout << "Waiting for transfer notification...\n";

        while (n_notifs == 0) {
            ret = agent.getNotifs(notif_map);
            nixl_exit_on_failure(ret, "Failed to get notifs", myName);
            n_notifs = notif_map.size();
        }

        std::vector<std::string> peer_notifs = notif_map[peerName];
        nixl_exit_on_failure((peer_notifs.size() == 1), "Incorrect notif size", myName);
        nixl_exit_on_failure(
            (peer_notifs.front() == "transfer_done"), "Incorrect notification", myName);

        std::cout << "Received notification: " << peer_notifs.front() << "\n";
    }

    // Teardown
    ret = agent.deregisterMem(dlist, &extra_params);
    nixl_exit_on_failure(ret, "Failed to deregister memory", myName);

    ret = agent.invalidateRemoteMD(peerName);
    nixl_exit_on_failure(ret, "Failed to invalidate remote MD", myName);

    ret = agent.invalidateLocalMD();
    nixl_exit_on_failure(ret, "Failed to invalidate local MD", myName);

    free(addr);

    std::cout << "Done (" << (isRequestor ? "client" : "server") << ")\n";
    return 0;
}

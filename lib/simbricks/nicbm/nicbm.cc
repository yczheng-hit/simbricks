/*
 * Copyright 2021 Max Planck Institute for Software Systems, and
 * National University of Singapore
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <simbricks/nicbm/nicbm.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <ctime>
#include <iostream>

// #define DEBUG_NICBM 1

#define DMA_MAX_PENDING 64

namespace nicbm {

static volatile int exiting = 0;

static uint64_t main_time = 0;

static void sigint_handler(int dummy) {
  exiting = 1;
}

static void sigusr1_handler(int dummy) {
  fprintf(stderr, "main_time = %lu\n", main_time);
}

volatile union SimbricksProtoPcieD2H *Runner::d2h_alloc(void) {
  volatile union SimbricksProtoPcieD2H *msg;
  while ((msg = nicsim_d2h_alloc(&nsparams, main_time)) == NULL) {
    fprintf(stderr, "d2h_alloc: no entry available\n");
  }
  return msg;
}

volatile union cosim_eth_proto_d2n *Runner::d2n_alloc(void) {
  volatile union cosim_eth_proto_d2n *msg;
  while ((msg = nicsim_d2n_alloc(&nsparams, main_time)) == NULL) {
    fprintf(stderr, "d2n_alloc: no entry available\n");
  }
  return msg;
}

void Runner::issue_dma(DMAOp &op) {
  if (dma_pending < DMA_MAX_PENDING) {
    // can directly issue
#ifdef DEBUG_NICBM
    printf("nicbm: issuing dma op %p addr %lx len %zu pending %zu\n", &op,
           op.dma_addr, op.len, dma_pending);
#endif
    dma_do(op);
  } else {
#ifdef DEBUG_NICBM
    printf("nicbm: enqueuing dma op %p addr %lx len %zu pending %zu\n", &op,
           op.dma_addr, op.len, dma_pending);
#endif
    dma_queue.push_back(&op);
  }
}

void Runner::dma_trigger() {
  if (dma_queue.empty() || dma_pending == DMA_MAX_PENDING)
    return;

  DMAOp *op = dma_queue.front();
  dma_queue.pop_front();

  dma_do(*op);
}

void Runner::dma_do(DMAOp &op) {
  volatile union SimbricksProtoPcieD2H *msg = d2h_alloc();
  dma_pending++;
#ifdef DEBUG_NICBM
  printf("nicbm: executing dma op %p addr %lx len %zu pending %zu\n", &op,
         op.dma_addr, op.len, dma_pending);
#endif

  if (op.write) {
    volatile struct SimbricksProtoPcieD2HWrite *write = &msg->write;
    if (dintro.d2h_elen < sizeof(*write) + op.len) {
      fprintf(stderr,
              "issue_dma: write too big (%zu), can only fit up "
              "to (%zu)\n",
              op.len, dintro.d2h_elen - sizeof(*write));
      abort();
    }

    write->req_id = (uintptr_t)&op;
    write->offset = op.dma_addr;
    write->len = op.len;
    memcpy((void *)write->data, (void *)op.data, op.len);
    // WMB();
    write->own_type =
        SIMBRICKS_PROTO_PCIE_D2H_MSG_WRITE | SIMBRICKS_PROTO_PCIE_D2H_OWN_HOST;
  } else {
    volatile struct SimbricksProtoPcieD2HRead *read = &msg->read;
    if (dintro.h2d_elen <
        sizeof(struct SimbricksProtoPcieH2DReadcomp) + op.len) {
      fprintf(stderr,
              "issue_dma: write too big (%zu), can only fit up "
              "to (%zu)\n",
              op.len,
              dintro.h2d_elen - sizeof(struct SimbricksProtoPcieH2DReadcomp));
      abort();
    }

    read->req_id = (uintptr_t)&op;
    read->offset = op.dma_addr;
    read->len = op.len;
    // WMB();
    read->own_type =
        SIMBRICKS_PROTO_PCIE_D2H_MSG_READ | SIMBRICKS_PROTO_PCIE_D2H_OWN_HOST;
  }
}

void Runner::msi_issue(uint8_t vec) {
  volatile union SimbricksProtoPcieD2H *msg = d2h_alloc();
#ifdef DEBUG_NICBM
  printf("nicbm: issue MSI interrupt vec %u\n", vec);
#endif
  volatile struct SimbricksProtoPcieD2HInterrupt *intr = &msg->interrupt;
  intr->vector = vec;
  intr->inttype = SIMBRICKS_PROTO_PCIE_INT_MSI;

  // WMB();
  intr->own_type =
      SIMBRICKS_PROTO_PCIE_D2H_MSG_INTERRUPT |
      SIMBRICKS_PROTO_PCIE_D2H_OWN_HOST;
}

void Runner::msix_issue(uint8_t vec) {
  volatile union SimbricksProtoPcieD2H *msg = d2h_alloc();
#ifdef DEBUG_NICBM
  printf("nicbm: issue MSI-X interrupt vec %u\n", vec);
#endif
  volatile struct SimbricksProtoPcieD2HInterrupt *intr = &msg->interrupt;
  intr->vector = vec;
  intr->inttype = SIMBRICKS_PROTO_PCIE_INT_MSIX;

  // WMB();
  intr->own_type =
      SIMBRICKS_PROTO_PCIE_D2H_MSG_INTERRUPT |
      SIMBRICKS_PROTO_PCIE_D2H_OWN_HOST;
}

void Runner::event_schedule(TimedEvent &evt) {
  events.insert(&evt);
}

void Runner::event_cancel(TimedEvent &evt) {
  events.erase(&evt);
}

void Runner::h2d_read(volatile struct SimbricksProtoPcieH2DRead *read) {
  volatile union SimbricksProtoPcieD2H *msg;
  volatile struct SimbricksProtoPcieD2HReadcomp *rc;

  msg = d2h_alloc();
  rc = &msg->readcomp;

  dev.reg_read(read->bar, read->offset, (void *)rc->data, read->len);
  rc->req_id = read->req_id;

#ifdef DEBUG_NICBM
  uint64_t dbg_val = 0;
  memcpy(&dbg_val, (const void *)rc->data, read->len <= 8 ? read->len : 8);
  printf("nicbm: read(off=0x%lx, len=%u, val=0x%lx)\n", read->offset, read->len,
         dbg_val);
#endif

  // WMB();
  rc->own_type =
      SIMBRICKS_PROTO_PCIE_D2H_MSG_READCOMP | SIMBRICKS_PROTO_PCIE_D2H_OWN_HOST;
}

void Runner::h2d_write(volatile struct SimbricksProtoPcieH2DWrite *write) {
  volatile union SimbricksProtoPcieD2H *msg;
  volatile struct SimbricksProtoPcieD2HWritecomp *wc;

  msg = d2h_alloc();
  wc = &msg->writecomp;

#ifdef DEBUG_NICBM
  uint64_t dbg_val = 0;
  memcpy(&dbg_val, (const void *)write->data, write->len <= 8 ? write->len : 8);
  printf("nicbm: write(off=0x%lx, len=%u, val=0x%lx)\n", write->offset,
         write->len, dbg_val);
#endif
  dev.reg_write(write->bar, write->offset, (void *)write->data, write->len);
  wc->req_id = write->req_id;

  // WMB();
  wc->own_type =
      SIMBRICKS_PROTO_PCIE_D2H_MSG_WRITECOMP |
      SIMBRICKS_PROTO_PCIE_D2H_OWN_HOST;
}

void Runner::h2d_readcomp(volatile struct SimbricksProtoPcieH2DReadcomp *rc) {
  DMAOp *op = (DMAOp *)(uintptr_t)rc->req_id;

#ifdef DEBUG_NICBM
  printf("nicbm: completed dma read op %p addr %lx len %zu\n", op, op->dma_addr,
         op->len);
#endif

  memcpy(op->data, (void *)rc->data, op->len);
  dev.dma_complete(*op);

  dma_pending--;
  dma_trigger();
}

void Runner::h2d_writecomp(volatile struct SimbricksProtoPcieH2DWritecomp *wc) {
  DMAOp *op = (DMAOp *)(uintptr_t)wc->req_id;

#ifdef DEBUG_NICBM
  printf("nicbm: completed dma write op %p addr %lx len %zu\n", op,
         op->dma_addr, op->len);
#endif

  dev.dma_complete(*op);

  dma_pending--;
  dma_trigger();
}

void Runner::h2d_devctrl(volatile struct SimbricksProtoPcieH2DDevctrl *dc) {
  dev.devctrl_update(*(struct SimbricksProtoPcieH2DDevctrl *)dc);
}

void Runner::eth_recv(volatile struct cosim_eth_proto_n2d_recv *recv) {
#ifdef DEBUG_NICBM
  printf("nicbm: eth rx: port %u len %u\n", recv->port, recv->len);
#endif

  dev.eth_rx(recv->port, (void *)recv->data, recv->len);
}

void Runner::eth_send(const void *data, size_t len) {
#ifdef DEBUG_NICBM
  printf("nicbm: eth tx: len %zu\n", len);
#endif

  volatile union cosim_eth_proto_d2n *msg = d2n_alloc();
  volatile struct cosim_eth_proto_d2n_send *send = &msg->send;
  send->port = 0;  // single port
  send->len = len;
  memcpy((void *)send->data, data, len);
  send->own_type = COSIM_ETH_PROTO_D2N_MSG_SEND | COSIM_ETH_PROTO_D2N_OWN_NET;
}

void Runner::poll_h2d() {
  volatile union SimbricksProtoPcieH2D *msg =
      nicif_h2d_poll(&nsparams, main_time);
  uint8_t type;

  if (msg == NULL)
    return;

  type = msg->dummy.own_type & SIMBRICKS_PROTO_PCIE_H2D_MSG_MASK;
  switch (type) {
    case SIMBRICKS_PROTO_PCIE_H2D_MSG_READ:
      h2d_read(&msg->read);
      break;

    case SIMBRICKS_PROTO_PCIE_H2D_MSG_WRITE:
      h2d_write(&msg->write);
      break;

    case SIMBRICKS_PROTO_PCIE_H2D_MSG_READCOMP:
      h2d_readcomp(&msg->readcomp);
      break;

    case SIMBRICKS_PROTO_PCIE_H2D_MSG_WRITECOMP:
      h2d_writecomp(&msg->writecomp);
      break;

    case SIMBRICKS_PROTO_PCIE_H2D_MSG_DEVCTRL:
      h2d_devctrl(&msg->devctrl);
      break;

    case SIMBRICKS_PROTO_PCIE_H2D_MSG_SYNC:
      break;

    default:
      fprintf(stderr, "poll_h2d: unsupported type=%u\n", type);
  }

  nicif_h2d_done(msg);
  nicif_h2d_next();
}

void Runner::poll_n2d() {
  volatile union cosim_eth_proto_n2d *msg =
      nicif_n2d_poll(&nsparams, main_time);
  uint8_t t;

  if (msg == NULL)
    return;

  t = msg->dummy.own_type & COSIM_ETH_PROTO_N2D_MSG_MASK;
  switch (t) {
    case COSIM_ETH_PROTO_N2D_MSG_RECV:
      eth_recv(&msg->recv);
      break;

    case COSIM_ETH_PROTO_N2D_MSG_SYNC:
      break;

    default:
      fprintf(stderr, "poll_n2d: unsupported type=%u", t);
  }

  nicif_n2d_done(msg);
  nicif_n2d_next();
}

uint64_t Runner::time_ps() const {
  return main_time;
}

uint64_t Runner::get_mac_addr() const {
  return mac_addr;
}

bool Runner::event_next(uint64_t &retval) {
  if (events.empty())
    return false;

  retval = (*events.begin())->time;
  return true;
}

void Runner::event_trigger() {
  auto it = events.begin();
  if (it == events.end())
    return;

  TimedEvent *ev = *it;

  // event is in the future
  if (ev->time > main_time)
    return;

  events.erase(it);
  dev.timed_event(*ev);
}

Runner::Runner(Device &dev_) : dev(dev_), events(event_cmp()) {
  // mac_addr = lrand48() & ~(3ULL << 46);
  dma_pending = 0;
  srand48(time(NULL) ^ getpid());
  mac_addr = lrand48();
  mac_addr <<= 16;
  mac_addr ^= lrand48();
  mac_addr &= ~3ULL;

  std::cerr << std::hex << mac_addr << std::endl;
}

int Runner::runMain(int argc, char *argv[]) {
  uint64_t next_ts;
  uint64_t max_step = 10000;
  uint64_t sync_period = 100 * 1000ULL;
  uint64_t pci_latency = 500 * 1000ULL;
  uint64_t eth_latency = 500 * 1000ULL;
  int sync_mode = SYNC_MODES;

  if (argc < 4 && argc > 9) {
    fprintf(stderr,
            "Usage: corundum_bm PCI-SOCKET ETH-SOCKET "
            "SHM [SYNC-MODE] [START-TICK] [SYNC-PERIOD] [PCI-LATENCY] "
            "[ETH-LATENCY]\n");
    return EXIT_FAILURE;
  }
  if (argc >= 5)
    sync_mode = strtol(argv[4], NULL, 0);
  if (argc >= 6)
    main_time = strtoull(argv[5], NULL, 0);
  if (argc >= 7)
    sync_period = strtoull(argv[6], NULL, 0) * 1000ULL;
  if (argc >= 8)
    pci_latency = strtoull(argv[7], NULL, 0) * 1000ULL;
  if (argc >= 9)
    eth_latency = strtoull(argv[8], NULL, 0) * 1000ULL;

  signal(SIGINT, sigint_handler);
  signal(SIGUSR1, sigusr1_handler);

  memset(&dintro, 0, sizeof(dintro));
  dev.setup_intro(dintro);

  nsparams.sync_pci = 1;
  nsparams.sync_eth = 1;
  nsparams.pci_socket_path = argv[1];
  nsparams.eth_socket_path = argv[2];
  nsparams.shm_path = argv[3];
  nsparams.pci_latency = pci_latency;
  nsparams.eth_latency = eth_latency;
  nsparams.sync_delay = sync_period;
  assert(sync_mode == SYNC_MODES || sync_mode == SYNC_BARRIER);
  nsparams.sync_mode = sync_mode;

  if (nicsim_init(&nsparams, &dintro)) {
    return EXIT_FAILURE;
  }
  fprintf(stderr, "sync_pci=%d sync_eth=%d\n", nsparams.sync_pci,
          nsparams.sync_eth);

  bool is_sync = nsparams.sync_pci || nsparams.sync_eth;

  while (!exiting) {
    while (nicsim_sync(&nsparams, main_time)) {
      fprintf(stderr, "warn: nicsim_sync failed (t=%lu)\n", main_time);
    }
    nicsim_advance_epoch(&nsparams, main_time);

    do {
      poll_h2d();
      poll_n2d();
      event_trigger();

      if (is_sync) {
        next_ts = nicsim_next_timestamp(&nsparams);
        if (next_ts > main_time + max_step)
          next_ts = main_time + max_step;
      } else {
        next_ts = main_time + max_step;
      }

      uint64_t ev_ts;
      if (event_next(ev_ts) && ev_ts < next_ts)
        next_ts = ev_ts;
    } while (next_ts <= main_time && !exiting);
    main_time = nicsim_advance_time(&nsparams, next_ts);
  }

  fprintf(stderr, "exit main_time: %lu\n", main_time);
  nicsim_cleanup();
  return 0;
}

void Runner::Device::timed_event(TimedEvent &te) {
}

void Runner::Device::devctrl_update(
    struct SimbricksProtoPcieH2DDevctrl &devctrl) {
  int_intx_en = devctrl.flags & SIMBRICKS_PROTO_PCIE_CTRL_INTX_EN;
  int_msi_en = devctrl.flags & SIMBRICKS_PROTO_PCIE_CTRL_MSI_EN;
  int_msix_en = devctrl.flags & SIMBRICKS_PROTO_PCIE_CTRL_MSIX_EN;
}

}  // namespace nicbm

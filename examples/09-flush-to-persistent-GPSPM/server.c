// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2020-2022, Intel Corporation */
/* Copyright 2021-2022, Fujitsu */

/*
 * server.c -- a server of the flush-to-persistent-GPSPM example
 *
 * Please see README.md for a detailed description of this example.
 */

#include <inttypes.h>
#include <librpma.h>
#include <stdlib.h>
#include <stdio.h>
#include "common-conn.h"
#include "common-map_file_with_signature_check.h"
#include "common-pmem_map_file.h"
#include "flush-to-persistent-GPSPM.h"

/* Generated by the protocol buffer compiler from: GPSPM_flush.proto */
#include "GPSPM_flush.pb-c.h"

#ifdef USE_PMEM
#define USAGE_STR "usage: %s <server_address> <port> [<pmem-path>]\n"PMEM_USAGE
#else
#define USAGE_STR "usage: %s <server_address> <port>\n"
#endif /* USE_PMEM */

int
main(int argc, char *argv[])
{
	/* validate parameters */
	if (argc < 3) {
		fprintf(stderr, USAGE_STR, argv[0]);
		return -1;
	}

	/* configure logging thresholds to see more details */
	rpma_log_set_threshold(RPMA_LOG_THRESHOLD, RPMA_LOG_LEVEL_INFO);
	rpma_log_set_threshold(RPMA_LOG_THRESHOLD_AUX, RPMA_LOG_LEVEL_INFO);

	/* read common parameters */
	char *addr = argv[1];
	char *port = argv[2];
	int ret;

	/* resources - memory region */
	struct common_mem mem;
	memset(&mem, 0, sizeof(mem));
	struct rpma_mr_local *mr = NULL;

	/* messaging resources */
	void *msg_ptr = NULL;
	void *send_ptr = NULL;
	void *recv_ptr = NULL;
	struct rpma_mr_local *msg_mr = NULL;
	GPSPMFlushRequest *flush_req;
	GPSPMFlushResponse flush_resp = GPSPM_FLUSH_RESPONSE__INIT;
	size_t flush_resp_size = 0;

#ifdef USE_PMEM
	char *pmem_path = NULL;

	if (argc >= 4) {
		pmem_path = argv[3];

		ret = common_pmem_map_file_with_signature_check(pmem_path, KILOBYTE, &mem);
		if (ret)
			goto err_free;
	}
#endif /* USE_PMEM */

	/* if no pmem support or it is not provided */
	if (mem.mr_ptr == NULL) {
		(void) fprintf(stderr, NO_PMEM_MSG);
		mem.mr_ptr = malloc_aligned(KILOBYTE);
		if (mem.mr_ptr == NULL)
			return -1;

		mem.mr_size = KILOBYTE;
	}

	/* allocate messaging buffer */
	msg_ptr = malloc_aligned(KILOBYTE);
	if (msg_ptr == NULL) {
		ret = -1;
		goto err_free;
	}
	send_ptr = (char *)msg_ptr + SEND_OFFSET;
	recv_ptr = (char *)msg_ptr + RECV_OFFSET;

	/* RPMA resources */
	struct rpma_peer *peer = NULL;
	struct rpma_ep *ep = NULL;
	struct rpma_conn_req *req = NULL;
	struct rpma_conn *conn = NULL;
	enum rpma_conn_event conn_event = RPMA_CONN_UNDEFINED;
	struct ibv_wc wc;

	/* if the string content is not empty */
	if (((char *)mem.mr_ptr + mem.data_offset)[0] != '\0') {
		(void) printf("Old value: %s\n", (char *)mem.mr_ptr + mem.data_offset);
	}

	/*
	 * lookup an ibv_context via the address and create a new peer using it
	 */
	if ((ret = server_peer_via_address(addr, &peer)))
		goto err_free;

	/* start a listening endpoint at addr:port */
	if ((ret = rpma_ep_listen(peer, addr, port, &ep)))
		goto err_peer_delete;

	/* register the memory */
	if ((ret = rpma_mr_reg(peer, mem.mr_ptr, mem.mr_size, RPMA_MR_USAGE_WRITE_DST |
			(mem.is_pmem ? RPMA_MR_USAGE_FLUSH_TYPE_PERSISTENT :
				RPMA_MR_USAGE_FLUSH_TYPE_VISIBILITY), &mr)))
		goto err_ep_shutdown;

#if defined USE_PMEM && defined IBV_ADVISE_MR_FLAGS_SUPPORTED
	/* rpma_mr_advise() should be called only in case of FsDAX */
	if (mem.is_pmem && strstr(pmem_path, "/dev/dax") == NULL) {
		ret = rpma_mr_advise(mr, 0, mem.mr_size, IBV_ADVISE_MR_ADVICE_PREFETCH_WRITE,
			IBV_ADVISE_MR_FLAG_FLUSH);
		if (ret)
			goto err_mr_dereg;
	}
#endif /* USE_PMEM && IBV_ADVISE_MR_FLAGS_SUPPORTED */

	/* register the messaging memory */
	if ((ret = rpma_mr_reg(peer, msg_ptr, KILOBYTE, RPMA_MR_USAGE_SEND | RPMA_MR_USAGE_RECV |
				RPMA_MR_USAGE_FLUSH_TYPE_VISIBILITY, &msg_mr))) {
		(void) rpma_mr_dereg(&mr);
		goto err_ep_shutdown;
	}

	/* get size of the memory region's descriptor */
	size_t mr_desc_size;
	ret = rpma_mr_get_descriptor_size(mr, &mr_desc_size);
	if (ret)
		goto err_mr_dereg;

	/* calculate data for the server read */
	struct common_data data = {0};
	data.data_offset = mem.data_offset;
	data.mr_desc_size = mr_desc_size;

	/* get the memory region's descriptor */
	if ((ret = rpma_mr_get_descriptor(mr, &data.descriptors[0])))
		goto err_mr_dereg;

	struct rpma_conn_cfg *cfg = NULL;
	if ((ret = rpma_conn_cfg_new(&cfg)))
		goto err_mr_dereg;

	if ((ret = rpma_conn_cfg_set_rcq_size(cfg, RCQ_SIZE)))
		goto err_cfg_delete;

	/*
	 * Wait for an incoming connection request, accept it and wait for its
	 * establishment.
	 */
	struct rpma_conn_private_data pdata;
	pdata.ptr = &data;
	pdata.len = sizeof(struct common_data);

	/* receive an incoming connection request */
	if ((ret = rpma_ep_next_conn_req(ep, cfg, &req)))
		goto err_req_delete;

	/* prepare buffer for a flush request */
	if ((ret = rpma_conn_req_recv(req, msg_mr, RECV_OFFSET, MSG_SIZE_MAX, NULL)))
		goto err_req_delete;

	/* accept the connection request and obtain the connection object */
	if ((ret = rpma_conn_req_connect(&req, &pdata, &conn))) {
		(void) rpma_conn_req_delete(&req);
		goto err_req_delete;
	}

	/* wait for the connection to be established */
	ret = rpma_conn_next_event(conn, &conn_event);
	if (!ret && conn_event != RPMA_CONN_ESTABLISHED) {
		fprintf(stderr,
			"rpma_conn_next_event returned an unexpected event: %s\n",
			rpma_utils_conn_event_2str(conn_event));
		ret = -1;
	}
	if (ret)
		goto err_conn_delete;

	/* wait for the receive completion to be ready */
	struct rpma_cq *rcq = NULL;
	if ((ret = rpma_conn_get_rcq(conn, &rcq)))
		goto err_conn_delete;
	if ((ret = rpma_cq_wait(rcq)))
		goto err_conn_delete;
	if ((ret = rpma_cq_get_wc(rcq, 1, &wc, NULL)))
		goto err_conn_delete;

	/* validate the receive completion */
	if (wc.status != IBV_WC_SUCCESS) {
		ret = -1;
		(void) fprintf(stderr, "rpma_recv() failed: %s\n",
				ibv_wc_status_str(wc.status));
		goto err_conn_delete;
	}

	if (wc.opcode != IBV_WC_RECV) {
		ret = -1;
		(void) fprintf(stderr,
				"unexpected wc.opcode value "
				"(0x%" PRIXPTR " != 0x%" PRIXPTR ")\n",
				(uintptr_t)wc.opcode,
				(uintptr_t)IBV_WC_RECV);
		goto err_conn_delete;
	}

	/* unpack a flush request from the received buffer */
	flush_req = gpspm_flush_request__unpack(NULL, wc.byte_len, recv_ptr);
	if (flush_req == NULL) {
		fprintf(stderr, "Cannot unpack the flush request buffer\n");
		goto err_conn_delete;
	}
	(void) printf("Flush request received: {offset: 0x%" PRIXPTR
			", length: 0x%" PRIXPTR ", op_context: 0x%" PRIXPTR
			"}\n", flush_req->offset, flush_req->length,
			flush_req->op_context);

#ifdef USE_PMEM
	if (mem.is_pmem) {
		void *op_ptr = (char *)mem.mr_ptr + flush_req->offset;
		mem.persist(op_ptr, flush_req->length);
	}
#else
	(void) printf(
			"At this point, persist function should be called if persistent memory will be in use\n");
#endif /* USE_PMEM */

	/* prepare a flush response and pack it to a send buffer */
	flush_resp.op_context = flush_req->op_context;
	flush_resp_size = gpspm_flush_response__get_packed_size(&flush_resp);
	if (flush_resp_size > MSG_SIZE_MAX) {
		fprintf(stderr,
				"Size of the packed flush response is bigger than the available space of the send buffer (%"
				PRIu64 " > %u\n", flush_resp_size,
				MSG_SIZE_MAX);
		goto err_conn_delete;
	}
	(void) gpspm_flush_response__pack(&flush_resp, send_ptr);
	gpspm_flush_request__free_unpacked(flush_req, NULL);

	/* send the flush response */
	if ((ret = rpma_send(conn, msg_mr, SEND_OFFSET, flush_resp_size, RPMA_F_COMPLETION_ALWAYS,
			NULL)))
		goto err_conn_delete;

	/* wait for the send completion to be ready */
	struct rpma_cq *cq = NULL;
	if ((ret = rpma_conn_get_cq(conn, &cq)))
		goto err_conn_delete;
	if ((ret = rpma_cq_wait(cq)))
		goto err_conn_delete;
	if ((ret = rpma_cq_get_wc(cq, 1, &wc, NULL)))
		goto err_conn_delete;

	/* validate the send completion */
	if (wc.status != IBV_WC_SUCCESS) {
		ret = -1;
		(void) fprintf(stderr, "rpma_send() failed: %s\n",
				ibv_wc_status_str(wc.status));
		goto err_conn_delete;
	}

	if (wc.opcode != IBV_WC_SEND) {
		ret = -1;
		(void) fprintf(stderr,
				"unexpected wc.opcode value "
				"(0x%" PRIXPTR " != 0x%" PRIXPTR ")\n",
				(uintptr_t)wc.opcode,
				(uintptr_t)IBV_WC_SEND);
		goto err_conn_delete;
	}

	/*
	 * Wait for RPMA_CONN_CLOSED, disconnect and delete the connection
	 * structure.
	 */
	ret = common_wait_for_conn_close_and_disconnect(&conn);
	if (ret)
		goto err_conn_delete;

	(void) printf("New value: %s\n", (char *)mem.mr_ptr + mem.data_offset);

err_conn_delete:
	(void) rpma_conn_delete(&conn);

err_req_delete:
	(void) rpma_conn_req_delete(&req);

err_cfg_delete:
	(void) rpma_conn_cfg_delete(&cfg);

err_mr_dereg:
	(void) rpma_mr_dereg(&msg_mr);
	(void) rpma_mr_dereg(&mr);

err_ep_shutdown:
	/* shutdown the endpoint */
	(void) rpma_ep_shutdown(&ep);

err_peer_delete:
	/* delete the peer object */
	(void) rpma_peer_delete(&peer);

err_free:
	free(msg_ptr);

#ifdef USE_PMEM
	if (mem.is_pmem) {
		common_pmem_unmap_file(&mem);

	} else
#endif /* USE_PMEM */

	if (mem.mr_ptr != NULL)
		free(mem.mr_ptr);

	return ret;
}
